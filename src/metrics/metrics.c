#include "metrics.h"

#include <string.h>

static void write_u32(uint8_t *buf, uint32_t value)
{
  buf[0] = (uint8_t)(value >> 24);
  buf[1] = (uint8_t)(value >> 16);
  buf[2] = (uint8_t)(value >> 8);
  buf[3] = (uint8_t)value;
}

static uint32_t read_u32(const uint8_t *buf)
{
  return ((uint32_t)buf[0] << 24) |
         ((uint32_t)buf[1] << 16) |
         ((uint32_t)buf[2] << 8) |
         (uint32_t)buf[3];
}

void pqc_metrics_init(pqc_metrics_snapshot_t *metrics, uint16_t node_id)
{
  if(metrics == NULL) {
    return;
  }

  memset(metrics, 0, sizeof(*metrics));
  metrics->version = PQC_METRICS_SCHEMA_VERSION;
  metrics->node_id = node_id;
}

void pqc_metrics_set_node_id(pqc_metrics_snapshot_t *metrics, uint16_t node_id)
{
  if(metrics == NULL) {
    return;
  }

  metrics->version = PQC_METRICS_SCHEMA_VERSION;
  metrics->node_id = node_id;
}

uint32_t pqc_metrics_elapsed_since(uint32_t started_at)
{
  return (uint32_t)(clock_time() - (clock_time_t)started_at);
}

void pqc_metrics_handshake_start(pqc_metrics_snapshot_t *metrics)
{
  if(metrics == NULL) {
    return;
  }

  metrics->handshake.attempts++;
  metrics->handshake.handshake_started_at = (uint32_t)clock_time();
  metrics->handshake.pk_tx_started_at = 0;
  metrics->handshake.pk_rx_completed_at = 0;
  metrics->handshake.ct_tx_started_at = 0;
  metrics->handshake.ct_rx_started_at = 0;
  metrics->handshake.ct_rx_completed_at = 0;
  metrics->handshake.ack_rx_at = 0;
  metrics->handshake.pk_tx_ticks_last = 0;
  metrics->handshake.pk_rx_ticks_last = 0;
  metrics->handshake.ct_tx_ticks_last = 0;
  metrics->handshake.ct_rx_ticks_last = 0;
  metrics->handshake.ack_wait_ticks_last = 0;
  metrics->handshake.keypair_ticks_last = 0;
  metrics->handshake.encaps_ticks_last = 0;
  metrics->handshake.decaps_ticks_last = 0;
  metrics->handshake.handshake_ticks_last = 0;
}

void pqc_metrics_handshake_success(pqc_metrics_snapshot_t *metrics)
{
  uint32_t elapsed;

  if(metrics == NULL) {
    return;
  }

  metrics->handshake.success++;

  if(metrics->handshake.handshake_started_at == 0) {
    return;
  }

  elapsed = pqc_metrics_elapsed_since(metrics->handshake.handshake_started_at);
  metrics->handshake.handshake_ticks_last = elapsed;
  if(elapsed > metrics->handshake.handshake_ticks_max) {
    metrics->handshake.handshake_ticks_max = elapsed;
  }
}

const char *pqc_metrics_group_name(uint8_t group)
{
  switch(group) {
  case PQC_METRICS_GROUP_HS:
    return "NODE_HS";
  case PQC_METRICS_GROUP_PK:
    return "NODE_PK";
  case PQC_METRICS_GROUP_CT:
    return "NODE_CT";
  case PQC_METRICS_GROUP_CRYPTO:
    return "NODE_CRYPTO";
  case PQC_METRICS_GROUP_FRAG:
    return "NODE_FRAG";
  case PQC_METRICS_GROUP_TEMP:
    return "NODE_TEMP";
  case PQC_METRICS_GROUP_RES:
    return "NODE_RES";
  default:
    return "NODE_UNKNOWN";
  }
}

