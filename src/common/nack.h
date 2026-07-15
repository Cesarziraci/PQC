#ifndef NACK_H
#define NACK_H

#include <stdint.h>
#include "protocol.h"
#include "fragmentation.h"

#define PQC_NACK_MAX_MISSING 16
#define PQC_NACK_FIXED_LEN 4
#define PQC_NACK_MAX_PAYLOAD_LEN (PQC_NACK_FIXED_LEN + PQC_NACK_MAX_MISSING)

typedef struct {
  uint8_t missing_msg_type;
  uint8_t missing_msg_id;
  uint8_t total_frags;
  uint8_t missing_count;
  uint8_t missing[PQC_NACK_MAX_MISSING];
} pqc_nack_t;

void pqc_nack_init(pqc_nack_t *nack,
                   uint8_t missing_msg_type,
                   uint8_t missing_msg_id,
                   uint8_t total_frags);

uint8_t pqc_nack_build_from_rx(const fragment_rx_ctx_t *rx,
                               pqc_nack_t *nack,
                               uint8_t max_missing);

uint8_t pqc_nack_encode(const pqc_nack_t *nack,
                        uint8_t *payload,
                        uint8_t payload_max_len);

uint8_t pqc_nack_decode(const uint8_t *payload,
                        uint8_t payload_len,
                        pqc_nack_t *nack);

int pqc_nack_contains(const pqc_nack_t *nack, uint8_t frag_idx);

#endif /* NACK_H */