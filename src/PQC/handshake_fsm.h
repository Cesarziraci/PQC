#ifndef HANDSHAKE_FSM_H
#define HANDSHAKE_FSM_H

#include <stdint.h>
#include "protocol.h"
#include "session_manager.h"

#define HS_MSG_PK  PQC_MSG_PK
#define HS_MSG_CT  PQC_MSG_CT
#define HS_MSG_ACK PQC_MSG_ACK
#define HS_MSG_NACK PQC_MSG_NACK

typedef int (*hs_send_fn_t)(const uint8_t *data, uint16_t len, void *user);
int parse_fragment_header(const uint8_t *data,
                                 uint16_t len,
                                 uint8_t *msg_type,
                                 uint16_t *node_id,
                                 uint8_t *msg_id,
                                 uint8_t *frag_idx,
                                 uint8_t *total_frags,
                                 uint8_t *frag_len);

int hs_fsm_root_init_keypair(void);
int hs_fsm_root_has_keypair(void);

int hs_fsm_start(session_t *s, hs_send_fn_t send_fn, void *user);

int hs_fsm_handle_fragment(session_t *s,
                           const uint8_t *data,
                           uint16_t len,
                           hs_send_fn_t send_fn,
                           void *user);

int hs_fsm_retry(session_t *s, hs_send_fn_t send_fn, void *user);

#endif /* HANDSHAKE_FSM_H */
