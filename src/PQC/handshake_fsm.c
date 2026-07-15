#include "handshake_fsm.h"

#include <string.h>
#include "crypto_worker.h"
#include "fragmentation.h"
#include "nack.h"
#include "PQC.h"
#include "metrics.h"

#ifndef HS_RX_SLOT_COUNT
#define HS_RX_SLOT_COUNT 2
#endif
#define HS_RX_TIMEOUT SESSION_RETRY_TIMEOUT
#define HS_RESPONSE_GRACE (10 * CLOCK_SECOND)
#define HS_RETRY_TX_DRAIN_DELAY CLOCK_SECOND
#define HS_NACK_REPEAT_DELAY (4 * CLOCK_SECOND)
#define HS_DEBUG_PROGRESS_STEP 10

static uint8_t root_pk[CW_HQC_PUBLIC_KEY_LEN];
static uint8_t root_sk[CW_HQC_SECRET_KEY_LEN];
static uint8_t root_has_keypair;
static uint8_t node_ct[CW_HQC_CIPHERTEXT_LEN];
static uint8_t root_ct[CW_HQC_CIPHERTEXT_LEN];

typedef struct {
  uint8_t active;
  session_t *owner;
  clock_time_t started_at;
  clock_time_t last_nack_at;
  uint8_t last_nack_received_frags;
  uint8_t last_nack_missing_count;
  fragment_rx_ctx_t ctx;
} hs_rx_slot_t;

static hs_rx_slot_t rx_slots[HS_RX_SLOT_COUNT];

static void rx_slots_gc(void)
{
  uint8_t i;
  clock_time_t now = clock_time();

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    if(rx_slots[i].active && now - rx_slots[i].started_at > HS_RX_TIMEOUT) {
      printf("[PQC] RX slot timeout node=%u msg=%u msg_id=%u progress=%u/%u len=%u\r\n",
             rx_slots[i].owner != NULL ? rx_slots[i].owner->node_id : SESSION_INVALID_NODE_ID,
             rx_slots[i].ctx.msg_type,
             rx_slots[i].ctx.msg_id,
             rx_slots[i].ctx.received_frags,
             rx_slots[i].ctx.total_frags,
             rx_slots[i].ctx.received_len);
      fragment_rx_reset(&rx_slots[i].ctx);
      rx_slots[i].active = 0;
      rx_slots[i].owner = NULL;
      rx_slots[i].started_at = 0;
      rx_slots[i].last_nack_at = 0;
      rx_slots[i].last_nack_received_frags = 0;
      rx_slots[i].last_nack_missing_count = 0;
    }
  }
}

static fragment_rx_ctx_t *rx_slot_get(session_t *s)
{
  uint8_t i;

  if(s == NULL) {
    return NULL;
  }

  rx_slots_gc();

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    if(rx_slots[i].active && rx_slots[i].owner == s) {
      return &rx_slots[i].ctx;
    }
  }

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    if(!rx_slots[i].active) {
      rx_slots[i].active = 1;
      rx_slots[i].owner = s;
      rx_slots[i].started_at = clock_time();
      rx_slots[i].last_nack_at = 0;
      rx_slots[i].last_nack_received_frags = 0;
      rx_slots[i].last_nack_missing_count = 0;
      fragment_rx_init(&rx_slots[i].ctx);
      printf("[PQC] RX slot allocated node=%u\r\n", s->node_id);
      return &rx_slots[i].ctx;
    }
  }

  return NULL;
}

static fragment_rx_ctx_t *rx_slot_find(session_t *s)
{
  uint8_t i;

  if(s == NULL) {
    return NULL;
  }

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    if(rx_slots[i].active && rx_slots[i].owner == s) {
      return &rx_slots[i].ctx;
    }
  }

  return NULL;
}

static void rx_slot_touch(session_t *s)
{
  uint8_t i;

  if(s == NULL) {
    return;
  }

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    if(rx_slots[i].active && rx_slots[i].owner == s) {
      rx_slots[i].started_at = clock_time();
      return;
    }
  }
}

static void rx_slot_release(session_t *s)
{
  uint8_t i;

  if(s == NULL) {
    return;
  }

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    if(rx_slots[i].active && rx_slots[i].owner == s) {
      fragment_rx_reset(&rx_slots[i].ctx);
      rx_slots[i].active = 0;
      rx_slots[i].owner = NULL;
      rx_slots[i].started_at = 0;
      rx_slots[i].last_nack_at = 0;
      rx_slots[i].last_nack_received_frags = 0;
      rx_slots[i].last_nack_missing_count = 0;
      return;
    }
  }
}

