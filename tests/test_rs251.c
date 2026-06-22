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

/* Unit tests for the rs251 library. Each failed check prints its location;
   the process exits nonzero if any check failed. */

#include <stdio.h>
#include <string.h>

#include "rs251/rs251.h"

static int failures;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static void fill_message(gf251_t *msg, uint16_t k) {
  for (uint16_t i = 0; i < k; i++) {
    msg[i] = (gf251_t)((i * 37 + 5) % 251);
  }
}

/* Replace the symbol at pos with a different valid field element. */
static void corrupt(gf251_t *code, uint16_t pos) {
  code[pos] = (gf251_t)((code[pos] + 1) % 251);
}

static void test_init_rejects_bad_params(void) {
  rs251_codec c;
  CHECK(rs251_codec_init(&c, 0, 1) == RS251_ERR_PARAMS);
  CHECK(rs251_codec_init(&c, 10, 0) == RS251_ERR_PARAMS);
  CHECK(rs251_codec_init(&c, 10, 11) == RS251_ERR_PARAMS);
  CHECK(rs251_codec_init(&c, RS251_MAX_N + 1, 1) == RS251_ERR_PARAMS);
  CHECK(rs251_codec_init(NULL, RS251_MAX_N, 1) == RS251_ERR_PARAMS);

  CHECK(rs251_codec_init(&c, RS251_MAX_N, 1) == RS251_OK);
}

static void test_rejects_null_and_bad_symbols(void) {
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  gf251_t msg[10] = {0};
  gf251_t code[20] = {0};
  uint16_t nerrors;

  CHECK(rs251_encode(NULL, msg, code) == RS251_ERR_PARAMS);
  CHECK(rs251_encode(c, NULL, code) == RS251_ERR_PARAMS);
  CHECK(rs251_encode(c, msg, NULL) == RS251_ERR_PARAMS);
  CHECK(rs251_decode(NULL, code, msg, &nerrors) == RS251_ERR_PARAMS);
  CHECK(rs251_decode(c, NULL, msg, &nerrors) == RS251_ERR_PARAMS);
  CHECK(rs251_decode(c, code, NULL, &nerrors) == RS251_ERR_PARAMS);

  msg[3] = 251; /* not a field element */
  CHECK(rs251_encode(c, msg, code) == RS251_ERR_PARAMS);
}

static void test_roundtrip_no_errors(void) {
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  gf251_t msg[10], code[20], out[10];
  uint16_t nerrors = 99;

  fill_message(msg, 10);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  CHECK(memcmp(code, msg, sizeof msg) == 0); /* systematic prefix */
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(memcmp(out, msg, sizeof msg) == 0);
  CHECK(nerrors == 0);
}

static void test_corrects_up_to_capacity(void) {
  /* RS[20, 10] corrects (20 - 10) / 2 = 5 errors. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  gf251_t msg[10], code[20], out[10];
  uint16_t nerrors = 99;
  const uint16_t bad[5] = {1, 4, 7, 13, 19};

  fill_message(msg, 10);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  for (int i = 0; i < 5; i++) {
    corrupt(code, bad[i]);
  }
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(memcmp(out, msg, sizeof msg) == 0);
  CHECK(nerrors == 5);
}

static void test_rejects_beyond_capacity(void) {
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  gf251_t msg[10], code[20], out[10];
  uint16_t nerrors;
  const uint16_t bad[6] = {1, 4, 7, 11, 13, 19};

  fill_message(msg, 10);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  for (int i = 0; i < 6; i++) {
    corrupt(code, bad[i]);
  }
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_ERR_DECODE);
}

static void test_corrects_max_erasures(void) {
  /* RS[20, 10] recovers from up to n - k = 10 erasures with no errors. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  gf251_t msg[10], code[20], out[10];
  uint16_t nerrors = 99;
  const uint16_t erased[10] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18};

  fill_message(msg, 10);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  for (int i = 0; i < 10; i++) {
    code[erased[i]] = RS251_ERASURE;
  }
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(memcmp(out, msg, sizeof msg) == 0);
  CHECK(nerrors == 0); /* erasures are not counted as errors */

  /* One erasure more than n - k must fail. */
  code[1] = RS251_ERASURE;
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_ERR_DECODE);
}

static void test_corrects_mixed_errors_and_erasures(void) {
  /* RS[20, 10]: 4 errors + 2 erasures satisfies 2t + e = 10 <= n - k. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  gf251_t msg[10], code[20], out[10];
  uint16_t nerrors = 99;

  fill_message(msg, 10);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  code[0] = RS251_ERASURE;
  code[15] = RS251_ERASURE;
  corrupt(code, 3);
  corrupt(code, 5);
  corrupt(code, 8);
  corrupt(code, 12);
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(memcmp(out, msg, sizeof msg) == 0);
  CHECK(nerrors == 4);

  /* One more erasure pushes past the bound: 2*4 + 3 > 10. */
  code[16] = RS251_ERASURE;
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_ERR_DECODE);
}

