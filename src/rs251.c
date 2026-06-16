/* MIT License
 *
 * Copyright (c) 2026 Robyn Kirkman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rs251/rs251.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
   GF(251) field arithmetic

   Arithmetic in the prime field GF(251), i.e. the integers mod 251. Every
   operation takes reduced inputs and returns a reduced result in [0, 250];
   plain integer arithmetic followed by one reduction is enough, since the
   largest intermediate, 250 * 250, comfortably fits in an unsigned int.
   ====================================================================== */

static inline gf251_t gf251_add(gf251_t a, gf251_t b) {
  return (gf251_t)(((unsigned)a + b) % RS251_PRIME);
}

static inline gf251_t gf251_sub(gf251_t a, gf251_t b) {
  /* Add the modulus first so the difference cannot go negative. */
  return (gf251_t)(((unsigned)a + RS251_PRIME - b) % RS251_PRIME);
}

static inline gf251_t gf251_mul(gf251_t a, gf251_t b) {
  return (gf251_t)((unsigned)a * b % RS251_PRIME);
}

static inline gf251_t gf251_pow(gf251_t a, unsigned exp) {
  /* Square-and-multiply. */
  unsigned result = 1;
  unsigned base = a;
  while (exp > 0) {
    if (exp & 1u) {
      result = result * base % RS251_PRIME;
    }
    base = base * base % RS251_PRIME;
    exp >>= 1;
  }
  return (gf251_t)result;
}

/* Multiplicative inverse; requires a != 0. By Fermat's little theorem,
   a^(p-1) = 1 for a != 0, so a^(p-2) is the inverse. */
static inline gf251_t gf251_inv(gf251_t a) {
  return gf251_pow(a, RS251_PRIME - 2);
}

/* Division; requires b != 0. */
static inline gf251_t gf251_div(gf251_t a, gf251_t b) {
  return gf251_mul(a, gf251_inv(b));
}

/* ======================================================================
   Dense-polynomial layer backing encode and the Gao decoder.

   Coefficients are degree-ascending: coeff[0] is the constant term,
   coeff[len-1] the leading term; len is the coefficient count (degree + 1),
   and len == 0 is the zero polynomial. Every polynomial passed between these
   routines is kept normalized (no trailing zero coefficients), so len - 1 is
   always the true degree. Outputs must fit within RS251_MAX_N + 1
   coefficients, which the encode/decode flows guarantee.
   ====================================================================== */

typedef struct {
  uint16_t len;
  gf251_t coeff[RS251_MAX_N + 1];
} rs251_poly;

static inline void poly_zero(rs251_poly *p) { p->len = 0; }

static inline void poly_one(rs251_poly *p) {
  p->len = 1;
  p->coeff[0] = 1;
}

/* Drop trailing (highest-degree) zero coefficients so len is the true
   degree + 1, or 0 for the zero polynomial. */
static inline void poly_normalize(rs251_poly *p) {
  while (p->len > 0 && p->coeff[p->len - 1] == 0) {
    p->len--;
  }
}

/* True degree of p, or -1 for the zero polynomial. */
static inline int poly_degree(const rs251_poly *p) { return (int)p->len - 1; }

/* Horner evaluation: returns p(x). */
static inline gf251_t poly_eval(const rs251_poly *p, gf251_t x) {
  gf251_t acc = 0;
  for (uint16_t i = p->len; i > 0; i--) {
    acc = gf251_add(gf251_mul(acc, x), p->coeff[i - 1]);
  }
  return acc;
}

/* In-place multiplication by the monic linear factor (x - root):
   new[i] = old[i-1] - root * old[i]. Walking from the top down lets each
   original coefficient be read before it is overwritten. */
static inline void poly_mul_linear(rs251_poly *p, gf251_t root) {
  if (p->len == 0) {
    return; /* zero times anything is zero */
  }
  p->coeff[p->len] = 0;
  for (uint16_t i = p->len; i > 0; i--) {
    p->coeff[i] = gf251_sub(p->coeff[i - 1], gf251_mul(root, p->coeff[i]));
  }
  p->coeff[0] = gf251_sub(0, gf251_mul(root, p->coeff[0]));
  p->len++;
}