static int rx_slot_nack_due(session_t *s, const fragment_rx_ctx_t *rx, uint8_t frag_idx)
{
  uint8_t i;
  clock_time_t now = clock_time();

  if(s == NULL || rx == NULL || rx->total_frags == 0 ||
     !rx->bitmap[rx->total_frags - 1]) {
    return 0;
  }

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    hs_rx_slot_t *slot = &rx_slots[i];

    if(!slot->active || slot->owner != s || &slot->ctx != rx) {
      continue;
    }

    if(slot->last_nack_missing_count == 0) {
      return frag_idx + 1 == rx->total_frags;
    }

    if(rx->received_frags >=
       (uint8_t)(slot->last_nack_received_frags + slot->last_nack_missing_count)) {
      return 1;
    }

    return now - slot->last_nack_at >= HS_NACK_REPEAT_DELAY;
  }

  return 0;
}

static void rx_slot_note_nack(session_t *s,
                              const fragment_rx_ctx_t *rx,
                              uint8_t missing_count)
{
  uint8_t i;

  if(s == NULL || rx == NULL || missing_count == 0) {
    return;
  }

  for(i = 0; i < HS_RX_SLOT_COUNT; i++) {
    hs_rx_slot_t *slot = &rx_slots[i];

    if(slot->active && slot->owner == s && &slot->ctx == rx) {
      slot->last_nack_at = clock_time();
      slot->last_nack_received_frags = rx->received_frags;
      slot->last_nack_missing_count = missing_count;
      s->retry_deadline = slot->last_nack_at + HS_NACK_REPEAT_DELAY;
      session_touch(s);
      return;
    }
  }
}

static int tx_has_room_for(uint16_t len)
{
  uint8_t needed = fragment_tx_count(len);

  return needed != 0 && pqc_tx_free_slots() >= needed;
}

static uint8_t rx_first_missing(const fragment_rx_ctx_t *rx)
{
  uint8_t i;

  if(rx == NULL || rx->total_frags == 0) {
    return 0xff;
  }

  for(i = 0; i < rx->total_frags && i < FRAG_MAX_FRAGS; i++) {
    if(!rx->bitmap[i]) {
      return i;
    }
  }

  return 0xff;
}

static void rx_debug_progress(session_t *s,
                              const fragment_rx_ctx_t *rx,
                              uint8_t msg_type,
                              uint8_t msg_id,
                              uint8_t frag_idx,
                              uint8_t total_frags)
{
  if(s == NULL || rx == NULL) {
    return;
  }

  if(frag_idx == 0 ||
     frag_idx + 1 == total_frags ||
     rx->received_frags == 1 ||
     (rx->received_frags % HS_DEBUG_PROGRESS_STEP) == 0) {
    printf("[PQC] %s node %u: RX progress msg=%u msg_id=%u progress=%u/%u len=%u first_missing=%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           msg_type,
           msg_id,
           rx->received_frags,
           rx->total_frags,
           rx->received_len,
           rx_first_missing(rx));
  }

  if(frag_idx + 1 == total_frags && !fragment_rx_complete(rx)) {
    printf("[PQC] %s node %u: last fragment received but incomplete msg=%u msg_id=%u progress=%u/%u first_missing=%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           msg_type,
           msg_id,
           rx->received_frags,
           rx->total_frags,
           rx_first_missing(rx));
  }
}

static clock_time_t hs_transfer_extra(uint16_t transfer_len)
{
  uint8_t frags = fragment_tx_count(transfer_len);
  clock_time_t extra = HS_RESPONSE_GRACE;

  if(frags != 0) {
    extra += (clock_time_t)frags * SEND_INTERVAL;
  }

  return extra;
}

static void hs_set_pending_for_transfer(session_t *s,
                                        uint8_t msg_type,
                                        uint8_t msg_id,
                                        uint16_t transfer_len)
{
  if(s == NULL) {
    return;
  }

  session_set_pending(s, msg_type, msg_id);
  s->retry_deadline = clock_time() + SESSION_RETRY_TIMEOUT + hs_transfer_extra(transfer_len);
}

static void hs_mark_retry_for_transfer(session_t *s, uint16_t transfer_len)
{
  if(s == NULL) {
    return;
  }

  s->retry_count++;
  s->metrics.resource.retries++;
  s->retry_deadline = clock_time() + SESSION_RETRY_TIMEOUT + hs_transfer_extra(transfer_len);
  session_touch(s);
}

static void hs_mark_rx_retry(session_t *s)
{
  if(s == NULL) {
    return;
  }

  s->retry_count++;
  s->metrics.resource.retries++;
  s->retry_deadline = clock_time() + SESSION_RETRY_TIMEOUT;
  session_touch(s);
}

