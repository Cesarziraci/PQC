#ifndef FRAGMENTATION_H
#define FRAGMENTATION_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#define FRAG_MAX_MESSAGE_LEN 4608
#define FRAG_MAX_PAYLOAD_LEN 64
#define FRAG_MAX_FRAGS ((FRAG_MAX_MESSAGE_LEN + FRAG_MAX_PAYLOAD_LEN - 1) / FRAG_MAX_PAYLOAD_LEN)
#define FRAG_HEADER_LEN PQC_HEADER_LEN

typedef struct {
  uint8_t active;
  uint8_t msg_type;
  uint16_t node_id;
  uint8_t msg_id;
  uint8_t total_frags;
  uint8_t received_frags;
  uint16_t received_len;
  uint8_t bitmap[FRAG_MAX_FRAGS];
  uint8_t buffer[FRAG_MAX_MESSAGE_LEN];
} fragment_rx_ctx_t;

typedef int (*fragment_send_fn_t)(const uint8_t *data, uint16_t len, void *user);

void fragment_rx_init(fragment_rx_ctx_t *ctx);
void fragment_rx_reset(fragment_rx_ctx_t *ctx);

int fragment_rx_add(fragment_rx_ctx_t *ctx,
                    uint8_t msg_type,
                    uint16_t node_id,
                    uint8_t msg_id,
                    uint8_t frag_idx,
                    uint8_t total_frags,
                    const uint8_t *data,
                    uint16_t len);

bool fragment_rx_complete(const fragment_rx_ctx_t *ctx);
const uint8_t *fragment_rx_data(const fragment_rx_ctx_t *ctx);
uint16_t fragment_rx_len(const fragment_rx_ctx_t *ctx);

uint8_t fragment_tx_count(uint16_t len);

int fragment_tx(const uint8_t *data,
                uint16_t len,
                uint8_t msg_type,
                uint16_t node_id,
                uint8_t msg_id,
                uint8_t flags,
                fragment_send_fn_t send_fn,
                void *user);

#endif /* FRAGMENTATION_H */