int pqc_metrics_build_report(pqc_metrics_snapshot_t *metrics,
                             uint8_t group,
                             pqc_metrics_report_t *report)
{
  if(metrics == NULL || report == NULL ||
     group == 0 || group > PQC_METRICS_GROUP_COUNT) {
    return 0;
  }

  metrics->version = PQC_METRICS_SCHEMA_VERSION;
  metrics->seq++;
  metrics->uptime_ticks = (uint32_t)clock_time();

  memset(report, 0, sizeof(*report));
  report->version = metrics->version;
  report->group = group;
  report->seq = metrics->seq;
  report->uptime_ticks = metrics->uptime_ticks;

  switch(group) {
  case PQC_METRICS_GROUP_HS:
    report->value0 = metrics->handshake.handshake_ticks_last;
    report->value1 = metrics->handshake.success;
    report->value2 = metrics->handshake.failure;
    report->value3 = metrics->handshake.attempts;
    break;
  case PQC_METRICS_GROUP_PK:
    report->value0 = metrics->handshake.pk_tx_ticks_last;
    report->value1 = metrics->handshake.pk_rx_ticks_last;
    report->value2 = metrics->handshake.pk_tx;
    report->value3 = metrics->handshake.pk_rx_complete;
    break;
  case PQC_METRICS_GROUP_CT:
    report->value0 = metrics->handshake.ct_tx_ticks_last;
    report->value1 = metrics->handshake.ct_rx_ticks_last;
    report->value2 = metrics->handshake.ct_tx;
    report->value3 = metrics->handshake.ct_rx_complete;
    break;
  case PQC_METRICS_GROUP_CRYPTO:
    report->value0 = metrics->handshake.keypair_ticks_last;
    report->value1 = metrics->handshake.encaps_ticks_last;
    report->value2 = metrics->handshake.decaps_ticks_last;
    report->value3 = metrics->handshake.ack_wait_ticks_last;
    break;
  case PQC_METRICS_GROUP_FRAG:
    report->value0 = metrics->fragmentation.fragments_tx;
    report->value1 = metrics->fragmentation.fragments_rx;
    report->value2 = metrics->fragmentation.nack_tx;
    report->value3 = metrics->fragmentation.nack_rx;
    break;
  case PQC_METRICS_GROUP_TEMP:
    report->value0 = metrics->temp.temp_tx;
    report->value1 = metrics->temp.temp_tx_fail;
    report->value2 = metrics->temp.temp_rx_ok;
    report->value3 = metrics->temp.temp_counter_resync;
    break;
  case PQC_METRICS_GROUP_RES:
    report->value0 = metrics->resource.retries;
    report->value1 = metrics->resource.tx_queue_full;
    report->value2 = metrics->resource.rx_slot_full;
    report->value3 = metrics->resource.rx_slot_timeout;
    break;
  default:
    return 0;
  }

  return 1;
}

uint8_t pqc_metrics_encode_report(const pqc_metrics_report_t *report,
                                  uint8_t *buf,
                                  uint8_t len)
{
  if(report == NULL || buf == NULL || len < PQC_METRICS_REPORT_LEN) {
    return 0;
  }

  buf[0] = report->version;
  buf[1] = report->group;
  write_u32(&buf[2], report->seq);
  write_u32(&buf[6], report->uptime_ticks);
  write_u32(&buf[10], report->value0);
  write_u32(&buf[14], report->value1);
  write_u32(&buf[18], report->value2);
  write_u32(&buf[22], report->value3);

  return PQC_METRICS_REPORT_LEN;
}

int pqc_metrics_decode_report(const uint8_t *buf,
                              uint8_t len,
                              pqc_metrics_report_t *report)
{
  if(buf == NULL || report == NULL || len != PQC_METRICS_REPORT_LEN ||
     buf[0] != PQC_METRICS_SCHEMA_VERSION ||
     buf[1] == 0 || buf[1] > PQC_METRICS_GROUP_COUNT) {
    return 0;
  }

  report->version = buf[0];
  report->group = buf[1];
  report->seq = read_u32(&buf[2]);
  report->uptime_ticks = read_u32(&buf[6]);
  report->value0 = read_u32(&buf[10]);
  report->value1 = read_u32(&buf[14]);
  report->value2 = read_u32(&buf[18]);
  report->value3 = read_u32(&buf[22]);

  return 1;
}

void pqc_metrics_handshake_failure(pqc_metrics_snapshot_t *metrics)
{
  uint32_t elapsed;

  if(metrics == NULL) {
    return;
  }

  metrics->handshake.failure++;

  if(metrics->handshake.handshake_started_at == 0) {
    return;
  }

  elapsed = pqc_metrics_elapsed_since(metrics->handshake.handshake_started_at);
  metrics->handshake.handshake_ticks_last = elapsed;
  if(elapsed > metrics->handshake.handshake_ticks_max) {
    metrics->handshake.handshake_ticks_max = elapsed;
  }
}
