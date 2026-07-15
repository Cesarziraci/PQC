#include "PQC.h"

#include <string.h>
#include "crypto_worker.h"
#include "fragmentation.h"
#include "metrics.h"

#define PQC_TX_QUEUE_SIZE 96
#define PQC_TX_MAX_PACKET_LEN (PQC_HEADER_LEN + FRAG_MAX_PAYLOAD_LEN)
#define PQC_TEMP_RX_COUNTER_WINDOW 16

#define PQC_TX_PRIORITY_HIGH   0
#define PQC_TX_PRIORITY_NORMAL 1
#define PQC_TX_PRIORITY_LOW    2

typedef struct {
  uint8_t active;
  uint8_t priority;
  uint16_t node_id;
  struct simple_udp_connection *udp_conn;
  uip_ipaddr_t dest;
  uint16_t len;
  uint8_t data[PQC_TX_MAX_PACKET_LEN];
} pqc_tx_entry_t;

static pqc_tx_entry_t tx_queue[PQC_TX_QUEUE_SIZE];
static uint8_t tx_tail;
static uint8_t tx_count;
static uint16_t tx_last_node_id;

static uint8_t pqc_tx_priority_for_msg(uint8_t msg_type)
{
  switch(msg_type) {
  case PQC_MSG_ACK:
  case PQC_MSG_NACK:
  case PQC_MSG_GETPK:
    return PQC_TX_PRIORITY_HIGH;
  case PQC_MSG_PK:
  case PQC_MSG_CT:
  case PQC_MSG_TEMP:
    return PQC_TX_PRIORITY_NORMAL;
  case PQC_MSG_METRICS:
  default:
    return PQC_TX_PRIORITY_LOW;
  }
}

static int pqc_tx_find_free_slot(void)
{
  uint8_t checked;

  for(checked = 0; checked < PQC_TX_QUEUE_SIZE; checked++) {
    uint8_t idx = (tx_tail + checked) % PQC_TX_QUEUE_SIZE;
    if(!tx_queue[idx].active) {
      tx_tail = idx;
      return idx;
    }
  }

  return -1;
}

static int pqc_tx_select_slot(void)
{
  uint8_t priority;

  for(priority = PQC_TX_PRIORITY_HIGH; priority <= PQC_TX_PRIORITY_LOW; priority++) {
    int first = -1;
    uint8_t i;

    for(i = 0; i < PQC_TX_QUEUE_SIZE; i++) {
      pqc_tx_entry_t *entry = &tx_queue[i];

      if(!entry->active || entry->priority != priority) {
        continue;
      }

      if(first < 0) {
        first = i;
      }

      if(entry->node_id != tx_last_node_id) {
        return i;
      }
    }

    if(first >= 0) {
      return first;
    }
  }

  return -1;
}

static void pqc_tx_clear_entry(uint8_t idx)
{
  if(idx >= PQC_TX_QUEUE_SIZE || !tx_queue[idx].active) {
    return;
  }

  memset(&tx_queue[idx], 0, sizeof(tx_queue[idx]));
  if(tx_count > 0) {
    tx_count--;
  }
}

void pqc_tx_init(void)
{
  memset(tx_queue, 0, sizeof(tx_queue));
  tx_tail = 0;
  tx_count = 0;
  tx_last_node_id = SESSION_INVALID_NODE_ID;
}

int pqc_tx_has_pending(void)
{
  return tx_count > 0;
}

uint8_t pqc_tx_free_slots(void)
{
  return PQC_TX_QUEUE_SIZE - tx_count;
}

uint8_t pqc_tx_drop(uint16_t node_id, uint8_t msg_type, uint8_t msg_id)
{
  uint8_t i;
  uint8_t dropped = 0;

  for(i = 0; i < PQC_TX_QUEUE_SIZE; i++) {
    if(tx_queue[i].active &&
       tx_queue[i].node_id == node_id &&
       tx_queue[i].len >= PQC_HEADER_LEN &&
       tx_queue[i].data[PQC_HDR_MSG_TYPE] == msg_type &&
       tx_queue[i].data[PQC_HDR_MSG_ID] == msg_id) {
      pqc_tx_clear_entry(i);
      dropped++;
    }
  }

  return dropped;
}