static int send_ack(session_t *s, uint8_t confirmed_msg_id, hs_send_fn_t send_fn, void *user)
{
  uint8_t ack_payload[1];
  uint8_t ack_msg_id;

  if(s == NULL || send_fn == NULL) {
    return 0;
  }

  if(!tx_has_room_for(sizeof(ack_payload))) {
    printf("[PQC] Root node %u: no TX room for ACK\r\n", s->node_id);
    return 0;
  }

  ack_payload[0] = confirmed_msg_id;
  ack_msg_id = session_next_msg_id(s);
  s->metrics.handshake.ack_tx++;
  printf("[PQC] Root node %u: enqueue ACK msg_id=%u confirms=%u\r\n",
         s->node_id, ack_msg_id, confirmed_msg_id);

  return fragment_tx(ack_payload,
                     sizeof(ack_payload),
                     HS_MSG_ACK,
                     s->node_id,
                     ack_msg_id,
                     PQC_FLAG_NONE,
                     send_fn,
                     user);
}

static uint8_t send_nack(session_t *s,
                         const fragment_rx_ctx_t *rx,
                         hs_send_fn_t send_fn,
                         void *user)
{
  pqc_nack_t nack;
  uint8_t nack_payload[PQC_NACK_MAX_PAYLOAD_LEN];
  uint8_t nack_len;
  uint8_t nack_msg_id;

  if(s == NULL || rx == NULL || send_fn == NULL) {
    return 0;
  }

  if(!pqc_nack_build_from_rx(rx, &nack, PQC_NACK_MAX_MISSING)) {
    return 0;
  }

  nack_len = pqc_nack_encode(&nack, nack_payload, sizeof(nack_payload));
  if(nack_len == 0) {
    return 0;
  }

  if(!tx_has_room_for(nack_len)) {
    printf("[PQC] %s node %u: no TX room for NACK msg=%u msg_id=%u missing=%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           nack.missing_msg_type,
           nack.missing_msg_id,
           nack.missing_count);
    return 0;
  }

  nack_msg_id = session_next_msg_id(s);
  s->metrics.fragmentation.nack_tx++;
  s->metrics.fragmentation.nack_rounds++;
  printf("[PQC] %s node %u: enqueue NACK msg_id=%u missing_msg=%u missing_msg_id=%u count=%u\r\n",
         session_is_root(s) ? "Root" : "Node",
         s->node_id,
         nack_msg_id,
         nack.missing_msg_type,
         nack.missing_msg_id,
         nack.missing_count);

  if(!fragment_tx(nack_payload,
                  nack_len,
                  HS_MSG_NACK,
                  s->node_id,
                  nack_msg_id,
                  PQC_FLAG_NONE,
                  send_fn,
                  user)) {
    return 0;
  }

  return nack.missing_count;
}

static int send_fragment_slice(const uint8_t *data,
                               uint16_t len,
                               uint8_t msg_type,
                               uint16_t node_id,
                               uint8_t msg_id,
                               uint8_t frag_idx,
                               uint8_t total_frags,
                               uint8_t flags,
                               hs_send_fn_t send_fn,
                               void *user)
{
  uint8_t frag[FRAG_HEADER_LEN + FRAG_MAX_PAYLOAD_LEN];
  uint16_t offset;
  uint16_t remaining;
  uint8_t frag_len;

  if(data == NULL || len == 0 || total_frags == 0 ||
     frag_idx >= total_frags || send_fn == NULL) {
    return 0;
  }

  offset = (uint16_t)frag_idx * FRAG_MAX_PAYLOAD_LEN;
  if(offset >= len) {
    return 0;
  }

  remaining = len - offset;
  frag_len = remaining > FRAG_MAX_PAYLOAD_LEN ? FRAG_MAX_PAYLOAD_LEN : remaining;

  pqc_header_write(frag,
                   msg_type,
                   node_id,
                   msg_id,
                   frag_idx,
                   total_frags,
                   frag_len,
                   flags);

  memcpy(&frag[FRAG_HEADER_LEN], &data[offset], frag_len);
  return send_fn(frag, FRAG_HEADER_LEN + frag_len, user);
}

