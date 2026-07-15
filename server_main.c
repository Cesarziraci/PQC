#include "contiki.h"
#include "PQC.h"
#include "handshake_fsm.h"
#include "session_manager.h"
#include "temp_measurement.h"
#include "metrics.h"
#include "reset_diagnostics.h"

#include <string.h>

#define ROOT_LOG(...) printf(__VA_ARGS__)

#define CSV_VALUE0_SECONDS 0x01
#define CSV_VALUE1_SECONDS 0x02
#define CSV_VALUE2_SECONDS 0x04
#define CSV_VALUE3_SECONDS 0x08

/*---------------------------------------------------------------------------*/
/*--------------------------- GLOBAL VARIABLES -----------------------------*/
/*---------------------------------------------------------------------------*/

static struct simple_udp_connection udp_conn;
struct etimer send_timer;
static struct etimer retry_timer;

typedef struct {
  uint8_t active;
  uint16_t node_id;
  uint32_t temp_rx_ok;
  uint32_t temp_rx_fail;
  uint32_t temp_counter_resync;
  uint32_t rx_counter;
} impact_snapshot_t;

typedef struct {
  uint8_t active;
  uint16_t target_node_id;
  uint8_t snapshot_count;
  uint32_t started_at;
  impact_snapshot_t snapshots[SESSION_MAX_NODES];
} impact_window_t;

static impact_window_t impact_window;

/*---------------------------------------------------------------------------*/
/*--------------------------- PROCESS DEFINITIONS ---------------------------*/
/*---------------------------------------------------------------------------*/

#define LOG_MODULE "PQC"
#define LOG_LEVEL LOG_LEVEL_INFO

PROCESS(PQC_process, "PQC");
PROCESS(PQC_send_process, "PQC sender");
PROCESS(PQC_retry_process, "PQC retry");
PROCESS(Serial_line_process, "Serial_line");
AUTOSTART_PROCESSES(&PQC_process, &PQC_send_process, &PQC_retry_process, &Serial_line_process);

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
static void cmd_handler(const char *cmd);
static void csv_header(void);
static void csv_event(uint16_t node_id,
                      const char *event,
                      uint32_t value0,
                      uint32_t value1,
                      uint32_t value2,
                      uint32_t value3);
static void csv_event_units(uint16_t node_id,
                            const char *event,
                            uint32_t value0,
                            uint32_t value1,
                            uint32_t value2,
                            uint32_t value3,
                            uint8_t seconds_mask);
static void csv_temp(uint16_t node_id, const pqc_temp_payload_t *measurement);
static void csv_hs_summary(const session_t *s, const char *event);
static void csv_node_metrics(uint16_t node_id, const pqc_metrics_report_t *report);
static void session_store_node_metrics(session_t *s, const pqc_metrics_report_t *report);
static void csv_session_summary_if_ready(session_t *s);
static void impact_start(uint16_t target_node_id);
static void impact_end(uint16_t target_node_id, uint8_t success);