uint8_t pqc_tx_drop_node(uint16_t node_id)
{
  uint8_t i;
  uint8_t dropped = 0;

  for(i = 0; i < PQC_TX_QUEUE_SIZE; i++) {
    if(tx_queue[i].active && tx_queue[i].node_id == node_id) {
      pqc_tx_clear_entry(i);
      dropped++;
    }
  }

  return dropped;
}

int pqc_tx_process(void)
{
  pqc_tx_entry_t *entry;
  int idx;

  if(tx_count == 0) {
    return 0;
  }

  idx = pqc_tx_select_slot();
  if(idx < 0) {
    return 0;
  }

  entry = &tx_queue[idx];
  if(entry->active && entry->udp_conn != NULL) {
    simple_udp_sendto(entry->udp_conn, entry->data, entry->len, &entry->dest);
  }

  tx_last_node_id = entry->node_id;
  memset(entry, 0, sizeof(*entry));
  tx_count--;
  return 1;
}

int pqc_udp_send_fragment(const uint8_t *data, uint16_t len, void *user)
{
  pqc_udp_send_ctx_t *ctx = (pqc_udp_send_ctx_t *)user;
  pqc_tx_entry_t *entry;
  uint8_t msg_type;
  int idx;

  if(ctx == NULL || ctx->udp_conn == NULL || ctx->dest == NULL ||
     data == NULL || len == 0 || len > PQC_TX_MAX_PACKET_LEN ||
     tx_count >= PQC_TX_QUEUE_SIZE || !pqc_header_valid(data, len)) {
    return 0;
  }

  idx = pqc_tx_find_free_slot();
  if(idx < 0) {
    return 0;
  }

  msg_type = data[PQC_HDR_MSG_TYPE];
  entry = &tx_queue[idx];
  entry->active = 1;
  entry->priority = pqc_tx_priority_for_msg(msg_type);
  entry->node_id = pqc_header_get_node_id(data);
  entry->udp_conn = ctx->udp_conn;
  entry->dest = *ctx->dest;
  entry->len = len;
  memcpy(entry->data, data, len);

  tx_tail = (idx + 1) % PQC_TX_QUEUE_SIZE;
  tx_count++;
  return 1;
}

int pqc_send_GETPK(struct simple_udp_connection *udp_conn,
                   uip_ipaddr_t *root_addr,
                   uint16_t node_id)
{
  uint8_t msg[PQC_HEADER_LEN];
  pqc_udp_send_ctx_t ctx;

  if(udp_conn == NULL || root_addr == NULL) {
    return 0;
  }

  pqc_header_write(msg,
                   PQC_MSG_GETPK,
                   node_id,
                   0,
                   0,
                   1,
                   0,
                   PQC_FLAG_NONE);

  ctx.udp_conn = udp_conn;
  ctx.dest = root_addr;
  return pqc_udp_send_fragment(msg, sizeof(msg), &ctx);
}

int pqc_send_encrypted_temp(session_t *s,
                            struct simple_udp_connection *udp_conn,
                            uip_ipaddr_t *dest,
                            const pqc_temp_payload_t *measurement)
{
  uint8_t packet[PQC_HEADER_LEN + sizeof(pqc_temp_payload_t)];
  uint8_t iv[CW_AES_IV_LEN];
  pqc_temp_payload_t payload;
  pqc_udp_send_ctx_t ctx;
  uint32_t counter;
  int queued;

  if(s == NULL || udp_conn == NULL || dest == NULL || measurement == NULL ||
     !session_is_established(s) || pqc_tx_free_slots() == 0) {
    if(s != NULL) {
      s->metrics.temp.temp_tx_fail++;
    }
    return 0;
  }

  payload = *measurement;
  counter = s->tx_counter;
  payload.counter = counter;

  memcpy(&packet[PQC_HEADER_LEN], &payload, sizeof(payload));

  crypto_worker_derive_aes_iv(s->ss, counter, iv);
  crypto_worker_aes_ctr_xcrypt(s->aes_key,
                               iv,
                               &packet[PQC_HEADER_LEN],
                               sizeof(payload));

  pqc_header_write(packet,
                   PQC_MSG_TEMP,
                   s->node_id,
                   session_next_msg_id(s),
                   0,
                   1,
                   sizeof(payload),
                   PQC_FLAG_NONE);

  ctx.udp_conn = udp_conn;
  ctx.dest = dest;
  queued = pqc_udp_send_fragment(packet, sizeof(packet), &ctx);

  if(queued) {
    s->tx_counter++;
    s->metrics.temp.temp_tx++;
  } else {
    s->metrics.temp.temp_tx_fail++;
  }

  crypto_worker_secure_zero(iv, sizeof(iv));
  crypto_worker_secure_zero(&payload, sizeof(payload));
  return queued;
}