/* acc += scale * p, growing acc to p's length if needed. */
static inline void poly_add_scaled(rs251_poly *acc, const rs251_poly *p,
                                   gf251_t scale) {
  while (acc->len < p->len) {
    acc->coeff[acc->len++] = 0;
  }
  for (uint16_t i = 0; i < p->len; i++) {
    acc->coeff[i] = gf251_add(acc->coeff[i], gf251_mul(scale, p->coeff[i]));
  }
}

/* out = a - b. Safe for out to alias either input. */
static inline void poly_sub(rs251_poly *out, const rs251_poly *a,
                            const rs251_poly *b) {
  uint16_t len = a->len > b->len ? a->len : b->len;
  for (uint16_t i = 0; i < len; i++) {
    gf251_t ai = i < a->len ? a->coeff[i] : 0;
    gf251_t bi = i < b->len ? b->coeff[i] : 0;
    out->coeff[i] = gf251_sub(ai, bi);
  }
  out->len = len;
  poly_normalize(out);
}

/* out = a * b. out must not alias a or b. */
static inline void poly_mul(rs251_poly *out, const rs251_poly *a,
                            const rs251_poly *b) {
  if (a->len == 0 || b->len == 0) {
    out->len = 0;
    return;
  }
  out->len = (uint16_t)(a->len + b->len - 1);
  memset(out->coeff, 0, out->len * sizeof(gf251_t));
  for (uint16_t i = 0; i < a->len; i++) {
    for (uint16_t j = 0; j < b->len; j++) {
      out->coeff[i + j] =
          gf251_add(out->coeff[i + j], gf251_mul(a->coeff[i], b->coeff[j]));
    }
  }
  /* A field has no zero divisors, so the leading coefficient is already
     nonzero and no normalization is needed. */
}

/* Schoolbook division a = q*b + r with deg(r) < deg(b). q and r must not
   alias the inputs or each other; either may be NULL if unwanted. Returns 0
   on success, negative if b is the zero polynomial. */
static inline int poly_divmod(rs251_poly *q, rs251_poly *r, const rs251_poly *a,
                              const rs251_poly *b) {
  if (b->len == 0) {
    return -1;
  }

  int deg_a = poly_degree(a);
  int deg_b = poly_degree(b);

  rs251_poly quo;
  quo.len = deg_a >= deg_b ? (uint16_t)(deg_a - deg_b + 1) : 0;
  memset(quo.coeff, 0, quo.len * sizeof(gf251_t));

  /* Long division: cancel the remainder's leading term with the matching
     multiple of b until its degree drops below deg(b). */
  rs251_poly rem = *a;
  gf251_t lead_inv = gf251_inv(b->coeff[b->len - 1]);
  for (int d = deg_a; d >= deg_b; d--) {
    gf251_t factor = gf251_mul(rem.coeff[d], lead_inv);
    if (factor == 0) {
      continue;
    }
    quo.coeff[d - deg_b] = factor;
    for (int i = 0; i <= deg_b; i++) {
      rem.coeff[d - deg_b + i] =
          gf251_sub(rem.coeff[d - deg_b + i], gf251_mul(factor, b->coeff[i]));
    }
  }
  rem.len = rem.len < (uint16_t)deg_b ? rem.len : (uint16_t)deg_b;
  poly_normalize(&rem);

  if (q != NULL) {
    *q = quo;
  }
  if (r != NULL) {
    *r = rem;
  }
  return 0;
}

/* Lagrange interpolation: build into *out the unique polynomial of degree
   < n through the n points (xs[i], ys[i]). The xs must be distinct and n must
   satisfy 1 <= n <= RS251_MAX_N. Returns 0 on success, negative on invalid
   input. */
static inline int poly_interpolate(rs251_poly *out, const gf251_t *xs,
                                   const gf251_t *ys, size_t n) {
  if (out == NULL || xs == NULL || ys == NULL || n < 1 || n > RS251_MAX_N) {
    return -1;
  }

  poly_zero(out);
  for (size_t i = 0; i < n; i++) {
    /* Numerator of the i-th Lagrange basis: prod_{j != i} (x - xs[j]). */
    rs251_poly basis;
    poly_one(&basis);
    for (size_t j = 0; j < n; j++) {
      if (j != i) {
        poly_mul_linear(&basis, xs[j]);
      }
    }

    /* Scaling by ys[i] / basis(xs[i]) makes the term equal ys[i] at xs[i]
       and 0 at every other sample point. A zero denominator means xs[i]
       also occurs among the other xs. */
    gf251_t denom = poly_eval(&basis, xs[i]);
    if (denom == 0) {
      return -1;
    }
    poly_add_scaled(out, &basis, gf251_div(ys[i], denom));
  }

  poly_normalize(out);
  return 0;
}

