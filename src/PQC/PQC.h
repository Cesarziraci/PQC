#ifndef PQC_H
#define PQC_H

#include "protocol.h"
#include "session_manager.h"

typedef struct {
  struct simple_udp_connection *udp_conn;
  uip_ipaddr_t *dest;
} pqc_udp_send_ctx_t;

void pqc_tx_init(void);
int pqc_tx_has_pending(void);
uint8_t pqc_tx_free_slots(void);
int pqc_tx_process(void);
uint8_t pqc_tx_drop(uint16_t node_id, uint8_t msg_type, uint8_t msg_id);
uint8_t pqc_tx_drop_node(uint16_t node_id);

int pqc_udp_send_fragment(const uint8_t *data, uint16_t len, void *user);

int pqc_send_GETPK(struct simple_udp_connection *udp_conn,
                   uip_ipaddr_t *root_addr,
                   uint16_t node_id);

int pqc_send_encrypted_temp(session_t *s,
                            struct simple_udp_connection *udp_conn,
                            uip_ipaddr_t *dest,
                            const pqc_temp_payload_t *measurement);

int pqc_send_metrics_report(session_t *s,
                            struct simple_udp_connection *udp_conn,
                            uip_ipaddr_t *dest,
                            uint8_t group);

int pqc_handle_encrypted_temp(session_t *s,
                              const uint8_t *data,
                              uint16_t len,
                              pqc_temp_payload_t *out);

#endif /* PQC_H */