static int retransmit_nack_fragments(session_t *s,
                                     const pqc_nack_t *nack,
                                     hs_send_fn_t send_fn,
                                     void *user)
{
  const uint8_t *data = NULL;
  uint16_t len = 0;
  uint8_t expected_frags;
  uint8_t i;

  if(s == NULL || nack == NULL || send_fn == NULL) {
    return 0;
  }

  s->metrics.fragmentation.nack_rx++;

  if(nack->missing_msg_id != s->pending_msg_id ||
     nack->missing_msg_type != s->pending_msg_type) {
    printf("[PQC] %s node %u: NACK rejected missing_msg=%u/%u pending=%u/%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           nack->missing_msg_type,
           nack->missing_msg_id,
           s->pending_msg_type,
           s->pending_msg_id);
    return 0;
  }

  if(session_is_root(s) && s->pending_msg_type == HS_MSG_PK) {
    data = root_pk;
    len = CW_HQC_PUBLIC_KEY_LEN;
  } else if(session_is_node(s) && s->pending_msg_type == HS_MSG_CT) {
    data = node_ct;
    len = CW_HQC_CIPHERTEXT_LEN;
  } else {
    printf("[PQC] %s node %u: NACK cannot be served for msg=%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           s->pending_msg_type);
    return 0;
  }

  expected_frags = fragment_tx_count(len);
  if(expected_frags == 0 || nack->total_frags != expected_frags) {
    printf("[PQC] %s node %u: NACK rejected total_frags=%u expected=%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           nack->total_frags,
           expected_frags);
    return 0;
  }

  if(pqc_tx_free_slots() < nack->missing_count) {
    printf("[PQC] %s node %u: no TX room for NACK retransmit count=%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           nack->missing_count);
    s->retry_deadline = clock_time() + HS_RETRY_TX_DRAIN_DELAY;
    return 1;
  }

  printf("[PQC] %s node %u: retransmit %u fragments for msg=%u msg_id=%u\r\n",
         session_is_root(s) ? "Root" : "Node",
         s->node_id,
         nack->missing_count,
         nack->missing_msg_type,
         nack->missing_msg_id);

  for(i = 0; i < nack->missing_count; i++) {
    if(!send_fragment_slice(data,
                            len,
                            nack->missing_msg_type,
                            s->node_id,
                            nack->missing_msg_id,
                            nack->missing[i],
                            expected_frags,
                            PQC_FLAG_RETRY,
                            send_fn,
                            user)) {
      return 0;
    }
  }

  s->metrics.fragmentation.nack_recovered_fragments += nack->missing_count;
  s->retry_deadline = clock_time() + SESSION_RETRY_TIMEOUT + hs_transfer_extra(len);
  session_touch(s);
  return 1;
}

int hs_fsm_root_init_keypair(void)
{
  if(root_has_keypair) {
    return 1;
  }

  printf("[PQC] Root: generating global HQC keypair\r\n");
  if(!crypto_worker_keypair(root_pk, root_sk)) {
    printf("[PQC] Root: global keypair generation failed\r\n");
    return 0;
  }

  root_has_keypair = 1;
  printf("[PQC] Root: global HQC keypair ready\r\n");
  return 1;
}

int hs_fsm_root_has_keypair(void)
{
  return root_has_keypair;
}

static int send_getpk_retry(session_t *s, hs_send_fn_t send_fn, void *user)
{
  uint8_t msg[PQC_HEADER_LEN];

  if(s == NULL || send_fn == NULL) {
    return 0;
  }

  printf("[PQC] Node %u: retry GETPK attempt=%u\r\n",
         s->node_id, s->retry_count + 1);

  pqc_header_write(msg,
                   PQC_MSG_GETPK,
                   s->node_id,
                   s->pending_msg_id,
                   0,
                   1,
                   0,
                   PQC_FLAG_RETRY);

  return send_fn(msg, sizeof(msg), user);
}

int parse_fragment_header(const uint8_t *data,
                          uint16_t len,
                          uint8_t *msg_type,
                          uint16_t *node_id,
                          uint8_t *msg_id,
                          uint8_t *frag_idx,
                          uint8_t *total_frags,
                          uint8_t *frag_len)
{
  if(data == NULL || msg_type == NULL || node_id == NULL || msg_id == NULL ||
     frag_idx == NULL || total_frags == NULL || frag_len == NULL ||
     len < FRAG_HEADER_LEN || data[PQC_HDR_VERSION] != PQC_PROTO_VERSION) {
    return 0;
  }

  *msg_type = data[PQC_HDR_MSG_TYPE];
  *node_id = pqc_header_get_node_id(data);
  *msg_id = data[PQC_HDR_MSG_ID];
  *frag_idx = data[PQC_HDR_FRAG_IDX];
  *total_frags = data[PQC_HDR_TOTAL_FRAGS];
  *frag_len = data[PQC_HDR_PAYLOAD_LEN];

  if(*total_frags == 0 || *total_frags > FRAG_MAX_FRAGS ||
     *frag_idx >= *total_frags || *frag_len > FRAG_MAX_PAYLOAD_LEN) {
    return 0;
  }

  return len >= (uint16_t)(FRAG_HEADER_LEN + *frag_len);
}

int hs_fsm_start(session_t *s, hs_send_fn_t send_fn, void *user)
{
  uint8_t msg_id;
  clock_time_t keypair_started_at;

  if(s == NULL || send_fn == NULL) {
    return 0;
  }

  rx_slots_gc();

  if(session_is_root(s)) {
    pqc_metrics_handshake_start(&s->metrics);
    keypair_started_at = root_has_keypair ? 0 : clock_time();

    if(!hs_fsm_root_init_keypair()) {
      session_set_failed(s);
      return 0;
    }
    if(keypair_started_at != 0) {
      s->metrics.handshake.keypair_ticks_last =
        (uint32_t)(clock_time() - keypair_started_at);
    }

    if(!tx_has_room_for(CW_HQC_PUBLIC_KEY_LEN)) {
      printf("[PQC] Root node %u: no TX room for PK\r\n", s->node_id);
      return 0;
    }

    msg_id = session_next_msg_id(s);
    session_set_wait_ct(s);
    hs_set_pending_for_transfer(s, HS_MSG_PK, msg_id, CW_HQC_PUBLIC_KEY_LEN);
    s->metrics.handshake.pk_tx++;
    s->metrics.handshake.pk_tx_started_at = (uint32_t)clock_time();
    s->metrics.fragmentation.fragments_tx += fragment_tx_count(CW_HQC_PUBLIC_KEY_LEN);

    printf("[PQC] Root node %u: enqueue global PK msg_id=%u bytes=%u\r\n",
           s->node_id, msg_id, CW_HQC_PUBLIC_KEY_LEN);

    if(!fragment_tx(root_pk,
                    CW_HQC_PUBLIC_KEY_LEN,
                    HS_MSG_PK,
                    s->node_id,
                    msg_id,
                    PQC_FLAG_NONE,
                    send_fn,
                    user)) {
      printf("[PQC] Root node %u: PK enqueue failed\r\n", s->node_id);
      session_set_failed(s);
      return 0;
    }
    s->metrics.handshake.pk_tx_ticks_last =
      pqc_metrics_elapsed_since(s->metrics.handshake.pk_tx_started_at);
    return 1;
  }

  if(session_is_node(s)) {
    if(s->state != SESSION_STATE_IDLE && s->state != SESSION_STATE_WAIT_PK) {
      printf("[PQC] Node %u: cannot start handshake in state=%u\r\n", s->node_id, s->state);
      return 0;
    }

    printf("[PQC] Node %u: waiting PK\r\n", s->node_id);
    session_set_wait_pk(s);
    return 1;
  }

  return 0;
}

int hs_fsm_handle_fragment(session_t *s,
                           const uint8_t *data,
                           uint16_t len,
                           hs_send_fn_t send_fn,
                           void *user)
{
  fragment_rx_ctx_t *rx;
  uint8_t msg_type;
  uint16_t node_id;
  uint8_t msg_id;
  uint8_t frag_idx;
  uint8_t total_frags;
  uint8_t frag_len;

  rx_slots_gc();

  if(s == NULL || !parse_fragment_header(data, len, &msg_type, &node_id, &msg_id,
                                         &frag_idx, &total_frags, &frag_len)) {
    return 0;
  }

  if(session_is_root(s) && s->node_id != node_id) {
    printf("[PQC] Root node %u: drop fragment for node %u\r\n", s->node_id, node_id);
    return 0;
  }

  if(session_is_node(s) && s->node_id == SESSION_INVALID_NODE_ID) {
    s->node_id = node_id;
  } else if(session_is_node(s) && s->node_id != node_id) {
    printf("[PQC] Node %u: drop fragment for node %u\r\n", s->node_id, node_id);
    return 0;
  }

  if(session_is_node(s) && msg_type == HS_MSG_PK &&
     (s->state != SESSION_STATE_WAIT_PK || s->pending_msg_type != PQC_MSG_GETPK)) {
    printf("[PQC] Node %u: unexpected PK in state=%u pending=%u\r\n",
           s->node_id, s->state, s->pending_msg_type);
    return 0;
  }

  if(session_is_node(s) && msg_type == HS_MSG_ACK &&
     (s->state != SESSION_STATE_WAIT_ACK || s->pending_msg_type != HS_MSG_CT)) {
    printf("[PQC] Node %u: unexpected ACK in state=%u pending=%u\r\n",
           s->node_id, s->state, s->pending_msg_type);
    return 0;
  }

  if(session_is_root(s) && session_is_established(s) && msg_type == HS_MSG_CT) {
    if(frag_idx == 0 || frag_idx + 1 == total_frags) {
      printf("[PQC] Root node %u: duplicate CT fragment msg_id=%u frag=%u/%u, re-sending ACK\r\n",
             s->node_id, msg_id, frag_idx + 1, total_frags);
      send_ack(s, msg_id, send_fn, user);
    }
    return 1;
  }

  if(session_is_root(s) && msg_type == HS_MSG_CT &&
     s->state != SESSION_STATE_WAIT_CT) {
    printf("[PQC] Root node %u: unexpected CT in state=%u\r\n", s->node_id, s->state);
    return 0;
  }

  if(msg_type == HS_MSG_NACK) {
    pqc_nack_t nack;

    if(frag_idx != 0 || total_frags != 1 ||
       !pqc_nack_decode(&data[FRAG_HEADER_LEN], frag_len, &nack)) {
      printf("[PQC] %s node %u: invalid NACK len=%u frag=%u/%u\r\n",
             session_is_root(s) ? "Root" : "Node",
             s->node_id,
             frag_len,
             frag_idx + 1,
             total_frags);
      return 0;
    }

    return retransmit_nack_fragments(s, &nack, send_fn, user);
  }

  rx = rx_slot_get(s);
  if(rx == NULL) {
    printf("[PQC] %s node %u: no RX slot available\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id);
    return 0;
  }

  if(frag_idx == 0 || frag_idx + 1 == total_frags) {
    printf("[PQC] %s node %u: RX msg=%u msg_id=%u frag=%u/%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           msg_type,
           msg_id,
           frag_idx + 1,
           total_frags);
  }

  if(!fragment_rx_add(rx,
                      msg_type,
                      node_id,
                      msg_id,
                      frag_idx,
                      total_frags,
                      &data[FRAG_HEADER_LEN],
                      frag_len)) {
    printf("[PQC] %s node %u: fragment rejected msg=%u msg_id=%u frag=%u/%u\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id,
           msg_type,
           msg_id,
           frag_idx + 1,
           total_frags);
    return 0;
  }

  s->metrics.fragmentation.fragments_rx++;
  rx_debug_progress(s, rx, msg_type, msg_id, frag_idx, total_frags);
  session_touch(s);

  if(!fragment_rx_complete(rx)) {
    if(msg_type != HS_MSG_NACK && rx_slot_nack_due(s, rx, frag_idx)) {
      uint8_t missing_count = send_nack(s, rx, send_fn, user);
      rx_slot_note_nack(s, rx, missing_count);
    }
    return 1;
  }

  printf("[PQC] %s node %u: message complete msg=%u msg_id=%u bytes=%u\r\n",
         session_is_root(s) ? "Root" : "Node",
         s->node_id,
         msg_type,
         msg_id,
         fragment_rx_len(rx));

  if(session_is_node(s) && msg_type == HS_MSG_PK) {
    const uint8_t *pk = fragment_rx_data(rx);
    uint8_t ct_msg_id;
    clock_time_t started_at;

    if(fragment_rx_len(rx) != CW_HQC_PUBLIC_KEY_LEN) {
      printf("[PQC] Node %u: invalid PK length=%u\r\n",
             s->node_id, fragment_rx_len(rx));
      session_set_failed(s);
      rx_slot_release(s);
      return 0;
    }

    s->metrics.handshake.pk_rx_complete++;
    s->metrics.handshake.pk_rx_completed_at = (uint32_t)clock_time();
    if(s->metrics.handshake.handshake_started_at != 0) {
      s->metrics.handshake.pk_rx_ticks_last =
        pqc_metrics_elapsed_since(s->metrics.handshake.handshake_started_at);
    }
    printf("PK recibida. Encapsulando en nodo %u\r\n", s->node_id);
    started_at = clock_time();
    if(!crypto_worker_encapsulate(pk, node_ct, s->ss)) {
      printf("[PQC] Node %u: encapsulation failed\r\n", s->node_id);
      session_set_failed(s);
      rx_slot_release(s);
      return 0;
    }
    s->metrics.handshake.encaps_ticks_last = (uint32_t)(clock_time() - started_at);

    crypto_worker_derive_aes_key(s->ss, s->aes_key);
    printf("Enviando CT desde nodo %u\r\n", s->node_id);
    rx_slot_release(s);

    if(!tx_has_room_for(CW_HQC_CIPHERTEXT_LEN)) {
      printf("[PQC] Node %u: no TX room for CT\r\n", s->node_id);
      crypto_worker_secure_zero(node_ct, sizeof(node_ct));
      session_set_failed(s);
      return 0;
    }

    session_clear_pending(s);
    ct_msg_id = session_next_msg_id(s);
    session_set_wait_ack(s);
    hs_set_pending_for_transfer(s, HS_MSG_CT, ct_msg_id, CW_HQC_CIPHERTEXT_LEN);
    s->metrics.handshake.ct_tx++;
    s->metrics.handshake.ct_tx_started_at = (uint32_t)clock_time();
    s->metrics.fragmentation.fragments_tx += fragment_tx_count(CW_HQC_CIPHERTEXT_LEN);

    if(!fragment_tx(node_ct,
                    CW_HQC_CIPHERTEXT_LEN,
                    HS_MSG_CT,
                    s->node_id,
                    ct_msg_id,
                    PQC_FLAG_NONE,
                    send_fn,
                    user)) {
      printf("[PQC] Node %u: CT enqueue failed\r\n", s->node_id);
      crypto_worker_secure_zero(node_ct, sizeof(node_ct));
      session_set_failed(s);
      return 0;
    }
    s->metrics.handshake.ct_tx_ticks_last =
      pqc_metrics_elapsed_since(s->metrics.handshake.ct_tx_started_at);
    return 1;
  }

  if(session_is_node(s) && msg_type == HS_MSG_ACK) {
    const uint8_t *ack = fragment_rx_data(rx);

    if(fragment_rx_len(rx) != 1 || ack == NULL || ack[0] != s->pending_msg_id) {
      printf("[PQC] Node %u: ACK rejected len=%u confirms=%u expected=%u\r\n",
             s->node_id,
             fragment_rx_len(rx),
             ack == NULL ? 0xff : ack[0],
             s->pending_msg_id);
      rx_slot_release(s);
      return 0;
    }

    session_set_established(s);
    s->metrics.handshake.ack_rx++;
    s->metrics.handshake.ack_rx_at = (uint32_t)clock_time();
    if(s->metrics.handshake.ct_tx_started_at != 0) {
      s->metrics.handshake.ack_wait_ticks_last =
        pqc_metrics_elapsed_since(s->metrics.handshake.ct_tx_started_at);
    }
    pqc_metrics_handshake_success(&s->metrics);
    crypto_worker_secure_zero(node_ct, sizeof(node_ct));
    rx_slot_release(s);
    printf("ACK recibida. Exito PQC nodo %u\r\n", s->node_id);
    return 1;
  }

  if(session_is_root(s) && msg_type == HS_MSG_CT) {
    const uint8_t *ct = fragment_rx_data(rx);
    clock_time_t started_at;

    if(fragment_rx_len(rx) != CW_HQC_CIPHERTEXT_LEN) {
      printf("[PQC] Root node %u: invalid CT length=%u\r\n",
             s->node_id, fragment_rx_len(rx));
      session_set_failed(s);
      rx_slot_release(s);
      return 0;
    }

    s->metrics.handshake.ct_rx_complete++;
    s->metrics.handshake.ct_rx_completed_at = (uint32_t)clock_time();
    if(s->metrics.handshake.ct_rx_started_at != 0) {
      s->metrics.handshake.ct_rx_ticks_last =
        pqc_metrics_elapsed_since(s->metrics.handshake.ct_rx_started_at);
    }
    memcpy(root_ct, ct, CW_HQC_CIPHERTEXT_LEN);
    rx_slot_release(s);

    if(session_is_established(s)) {
      printf("[PQC] Root node %u: duplicate CT, re-sending ACK\r\n", s->node_id);
      crypto_worker_secure_zero(root_ct, sizeof(root_ct));
      return send_ack(s, msg_id, send_fn, user);
    }

    if(s->state != SESSION_STATE_WAIT_CT) {
      printf("[PQC] Root node %u: CT rejected in state=%u\r\n", s->node_id, s->state);
      crypto_worker_secure_zero(root_ct, sizeof(root_ct));
      return 0;
    }

    if(!hs_fsm_root_has_keypair()) {
      printf("[PQC] Root node %u: missing global keypair\r\n", s->node_id);
      session_set_failed(s);
      crypto_worker_secure_zero(root_ct, sizeof(root_ct));
      return 0;
    }

    printf("Desencapsulando CT del nodo %u\r\n", s->node_id);
    started_at = clock_time();
    if(!crypto_worker_decapsulate(root_sk, root_ct, s->ss)) {
      printf("[PQC] Root node %u: decapsulation failed\r\n", s->node_id);
      session_set_failed(s);
      crypto_worker_secure_zero(root_ct, sizeof(root_ct));
      return 0;
    }
    s->metrics.handshake.decaps_ticks_last = (uint32_t)(clock_time() - started_at);

    crypto_worker_secure_zero(root_ct, sizeof(root_ct));
    crypto_worker_derive_aes_key(s->ss, s->aes_key);
    session_set_established(s);
    pqc_metrics_handshake_success(&s->metrics);
    printf("Exito PQC nodo %u\r\n", s->node_id);

    return send_ack(s, msg_id, send_fn, user);
  }

  printf("[PQC] %s node %u: unexpected complete msg=%u, resetting RX\r\n",
         session_is_root(s) ? "Root" : "Node",
         s->node_id,
         msg_type);
  rx_slot_release(s);
  return 0;
}

int hs_fsm_retry(session_t *s, hs_send_fn_t send_fn, void *user)
{
  uint8_t flags;
  int queued;

  rx_slots_gc();

  if(s == NULL || send_fn == NULL || !session_retry_due(s)) {
    return 1;
  }

  if(pqc_tx_has_pending()) {
    s->retry_deadline = clock_time() + HS_RETRY_TX_DRAIN_DELAY;
    return 1;
  }

  if(!session_can_retry(s)) {
    printf("[PQC] %s node %u: max retries reached\r\n",
           session_is_root(s) ? "Root" : "Node",
           s->node_id);
    session_set_failed(s);
    rx_slot_release(s);
    if(session_is_node(s)) {
      crypto_worker_secure_zero(node_ct, sizeof(node_ct));
    }
    return 0;
  }

  flags = PQC_FLAG_RETRY;

  if(session_is_root(s) && s->pending_msg_type == HS_MSG_CT) {
    fragment_rx_ctx_t *rx = rx_slot_find(s);

    if(rx != NULL && rx->active && !fragment_rx_complete(rx)) {
      uint8_t missing_count;

      printf("[PQC] Root node %u: CT incomplete at retry, requesting missing fragments attempt=%u\r\n",
             s->node_id, s->retry_count + 1);
      missing_count = send_nack(s, rx, send_fn, user);
      if(missing_count != 0) {
        rx_slot_touch(s);
        rx_slot_note_nack(s, rx, missing_count);
        hs_mark_rx_retry(s);
        return 1;
      }
    }

    printf("[PQC] Root node %u: CT receive timeout, no recoverable RX state\r\n",
           s->node_id);
    session_set_failed(s);
    rx_slot_release(s);
    return 0;
  }

  rx_slot_release(s);

  if(session_is_node(s) && s->pending_msg_type == PQC_MSG_GETPK) {
    queued = send_getpk_retry(s, send_fn, user);
    if(queued) {
      hs_mark_retry_for_transfer(s, PQC_HEADER_LEN);
    }
    return queued;
  }

  if(session_is_root(s) && s->pending_msg_type == HS_MSG_PK) {
    if(!hs_fsm_root_has_keypair()) {
      printf("[PQC] Root node %u: cannot retry PK, missing global keypair\r\n", s->node_id);
      session_set_failed(s);
      return 0;
    }

    if(!tx_has_room_for(CW_HQC_PUBLIC_KEY_LEN)) {
      printf("[PQC] Root node %u: no TX room to retry PK\r\n", s->node_id);
      s->retry_deadline = clock_time() + HS_RETRY_TX_DRAIN_DELAY;
      return 1;
    }

    printf("[PQC] Root node %u: retry global PK msg_id=%u attempt=%u\r\n",
           s->node_id, s->pending_msg_id, s->retry_count + 1);
    queued = fragment_tx(root_pk,
                         CW_HQC_PUBLIC_KEY_LEN,
                         HS_MSG_PK,
                         s->node_id,
                         s->pending_msg_id,
                         flags,
                         send_fn,
                         user);
    if(queued) {
      hs_mark_retry_for_transfer(s, CW_HQC_PUBLIC_KEY_LEN);
    }
    return queued;
  }

  if(session_is_node(s) && s->pending_msg_type == HS_MSG_CT) {
    if(!tx_has_room_for(CW_HQC_CIPHERTEXT_LEN)) {
      printf("[PQC] Node %u: no TX room to retry CT\r\n", s->node_id);
      s->retry_deadline = clock_time() + HS_RETRY_TX_DRAIN_DELAY;
      return 1;
    }

    printf("[PQC] Node %u: retry CT msg_id=%u attempt=%u\r\n",
           s->node_id, s->pending_msg_id, s->retry_count + 1);
    queued = fragment_tx(node_ct,
                         CW_HQC_CIPHERTEXT_LEN,
                         HS_MSG_CT,
                         s->node_id,
                         s->pending_msg_id,
                         flags,
                         send_fn,
                         user);
    if(queued) {
      hs_mark_retry_for_transfer(s, CW_HQC_CIPHERTEXT_LEN);
    }
    return queued;
  }

  printf("[PQC] %s node %u: retry requested without pending message\r\n",
         session_is_root(s) ? "Root" : "Node",
         s->node_id);
  return 0;
}
