#include "nack.h"

#include <string.h>

void pqc_nack_init(pqc_nack_t *nack,
                   uint8_t missing_msg_type,
                   uint8_t missing_msg_id,
                   uint8_t total_frags)
{
  if(nack == NULL) {
    return;
  }

  memset(nack, 0, sizeof(*nack));
  nack->missing_msg_type = missing_msg_type;
  nack->missing_msg_id = missing_msg_id;
  nack->total_frags = total_frags;
}

uint8_t pqc_nack_build_from_rx(const fragment_rx_ctx_t *rx,
                               pqc_nack_t *nack,
                               uint8_t max_missing)
{
  uint8_t i;
  uint8_t limit;

  if(rx == NULL || nack == NULL || !rx->active || rx->total_frags == 0) {
    return 0;
  }

  if(max_missing == 0 || max_missing > PQC_NACK_MAX_MISSING) {
    max_missing = PQC_NACK_MAX_MISSING;
  }

  pqc_nack_init(nack, rx->msg_type, rx->msg_id, rx->total_frags);
  limit = rx->total_frags;
  if(limit > FRAG_MAX_FRAGS) {
    limit = FRAG_MAX_FRAGS;
  }

  for(i = 0; i < limit && nack->missing_count < max_missing; i++) {
    if(!rx->bitmap[i]) {
      nack->missing[nack->missing_count++] = i;
    }
  }

  return nack->missing_count;
}

uint8_t pqc_nack_encode(const pqc_nack_t *nack,
                        uint8_t *payload,
                        uint8_t payload_max_len)
{
  uint8_t i;
  uint8_t len;

  if(nack == NULL || payload == NULL ||
     nack->missing_count == 0 ||
     nack->missing_count > PQC_NACK_MAX_MISSING) {
    return 0;
  }

  len = PQC_NACK_FIXED_LEN + nack->missing_count;
  if(payload_max_len < len) {
    return 0;
  }

  payload[0] = nack->missing_msg_type;
  payload[1] = nack->missing_msg_id;
  payload[2] = nack->total_frags;
  payload[3] = nack->missing_count;

  for(i = 0; i < nack->missing_count; i++) {
    payload[PQC_NACK_FIXED_LEN + i] = nack->missing[i];
  }

  return len;
}

uint8_t pqc_nack_decode(const uint8_t *payload,
                        uint8_t payload_len,
                        pqc_nack_t *nack)
{
  uint8_t i;
  uint8_t count;

  if(payload == NULL || nack == NULL || payload_len < PQC_NACK_FIXED_LEN) {
    return 0;
  }

  count = payload[3];
  if(count == 0 || count > PQC_NACK_MAX_MISSING ||
     payload[2] == 0 || payload[2] > FRAG_MAX_FRAGS ||
     payload_len != (uint8_t)(PQC_NACK_FIXED_LEN + count)) {
    return 0;
  }

  pqc_nack_init(nack, payload[0], payload[1], payload[2]);
  nack->missing_count = count;

  for(i = 0; i < count; i++) {
    if(payload[PQC_NACK_FIXED_LEN + i] >= nack->total_frags) {
      memset(nack, 0, sizeof(*nack));
      return 0;
    }
    nack->missing[i] = payload[PQC_NACK_FIXED_LEN + i];
  }

  return 1;
}

int pqc_nack_contains(const pqc_nack_t *nack, uint8_t frag_idx)
{
  uint8_t i;

  if(nack == NULL) {
    return 0;
  }

  for(i = 0; i < nack->missing_count; i++) {
    if(nack->missing[i] == frag_idx) {
      return 1;
    }
  }

  return 0;
}