/*---------------------------------------------------------------------------*/
/*--------------------------- PROCESS THREAD --------------------------------*/
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(PQC_process, ev, data)
{
  PROCESS_BEGIN();

  print_reset_cause();
  ROOT_LOG("[PQC] Root starting\r\n");
  csv_header();
  csv_event(SESSION_INVALID_NODE_ID, "ROOT_START", 0, 0, 0, 0);

  NETSTACK_ROUTING.init();
  NETSTACK_ROUTING.root_start();

  simple_udp_register(&udp_conn,
                      UDP_SERVER_PORT,
                      NULL,
                      UDP_CLIENT_PORT,
                      udp_rx_callback);

  ROOT_LOG("[PQC] Root UDP listening on port %u\r\n", UDP_SERVER_PORT);
  csv_event(SESSION_INVALID_NODE_ID, "UDP_LISTEN", UDP_SERVER_PORT, 0, 0, 0);

  pqc_tx_init();
  session_manager_init(SESSION_ROLE_ROOT);

  if(!hs_fsm_root_init_keypair()) {
    ROOT_LOG("[PQC] Root keypair init failed; handshakes disabled\r\n");
    csv_event(SESSION_INVALID_NODE_ID, "ROOT_KEYPAIR_FAIL", 0, 0, 0, 0);
  } else {
    csv_event(SESSION_INVALID_NODE_ID, "ROOT_KEYPAIR_READY", 0, 0, 0, 0);
  }

  while(1) {
    PROCESS_YIELD();
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
    uint8_t i;

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&retry_timer));

    for(i = 0; i < SESSION_MAX_NODES; i++) {
      session_t *s = session_manager_get_by_index(i);

      if(s != NULL && !session_is_established(s) && s->has_peer_addr) {
        pqc_udp_send_ctx_t send_ctx;

        send_ctx.udp_conn = &udp_conn;
        send_ctx.dest = &s->peer_addr;
        hs_fsm_retry(s, pqc_udp_send_fragment, &send_ctx);

        if(session_is_failed(s)) {
          ROOT_LOG("[PQC] Root node %u: handshake failed, resetting session\r\n", s->node_id);
          csv_hs_summary(s, "HS_FAIL");
          impact_end(s->node_id, 0);
          csv_event(s->node_id, "HS_FAIL_RESET", s->state, s->retry_count, 0, 0);
          pqc_tx_drop_node(s->node_id);
          session_reset(s);
        }
      }
    }

    etimer_reset(&retry_timer);
  }

  PROCESS_END();
}

PROCESS_THREAD(Serial_line_process, ev, data)
{
  PROCESS_BEGIN();

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == serial_line_event_message);
    cmd_handler((char *)data);
  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*--------------------------- FUNCTION DEFINITIONS --------------------------*/
/*---------------------------------------------------------------------------*/

static void cmd_handler(const char *cmd)
{
  /* TODO: add serial commands for metrics, session reset and debug. */
  (void)cmd;
}

static void csv_header(void)
{
  printf("PQC_CSV_HEADER,record,root_seconds,node_id,event,value0,value1,value2,value3\r\n");
}

static void csv_print_seconds(uint32_t ticks)
{
  uint32_t seconds = ticks / CLOCK_SECOND;
  uint32_t millis = ((ticks % CLOCK_SECOND) * 1000 + (CLOCK_SECOND / 2)) / CLOCK_SECOND;

  if(millis >= 1000) {
    seconds++;
    millis -= 1000;
  }

  printf("%lu.%03lu", (unsigned long)seconds, (unsigned long)millis);
}

static void csv_print_value(uint32_t value, uint8_t as_seconds)
{
  if(as_seconds) {
    csv_print_seconds(value);
  } else {
    printf("%lu", (unsigned long)value);
  }
}

static void csv_event_units(uint16_t node_id,
                            const char *event,
                            uint32_t value0,
                            uint32_t value1,
                            uint32_t value2,
                            uint32_t value3,
                            uint8_t seconds_mask)
{
  if(event == NULL) {
    event = "UNKNOWN";
  }

  printf("PQC_CSV,EVENT,");
  csv_print_seconds((uint32_t)clock_time());
  printf(",%u,%s,", node_id, event);
  csv_print_value(value0, seconds_mask & CSV_VALUE0_SECONDS);
  printf(",");
  csv_print_value(value1, seconds_mask & CSV_VALUE1_SECONDS);
  printf(",");
  csv_print_value(value2, seconds_mask & CSV_VALUE2_SECONDS);
  printf(",");
  csv_print_value(value3, seconds_mask & CSV_VALUE3_SECONDS);
  printf("\r\n");
}

static void csv_event(uint16_t node_id,
                      const char *event,
                      uint32_t value0,
                      uint32_t value1,
                      uint32_t value2,
                      uint32_t value3)
{
  csv_event_units(node_id, event, value0, value1, value2, value3, 0);
}

static void csv_temp(uint16_t node_id, const pqc_temp_payload_t *measurement)
{
  if(measurement == NULL) {
    return;
  }

  printf("PQC_CSV,TEMP,");
  csv_print_seconds((uint32_t)clock_time());
  printf(",%u,TEMP_RX,%lu,%ld,%ld,0\r\n",
         node_id,
         (unsigned long)measurement->counter,
         (long)measurement->rh_milli,
         (long)measurement->temp_milli);
}

