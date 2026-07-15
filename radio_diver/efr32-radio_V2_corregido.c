/*
 *  Copyright (c) 2017, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
/*
 *  Copyright (c) 2018, RISE SICS
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file
 *   This file implements the OpenThread platform abstraction for radio communication.
 *
 * Adapted to Contiki-NG, Joakim Eriksson
 */

#include "contiki.h"
#include "sys/clock.h"
#include "dev/radio.h"
#include "dev/watchdog.h"
#include "net/packetbuf.h"
#include "net/netstack.h"
#include "em_core.h"
#include "em_system.h"
#include "pa_conversions_efr32.h"
#include "rail.h"
#include <ieee802154/rail_ieee802154.h>
#include <string.h>
#include <stdio.h>

#define DEBUG 0
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif


enum
{
    IEEE802154_MIN_LENGTH      = 5,
    IEEE802154_MAX_LENGTH      = 127,
    IEEE802154_ACK_LENGTH      = 5,
    IEEE802154_FRAME_TYPE_MASK = 0x7,
    IEEE802154_FRAME_TYPE_ACK  = 0x2,
    IEEE802154_FRAME_PENDING   = 1 << 4,
    IEEE802154_ACK_REQUEST     = 1 << 5,
    IEEE802154_DSN_OFFSET      = 2,
};

#define CHANNEL_MIN            11
#define CHANNEL_MAX            26

/* Quick anti-lockup timeouts for blocking TX waits */
#define TX_TIMEOUT_TICKS      CLOCK_SECOND
#define AUTOACK_WAIT_US       1000U
#define AUTOACK_TIMEOUT_US    5000U

typedef enum
{
  TX_IDLE,
  TX_SENDING,
  TX_SENT,
  TX_NO_ACK,
  TX_CHANNEL_BUSY,
  TX_ERROR
} tx_status_t;

volatile tx_status_t tx_status;
uint64_t last_rx_time = 0;

typedef struct
{
  int16_t rssi;
  int16_t lqi;
  uint8_t len;
  uint8_t buf[128];
} rx_buffer_t;


/* Allocate 4 rx buffers for bursty receives */
#define RX_BUF_COUNT 4
rx_buffer_t rx_buf[RX_BUF_COUNT];
volatile int next_read = 0;
volatile int next_write = 0;
volatile uint32_t radio_stage = 0;

int
has_packet(void)
{
  int result;
  CORE_DECLARE_IRQ_STATE;

  CORE_ENTER_CRITICAL();
  result = rx_buf[next_read].len > 0;
  CORE_EXIT_CRITICAL();

  return result;
}

rx_buffer_t *
get_full_rx_buf(void)
{
  int nr;
  rx_buffer_t *result = NULL;
  CORE_DECLARE_IRQ_STATE;

  CORE_ENTER_CRITICAL();

  if(rx_buf[nr = next_read].len > 0) {
    /* return buffert and intrease last_read as it was full. */
    /* Write will have to check if it can allocate new buffers when
       increating the write pos */
    next_read = (next_read + 1) % RX_BUF_COUNT;
    result = &rx_buf[nr];
  }

  CORE_EXIT_CRITICAL();

  return result;
}

rx_buffer_t *
get_empty_rx_buf(void)
{
  int nw;
  rx_buffer_t *result = NULL;
  CORE_DECLARE_IRQ_STATE;

  CORE_ENTER_CRITICAL();

  /* if the next write buf is empty is should be ok to write to it */
  if(rx_buf[nw = next_write].len == 0) {
    next_write = (next_write + 1) % RX_BUF_COUNT;
    result = &rx_buf[nw];
  }

  CORE_EXIT_CRITICAL();

  return result;
}

void
free_rx_buf(rx_buffer_t *rx_buf)
{
  CORE_DECLARE_IRQ_STATE;

  CORE_ENTER_CRITICAL();
  /* set len to zero */
  rx_buf->len = 0;
  CORE_EXIT_CRITICAL();
}

enum
{
    EFR32_RECEIVE_SENSITIVITY = -100, // dBm
};

/* The process for receiving packets */
PROCESS(radio_proc, "efr32 radio driver");

static void RAILCb_Generic(RAIL_Handle_t aRailHandle, RAIL_Events_t aEvents);

static uint8_t sRailTxFifo[1 + IEEE802154_MAX_LENGTH];

typedef struct RAILStateDummy_t {
  uint32_t a;
  uint32_t b;
  uint16_t c;
  bool d;
} RAILState_Dummy_t;

static RAILState_Dummy_t railState;
static RAILSched_Config_t railSched;


