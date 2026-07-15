#ifndef CRYPTO_WORKER_H
#define CRYPTO_WORKER_H

#include <stdint.h>
#include <stddef.h>
#include "api.h"
#include "aes.h"
#include <string.h>
#include "fips202.h"

#define CW_HQC_PUBLIC_KEY_LEN  CRYPTO_PUBLICKEYBYTES
#define CW_HQC_SECRET_KEY_LEN  CRYPTO_SECRETKEYBYTES
#define CW_HQC_CIPHERTEXT_LEN  CRYPTO_CIPHERTEXTBYTES
#define CW_HQC_SHARED_LEN      CRYPTO_BYTES
#define CW_AES_KEY_LEN         AES_KEYLEN
#define CW_AES_IV_LEN          AES_BLOCKLEN

int crypto_worker_keypair(uint8_t *pk, uint8_t *sk);
int crypto_worker_encapsulate(const uint8_t *pk, uint8_t *ct, uint8_t *ss);
int crypto_worker_decapsulate(const uint8_t *sk, const uint8_t *ct, uint8_t *ss);

void crypto_worker_derive_aes_key(const uint8_t *ss, uint8_t *aes_key);
void crypto_worker_derive_aes_iv(const uint8_t *ss, uint32_t counter, uint8_t *iv);

void crypto_worker_aes_ctr_xcrypt(const uint8_t *key,
                                  const uint8_t *iv,
                                  uint8_t *buf,
                                  size_t len);

void crypto_worker_secure_zero(void *ptr, size_t len);
#endif /* CRYPTO_WORKER_H */