static void csv_hs_summary(const session_t *s, const char *event)
{
  const pqc_metrics_snapshot_t *m;

  if(s == NULL) {
    return;
  }

  m = &s->metrics;
  if(event == NULL) {
    event = "HS_SUMMARY";
  }

  csv_event_units(s->node_id,
                  event,
                  m->handshake.handshake_ticks_last,
                  m->resource.retries,
                  m->fragmentation.nack_tx,
                  m->fragmentation.nack_rx,
                  CSV_VALUE0_SECONDS);
  csv_event_units(s->node_id,
                  "HS_PK",
                  m->handshake.pk_tx_ticks_last,
                  m->handshake.pk_rx_ticks_last,
                  m->handshake.pk_tx,
                  m->handshake.pk_rx_complete,
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS);
  csv_event_units(s->node_id,
                  "HS_CT",
                  m->handshake.ct_tx_ticks_last,
                  m->handshake.ct_rx_ticks_last,
                  m->handshake.ct_tx,
                  m->handshake.ct_rx_complete,
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS);
  csv_event_units(s->node_id,
                  "HS_CRYPTO",
                  m->handshake.keypair_ticks_last,
                  m->handshake.encaps_ticks_last,
                  m->handshake.decaps_ticks_last,
                  m->handshake.ack_wait_ticks_last,
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS |
                  CSV_VALUE2_SECONDS | CSV_VALUE3_SECONDS);
}

static void csv_node_metrics(uint16_t node_id, const pqc_metrics_report_t *report)
{
  uint8_t seconds_mask = 0;

  if(report == NULL) {
    return;
  }

  switch(report->group) {
  case PQC_METRICS_GROUP_HS:
    seconds_mask = CSV_VALUE0_SECONDS;
    break;
  case PQC_METRICS_GROUP_PK:
  case PQC_METRICS_GROUP_CT:
    seconds_mask = CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS;
    break;
  case PQC_METRICS_GROUP_CRYPTO:
    seconds_mask = CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS |
                   CSV_VALUE2_SECONDS | CSV_VALUE3_SECONDS;
    break;
  default:
    seconds_mask = 0;
    break;
  }

  csv_event_units(node_id,
                  pqc_metrics_group_name(report->group),
                  report->value0,
                  report->value1,
                  report->value2,
                  report->value3,
                  seconds_mask);
  csv_event_units(node_id,
                  "NODE_METRICS_META",
                  report->group,
                  report->seq,
                  report->uptime_ticks,
                  report->version,
                  CSV_VALUE2_SECONDS);
}

static void session_store_node_metrics(session_t *s, const pqc_metrics_report_t *report)
{
  if(s == NULL || report == NULL ||
     report->group == 0 || report->group > PQC_METRICS_GROUP_COUNT) {
    return;
  }

  s->node_metric_values[report->group][0] = report->value0;
  s->node_metric_values[report->group][1] = report->value1;
  s->node_metric_values[report->group][2] = report->value2;
  s->node_metric_values[report->group][3] = report->value3;
  s->node_metrics_mask |= (uint8_t)(1 << (report->group - 1));
}

