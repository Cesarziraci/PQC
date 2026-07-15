#include "contiki.h"
#include "protocol.h"
#include "PQC.h"
#include "handshake_fsm.h"
#include "session_manager.h"
#include "temp_measurement.h"
#include "metrics.h"
#include "reset_diagnostics.h"
#include "net/ipv6/uip-ds6-route.h"
#include "em_gpio.h"
#include "i2cspm.h"
#include "si7021.h"

/*---------------------------------------------------------------------------*/
/*--------------------------- GLOBAL VARIABLES -----------------------------*/
/*---------------------------------------------------------------------------*/

static struct simple_udp_connection udp_conn;
static uip_ipaddr_t dest_address;
struct etimer measurement_timer, send_timer, routing_timer;
static struct etimer retry_timer;
static session_t *session;
static uint16_t node_id;
static pqc_udp_send_ctx_t send_ctx;
static uint8_t getpk_in_flight;
static uint8_t metrics_pending;
static uint8_t has_last_next_hop;
static uip_ipaddr_t last_next_hop;

/*---------------------------------------------------------------------------*/
/*--------------------------- PROCESS DEFINITIONS ---------------------------*/
/*---------------------------------------------------------------------------*/

PROCESS(PQC_process, "PQC");
PROCESS(PQC_send_process, "PQC sender");
PROCESS(PQC_retry_process, "PQC retry");
PROCESS(Temp_measurement_process, "Temp_measurement");
AUTOSTART_PROCESSES(&PQC_process, &PQC_send_process, &PQC_retry_process, &Temp_measurement_process);

/*---------------------------------------------------------------------------*/
/*--------------------------- FUNCTION DECLARATIONS -------------------------*/
/*---------------------------------------------------------------------------*/

static void udp_rx_callback(struct simple_udp_connection *c,
                            const uip_ipaddr_t *sender_addr,
                            uint16_t sender_port,
                            const uip_ipaddr_t *receiver_addr,
                            uint16_t receiver_port,
                            const uint8_t *data,
                            uint16_t datalen);
static void clear_getpk_wait(void);
static int should_request_pk(void);
static int send_node_metrics(void);
static void log_next_hop_if_changed(void);

/*---------------------------------------------------------------------------*/
/*--------------------------- PROCESS THREAD --------------------------------*/
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(PQC_process, ev, data)
{
  PROCESS_BEGIN();

  print_reset_cause();
  NETSTACK_ROUTING.init();

  printf("Starting PQC Process...\r");

  pqc_tx_init();
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT, udp_rx_callback);
  printf("[PQC] Node UDP listening on port %u\r\n", UDP_CLIENT_PORT);

  etimer_set(&routing_timer, CLOCK_SECOND * 4);

  node_id = pqc_node_id_from_linkaddr();
  session_manager_init(SESSION_ROLE_NODE);
  session = session_manager_get_local();
  session->node_id = node_id;
  getpk_in_flight = 0;
  metrics_pending = 0;
  has_last_next_hop = 0;

  send_ctx.udp_conn = &udp_conn;
  send_ctx.dest = &dest_address;

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&routing_timer));
    log_next_hop_if_changed();
    if(NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dest_address) && NETSTACK_ROUTING.node_has_joined()) {

      if(should_request_pk()) {
        printf("Node is reachable, root address: ");
        uiplib_ipaddr_print(&dest_address);
        printf("\r\n");
        printf("Session idle, sending GETPK\r\n");
        if(pqc_send_GETPK(&udp_conn, &dest_address, node_id)) {
          pqc_metrics_handshake_start(&session->metrics);
          session->metrics.handshake.getpk_tx++;
          getpk_in_flight = 1;
          session_set_wait_pk(session);
          session_set_pending(session, PQC_MSG_GETPK, 0);
        } else {
          printf("[PQC] GETPK enqueue failed, will retry later\r\n");
        }
      }
    }else{
      printf("Node not Reachable yet...\r\n");
    }
    etimer_reset(&routing_timer);
  }

  PROCESS_END();
}

PROCESS_THREAD(PQC_send_process, ev, data)
{
  PROCESS_BEGIN();

  etimer_set(&send_timer, SEND_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
    pqc_tx_process();
    etimer_reset(&send_timer);
  }

  PROCESS_END();
}

