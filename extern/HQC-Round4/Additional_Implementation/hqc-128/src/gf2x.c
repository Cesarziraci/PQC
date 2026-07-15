/**
 * @file gf2x.c
 * @brief Implementation of multiplication of two polynomials
 */

#include "gf2x.h"
#include "parameters.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


void base_mul(uint64_t *c, uint64_t a, uint64_t b);
void karatsuba_add2(uint64_t *o, uint64_t *tmp1, uint64_t* tmp2, uint64_t size_l, uint64_t size_h);
void karatsuba_add1(uint64_t *alh, uint64_t *blh, const uint64_t *a, const uint64_t *b, uint64_t size_l, uint64_t size_h);
void karatsuba(uint64_t *o, const uint64_t *a, const uint64_t *b, uint64_t size, uint64_t *stack);
static void reduce(uint64_t *o, const uint64_t *a);


/**
 * @brief Caryless multiplication of two words of 64 bits
 *
 * Implemntation of the algorithm mul1 in https://hal.inria.fr/inria-00188261v4/document.
 * With w = 64 and s = 4
 *
 * @param[out] c The result c = a * b
 * @param[in] a The first value a
 * @param[in] b The second value b
 */
void base_mul(uint64_t *c, uint64_t a, uint64_t b) {
   uint64_t h = 0;
   uint64_t l = 0;
   uint64_t g;
   uint64_t u[16] = {0};
   uint64_t mask_tab[4] = {0};
   
   // Step 1
   u[0] = 0;
   u[1] = b & (((uint64_t)1 << (64 - 4)) - 1UL);
   u[2] = u[1] << 1;
   u[3] = u[2] ^ u[1];
   u[4] = u[2] << 1;
   u[5] = u[4] ^ u[1];
   u[6] = u[3] << 1;
   u[7] = u[6] ^ u[1];
   u[8] = u[4] << 1;
   u[9] = u[8] ^ u[1];
   u[10] = u[5] << 1;
   u[11] = u[10] ^ u[1];
   u[12] = u[6] << 1;
   u[13] = u[12] ^ u[1];
   u[14] = u[7] << 1;
   u[15] = u[14] ^ u[1];
 
   g=0;
   uint64_t tmp1 = a & 15;

   for(int i = 0; i < 16; i++) {
    uint64_t tmp2 = tmp1 - i;
    g ^= (u[i] & -(1 - ((tmp2 | -tmp2) >> 63)));
   }
   l = g;
   h = 0;

   // Step 2
   for (uint8_t i = 4; i < 64; i += 4) {
      g = 0;
      uint64_t tmp1 = (a >> i) & 15;
      for (int j = 0; j < 16; ++j) {
        uint64_t tmp2 = tmp1 - j;
        g ^= (u[j] & -(1 - ((tmp2 | -tmp2) >> 63)));
      }

      l ^= g << i;
      h ^= g >> (64 - i);
   }

   // Step 3
   mask_tab [0] = - ((b >> 60) & 1);
   mask_tab [1] = - ((b >> 61) & 1);
   mask_tab [2] = - ((b >> 62) & 1);
   mask_tab [3] = - ((b >> 63) & 1);

   l ^= ((a << 60) & mask_tab[0]);
   h ^= ((a >> 4) & mask_tab[0]);

   l ^= ((a << 61) & mask_tab[1]);
   h ^= ((a >> 3) & mask_tab[1]);

   l ^= ((a << 62) & mask_tab[2]);
   h ^= ((a >> 2) & mask_tab[2]);

   l ^= ((a << 63) & mask_tab[3]);
   h ^= ((a >> 1) & mask_tab[3]);

   c[0] = l;
   c[1] = h;
}




void karatsuba_add1(uint64_t *alh, uint64_t *blh, const uint64_t *a, const uint64_t *b, uint64_t size_l, uint64_t size_h) {
   for (uint64_t i = 0; i < size_h; i++) {
      alh[i] = a[i] ^ a[i + size_l];
      blh[i] = b[i] ^ b[i + size_l];
   }

   if (size_h < size_l) {
      alh[size_h] = a[size_h];
      blh[size_h] = b[size_h];
   }
}