static void csv_session_summary_if_ready(session_t *s)
{
  const pqc_metrics_snapshot_t *root;
  const uint32_t *node_hs;
  const uint32_t *node_pk;
  const uint32_t *node_ct;
  const uint32_t *node_crypto;
  const uint32_t *node_frag;
  const uint32_t *node_temp;
  const uint32_t *node_res;
  uint8_t required;

  if(s == NULL || s->session_summary_emitted || !session_is_established(s)) {
    return;
  }

  required = (uint8_t)((1 << (PQC_METRICS_GROUP_HS - 1)) |
                       (1 << (PQC_METRICS_GROUP_PK - 1)) |
                       (1 << (PQC_METRICS_GROUP_CT - 1)) |
                       (1 << (PQC_METRICS_GROUP_CRYPTO - 1)) |
                       (1 << (PQC_METRICS_GROUP_FRAG - 1)) |
                       (1 << (PQC_METRICS_GROUP_TEMP - 1)) |
                       (1 << (PQC_METRICS_GROUP_RES - 1)));
  if((s->node_metrics_mask & required) != required) {
    return;
  }

  root = &s->metrics;
  node_hs = s->node_metric_values[PQC_METRICS_GROUP_HS];
  node_pk = s->node_metric_values[PQC_METRICS_GROUP_PK];
  node_ct = s->node_metric_values[PQC_METRICS_GROUP_CT];
  node_crypto = s->node_metric_values[PQC_METRICS_GROUP_CRYPTO];
  node_frag = s->node_metric_values[PQC_METRICS_GROUP_FRAG];
  node_temp = s->node_metric_values[PQC_METRICS_GROUP_TEMP];
  node_res = s->node_metric_values[PQC_METRICS_GROUP_RES];

  csv_event_units(s->node_id,
                  "SESSION_HS",
                  root->handshake.handshake_ticks_last,
                  node_hs[0],
                  root->resource.retries,
                  node_res[0],
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS);
  csv_event_units(s->node_id,
                  "SESSION_PK",
                  root->handshake.pk_tx_ticks_last,
                  node_pk[1],
                  root->handshake.pk_tx,
                  node_pk[3],
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS);
  csv_event_units(s->node_id,
                  "SESSION_CT",
                  node_ct[0],
                  root->handshake.ct_rx_ticks_last,
                  node_ct[2],
                  root->handshake.ct_rx_complete,
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS);
  csv_event_units(s->node_id,
                  "SESSION_CRYPTO",
                  root->handshake.keypair_ticks_last,
                  node_crypto[1],
                  root->handshake.decaps_ticks_last,
                  node_crypto[3],
                  CSV_VALUE0_SECONDS | CSV_VALUE1_SECONDS |
                  CSV_VALUE2_SECONDS | CSV_VALUE3_SECONDS);
  csv_event(s->node_id,
            "SESSION_FRAG",
            root->fragmentation.nack_tx,
            root->fragmentation.nack_rx,
            node_frag[2],
            node_frag[3]);
  csv_event(s->node_id,
            "SESSION_TEMP",
            root->temp.temp_rx_ok,
            root->temp.temp_rx_fail,
            node_temp[0],
            node_temp[1]);

  s->session_summary_emitted = 1;
}

static void impact_start(uint16_t target_node_id)
{
  uint8_t i;

  if(impact_window.active) {
    csv_event(impact_window.target_node_id,
              "IMPACT_OVERLAP",
              target_node_id,
              impact_window.snapshot_count,
              0,
              0);
    return;
  }

  memset(&impact_window, 0, sizeof(impact_window));
  impact_window.active = 1;
  impact_window.target_node_id = target_node_id;
  impact_window.started_at = (uint32_t)clock_time();

  for(i = 0; i < SESSION_MAX_NODES; i++) {
    session_t *s = session_manager_get_by_index(i);
    impact_snapshot_t *snapshot;

    if(s == NULL || !session_is_established(s) || s->node_id == target_node_id ||
       impact_window.snapshot_count >= SESSION_MAX_NODES) {
      continue;
    }

    snapshot = &impact_window.snapshots[impact_window.snapshot_count++];
    snapshot->active = 1;
    snapshot->node_id = s->node_id;
    snapshot->temp_rx_ok = s->metrics.temp.temp_rx_ok;
    snapshot->temp_rx_fail = s->metrics.temp.temp_rx_fail;
    snapshot->temp_counter_resync = s->metrics.temp.temp_counter_resync;
    snapshot->rx_counter = s->rx_counter;
  }

  csv_event(target_node_id,
            "IMPACT_START",
            impact_window.snapshot_count,
            0,
            0,
            0);
}