/* Vanishing polynomial g0(x) = prod_i (x - points[i]) over n points. */
static inline void poly_vanishing(rs251_poly *out, const gf251_t *points,
                                  size_t n) {
  poly_one(out);
  for (size_t i = 0; i < n; i++) {
    poly_mul_linear(out, points[i]);
  }
}

/* Partial extended Euclidean algorithm on (g0, g1): iterate the remainder
   sequence until the current remainder has degree < stop_degree, tracking the
   Bezout coefficient v with current = u*g0 + v*g1. On return *g is that
   remainder and *v the corresponding coefficient (the error-locator analogue
   in Gao decoding). */
static inline void partial_eea(rs251_poly *g, rs251_poly *v,
                               const rs251_poly *g0, const rs251_poly *g1,
                               int stop_degree) {
  /* Remainder sequence (r_prev, r_cur) seeded with (g0, g1), and alongside
     it the Bezout coefficients of g1, seeded with (0, 1), maintaining the
     invariant r_cur = u * g0 + v_cur * g1 (u itself is never needed). */
  rs251_poly r_prev = *g0;
  rs251_poly r_cur = *g1;
  rs251_poly v_prev, v_cur;
  poly_zero(&v_prev);
  poly_one(&v_cur);

  while (poly_degree(&r_cur) >= stop_degree) {
    rs251_poly quo, rem;
    poly_divmod(&quo, &rem, &r_prev, &r_cur);

    /* One Euclidean step: (r_prev, r_cur) <- (r_cur, r_prev mod r_cur),
       applying the same recurrence to the Bezout coefficients. */
    rs251_poly qv, v_next;
    poly_mul(&qv, &quo, &v_cur);
    poly_sub(&v_next, &v_prev, &qv);

    r_prev = r_cur;
    r_cur = rem;
    v_prev = v_cur;
    v_cur = v_next;
  }

  *g = r_cur;
  *v = v_cur;
}

/* ======================================================================
   Public codec API

   Full definition of the opaque rs251_codec. Caches the precomputed vanishing
   polynomial g0(x) = prod_i (x - i) (degree-ascending coefficients in
   vanishing[0..vanishing_len-1]) used by the Gao decoder.
   ====================================================================== */

struct rs251_codec {
  uint16_t n;
  uint16_t k;
  gf251_t vanishing[RS251_MAX_N + 1];
  uint16_t vanishing_len;
};

const char *rs251_version(void) { return RS251_VERSION; }

rs251_codec *rs251_codec_create(uint16_t n, uint16_t k) {
  if (n < 1 || n > RS251_MAX_N || k < 1 || k > n) {
    return NULL;
  }

  rs251_codec *c = malloc(sizeof *c);
  if (c == NULL) {
    return NULL;
  }
  c->n = n;
  c->k = k;

  /* Precompute the vanishing polynomial g0(x) = prod_i (x - i) over all n
     evaluation points; the decoder starts from it on every call. */
  gf251_t points[RS251_MAX_N];
  for (uint16_t i = 0; i < n; i++) {
    points[i] = (gf251_t)i;
  }
  rs251_poly g0;
  poly_vanishing(&g0, points, n);
  c->vanishing_len = g0.len;
  memcpy(c->vanishing, g0.coeff, g0.len * sizeof(gf251_t));

  return c;
}

void rs251_codec_free(rs251_codec *c) { free(c); }