void karatsuba_add2(uint64_t *o, uint64_t *tmp1, uint64_t* tmp2, uint64_t size_l, uint64_t size_h) {
   for (uint64_t i = 0; i < (2 * size_l) ; i++) {
      tmp1[i] = tmp1[i] ^ o[i];
   }

   for (uint64_t i = 0; i < ( 2 * size_h); i++) {
      tmp1[i] = tmp1[i] ^ tmp2[i];
   }

   for (uint64_t i = 0; i <  (2 * size_l); i++) {
      o[i + size_l] = o[i + size_l] ^ tmp1[i];
   }
}



/**
 * Karatsuba multiplication of a and b, Implementation inspired from the NTL library.
 *
 * \param[out] o Polynomial
 * \param[in] a Polynomial
 * \param[in] b Polynomial
 * \param[in] size Length of polynomial
 * \param[in] stack Length of polynomial
 */
void karatsuba(uint64_t *o, const uint64_t *a, const uint64_t *b, uint64_t size, uint64_t *stack) {
   uint64_t size_l, size_h;
   const uint64_t *ah, *bh;

   if (size == 1) {
      base_mul(o, a[0], b[0]);
      return;
   }

   size_h = size / 2;
   size_l = (size + 1) / 2;

   uint64_t *alh = stack;
   uint64_t *blh = alh + size_l;
   uint64_t *tmp1 = blh + size_l;
   uint64_t *tmp2 = o + 2 * size_l;
   
   stack += 4 * size_l;

   ah = a + size_l;
   bh = b + size_l;

   karatsuba(o, a, b, size_l, stack);

   karatsuba(tmp2, ah, bh, size_h, stack);

   karatsuba_add1(alh, blh, a, b, size_l, size_h);

   karatsuba(tmp1, alh, blh, size_l, stack);

   karatsuba_add2(o, tmp1, tmp2, size_l, size_h);
}



/**
 * @brief Compute o(x) = a(x) mod \f$ X^n - 1\f$
 *
 * This function computes the modular reduction of the polynomial a(x)
 *
 * @param[in] a Pointer to the polynomial a(x)
 * @param[out] o Pointer to the result
 */
static void reduce(uint64_t *o, const uint64_t *a) {
    size_t i;
    uint64_t r;
    uint64_t carry;

    for (i = 0; i < VEC_N_SIZE_64; i++) {
        r = a[i + VEC_N_SIZE_64 - 1] >> (PARAM_N & 0x3F);
        carry = (uint64_t ) (a[i + VEC_N_SIZE_64 ] << (64 - (PARAM_N & 0x3F)));
        o[i] = a[i] ^ r ^ carry;
    }
    o[VEC_N_SIZE_64 - 1] &= RED_MASK;
}



/**
 * @brief Multiply two polynomials modulo \f$ X^n - 1\f$.
 *
 * This functions multiplies a sparse polynomial <b>a1</b> (of Hamming weight equal to <b>weight</b>)
 * and a dense polynomial <b>a2</b>. The multiplication is done modulo \f$ X^n - 1\f$.
 *
 * @param[out] o Pointer to the result
 * @param[in] a1 Pointer to the sparse polynomial
 * @param[in] a2 Pointer to the dense polynomial
 * @param[in] weight Integer that is the weigt of the sparse polynomial
 * @param[in] ctx Pointer to the randomness context
 */
void vect_mul(uint64_t *o, const uint64_t *a1, const uint64_t *a2) {
    uint64_t stack[VEC_N_SIZE_64 << 3] = {0};
    uint64_t o_karat[(VEC_N_SIZE_64 << 1) + 1] = {0};

    karatsuba(o_karat, a1, a2, VEC_N_SIZE_64, stack);
    reduce(o, o_karat);
}
