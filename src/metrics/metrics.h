#ifndef METRICS_H
#define METRICS_H

#include "contiki.h"
#include "metrics_schema.h"

void pqc_metrics_init(pqc_metrics_snapshot_t *metrics, uint16_t node_id);
void pqc_metrics_set_node_id(pqc_metrics_snapshot_t *metrics, uint16_t node_id);

void pqc_metrics_handshake_start(pqc_metrics_snapshot_t *metrics);
void pqc_metrics_handshake_success(pqc_metrics_snapshot_t *metrics);
void pqc_metrics_handshake_failure(pqc_metrics_snapshot_t *metrics);

uint32_t pqc_metrics_elapsed_since(uint32_t started_at);

const char *pqc_metrics_group_name(uint8_t group);
int pqc_metrics_build_report(pqc_metrics_snapshot_t *metrics,
                             uint8_t group,
                             pqc_metrics_report_t *report);
uint8_t pqc_metrics_encode_report(const pqc_metrics_report_t *report,
                                  uint8_t *buf,
                                  uint8_t len);
int pqc_metrics_decode_report(const uint8_t *buf,
                              uint8_t len,
                              pqc_metrics_report_t *report);

#endif /* METRICS_H */