rs251_status rs251_encode(const rs251_codec *c, const gf251_t *msg,
                          gf251_t *code) {
  if (c == NULL || msg == NULL || code == NULL) {
    return RS251_ERR_PARAMS;
  }
  for (uint16_t i = 0; i < c->k; i++) {
    if (msg[i] >= RS251_PRIME) {
      return RS251_ERR_PARAMS; /* not a field element */
    }
  }

  /* Systematic: the message symbols are the codeword's first k positions. */
  memcpy(code, msg, c->k * sizeof(gf251_t));

  /* Parity: interpolate the message polynomial f (degree < k) through the
     points (i, msg[i]) and evaluate it at the parity positions k..n-1. */
  gf251_t xs[RS251_MAX_N];
  for (uint16_t i = 0; i < c->k; i++) {
    xs[i] = (gf251_t)i;
  }
  rs251_poly f;
  if (poly_interpolate(&f, xs, msg, c->k) != 0) {
    return RS251_ERR_PARAMS; /* unreachable: the points are distinct */
  }
  for (uint16_t i = c->k; i < c->n; i++) {
    code[i] = poly_eval(&f, (gf251_t)i);
  }
  return RS251_OK;
}

rs251_status rs251_decode(const rs251_codec *c, const gf251_t *recv,
                          gf251_t *msg, uint16_t *nerrors) {
  if (c == NULL || recv == NULL || msg == NULL) {
    return RS251_ERR_PARAMS;
  }

  /* Any received value outside the field marks an erasure; decoding works
     over the surviving (position, symbol) pairs only. */
  gf251_t xs[RS251_MAX_N];
  gf251_t ys[RS251_MAX_N];
  uint16_t n_surviving = 0;
  for (uint16_t i = 0; i < c->n; i++) {
    if (recv[i] < RS251_PRIME) {
      xs[n_surviving] = (gf251_t)i;
      ys[n_surviving] = recv[i];
      n_surviving++;
    }
  }
  uint16_t n_erased = (uint16_t)(c->n - n_surviving);
  if (n_erased > c->n - c->k) {
    return RS251_ERR_DECODE;
  }

  /* g0: the vanishing polynomial over the surviving points, obtained from
     the cached product over all points by dividing out each erased factor
     (x - i). */
  rs251_poly g0;
  g0.len = c->vanishing_len;
  memcpy(g0.coeff, c->vanishing, c->vanishing_len * sizeof(gf251_t));
  for (uint16_t i = 0; i < c->n; i++) {
    if (recv[i] >= RS251_PRIME) {
      rs251_poly factor, quotient;
      factor.len = 2;
      factor.coeff[0] = gf251_sub(0, (gf251_t)i);
      factor.coeff[1] = 1;
      poly_divmod(&quotient, NULL, &g0, &factor);
      g0 = quotient;
    }
  }

  /* g1: the polynomial through the surviving symbols. */
  rs251_poly g1;
  if (poly_interpolate(&g1, xs, ys, n_surviving) != 0) {
    return RS251_ERR_PARAMS; /* unreachable: positions are distinct */
  }

  /* Gao's key step: run the Euclidean remainder sequence on (g0, g1) until
     the remainder's degree falls below (n' + k)/2, where n' is the number
     of surviving points. That remainder equals f * v for the error locator
     v, so the message polynomial f is recovered by exact division. */
  int stop_degree = (n_surviving + c->k + 1) / 2;
  rs251_poly g, v;
  partial_eea(&g, &v, &g0, &g1, stop_degree);

  rs251_poly f, rem;
  if (poly_divmod(&f, &rem, &g, &v) != 0 || rem.len != 0 ||
      poly_degree(&f) >= (int)c->k) {
    return RS251_ERR_DECODE; /* division not exact: too many errors */
  }

  /* Count the corrected errors and reject any result beyond the guaranteed
     bound 2t + e <= n - k rather than risk a miscorrection. */
  unsigned t = 0;
  for (uint16_t s = 0; s < n_surviving; s++) {
    if (poly_eval(&f, xs[s]) != ys[s]) {
      t++;
    }
  }
  if (2 * t + n_erased > (unsigned)(c->n - c->k)) {
    return RS251_ERR_DECODE;
  }

  /* Systematic code: the message is f evaluated at positions 0..k-1. */
  for (uint16_t i = 0; i < c->k; i++) {
    msg[i] = poly_eval(&f, (gf251_t)i);
  }
  if (nerrors != NULL) {
    *nerrors = (uint16_t)t;
  }
  return RS251_OK;
}

