#include "fragmentation.h"

#include <string.h>

void fragment_rx_init(fragment_rx_ctx_t *ctx)
{
  if(ctx != NULL) {
    memset(ctx, 0, sizeof(*ctx));
  }
}

void fragment_rx_reset(fragment_rx_ctx_t *ctx)
{
  fragment_rx_init(ctx);
}

int fragment_rx_add(fragment_rx_ctx_t *ctx,
                    uint8_t msg_type,
                    uint16_t node_id,
                    uint8_t msg_id,
                    uint8_t frag_idx,
                    uint8_t total_frags,
                    const uint8_t *data,
                    uint16_t len)
{
  uint16_t offset;

  if(ctx == NULL || data == NULL || total_frags == 0 ||
     frag_idx >= total_frags || total_frags > FRAG_MAX_FRAGS ||
     len > FRAG_MAX_PAYLOAD_LEN) {
    return 0;
  }

  offset = (uint16_t)frag_idx * FRAG_MAX_PAYLOAD_LEN;
  if((uint32_t)offset + len > sizeof(ctx->buffer)) {
    return 0;
  }

  if(!ctx->active) {
    ctx->active = 1;
    ctx->msg_type = msg_type;
    ctx->node_id = node_id;
    ctx->msg_id = msg_id;
    ctx->total_frags = total_frags;
  } else if(ctx->msg_type != msg_type ||
            ctx->node_id != node_id ||
            ctx->msg_id != msg_id ||
            ctx->total_frags != total_frags) {
    return 0;
  }

  if(!ctx->bitmap[frag_idx]) {
    memcpy(&ctx->buffer[offset], data, len);
    ctx->bitmap[frag_idx] = 1;
    ctx->received_frags++;
    ctx->received_len += len;
  }

  return 1;
}

bool fragment_rx_complete(const fragment_rx_ctx_t *ctx)
{
  return ctx != NULL && ctx->active && ctx->received_frags == ctx->total_frags;
}

const uint8_t *fragment_rx_data(const fragment_rx_ctx_t *ctx)
{
  return fragment_rx_complete(ctx) ? ctx->buffer : NULL;
}

uint16_t fragment_rx_len(const fragment_rx_ctx_t *ctx)
{
  return fragment_rx_complete(ctx) ? ctx->received_len : 0;
}

uint8_t fragment_tx_count(uint16_t len)
{
  uint16_t total_frags;

  if(len == 0) {
    return 0;
  }

  total_frags = (len + FRAG_MAX_PAYLOAD_LEN - 1) / FRAG_MAX_PAYLOAD_LEN;
  if(total_frags == 0 || total_frags > FRAG_MAX_FRAGS) {
    return 0;
  }

  return (uint8_t)total_frags;
}

int fragment_tx(const uint8_t *data,
                uint16_t len,
                uint8_t msg_type,
                uint16_t node_id,
                uint8_t msg_id,
                uint8_t flags,
                fragment_send_fn_t send_fn,
                void *user)
{
  uint8_t frag[FRAG_HEADER_LEN + FRAG_MAX_PAYLOAD_LEN];
  uint8_t total_frags;
  uint8_t frag_idx;

  if(data == NULL || len == 0 || send_fn == NULL) {
    return 0;
  }

  total_frags = fragment_tx_count(len);
  if(total_frags == 0) {
    return 0;
  }

  for(frag_idx = 0; frag_idx < total_frags; frag_idx++) {
    uint16_t offset = (uint16_t)frag_idx * FRAG_MAX_PAYLOAD_LEN;
    uint16_t remaining = len - offset;
    uint8_t frag_len = remaining > FRAG_MAX_PAYLOAD_LEN ? FRAG_MAX_PAYLOAD_LEN : remaining;

    pqc_header_write(frag,
                     msg_type,
                     node_id,
                     msg_id,
                     frag_idx,
                     total_frags,
                     frag_len,
                     flags);

    memcpy(&frag[FRAG_HEADER_LEN], &data[offset], frag_len);

    if(!send_fn(frag, FRAG_HEADER_LEN + frag_len, user)) {
      return 0;
    }
  }

  return 1;
}
