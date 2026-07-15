#ifndef PROTOCOL_H
#define PROTOCOL_H
#include "contiki.h"
#include <stdio.h>
#include <string.h>
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uiplib.h"
#include "net/linkaddr.h"
#include "dev/serial-line.h"
#include "sys/log.h"
#include "lib/random.h"

#define PQC_PROTO_VERSION 1

#define PQC_MSG_PK        1
#define PQC_MSG_CT        2
#define PQC_MSG_ACK       3
#define PQC_MSG_TEMP      4
#define PQC_MSG_METRICS   5
#define PQC_MSG_GETPK     6
#define PQC_MSG_NACK      7

#define PQC_FLAG_NONE     0x00
#define PQC_FLAG_RETRY    0x01
#define PQC_FLAG_LAST     0x02

#define PQC_HEADER_LEN    10

#define PQC_HDR_VERSION      0
#define PQC_HDR_MSG_TYPE     1
#define PQC_HDR_NODE_ID_H    2
#define PQC_HDR_NODE_ID_L    3
#define PQC_HDR_MSG_ID       4
#define PQC_HDR_FRAG_IDX     5
#define PQC_HDR_TOTAL_FRAGS  6
#define PQC_HDR_PAYLOAD_LEN  7
#define PQC_HDR_FLAGS        8
#define PQC_HDR_RESERVED     9

#define MEASUREMENTS_INTERVAL (7 * CLOCK_SECOND)
#define SEND_INTERVAL (CLOCK_SECOND / 4)

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

extern struct etimer measurement_timer, send_timer, routing_timer, serial_timer;

typedef struct {
  uint32_t counter;
  int32_t rh_milli;
  int32_t temp_milli;
} pqc_temp_payload_t;

static inline uint16_t
pqc_node_id_from_linkaddr(void)
{
#if LINKADDR_SIZE >= 2
  return ((uint16_t)linkaddr_node_addr.u8[LINKADDR_SIZE - 2] << 8) |
          linkaddr_node_addr.u8[LINKADDR_SIZE - 1];
#else
  return linkaddr_node_addr.u8[0];
#endif
}

static inline int
pqc_header_valid(const uint8_t *buf, uint16_t len)
{
  return buf != NULL &&
         len >= PQC_HEADER_LEN &&
         buf[PQC_HDR_VERSION] == PQC_PROTO_VERSION &&
         len >= (uint16_t)(PQC_HEADER_LEN + buf[PQC_HDR_PAYLOAD_LEN]);
}

static inline uint16_t
pqc_header_get_node_id(const uint8_t *buf)
{
  return ((uint16_t)buf[PQC_HDR_NODE_ID_H] << 8) | buf[PQC_HDR_NODE_ID_L];
}

static inline void
pqc_header_set_node_id(uint8_t *buf, uint16_t node_id)
{
  buf[PQC_HDR_NODE_ID_H] = (uint8_t)(node_id >> 8);
  buf[PQC_HDR_NODE_ID_L] = (uint8_t)node_id;
}

static inline void
pqc_header_write(uint8_t *buf,
                 uint8_t msg_type,
                 uint16_t node_id,
                 uint8_t msg_id,
                 uint8_t frag_idx,
                 uint8_t total_frags,
                 uint8_t payload_len,
                 uint8_t flags)
{
  buf[PQC_HDR_VERSION] = PQC_PROTO_VERSION;
  buf[PQC_HDR_MSG_TYPE] = msg_type;
  pqc_header_set_node_id(buf, node_id);
  buf[PQC_HDR_MSG_ID] = msg_id;
  buf[PQC_HDR_FRAG_IDX] = frag_idx;
  buf[PQC_HDR_TOTAL_FRAGS] = total_frags;
  buf[PQC_HDR_PAYLOAD_LEN] = payload_len;
  buf[PQC_HDR_FLAGS] = flags;
  buf[PQC_HDR_RESERVED] = 0;
}

#endif /* PROTOCOL_H */