static void test_full_rate_code(void) {
  /* n == k: no parity, decoding is bare interpolation. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 8, 8) == RS251_OK);
  gf251_t msg[8], code[8], out[8];
  uint16_t nerrors = 99;

  fill_message(msg, 8);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  CHECK(memcmp(code, msg, sizeof msg) == 0);
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(memcmp(out, msg, sizeof msg) == 0);
  CHECK(nerrors == 0);
}

static void test_max_length_code(void) {
  /* RS[251, 200] at full field length corrects 25 errors. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, RS251_MAX_N, 200) == RS251_OK);
  static gf251_t msg[200], code[RS251_MAX_N], out[200];
  uint16_t nerrors = 99;

  fill_message(msg, 200);
  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  for (int i = 0; i < 25; i++) {
    corrupt(code, (uint16_t)((7 * i + 3) % RS251_MAX_N));
  }
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(memcmp(out, msg, sizeof msg) == 0);
  CHECK(nerrors == 25);
}

static void test_message_bytes_sizes(void) {
  /* Largest B with 256^B <= 251^k, for a few k. */
  const struct {
    uint16_t k;
    size_t b;
  } cases[] = {{1, 0}, {2, 1}, {3, 2}, {10, 9}, {100, 99}, {200, 199}, {251, 250}};

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    rs251_codec c;
    CHECK(rs251_codec_init(&c, RS251_MAX_N, cases[i].k) == RS251_OK);
    CHECK(rs251_message_bytes(&c) == cases[i].b);
  }
  CHECK(rs251_message_bytes(NULL) == 0);
}

static void test_bytes_message_known_vector(void) {
  /* k=3 -> B=2 (256^2 = 65536 <= 251^3 = 15813251 < 256^3). The block
     {0x01,0x00} = 256 is base-251 {1,5}, zero-padded to k symbols: {0,1,5}. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 5, 3) == RS251_OK);
  CHECK(rs251_message_bytes(c) == 2);

  const uint8_t in[2] = {0x01, 0x00};
  gf251_t msg[3];
  uint8_t back[2];
  CHECK(rs251_bytes_to_message(c, in, msg) == RS251_OK);
  CHECK(msg[0] == 0 && msg[1] == 1 && msg[2] == 5);
  CHECK(rs251_message_to_bytes(c, msg, back) == RS251_OK);
  CHECK(back[0] == 0x01 && back[1] == 0x00);
}

static void test_bytes_message_roundtrip(void) {
  /* Pack bytes into a message, push it through a lossy channel, and confirm
     the corrected message unpacks to the original block. */
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 20, 10) == RS251_OK);
  size_t b = rs251_message_bytes(c); /* k=10 -> 9 bytes */
  CHECK(b == 9);

  uint8_t in[9], back[9];
  gf251_t msg[10], code[20], out[10];
  uint16_t nerrors = 99;
  for (size_t i = 0; i < b; i++) {
    in[i] = (uint8_t)(i * 90 + 0x83); /* values above 250 to exercise carries */
  }

  CHECK(rs251_bytes_to_message(c, in, msg) == RS251_OK);
  for (uint16_t i = 0; i < 10; i++) {
    CHECK(msg[i] < 251); /* every symbol is a valid field element */
  }

  CHECK(rs251_encode(c, msg, code) == RS251_OK);
  corrupt(code, 2);
  corrupt(code, 7);
  corrupt(code, 14);
  CHECK(rs251_decode(c, code, out, &nerrors) == RS251_OK);
  CHECK(rs251_message_to_bytes(c, out, back) == RS251_OK);
  CHECK(memcmp(in, back, b) == 0);
}

static void test_message_to_bytes_range_and_errors(void) {
  rs251_codec ctx;
  rs251_codec *c = &ctx;
  CHECK(rs251_codec_init(c, 4, 2) == RS251_OK); /* k=2 -> B=1 */
  CHECK(rs251_message_bytes(c) == 1);

  uint8_t byte;
  gf251_t msg[2];

  /* A message whose value is below 256 fits the single byte. */
  msg[0] = 0;
  msg[1] = 200;
  CHECK(rs251_message_to_bytes(c, msg, &byte) == RS251_OK);
  CHECK(byte == 200);

  /* 250*251 + 250 = 63000 > 255: too large for one byte. */
  msg[0] = 250;
  msg[1] = 250;
  CHECK(rs251_message_to_bytes(c, msg, &byte) == RS251_ERR_PARAMS);

  /* A symbol outside the field is rejected. */
  msg[0] = 251;
  msg[1] = 0;
  CHECK(rs251_message_to_bytes(c, msg, &byte) == RS251_ERR_PARAMS);

  /* NULL arguments. */
  uint8_t in[1] = {42};
  CHECK(rs251_bytes_to_message(NULL, in, msg) == RS251_ERR_PARAMS);
  CHECK(rs251_bytes_to_message(c, NULL, msg) == RS251_ERR_PARAMS);
  CHECK(rs251_message_to_bytes(c, NULL, &byte) == RS251_ERR_PARAMS);
}

int main(void) {
  CHECK(rs251_version() != NULL && rs251_version()[0] != '\0');

  test_init_rejects_bad_params();
  test_rejects_null_and_bad_symbols();
  test_roundtrip_no_errors();
  test_corrects_up_to_capacity();
  test_rejects_beyond_capacity();
  test_corrects_max_erasures();
  test_corrects_mixed_errors_and_erasures();
  test_full_rate_code();
  test_max_length_code();
  test_message_bytes_sizes();
  test_bytes_message_known_vector();
  test_bytes_message_roundtrip();
  test_message_to_bytes_range_and_errors();

  if (failures == 0) {
    printf("all tests passed\n");
    return 0;
  }
  fprintf(stderr, "%d check(s) failed\n", failures);
  return 1;
}
