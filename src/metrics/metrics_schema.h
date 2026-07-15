#ifndef METRICS_SCHEMA_H
#define METRICS_SCHEMA_H

#include <stdint.h>

#define PQC_METRICS_SCHEMA_VERSION 1

#define PQC_METRICS_REPORT_LEN 26

#define PQC_METRICS_GROUP_HS      1
#define PQC_METRICS_GROUP_PK      2
#define PQC_METRICS_GROUP_CT      3
#define PQC_METRICS_GROUP_CRYPTO  4
#define PQC_METRICS_GROUP_FRAG    5
#define PQC_METRICS_GROUP_TEMP    6
#define PQC_METRICS_GROUP_RES     7
#define PQC_METRICS_GROUP_COUNT   7

typedef struct {
  uint32_t attempts;
  uint32_t success;
  uint32_t failure;

  uint32_t getpk_tx;
  uint32_t getpk_rx;
  uint32_t pk_tx;
  uint32_t pk_rx_complete;
  uint32_t ct_tx;
  uint32_t ct_rx_complete;
  uint32_t ack_tx;
  uint32_t ack_rx;

  uint32_t handshake_started_at;
  uint32_t pk_tx_started_at;
  uint32_t pk_rx_completed_at;
  uint32_t ct_tx_started_at;
  uint32_t ct_rx_started_at;
  uint32_t ct_rx_completed_at;
  uint32_t ack_rx_at;

  uint32_t pk_tx_ticks_last;
  uint32_t pk_rx_ticks_last;
  uint32_t ct_tx_ticks_last;
  uint32_t ct_rx_ticks_last;
  uint32_t ack_wait_ticks_last;
  uint32_t keypair_ticks_last;
  uint32_t encaps_ticks_last;
  uint32_t decaps_ticks_last;
  uint32_t handshake_ticks_last;
  uint32_t handshake_ticks_max;
} pqc_metrics_handshake_t;

typedef struct {
  uint32_t fragments_tx;
  uint32_t fragments_rx;
  uint32_t fragments_duplicate;
  uint32_t fragments_rejected;

  uint32_t nack_tx;
  uint32_t nack_rx;
  uint32_t nack_rounds;
  uint32_t nack_recovered_fragments;
} pqc_metrics_fragmentation_t;

typedef struct {
  uint32_t temp_tx;
  uint32_t temp_tx_fail;
  uint32_t temp_rx;
  uint32_t temp_rx_ok;
  uint32_t temp_rx_fail;
  uint32_t temp_counter_resync;
} pqc_metrics_temp_t;

typedef struct {
  uint32_t tx_queue_high_water;
  uint32_t tx_queue_full;
  uint32_t rx_slot_alloc;
  uint32_t rx_slot_full;
  uint32_t rx_slot_timeout;
  uint32_t retries;
} pqc_metrics_resource_t;

typedef struct {
  uint8_t version;
  uint16_t node_id;
  uint32_t seq;
  uint32_t uptime_ticks;

  pqc_metrics_handshake_t handshake;
  pqc_metrics_fragmentation_t fragmentation;
  pqc_metrics_temp_t temp;
  pqc_metrics_resource_t resource;
} pqc_metrics_snapshot_t;

typedef struct {
  uint8_t version;
  uint8_t group;
  uint32_t seq;
  uint32_t uptime_ticks;
  uint32_t value0;
  uint32_t value1;
  uint32_t value2;
  uint32_t value3;
} pqc_metrics_report_t;

#endif /* METRICS_SCHEMA_H */