PROCESS_THREAD(PQC_retry_process, ev, data)
{
  PROCESS_BEGIN();

  etimer_set(&retry_timer, CLOCK_SECOND);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&retry_timer));

    if(session != NULL && !session_is_established(session) &&
       NETSTACK_ROUTING.node_is_reachable() &&
       NETSTACK_ROUTING.get_root_ipaddr(&dest_address)) {
      send_ctx.dest = &dest_address;

      hs_fsm_retry(session, pqc_udp_send_fragment, &send_ctx);

      if(session_is_failed(session)) {
        printf("Handshake failed, resetting session\r\n");
        session_reset(session);
        session->node_id = node_id;
        getpk_in_flight = 0;
      }
    }

    etimer_reset(&retry_timer);
  }

  PROCESS_END();
}

PROCESS_THREAD(Temp_measurement_process, ev, data)
{
  PROCESS_BEGIN();

  GPIO_PinModeSet(gpioPortF, 9, gpioModePushPull, 1);
  GPIO_PinOutSet(gpioPortF, 9);

  I2CSPM_Init_TypeDef i2cspmInit = I2CSPM_INIT_DEFAULT;
  I2CSPM_Init(&i2cspmInit);

  if(SI7021_init() != 0) {
    printf("Si7021 Fail\r\n");
    PROCESS_EXIT();
  }

  printf("Temp measurement initialized\r\n");

  etimer_set(&measurement_timer, MEASUREMENTS_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&measurement_timer));

    if(session_is_established(session)) {
      pqc_temp_payload_t measurement;

      temp_measurement_read(&measurement);
      print_measurement(measurement.rh_milli, measurement.temp_milli);
      pqc_send_encrypted_temp(session, &udp_conn, &dest_address, &measurement);
    }

    etimer_reset(&measurement_timer);
  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*--------------------------- FUNCTION DEFINITIONS --------------------------*/
/*---------------------------------------------------------------------------*/

static void
log_next_hop_if_changed(void)
{
  const uip_ipaddr_t *next_hop = uip_ds6_defrt_choose();

  if(next_hop == NULL) {
    if(has_last_next_hop) {
      printf("[TOPOLOGY] node=%u next_hop=none\r\n", node_id);
      has_last_next_hop = 0;
    }
    return;
  }

  if(!has_last_next_hop || !uip_ipaddr_cmp(&last_next_hop, next_hop)) {
    printf("[TOPOLOGY] node=%u next_hop=", node_id);
    uiplib_ipaddr_print(next_hop);
    printf("\r\n");

    uip_ipaddr_copy(&last_next_hop, next_hop);
    has_last_next_hop = 1;
  }
}

static void clear_getpk_wait(void)
{
  getpk_in_flight = 0;
}

static int should_request_pk(void)
{
  return session != NULL &&
         session->state == SESSION_STATE_IDLE &&
         session->pending_msg_type == 0 &&
         !getpk_in_flight &&
         !session_is_established(session);
}

static int send_node_metrics(void)
{
  uint8_t group;

  for(group = PQC_METRICS_GROUP_HS; group <= PQC_METRICS_GROUP_COUNT; group++) {
    if(pqc_tx_free_slots() == 0) {
      metrics_pending = 1;
      return 0;
    }

    if(!pqc_send_metrics_report(session, &udp_conn, &dest_address, group)) {
      metrics_pending = 1;
      return 0;
    }
  }

  metrics_pending = 0;
  return 1;
}

static void udp_rx_callback(struct simple_udp_connection *c,
                            const uip_ipaddr_t *sender_addr,
                            uint16_t sender_port,
                            const uip_ipaddr_t *receiver_addr,
                            uint16_t receiver_port,
                            const uint8_t *data,
                            uint16_t datalen)
{
  uint8_t was_established;
//  printf("[PQC] Node RX packet from ");
//  uiplib_ipaddr_print(sender_addr);
//  printf(" len=%u\r\n", datalen);

  if(pqc_header_valid(data, datalen)) {
    was_established = session_is_established(session);
    hs_fsm_handle_fragment(session, data, datalen, pqc_udp_send_fragment, &send_ctx);
    if(session != NULL && session->pending_msg_type != PQC_MSG_GETPK) {
      clear_getpk_wait();
    }
    if(!was_established && session_is_established(session)) {
      metrics_pending = 1;
      (void)send_node_metrics();
    } else if(metrics_pending && session_is_established(session)) {
      (void)send_node_metrics();
    }
  }
}