static void impact_end(uint16_t target_node_id, uint8_t success)
{
  uint8_t i;
  uint32_t duration;

  if(!impact_window.active || impact_window.target_node_id != target_node_id) {
    return;
  }

  duration = (uint32_t)(clock_time() - (clock_time_t)impact_window.started_at);

  for(i = 0; i < impact_window.snapshot_count; i++) {
    impact_snapshot_t *snapshot = &impact_window.snapshots[i];
    session_t *s;
    uint32_t temp_ok_delta;
    uint32_t temp_fail_delta;
    uint32_t resync_delta;
    uint32_t rx_counter_delta;
    uint32_t counter_gap;

    if(!snapshot->active) {
      continue;
    }

    s = session_manager_find(snapshot->node_id);
    if(s == NULL) {
      csv_event(snapshot->node_id,
                "IMPACT_NODE_LOST",
                target_node_id,
                0,
                0,
                0);
      continue;
    }

    temp_ok_delta = s->metrics.temp.temp_rx_ok - snapshot->temp_rx_ok;
    temp_fail_delta = s->metrics.temp.temp_rx_fail - snapshot->temp_rx_fail;
    resync_delta = s->metrics.temp.temp_counter_resync - snapshot->temp_counter_resync;
    rx_counter_delta = s->rx_counter - snapshot->rx_counter;
    counter_gap = rx_counter_delta > temp_ok_delta ? rx_counter_delta - temp_ok_delta : 0;

    csv_event(snapshot->node_id,
              "IMPACT_TEMP",
              target_node_id,
              temp_ok_delta,
              temp_fail_delta,
              counter_gap);
    csv_event(snapshot->node_id,
              "IMPACT_RESYNC",
              target_node_id,
              resync_delta,
              rx_counter_delta,
              session_is_established(s));
  }

  csv_event_units(target_node_id,
                  "IMPACT_END",
                  duration,
                  impact_window.snapshot_count,
                  success,
                  0,
                  CSV_VALUE0_SECONDS);

  memset(&impact_window, 0, sizeof(impact_window));
}