static RAIL_Config_t sRailConfig = {
    .eventsCallback = &RAILCb_Generic,
    .protocol = (void *)&railState,
    .scheduler = &railSched
};

static const RAIL_IEEE802154_Config_t sRailIeee802154Config = {
    NULL, // addresses
    {
        // ackConfig
        true, // ackConfig.enable
        894,  // ackConfig.ackTimeout
        {
            // ackConfig.rxTransitions
            RAIL_RF_STATE_RX, // ackConfig.rxTransitions.success
            RAIL_RF_STATE_RX, // ackConfig.rxTransitions.error
        },
        {
            // ackConfig.txTransitions
            RAIL_RF_STATE_RX, // ackConfig.txTransitions.success
            RAIL_RF_STATE_RX, // ackConfig.txTransitions.error
        },
    },
    {
        // timings
        100,      // timings.idleToRx
        192 - 10, // timings.txToRx
        100,      // timings.idleToTx
        192,      // timings.rxToTx
        0,        // timings.rxSearchTimeout
        0,        // timings.txToRxSearchTimeout
    },
    RAIL_IEEE802154_ACCEPT_STANDARD_FRAMES, // framesMask
    false,                                  // promiscuousMode
    false,                                  // isPanCoordinator
};

static RAIL_Handle_t sRailHandle = NULL;

RAIL_DECLARE_TX_POWER_VBAT_CURVES(piecewiseSegments, curvesSg, curves24Hp, curves24Lp);


static uint16_t panid = 0xabcd;
static int channel = 26;
static int cca_threshold = -85;
static RAIL_TxOptions_t  txOptions = RAIL_TX_OPTIONS_DEFAULT;

static int
init(void)
{
  sRailHandle = RAIL_Init(&sRailConfig, NULL);

  if(sRailHandle == NULL) {
    PRINTF("EFR32 Radio - failed init - did not get handle.\n");
    return 0;
  }

  RAIL_DataConfig_t railDataConfig = {
    TX_PACKET_DATA,
    RX_PACKET_DATA,
    PACKET_MODE,
    PACKET_MODE,
  };

  RAIL_ConfigData(sRailHandle, &railDataConfig);
  RAIL_ConfigCal(sRailHandle, RAIL_CAL_ALL);
  RAIL_IEEE802154_Config2p4GHzRadio(sRailHandle);
  RAIL_IEEE802154_Init(sRailHandle, &sRailIeee802154Config);

  RAIL_ConfigEvents(sRailHandle, RAIL_EVENTS_ALL,
                    RAIL_EVENT_RX_ACK_TIMEOUT |                  //
                    RAIL_EVENT_TX_PACKET_SENT |                  //
                    RAIL_EVENT_RX_PACKET_RECEIVED |              //
                    RAIL_EVENT_TX_CHANNEL_BUSY |                 //
                    RAIL_EVENT_TX_ABORTED |                      //
                    RAIL_EVENT_TX_BLOCKED |                      //
                    RAIL_EVENT_TX_UNDERFLOW |                    //
                    RAIL_EVENT_IEEE802154_DATA_REQUEST_COMMAND | //
                    RAIL_EVENT_CAL_NEEDED                        //
                    );

  RAIL_TxPowerCurvesConfig_t txPowerCurvesConfig =
  	  {curves24Hp, curvesSg, curves24Lp, piecewiseSegments};
  RAIL_InitTxPowerCurves(&txPowerCurvesConfig);

  RAIL_TxPowerConfig_t txPowerConfig = {RAIL_TX_POWER_MODE_2P4_HP, 3300, 10};
  RAIL_ConfigTxPower(sRailHandle, &txPowerConfig);

  /* set tx Power in dBm */
  RAIL_SetTxPowerDbm(sRailHandle, ((RAIL_TxPower_t) -10 * 10));		// TODO: transmission power is modified here
  RAIL_SetTxFifo(sRailHandle, sRailTxFifo, 0, sizeof(sRailTxFifo));
  RAIL_StartRx(sRailHandle, channel, NULL);

  process_start(&radio_proc, NULL);
  PRINTF("EFR32 Radio initialized\n");

  return 0;
}
/*---------------------------------------------------------------------------*/
static void
move_to_rx_buffer(RAIL_Handle_t handle)
{
  RAIL_RxPacketHandle_t packet_handle;
  RAIL_RxPacketInfo_t packet_info;
  RAIL_RxPacketDetails_t packet_details;
  RAIL_Status_t status;
  rx_buffer_t *dest;
  uint16_t raw_len;
  uint16_t payload_len;

  radio_stage = 0x12;

  packet_handle = RAIL_GetRxPacketInfo(
    handle,
    RAIL_RX_PACKET_HANDLE_OLDEST,
    &packet_info
  );

  if(packet_handle == RAIL_RX_PACKET_HANDLE_INVALID) {
    radio_stage = 0xE1;
    return;
  }

  radio_stage = 0x13;

  last_rx_time = RAIL_GetTime();


  if(packet_info.packetStatus != RAIL_RX_PACKET_READY_SUCCESS) {
    radio_stage = 0xE2;
    goto release_packet;
  }

  raw_len = packet_info.packetBytes;

  if(raw_len < 1 || raw_len > sizeof(rx_buf[0].buf)) {
    radio_stage = 0xE3;
    goto release_packet;
  }

  payload_len = raw_len - 1;


  if(payload_len > sizeof(rx_buf[0].buf)) {
    radio_stage = 0xE7;
    goto release_packet;
  }

  memset(&packet_details, 0, sizeof(packet_details));

  packet_details.timeReceived.timePosition =
    RAIL_PACKET_TIME_INVALID;

  packet_details.timeReceived.totalPacketBytes = 0;

  status = RAIL_GetRxPacketDetails(
    handle,
    packet_handle,
    &packet_details
  );

  if(status != RAIL_STATUS_NO_ERROR) {
    radio_stage = 0xE4;
    goto release_packet;
  }

  radio_stage = 0x14;


  if(!packet_details.crcPassed) {
    radio_stage = 0xE5;
    goto release_packet;
  }

  dest = get_empty_rx_buf();

  if(dest == NULL) {

    radio_stage = 0xE6;
    goto release_packet;
  }

  radio_stage = 0x15;


  RAIL_CopyRxPacket(dest->buf, &packet_info);


  if(payload_len > 0) {
    memmove(
      dest->buf,
      dest->buf + 1,
      payload_len
    );
  }

  radio_stage = 0x16;

  dest->rssi = packet_details.rssi;
  dest->lqi = packet_details.lqi;


  dest->len = (uint8_t)payload_len;

release_packet:


  RAIL_ReleaseRxPacket(handle, packet_handle);


  if(radio_stage < 0xE0) {
    radio_stage = 0x17;
  }
}