/* ======================================================================
   Byte-block <-> message conversion

   A k-symbol message carries a fixed B = rs251_message_bytes(c) whole bytes
   (the largest B with 256^B <= 251^k). The byte block and the message are the
   same integer in two bases -- the bytes big-endian base 256, the message
   big-endian base 251 -- so converting is base re-expression by Horner's
   method: walk the input digits most significant first, accumulating

       acc = acc * (input base) + input_digit

   into the output buffer. The accumulator is held little-endian so each carry
   is an append at the growing end; afterwards it is reversed and shifted into
   the fixed-width, zero-padded big-endian output.
   ====================================================================== */

/* Reverse digits[0 .. len-1] in place. */
static inline void reverse_digits(uint8_t *digits, size_t len) {
  for (size_t i = 0, j = len; i < j;) {
    j--;
    uint8_t tmp = digits[i];
    digits[i] = digits[j];
    digits[j] = tmp;
    i++;
  }
}

uint16_t rs251_message_bytes(const rs251_codec *c) {
  /* B is the largest value with 256^B <= 251^k, which equals k - 1 for every
     k in [1, RS251_MAX_N]: 256^(k-1) <= 251^k reduces to (256/251)^(k-1) <=
     251, and the left side only reaches 251 around k = 282, past the field
     length. (256^k > 251^k makes B < k, so B is exactly k - 1.) */
  return c == NULL ? 0 : (uint16_t)(c->k - 1);
}

rs251_status rs251_bytes_to_message(const rs251_codec *c, const uint8_t *bytes,
                                    gf251_t *msg) {
  if (c == NULL || bytes == NULL || msg == NULL) {
    return RS251_ERR_PARAMS;
  }

  size_t b = rs251_message_bytes(c);
  size_t len = 0; /* msg[0..len-1]: the value so far, little-endian base 251 */
  for (size_t i = 0; i < b; i++) {
    unsigned carry = bytes[i]; /* acc = acc * 256 + bytes[i] */
    for (size_t j = 0; j < len; j++) {
      unsigned acc = (unsigned)msg[j] * 256u + carry;
      msg[j] = (gf251_t)(acc % RS251_PRIME);
      carry = acc / RS251_PRIME;
    }
    /* 256^msg_bytes <= 251^k bounds the value below 251^k, so it never needs
       more than k base-251 digits; len stays within the message. */
    while (carry > 0) {
      msg[len++] = (gf251_t)(carry % RS251_PRIME);
      carry /= RS251_PRIME;
    }
  }

  /* Lay the len little-endian digits out as a zero-padded big-endian message
     of exactly k symbols. */
  reverse_digits(msg, len);
  memmove(msg + (c->k - len), msg, len * sizeof(gf251_t));
  memset(msg, 0, (c->k - len) * sizeof(gf251_t));
  return RS251_OK;
}

rs251_status rs251_message_to_bytes(const rs251_codec *c, const gf251_t *msg,
                                    uint8_t *bytes) {
  if (c == NULL || msg == NULL || bytes == NULL) {
    return RS251_ERR_PARAMS;
  }

  size_t b = rs251_message_bytes(c);
  size_t len = 0; /* bytes[0..len-1]: the value so far, little-endian base 256 */
  for (size_t i = 0; i < c->k; i++) {
    if (msg[i] >= RS251_PRIME) {
      return RS251_ERR_PARAMS; /* not a base-251 digit */
    }
    unsigned carry = msg[i]; /* acc = acc * 251 + msg[i] */
    for (size_t j = 0; j < len; j++) {
      unsigned acc = (unsigned)bytes[j] * RS251_PRIME + carry;
      bytes[j] = (uint8_t)(acc & 0xFFu);
      carry = acc >> 8;
    }
    while (carry > 0) {
      if (len >= b) {
        return RS251_ERR_PARAMS; /* value exceeds 256^B: not a B-byte block */
      }
      bytes[len++] = (uint8_t)(carry & 0xFFu);
      carry >>= 8;
    }
  }

  /* Lay the len little-endian digits out as a zero-padded big-endian block of
     exactly B bytes. */
  reverse_digits(bytes, len);
  memmove(bytes + (b - len), bytes, len);
  memset(bytes, 0, b - len);
  return RS251_OK;
}