int pqc_send_metrics_report(session_t *s,
                            struct simple_udp_connection *udp_conn,
                            uip_ipaddr_t *dest,
                            uint8_t group)
{
  uint8_t packet[PQC_HEADER_LEN + PQC_METRICS_REPORT_LEN];
  uint8_t payload[PQC_METRICS_REPORT_LEN];
  pqc_metrics_report_t report;
  pqc_udp_send_ctx_t ctx;
  uint8_t payload_len;

  if(s == NULL || udp_conn == NULL || dest == NULL ||
     !session_is_established(s) || pqc_tx_free_slots() == 0) {
    if(s != NULL) {
      s->metrics.resource.tx_queue_full++;
    }
    return 0;
  }

  if(!pqc_metrics_build_report(&s->metrics, group, &report)) {
    return 0;
  }

  payload_len = pqc_metrics_encode_report(&report, payload, sizeof(payload));
  if(payload_len == 0) {
    return 0;
  }

  pqc_header_write(packet,
                   PQC_MSG_METRICS,
                   s->node_id,
                   session_next_msg_id(s),
                   0,
                   1,
                   payload_len,
                   PQC_FLAG_NONE);
  memcpy(&packet[PQC_HEADER_LEN], payload, payload_len);

  ctx.udp_conn = udp_conn;
  ctx.dest = dest;
  if(!pqc_udp_send_fragment(packet, PQC_HEADER_LEN + payload_len, &ctx)) {
    s->metrics.resource.tx_queue_full++;
    return 0;
  }

  return 1;
}

int pqc_handle_encrypted_temp(session_t *s,
                              const uint8_t *data,
                              uint16_t len,
                              pqc_temp_payload_t *out)
{
  uint8_t iv[CW_AES_IV_LEN];
  pqc_temp_payload_t payload;
  uint32_t expected_counter;
  uint32_t trial_counter;
  uint8_t offset;

  if(s == NULL || data == NULL || out == NULL || !session_is_established(s) ||
     !pqc_header_valid(data, len) ||
     data[PQC_HDR_MSG_TYPE] != PQC_MSG_TEMP ||
     data[PQC_HDR_FRAG_IDX] != 0 ||
     data[PQC_HDR_TOTAL_FRAGS] != 1 ||
     data[PQC_HDR_PAYLOAD_LEN] != sizeof(pqc_temp_payload_t)) {
    if(s != NULL) {
      s->metrics.temp.temp_rx_fail++;
    }
    return 0;
  }

  s->metrics.temp.temp_rx++;
  expected_counter = session_peek_rx_counter(s);

  /*
   * UDP TEMP packets can be lost while the root is busy receiving/decapsulating
   * a large CT from another node. If one counter is missed, strict single-counter
   * decrypt would make every following TEMP fail forever. Try a small forward
   * window and resynchronise rx_counter when the embedded counter matches.
   */
  for(offset = 0; offset < PQC_TEMP_RX_COUNTER_WINDOW; offset++) {
    trial_counter = expected_counter + offset;
    memcpy(&payload, &data[PQC_HEADER_LEN], sizeof(payload));

    crypto_worker_derive_aes_iv(s->ss, trial_counter, iv);
    crypto_worker_aes_ctr_xcrypt(s->aes_key,
                                 iv,
                                 (uint8_t *)&payload,
                                 sizeof(payload));

    if(payload.counter == trial_counter) {
      if(offset != 0) {
        s->metrics.temp.temp_counter_resync++;
        printf("[PQC] Node %u: TEMP counter resync expected=%lu got=%lu skipped=%u\r\n",
               s->node_id,
               (unsigned long)expected_counter,
               (unsigned long)trial_counter,
               offset);
      }

      s->rx_counter = trial_counter + 1;
      session_touch(s);
      *out = payload;
      s->metrics.temp.temp_rx_ok++;
      crypto_worker_secure_zero(iv, sizeof(iv));
      return 1;
    }
  }

  crypto_worker_secure_zero(iv, sizeof(iv));
  crypto_worker_secure_zero(&payload, sizeof(payload));
  s->metrics.temp.temp_rx_fail++;
  return 0;
}
