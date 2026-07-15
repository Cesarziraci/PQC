#include "session_manager.h"

#include <string.h>
#include "metrics.h"

static session_role_t manager_role;
static session_t local_session;
static session_t sessions[SESSION_MAX_NODES];

void session_clear_secrets(session_t *s)
{
  if(s == NULL) {
    return;
  }

  crypto_worker_secure_zero(s->ss, sizeof(s->ss));
  crypto_worker_secure_zero(s->aes_key, sizeof(s->aes_key));
}

void session_init(session_t *s, session_role_t role, uint16_t node_id)
{
  if(s == NULL) {
    return;
  }

  memset(s, 0, sizeof(*s));
  s->used = 1;
  s->role = role;
  s->node_id = node_id;
  s->state = SESSION_STATE_IDLE;
  pqc_metrics_init(&s->metrics, node_id);
  session_touch(s);
}

void session_reset(session_t *s)
{
  session_role_t role;
  uint16_t node_id;
  uint8_t has_peer_addr;
  uip_ipaddr_t peer_addr;
  pqc_metrics_snapshot_t metrics;

  if(s == NULL) {
    return;
  }

  role = s->role;
  node_id = s->node_id;
  has_peer_addr = s->has_peer_addr;
  peer_addr = s->peer_addr;
  metrics = s->metrics;

  session_clear_secrets(s);
  session_init(s, role, node_id);
  s->metrics = metrics;
  pqc_metrics_set_node_id(&s->metrics, node_id);

  if(has_peer_addr) {
    session_set_peer_addr(s, &peer_addr);
  }
}

void session_manager_init(session_role_t local_role)
{
  manager_role = local_role;
  memset(sessions, 0, sizeof(sessions));

  if(local_role == SESSION_ROLE_NODE) {
    session_init(&local_session, SESSION_ROLE_NODE, SESSION_INVALID_NODE_ID);
  } else {
    memset(&local_session, 0, sizeof(local_session));
  }
}

session_t *session_manager_get_local(void)
{
  return manager_role == SESSION_ROLE_NODE ? &local_session : NULL;
}

session_t *session_manager_find(uint16_t node_id)
{
  int i;

  for(i = 0; i < SESSION_MAX_NODES; i++) {
    if(sessions[i].used && sessions[i].node_id == node_id) {
      return &sessions[i];
    }
  }

  return NULL;
}

session_t *session_manager_get_by_index(uint8_t index)
{
  if(index >= SESSION_MAX_NODES || !sessions[index].used) {
    return NULL;
  }

  return &sessions[index];
}

session_t *session_manager_get_or_create(uint16_t node_id)
{
  int i;
  session_t *s;

  s = session_manager_find(node_id);
  if(s != NULL) {
    return s;
  }

  for(i = 0; i < SESSION_MAX_NODES; i++) {
    if(!sessions[i].used) {
      session_init(&sessions[i], SESSION_ROLE_ROOT, node_id);
      return &sessions[i];
    }
  }

  return NULL;
}

int session_is_root(const session_t *s)
{
  return s != NULL && s->role == SESSION_ROLE_ROOT;
}

int session_is_node(const session_t *s)
{
  return s != NULL && s->role == SESSION_ROLE_NODE;
}

int session_is_established(const session_t *s)
{
  return s != NULL && s->state == SESSION_STATE_ESTABLISHED && s->established;
}

int session_is_failed(const session_t *s)
{
  return s != NULL && s->state == SESSION_STATE_FAILED;
}

void session_touch(session_t *s)
{
  if(s != NULL) {
    s->last_activity = clock_time();
  }
}

void session_set_peer_addr(session_t *s, const uip_ipaddr_t *addr)
{
  if(s == NULL || addr == NULL) {
    return;
  }

  s->peer_addr = *addr;
  s->has_peer_addr = 1;
  session_touch(s);
}

void session_set_pending(session_t *s, uint8_t msg_type, uint8_t msg_id)
{
  if(s == NULL) {
    return;
  }

  s->pending_msg_type = msg_type;
  s->pending_msg_id = msg_id;
  s->waiting_response = 1;
  s->retry_deadline = clock_time() + SESSION_RETRY_TIMEOUT;
  session_touch(s);
}

void session_clear_pending(session_t *s)
{
  if(s == NULL) {
    return;
  }

  s->pending_msg_type = 0;
  s->pending_msg_id = 0;
  s->waiting_response = 0;
  s->retry_count = 0;
}

int session_retry_due(const session_t *s)
{
  if(s == NULL || !s->waiting_response) {
    return 0;
  }

  return clock_time() >= s->retry_deadline;
}

int session_can_retry(const session_t *s)
{
  return s != NULL && s->retry_count < SESSION_MAX_RETRIES;
}

void session_mark_retry(session_t *s)
{
  if(s == NULL) {
    return;
  }

  s->retry_count++;
  s->retry_deadline = clock_time() + SESSION_RETRY_TIMEOUT;
  session_touch(s);
}

void session_set_wait_pk(session_t *s)
{
  if(s != NULL) {
    s->state = SESSION_STATE_WAIT_PK;
  }
}

void session_set_wait_ct(session_t *s)
{
  if(s != NULL) {
    s->state = SESSION_STATE_WAIT_CT;
  }
}

void session_set_wait_ack(session_t *s)
{
  if(s != NULL) {
    s->state = SESSION_STATE_WAIT_ACK;
  }
}

void session_set_established(session_t *s)
{
  if(s != NULL) {
    s->state = SESSION_STATE_ESTABLISHED;
    s->established = 1;
    session_clear_pending(s);
  }
}

void session_set_failed(session_t *s)
{
  if(s != NULL) {
    if(s->state != SESSION_STATE_FAILED) {
      pqc_metrics_handshake_failure(&s->metrics);
    }
    s->state = SESSION_STATE_FAILED;
    s->established = 0;
    session_clear_pending(s);
  }
}

uint8_t session_next_msg_id(session_t *s)
{
  if(s == NULL) {
    return 0;
  }

  return s->next_msg_id++;
}

uint32_t session_next_tx_counter(session_t *s)
{
  if(s == NULL) {
    return 0;
  }

  return s->tx_counter++;
}

uint32_t session_next_rx_counter(session_t *s)
{
  uint32_t counter;

  counter = session_peek_rx_counter(s);
  session_commit_rx_counter(s);
  return counter;
}

uint32_t session_peek_rx_counter(const session_t *s)
{
  if(s == NULL) {
    return 0;
  }

  return s->rx_counter;
}

void session_commit_rx_counter(session_t *s)
{
  if(s != NULL) {
    s->rx_counter++;
  }
}