static void
RAILCb_Generic(RAIL_Handle_t aRailHandle, RAIL_Events_t aEvents)
{

  if(aEvents & (RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW)) {
    tx_status = TX_ERROR;
    PRINTF("Error: TX aborted o TX blocked o TX underflow\n"); 		// OJO
  }

  if(aEvents & RAIL_EVENT_RX_ACK_TIMEOUT) {
    tx_status = TX_NO_ACK;
    PRINTF("Error: TX no ack\n"); 		// OJO
  }

  if(aEvents & RAIL_EVENT_RX_PACKET_RECEIVED) {
    PRINTF("EFR32 Radio: Receive event - poll.\n");
    move_to_rx_buffer(aRailHandle);
    process_poll(&radio_proc);
  }

  if(aEvents & RAIL_EVENT_TX_PACKET_SENT) {
    PRINTF("EFR32 Radio: packet sent\n");
    tx_status = TX_SENT;
  }

  if(aEvents & RAIL_EVENT_TX_CHANNEL_BUSY) {
    tx_status = TX_CHANNEL_BUSY;
    PRINTF("Channel is busy\n"); 		// OJO
  }

  if(aEvents & RAIL_EVENT_CAL_NEEDED) {
    RAIL_Calibrate(aRailHandle, NULL, RAIL_CAL_ALL_PENDING);
    PRINTF("Un evento nos pide calibrar\n"); 		// OJO

  }
}

