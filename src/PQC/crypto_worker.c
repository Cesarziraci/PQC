#include "crypto_worker.h"

#include <stdio.h>
#include <string.h>
#include "fips202.h"

int crypto_worker_keypair(uint8_t *pk, uint8_t *sk)
{
  int rc;

  if(pk == NULL || sk == NULL) {
    printf("[PQC] crypto_worker_keypair: invalid args\r\n");
    return 0;
  }

  printf("[PQC] crypto_worker_keypair: begin\r\n");
  rc = crypto_kem_keypair(pk, sk);
  printf("[PQC] crypto_worker_keypair: done rc=%d\r\n", rc);

  return rc == 0;
}

int crypto_worker_encapsulate(const uint8_t *pk, uint8_t *ct, uint8_t *ss)
{
  if(pk == NULL || ct == NULL || ss == NULL) {
    return 0;
  }

  return crypto_kem_enc(ct, ss, pk) == 0;
}

int crypto_worker_decapsulate(const uint8_t *sk, const uint8_t *ct, uint8_t *ss)
{
  if(sk == NULL || ct == NULL || ss == NULL) {
    return 0;
  }

  return crypto_kem_dec(ss, ct, sk) == 0;
}

void crypto_worker_derive_aes_key(const uint8_t *ss, uint8_t *aes_key)
{
  if(ss == NULL || aes_key == NULL) {
    return;
  }

  shake256(aes_key, CW_AES_KEY_LEN, ss, CW_HQC_SHARED_LEN);
}

void crypto_worker_derive_aes_iv(const uint8_t *ss, uint32_t counter, uint8_t *iv)
{
  uint8_t input[CW_HQC_SHARED_LEN + sizeof(counter)];

  if(ss == NULL || iv == NULL) {
    return;
  }

  memcpy(input, ss, CW_HQC_SHARED_LEN);
  input[CW_HQC_SHARED_LEN + 0] = (uint8_t)(counter >> 24);
  input[CW_HQC_SHARED_LEN + 1] = (uint8_t)(counter >> 16);
  input[CW_HQC_SHARED_LEN + 2] = (uint8_t)(counter >> 8);
  input[CW_HQC_SHARED_LEN + 3] = (uint8_t)counter;

  shake256(iv, CW_AES_IV_LEN, input, sizeof(input));
  crypto_worker_secure_zero(input, sizeof(input));
}

void crypto_worker_aes_ctr_xcrypt(const uint8_t *key,
                                  const uint8_t *iv,
                                  uint8_t *buf,
                                  size_t len)
{
  struct AES_ctx ctx;

  if(key == NULL || iv == NULL || buf == NULL || len == 0) {
    return;
  }

  AES_init_ctx_iv(&ctx, key, iv);
  AES_CTR_xcrypt_buffer(&ctx, buf, len);
  crypto_worker_secure_zero(&ctx, sizeof(ctx));
}

void crypto_worker_secure_zero(void *ptr, size_t len)
{
  volatile uint8_t *p = (volatile uint8_t *)ptr;

  if(p == NULL) {
    return;
  }

  while(len--) {
    *p++ = 0;
  }
}