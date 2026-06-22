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

#ifndef RS251_H
#define RS251_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RS251_PRIME 251u

/* Maximum length of a codeword. */
#define RS251_MAX_N 251

/* A value in [0, 250]. */
typedef uint8_t gf251_t;
#define RS251_ERASURE ((gf251_t)0xFF)

/* Returns the library version string, e.g. "0.1.0". */
const char *rs251_version(void);

typedef enum {
  RS251_OK = 0,
  RS251_ERR_PARAMS = -1, /* invalid argument passed to encode/decode */
  RS251_ERR_DECODE = -2, /* more than (n-k) erasures+2*errors: decode failed */
} rs251_status;

/* RS251 context. The fields are an implementation detail and must not be read
   or written directly; the layout is exposed here only so callers can allocate
   a codec themselves (on the stack, in static storage, wherever they choose).
   Every codec has the same fixed size regardless of n and k. Initialize one
   with rs251_codec_init before passing it to any other function. */
typedef struct rs251_codec {
  uint16_t n;
  uint16_t k;
  uint16_t vanishing_len;
  gf251_t vanishing[RS251_MAX_N + 1];
} rs251_codec;

/* Initializes a caller-allocated codec for an RS(n, k) code, requiring
   1 <= k <= n <= RS251_MAX_N. Returns RS251_OK, or RS251_ERR_PARAMS on a NULL
   pointer or out-of-range n/k. Acquires no resources: there is nothing to
   free, and a codec may be re-initialized at any time. */
rs251_status rs251_codec_init(rs251_codec *c, uint16_t n, uint16_t k);

/* Systematic encoding: writes the n-symbol codeword for the k-symbol message
   into code. The first k codeword symbols equal msg; the remaining n-k are
   parity. Returns RS251_OK, or RS251_ERR_PARAMS on a NULL argument or a
   message symbol outside 0..250. */
rs251_status rs251_encode(const rs251_codec *c, const gf251_t *msg,
                          gf251_t *code);

/* Decodes the n received symbols in recv into the k-symbol message msg.
   Positions with recv[i] > 250 (e.g. set to RS251_ERASURE) are treated as
   erasures. With e erasures and t errors, decoding succeeds iff 2t + e <=
   n - k; if nerrors is non-NULL it receives t (erasures are not counted).
   Returns RS251_OK on success, RS251_ERR_DECODE if that bound is exceeded, or
   RS251_ERR_PARAMS on a NULL argument. recv is left unchanged. */
rs251_status rs251_decode(const rs251_codec *c, const gf251_t *recv,
                          gf251_t *msg, uint16_t *nerrors);

/* Number of whole bytes a k-symbol message holds for this codec, i.e. the
   largest B with 256^B <= 251^k. Returns 0 if c is NULL. */
uint16_t rs251_message_bytes(const rs251_codec *c);

/* Packs exactly rs251_message_bytes(c) big-endian bytes into the codec's
   k-symbol message msg, written in full and zero-padded on the high end (msg
   must have room for k symbols). Every B-byte block fits, so this only fails
   with RS251_ERR_PARAMS on a NULL argument. */
rs251_status rs251_bytes_to_message(const rs251_codec *c, const uint8_t *bytes,
                                    gf251_t *msg);

/* Inverse of rs251_bytes_to_message: unpacks the codec's k-symbol message msg
   into exactly rs251_message_bytes(c) big-endian bytes. Returns RS251_OK, or
   RS251_ERR_PARAMS on a NULL argument, a symbol >= 251, or a message whose
   value does not fit in that many bytes. */
rs251_status rs251_message_to_bytes(const rs251_codec *c, const gf251_t *msg,
                                    uint8_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* RS251_H */