/*---------------------------------------------------------------------------*/
static radio_result_t
get_value(radio_param_t param, radio_value_t *value)
{
  if(!value) {
    return RADIO_RESULT_INVALID_VALUE;
  }

  switch(param) {
  case RADIO_PARAM_PAN_ID:
    *value = panid;
    return RADIO_RESULT_OK;
  case RADIO_PARAM_CHANNEL:
    *value = channel;
    return RADIO_RESULT_OK;
  case RADIO_PARAM_RSSI:
    {
      int16_t rssi_value = RAIL_GetRssi(sRailHandle, true);
      if(rssi_value != RAIL_RSSI_INVALID) {
        /* RAIL_RxGetRSSI() returns value in quarter dBm (dBm * 4) */
        *value = rssi_value / 4;
        return RADIO_RESULT_OK;
      }
      return RADIO_RESULT_ERROR;
    }
  }
  return RADIO_RESULT_NOT_SUPPORTED;
}
/*---------------------------------------------------------------------------*/
static radio_result_t
set_value(radio_param_t param, radio_value_t value)
{
  switch(param) {
  case RADIO_PARAM_CHANNEL:
    if(value < CHANNEL_MIN ||
       value > CHANNEL_MAX) {
      return RADIO_RESULT_INVALID_VALUE;
    }
    channel = value;
    /* start receiving on that channel */
    RAIL_StartRx(sRailHandle, channel, NULL);
    return RADIO_RESULT_OK;
  case RADIO_PARAM_PAN_ID:
    panid = value & 0xffff;
    RAIL_IEEE802154_SetPanId(sRailHandle, panid, 0);
    return RADIO_RESULT_OK;
  }
  return RADIO_RESULT_NOT_SUPPORTED;
}
/*---------------------------------------------------------------------------*/
static radio_result_t
get_object(radio_param_t param, void *dest, size_t size)
{
  return RADIO_RESULT_NOT_SUPPORTED;
}
/*---------------------------------------------------------------------------*/
static radio_result_t
set_object(radio_param_t param, const void *src, size_t size)
{
  if(param == RADIO_PARAM_64BIT_ADDR) {
    uint8_t longAddr[8];
    int i;
    if(size != 8 || src == NULL) {
      return RADIO_RESULT_INVALID_VALUE;
    }
    for(i = 0; i < 8; i++) {
      longAddr[i] = ((uint8_t *)src)[7 - i];
    }
    RAIL_IEEE802154_SetLongAddress(sRailHandle, longAddr, 0);
    return RADIO_RESULT_OK;
  }
  return RADIO_RESULT_NOT_SUPPORTED;
}
/*---------------------------------------------------------------------------*/
static int
channel_clear(void)
{
  if(RAIL_GetRssi(sRailHandle, true) / 4 > cca_threshold) {
    return 0;
  }
  return 1;
}
/*---------------------------------------------------------------------------*/
static int
prepare(const void *payload, unsigned short payload_len)
{
  uint8_t plen;
  const uint8_t *data = (const uint8_t *)payload;
  int written = 0;

  if(payload == NULL || payload_len == 0) {
    return RADIO_TX_ERR;
  }

  if(payload_len > IEEE802154_MAX_LENGTH - 2) {
    return RADIO_TX_ERR;
  }

  plen = (uint8_t)(payload_len + 2);

  /* write the length byte */
  written += RAIL_WriteTxFifo(sRailHandle, &plen, 1, true);
  /* write the payload */
  written += RAIL_WriteTxFifo(sRailHandle, payload, payload_len, false);

  PRINTF("EFR32 Radio - wrote %d bytes to TX fifo\n", written);

  if(written != (int)payload_len + 1) {
    return RADIO_TX_ERR;
  }

  if(data[0] & IEEE802154_ACK_REQUEST) {
    txOptions = RAIL_TX_OPTION_WAIT_FOR_ACK;
  } else {
    txOptions = RAIL_TX_OPTIONS_DEFAULT;
  }

  return RADIO_TX_OK;
}
/*---------------------------------------------------------------------------*/
static int
transmit(unsigned short transmit_len)
{
  static RAIL_SchedulerInfo_t schedInfo = {
    .priority = 10,
    .slipTime = 20,
    .transactionTime = 20
  };

  RAIL_CsmaConfig_t csmaConfig =
    RAIL_CSMA_CONFIG_802_15_4_2003_2p4_GHz_OQPSK_CSMA;
  RAIL_Status_t     status;
  int ret = RADIO_TX_OK;

  /*
   * Safety cleanup: if a previous TX left a terminal status behind,
   * allow the next transmission to start. TX_SENDING is the only state
   * that must be treated as a real busy condition.
   */
  if(tx_status == TX_SENT ||
     tx_status == TX_NO_ACK ||
     tx_status == TX_CHANNEL_BUSY ||
     tx_status == TX_ERROR) {
    PRINTF("EFR32 Radio: forcing terminal tx_status %u to TX_IDLE\n", tx_status);
    tx_status = TX_IDLE;
  }

  if(tx_status != TX_IDLE) {
    PRINTF("EFR32 Radio: transmit called while busy, status=%u\n", tx_status);
    return RADIO_TX_ERR;
  }

  /*
   * Avoid being in anything else than IDLE when reading out packet.
   * This was a pure busy-wait; keep it bounded to avoid watchdog resets.
   */
  {
    uint32_t ack_wait_start = RAIL_GetTime();

    while(((uint32_t)(RAIL_GetTime() - last_rx_time)) < AUTOACK_WAIT_US) {
      watchdog_periodic();

      if(((uint32_t)(RAIL_GetTime() - ack_wait_start)) > AUTOACK_TIMEOUT_US) {
        PRINTF("EFR32 Radio: AutoACK wait timeout\n");
        break;
      }
    }
  }

  tx_status = TX_SENDING;
  PRINTF("EFR32 Radio: Sending packet %d bytes\n", transmit_len);

  status = RAIL_StartCcaCsmaTx(sRailHandle, channel, txOptions, &csmaConfig, &schedInfo);
  if(status != RAIL_STATUS_NO_ERROR) {
    PRINTF("EFR32 Radio: Failed to send\n");
    tx_status = TX_IDLE;
    return RADIO_TX_ERR;
  }

  /*
   * Wait for the transmission, but never forever.
   * If RAIL does not deliver a terminal TX event, abort and return error.
   */
  {
    clock_time_t tx_start = clock_time();

    while(tx_status == TX_SENDING) {
      watchdog_periodic();

      if((clock_time_t)(clock_time() - tx_start) > TX_TIMEOUT_TICKS) {
        PRINTF("EFR32 Radio: TX timeout\n");

        RAIL_Idle(sRailHandle, RAIL_IDLE_ABORT, true);

        tx_status = TX_IDLE;
        return RADIO_TX_ERR;
      }
    }
  }

  if(tx_status == TX_SENT) {
    /* OK! */
    PRINTF("EFR32: OK - packet sent.\n");
    ret = RADIO_TX_OK;
  } else if(tx_status == TX_NO_ACK) {
    PRINTF("EFR32: TX no ack\n");
    ret = RADIO_TX_NOACK;
  } else if(tx_status == TX_CHANNEL_BUSY) {
    PRINTF("EFR32: TX collision/channel busy\n");
    ret = RADIO_TX_COLLISION;
  } else {
    PRINTF("EFR32: TX error %d\n", tx_status);
    ret = RADIO_TX_ERR;
  }

  /* always IDLE after transmission */
  tx_status = TX_IDLE;

  return ret;
}
/*---------------------------------------------------------------------------*/
static int
send(const void *payload, unsigned short payload_len)
{
  int ret;
  ret = prepare(payload, payload_len);
  if(ret) {
    return ret;
  }
  return transmit(payload_len);
}
/*---------------------------------------------------------------------------*/
static int
on(void)
{
  return RAIL_StartRx(sRailHandle, channel, NULL) == RAIL_STATUS_NO_ERROR;
}
/*---------------------------------------------------------------------------*/
static int
off(void)
{
  RAIL_Idle(sRailHandle, RAIL_IDLE, true);
  return 1;
}
/*---------------------------------------------------------------------------*/
static int
receiving_packet(void)
{
  /* how do we check this? */
  return 0;
}
/*---------------------------------------------------------------------------*/
static int
read(void *buf, unsigned short bufsize)
{
  rx_buffer_t *rx_buf;
  int len;

  if(buf == NULL || bufsize == 0) {
    return 0;
  }

  rx_buf = get_full_rx_buf();
  if(rx_buf == NULL) {
    return 0;
  }
  PRINTF("EFR32 Radio Read: %d\n", rx_buf->len);
  len = rx_buf->len;

  if(len > bufsize) {
    free_rx_buf(rx_buf);
    return 0;
  }

  /* copy packet */
  memcpy(buf, rx_buf->buf, len);
  packetbuf_set_attr(PACKETBUF_ATTR_RSSI, rx_buf->rssi);
  packetbuf_set_attr(PACKETBUF_ATTR_LINK_QUALITY, rx_buf->lqi);
  free_rx_buf(rx_buf);
  return len;
}
/*---------------------------------------------------------------------------*/
static int
pending_packet(void)
{
  return has_packet();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(radio_proc, ev, data)
{
  static struct etimer periodic;
  int len;

  PROCESS_BEGIN();

  etimer_set(&periodic, CLOCK_SECOND);
  while(1) {
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL || etimer_expired(&periodic)
                        || has_packet());

    if(etimer_expired(&periodic)) {
      etimer_reset(&periodic);
    }

    if(has_packet()) {
      packetbuf_clear();
      len = read(packetbuf_dataptr(), PACKETBUF_SIZE);

      if(len > 0) {
        packetbuf_set_datalen(len);
        NETSTACK_MAC.input();
        /* poll again to check if there is more to read out */
        process_poll(&radio_proc);
      }
    }
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
const struct radio_driver efr32_radio_driver = {
  init,
  prepare,
  transmit,
  send,
  read,
  channel_clear,
  receiving_packet,
  pending_packet,
  on,
  off,
  get_value,
  set_value,
  get_object,
  set_object
};
/*---------------------------------------------------------------------------*/
