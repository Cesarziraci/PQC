#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <stdint.h>
#include "contiki.h"
#include "net/ipv6/uip.h"
#include "crypto_worker.h"
#include "metrics_schema.h"

#define SESSION_MAX_NODES 10
#define SESSION_INVALID_NODE_ID 0xffff
#define SESSION_MAX_RETRIES 3
/*
 * HQC messages are large and are sent as many UDP fragments. The retry timer
 * must cover queue drain time, radio latency and KEM decapsulation before the
 * ACK can arrive; otherwise the node retransmits CT even on successful runs.
 */
#define SESSION_RETRY_TIMEOUT (120 * CLOCK_SECOND)

typedef enum {
  SESSION_ROLE_NODE = 0,
  SESSION_ROLE_ROOT = 1
} session_role_t;

typedef enum {
  SESSION_STATE_UNUSED = 0,
  SESSION_STATE_IDLE,
  SESSION_STATE_WAIT_PK,
  SESSION_STATE_WAIT_CT,
  SESSION_STATE_WAIT_ACK,
  SESSION_STATE_ESTABLISHED,
  SESSION_STATE_FAILED
} session_state_t;

typedef struct {
  uint8_t used;
  uint16_t node_id;

  session_role_t role;
  session_state_t state;

  uint8_t established;

  uint8_t next_msg_id;

  uint32_t tx_counter;
  uint32_t rx_counter;

  uint8_t ss[CW_HQC_SHARED_LEN];
  uint8_t aes_key[CW_AES_KEY_LEN];

  uint8_t retry_count;
  uint8_t pending_msg_type;
  uint8_t pending_msg_id;
  uint8_t waiting_response;

  uint8_t has_peer_addr;
  uip_ipaddr_t peer_addr;

  clock_time_t last_activity;
  clock_time_t retry_deadline;

  pqc_metrics_snapshot_t metrics;
  uint32_t node_metric_values[PQC_METRICS_GROUP_COUNT + 1][4];
  uint8_t node_metrics_mask;
  uint8_t session_summary_emitted;
} session_t;

void session_manager_init(session_role_t local_role);

session_t *session_manager_get_local(void);
session_t *session_manager_get_or_create(uint16_t node_id);
session_t *session_manager_find(uint16_t node_id);
session_t *session_manager_get_by_index(uint8_t index);

void session_init(session_t *s, session_role_t role, uint16_t node_id);
void session_reset(session_t *s);
void session_clear_secrets(session_t *s);

int session_is_root(const session_t *s);
int session_is_node(const session_t *s);
int session_is_established(const session_t *s);
int session_is_failed(const session_t *s);
int session_retry_due(const session_t *s);
int session_can_retry(const session_t *s);

void session_touch(session_t *s);
void session_set_peer_addr(session_t *s, const uip_ipaddr_t *addr);
void session_set_pending(session_t *s, uint8_t msg_type, uint8_t msg_id);
void session_clear_pending(session_t *s);
void session_mark_retry(session_t *s);

void session_set_wait_pk(session_t *s);
void session_set_wait_ct(session_t *s);
void session_set_wait_ack(session_t *s);
void session_set_established(session_t *s);
void session_set_failed(session_t *s);

uint8_t session_next_msg_id(session_t *s);
uint32_t session_next_tx_counter(session_t *s);
uint32_t session_next_rx_counter(session_t *s);
uint32_t session_peek_rx_counter(const session_t *s);
void session_commit_rx_counter(session_t *s);

#endif /* SESSION_MANAGER_H */