static void udp_rx_callback(struct simple_udp_connection *c,
                            const uip_ipaddr_t *sender_addr,
                            uint16_t sender_port,
                            const uip_ipaddr_t *receiver_addr,
                            uint16_t receiver_port,
                            const uint8_t *data,
                            uint16_t datalen)
{
  uint16_t node_id;
  session_t *s;
  pqc_udp_send_ctx_t send_ctx;
  uint8_t msg_type;
  uint8_t msg_id;
  uint8_t was_established;
//  uint8_t frag_idx;
//  uint8_t total_frags;

  if(!pqc_header_valid(data, datalen)) {
    ROOT_LOG("[PQC] Root RX invalid packet len=%u\r\n", datalen);
    csv_event(SESSION_INVALID_NODE_ID, "RX_INVALID", datalen, 0, 0, 0);
    return;
  }

  msg_type = data[PQC_HDR_MSG_TYPE];
  msg_id = data[PQC_HDR_MSG_ID];
//  frag_idx = data[PQC_HDR_FRAG_IDX];
//  total_frags = data[PQC_HDR_TOTAL_FRAGS];
  node_id = pqc_header_get_node_id(data);

// printf("[PQC] Root RX msg=%u node=%u msg_id=%u frag=%u/%u len=%u\r\n",
//         msg_type, node_id, msg_id, frag_idx + 1, total_frags, datalen);

  s = session_manager_get_or_create(node_id);

  if(s == NULL) {
    ROOT_LOG("[PQC] Root node %u: no free session slot\r\n", node_id);
    csv_event(node_id, "NO_SESSION_SLOT", 0, 0, 0, 0);
    return;
  }

  session_set_peer_addr(s, sender_addr);
  was_established = session_is_established(s);

  send_ctx.udp_conn = &udp_conn;
  send_ctx.dest = (uip_ipaddr_t *)sender_addr;

  if(msg_type == PQC_MSG_GETPK) {
    s->metrics.handshake.getpk_rx++;

    if(session_is_failed(s)) {
      ROOT_LOG("[PQC] Root node %u: GETPK received after failure, restarting handshake\r\n", node_id);
      csv_event(node_id, "GETPK_AFTER_FAIL", s->retry_count, 0, 0, 0);
      pqc_tx_drop_node(node_id);
      session_reset(s);
      session_set_peer_addr(s, sender_addr);
    }

    if(s->state == SESSION_STATE_IDLE) {
      ROOT_LOG("GETPK recibida del nodo %u\r\n", node_id);
      csv_event(node_id, "GETPK_ACCEPT", msg_id, 0, 0, 0);
      ROOT_LOG("Enviando PK al nodo %u\r\n", node_id);
      impact_start(node_id);
      if(!hs_fsm_start(s, pqc_udp_send_fragment, &send_ctx)) {
        impact_end(node_id, 0);
      }
    } else if(s->state == SESSION_STATE_WAIT_CT || session_is_established(s)) {
      ROOT_LOG("[PQC] Root node %u: GETPK received in state=%u, restarting handshake\r\n", node_id, s->state);
      csv_event(node_id, "GETPK_RESTART", s->state, msg_id, 0, 0);
      pqc_tx_drop_node(node_id);
      session_reset(s);
      session_set_peer_addr(s, sender_addr);
      ROOT_LOG("Enviando PK al nodo %u\r\n", node_id);
      impact_start(node_id);
      if(!hs_fsm_start(s, pqc_udp_send_fragment, &send_ctx)) {
        impact_end(node_id, 0);
      }
    } else {
      ROOT_LOG("[PQC] Root node %u: GETPK ignored in state=%u\r\n", node_id, s->state);
      csv_event(node_id, "GETPK_IGNORE", s->state, msg_id, 0, 0);
    }
    return;
  }

  if(msg_type == PQC_MSG_CT &&
     s->state == SESSION_STATE_WAIT_CT &&
     s->pending_msg_type == PQC_MSG_PK) {
    uint8_t dropped = pqc_tx_drop(node_id, PQC_MSG_PK, s->pending_msg_id);
    ROOT_LOG("[PQC] Root node %u: CT started, clearing pending PK retry, dropped=%u\r\n",
             node_id, dropped);
    ROOT_LOG("Recibiendo CT del nodo %u\r\n", node_id);
    csv_event(node_id, "CT_RX_START", msg_id, dropped, 0, 0);
    s->metrics.handshake.ct_rx_started_at = (uint32_t)clock_time();
    s->metrics.handshake.ct_rx_completed_at = 0;
    session_set_pending(s, PQC_MSG_CT, msg_id);
  }

  if(msg_type == PQC_MSG_TEMP) {
    pqc_temp_payload_t measurement;

    if(pqc_handle_encrypted_temp(s, data, datalen, &measurement)) {
      ROOT_LOG("Temp nodo %u: ", node_id);
      print_measurement(measurement.rh_milli, measurement.temp_milli);
      csv_temp(node_id, &measurement);
    } else {
      ROOT_LOG("[PQC] Root node %u: encrypted TEMP decrypt failed\r\n", node_id);
      csv_event(node_id, "TEMP_DECRYPT_FAIL", msg_id, datalen, 0, 0);
    }
    return;
  }

  if(msg_type == PQC_MSG_METRICS) {
    pqc_metrics_report_t report;

    if(datalen == (uint16_t)(PQC_HEADER_LEN + PQC_METRICS_REPORT_LEN) &&
       data[PQC_HDR_FRAG_IDX] == 0 &&
       data[PQC_HDR_TOTAL_FRAGS] == 1 &&
       data[PQC_HDR_PAYLOAD_LEN] == PQC_METRICS_REPORT_LEN &&
       pqc_metrics_decode_report(&data[PQC_HEADER_LEN],
                                 data[PQC_HDR_PAYLOAD_LEN],
                                 &report)) {
      csv_node_metrics(node_id, &report);
      session_store_node_metrics(s, &report);
      csv_session_summary_if_ready(s);
    } else {
      ROOT_LOG("[PQC] Root node %u: invalid metrics report len=%u\r\n",
               node_id,
               datalen);
      csv_event(node_id, "NODE_METRICS_INVALID", datalen, msg_id, 0, 0);
    }
    return;
  }

  hs_fsm_handle_fragment(s, data, datalen, pqc_udp_send_fragment, &send_ctx);
  if(!was_established && session_is_established(s)) {
    csv_hs_summary(s, "HS_SUCCESS");
    impact_end(s->node_id, 1);
    csv_session_summary_if_ready(s);
  }
}
