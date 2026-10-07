/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// Argon2id v1.3 (0x13) per RFC 9106, compact single-file variant of
// the public-domain/CC0 reference implementation's algorithm:
//   - BLAKE2b (RFC 7693) for H0, the variable-length hash H' and the
//     final tag
//   - the multiplication-hardened permutation (fill_block) for memory
//     compression
//   - data-independent addressing for the first half of the first
//     pass, data-dependent afterwards (the "id" hybrid)
// Lanes are filled sequentially — the schedule's outputs are
// deterministic, so single-threaded results are identical to the
// threaded reference implementation (verified against libargon2 in
// autotests/securestore).

#include "argon2id.h"

#include <stdlib.h>
#include <string.h>

#define ARGON2_BLOCK_WORDS 128 /* 1024 bytes of uint64 */
#define ARGON2_SYNC_POINTS 4
#define ARGON2_ADDRESSES_IN_BLOCK 128
#define ARGON2_VERSION_13 0x13
#define ARGON2_TYPE_ID 2

static uint64_t rotr64(uint64_t x, unsigned n)
{
    return (x >> n) | (x << (64 - n));
}

static void store32le(void *dst, uint32_t v)
{
    unsigned char *p = (unsigned char *)dst;
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void store64le(void *dst, uint64_t v)
{
    unsigned char *p = (unsigned char *)dst;
    int i;
    for (i = 0; i < 8; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t load64le(const void *src)
{
    const unsigned char *p = (const unsigned char *)src;
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

static void secure_wipe(void *p, size_t n)
{
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
}

/* ------------------------------------------------------------------ */
/* BLAKE2b (unkeyed), streaming-capable                                */

typedef struct {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    unsigned char buf[128];
    size_t buflen;
    size_t outlen;
} blake2b_ctx;

static const uint64_t blake2b_iv[8] = {
    UINT64_C(0x6a09e667f3bcc908), UINT64_C(0xbb67ae8584caa73b),
    UINT64_C(0x3c6ef372fe94f82b), UINT64_C(0xa54ff53a5f1d36f1),
    UINT64_C(0x510e527fade682d1), UINT64_C(0x9b05688c2b3e6c1f),
    UINT64_C(0x1f83d9abfb41bd6b), UINT64_C(0x5be0cd19137e2179)
};

static const unsigned char blake2b_sigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

static void blake2b_compress(blake2b_ctx *ctx, const unsigned char block[128])
{
    uint64_t m[16];
    uint64_t v[16];
    int i, r;

    for (i = 0; i < 16; ++i)
        m[i] = load64le(block + i * 8);
    for (i = 0; i < 8; ++i)
        v[i] = ctx->h[i];
    for (i = 0; i < 8; ++i)
        v[i + 8] = blake2b_iv[i];
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    v[14] ^= ctx->f[0];
    v[15] ^= ctx->f[1];

#define B2B_G(r, i, a, b, c, d) do {                                     \
    a = a + b + m[blake2b_sigma[r][2 * (i) + 0]];                        \
    d = rotr64(d ^ a, 32);                                               \
    c = c + d;                                                           \
    b = rotr64(b ^ c, 24);                                               \
    a = a + b + m[blake2b_sigma[r][2 * (i) + 1]];                        \
    d = rotr64(d ^ a, 16);                                               \
    c = c + d;                                                           \
    b = rotr64(b ^ c, 63);                                               \
} while (0)

    for (r = 0; r < 12; ++r) {
        B2B_G(r, 0, v[0], v[4], v[8],  v[12]);
        B2B_G(r, 1, v[1], v[5], v[9],  v[13]);
        B2B_G(r, 2, v[2], v[6], v[10], v[14]);
        B2B_G(r, 3, v[3], v[7], v[11], v[15]);
        B2B_G(r, 4, v[0], v[5], v[10], v[15]);
        B2B_G(r, 5, v[1], v[6], v[11], v[12]);
        B2B_G(r, 6, v[2], v[7], v[8],  v[13]);
        B2B_G(r, 7, v[3], v[4], v[9],  v[14]);
    }
#undef B2B_G

    for (i = 0; i < 8; ++i)
        ctx->h[i] ^= v[i] ^ v[i + 8];
}

static void blake2b_init(blake2b_ctx *ctx, size_t outlen)
{
    int i;
    memset(ctx, 0, sizeof(*ctx));
    for (i = 0; i < 8; ++i)
        ctx->h[i] = blake2b_iv[i];
    ctx->h[0] ^= UINT64_C(0x01010000) ^ (uint64_t)outlen;
    ctx->outlen = outlen;
}

static void blake2b_update(blake2b_ctx *ctx, const void *in, size_t inlen)
{
    const unsigned char *p = (const unsigned char *)in;
    if (!inlen)
        return;
    while (inlen) {
        size_t left = ctx->buflen;
        size_t fill = 128 - left;
        if (inlen > fill) {
            memcpy(ctx->buf + left, p, fill);
            ctx->buflen = 0;
            ctx->t[0] += 128;
            if (ctx->t[0] < 128)
                ctx->t[1]++;
            blake2b_compress(ctx, ctx->buf);
            p += fill;
            inlen -= fill;
        } else {
            memcpy(ctx->buf + left, p, inlen);
            ctx->buflen = left + inlen;
            return;
        }
    }
}

static void blake2b_final(blake2b_ctx *ctx, void *out)
{
    unsigned char full[64];
    size_t i;
    ctx->t[0] += ctx->buflen;
    if (ctx->t[0] < ctx->buflen)
        ctx->t[1]++;
    ctx->f[0] = UINT64_MAX;
    memset(ctx->buf + ctx->buflen, 0, 128 - ctx->buflen);
    blake2b_compress(ctx, ctx->buf);
    for (i = 0; i < 8; ++i)
        store64le(full + i * 8, ctx->h[i]);
    memcpy(out, full, ctx->outlen);
    secure_wipe(full, sizeof(full));
    secure_wipe(ctx, sizeof(*ctx));
}

static void blake2b(void *out, size_t outlen, const void *in, size_t inlen)
{
    blake2b_ctx ctx;
    blake2b_init(&ctx, outlen);
    blake2b_update(&ctx, in, inlen);
    blake2b_final(&ctx, out);
}

// H' — the variable-length hash from the Argon2 spec: hashes
// LE32(outlen) || in and chains 64-byte digests for outlen > 64.
static void blake2b_long(void *out, size_t outlen, const void *in, size_t inlen)
{
    unsigned char *dst = (unsigned char *)out;
    unsigned char le[4];
    unsigned char v[64];
    size_t produced;
    blake2b_ctx ctx;

    store32le(le, (uint32_t)outlen);
    blake2b_init(&ctx, outlen <= 64 ? outlen : 64);
    blake2b_update(&ctx, le, 4);
    blake2b_update(&ctx, in, inlen);
    blake2b_final(&ctx, v);

    if (outlen <= 64) {
        memcpy(out, v, outlen);
        secure_wipe(v, sizeof(v));
        return;
    }
    memcpy(dst, v, 32);
    produced = 32;
    while (outlen - produced > 64) {
        blake2b(v, 64, v, 64);
        memcpy(dst + produced, v, 32);
        produced += 32;
    }
    blake2b(dst + produced, outlen - produced, v, 64);
    secure_wipe(v, sizeof(v));
}

/* ------------------------------------------------------------------ */
/* Argon2 compression function G                                       */

// fBlaMka: the Argon2-modified BlaMka mixer — x + y + 2*lo32(x)*lo32(y).
static uint64_t fblamka(uint64_t x, uint64_t y)
{
    return x + y + 2ULL * (x & UINT64_C(0xffffffff))
                      * (y & UINT64_C(0xffffffff));
}

#define ARGON2_G(a, b, c, d) do {                                        \
    a = fblamka(a, b); d = rotr64(d ^ a, 32);                            \
    c = fblamka(c, d); b = rotr64(b ^ c, 24);                            \
    a = fblamka(a, b); d = rotr64(d ^ a, 16);                            \
    c = fblamka(c, d); b = rotr64(b ^ c, 63);                            \
} while (0)

// One BLAKE2b-style round over 16 words (no message schedule).
static void argon2_round(uint64_t v[16])
{
    ARGON2_G(v[0], v[4], v[8],  v[12]);
    ARGON2_G(v[1], v[5], v[9],  v[13]);
    ARGON2_G(v[2], v[6], v[10], v[14]);
    ARGON2_G(v[3], v[7], v[11], v[15]);
    ARGON2_G(v[0], v[5], v[10], v[15]);
    ARGON2_G(v[1], v[6], v[11], v[12]);
    ARGON2_G(v[2], v[7], v[8],  v[13]);
    ARGON2_G(v[3], v[4], v[9],  v[14]);
}

// out = G(prev, ref) = (prev ^ ref) ^ P(prev ^ ref), optionally XORed
// into the previous content of out (passes after the first).
static void fill_block(const uint64_t *prev, const uint64_t *ref,
                       uint64_t *next, int with_xor)
{
    uint64_t r[ARGON2_BLOCK_WORDS];
    uint64_t z[ARGON2_BLOCK_WORDS];
    int i, j;

    for (i = 0; i < ARGON2_BLOCK_WORDS; ++i) {
        r[i] = prev[i] ^ ref[i];
        z[i] = r[i];
    }
    /* permutation on the 8 rows */
    for (i = 0; i < 8; ++i)
        argon2_round(&z[16 * i]);
    /* permutation on the 8 columns (each column pairs words 2i,2i+1) */
    for (i = 0; i < 8; ++i) {
        uint64_t column[16];
        for (j = 0; j < 8; ++j) {
            column[2 * j]     = z[16 * j + 2 * i];
            column[2 * j + 1] = z[16 * j + 2 * i + 1];
        }
        argon2_round(column);
        for (j = 0; j < 8; ++j) {
            z[16 * j + 2 * i]     = column[2 * j];
            z[16 * j + 2 * i + 1] = column[2 * j + 1];
        }
    }
    for (i = 0; i < ARGON2_BLOCK_WORDS; ++i) {
        uint64_t mixed = z[i] ^ r[i];
        next[i] = with_xor ? (next[i] ^ mixed) : mixed;
    }
    secure_wipe(r, sizeof(r));
    secure_wipe(z, sizeof(z));
}

/* ------------------------------------------------------------------ */
/* Memory fill                                                         */

typedef struct {
    uint64_t *memory;     /* memory_blocks * ARGON2_BLOCK_WORDS */
    uint32_t memory_blocks;
    uint32_t segment_length;
    uint32_t lane_length;
    uint32_t lanes;
    uint32_t passes;
} argon2_ctx;

static void next_addresses(uint64_t *input_block,
                           const uint64_t *zero_block,
                           uint64_t *address_block)
{
    input_block[6]++;
    fill_block(zero_block, input_block, address_block, 0);
    fill_block(zero_block, address_block, address_block, 0);
}

// Maps the 32-bit pseudo-random value to a reference-block index in
// the allowed area for this position (same maths as the reference
// implementation, including the edge cases at index 0 / slice 0).
static uint32_t index_alpha(const argon2_ctx *ctx,
                            uint32_t pass, uint32_t slice,
                            uint32_t index, uint32_t pseudo_rand32,
                            int same_lane)
{
    uint32_t reference_area_size;
    uint64_t relative_position;
    uint32_t start_position = 0;

    if (pass == 0) {
        if (slice == 0) {
            reference_area_size = index - 1;
        } else if (same_lane) {
            reference_area_size = slice * ctx->segment_length + index - 1;
        } else {
            reference_area_size = slice * ctx->segment_length
                + (index == 0 ? (uint32_t)-1 : 0);
        }
    } else {
        if (same_lane) {
            reference_area_size = ctx->lane_length - ctx->segment_length
                + index - 1;
        } else {
            reference_area_size = ctx->lane_length - ctx->segment_length
                + (index == 0 ? (uint32_t)-1 : 0);
        }
    }

    relative_position = pseudo_rand32;
    relative_position = (relative_position * relative_position) >> 32;
    relative_position = reference_area_size - 1
        - ((reference_area_size * relative_position) >> 32);

    if (pass != 0)
        start_position = (slice == ARGON2_SYNC_POINTS - 1)
            ? 0 : (slice + 1) * ctx->segment_length;

    return (uint32_t)((start_position + relative_position)
                      % ctx->lane_length);
}

static void fill_segment(argon2_ctx *ctx, uint32_t pass, uint32_t lane,
                         uint32_t slice, int data_independent)
{
    uint64_t input_block[ARGON2_BLOCK_WORDS];
    uint64_t zero_block[ARGON2_BLOCK_WORDS];
    uint64_t address_block[ARGON2_BLOCK_WORDS];
    uint32_t starting_index = (pass == 0 && slice == 0) ? 2 : 0;
    uint32_t curr_offset = lane * ctx->lane_length
        + slice * ctx->segment_length + starting_index;
    uint32_t prev_offset;
    uint32_t i;

    memset(input_block, 0, sizeof(input_block));
    memset(zero_block, 0, sizeof(zero_block));
    memset(address_block, 0, sizeof(address_block));

    if (curr_offset % ctx->lane_length == 0)
        prev_offset = curr_offset + ctx->lane_length - 1;
    else
        prev_offset = curr_offset - 1;

    if (data_independent) {
        input_block[0] = pass;
        input_block[1] = lane;
        input_block[2] = slice;
        input_block[3] = ctx->memory_blocks;
        input_block[4] = ctx->passes;
        input_block[5] = ARGON2_TYPE_ID;
        input_block[6] = 0;
        /* The first two lane blocks are pre-seeded, so the loop starts
           at index 2 and the i%128 in-loop trigger never fires for the
           opening batch — generate it up front. */
        if (pass == 0 && slice == 0)
            next_addresses(input_block, zero_block, address_block);
    }

    for (i = starting_index; i < ctx->segment_length;
         ++i, ++curr_offset, ++prev_offset) {
        uint64_t pseudo_rand;
        uint32_t ref_lane;
        uint32_t ref_index;

        /* prev_offset walked off the lane start's wrap-around block:
           from the second block on it simply trails curr_offset. */
        if (curr_offset % ctx->lane_length == 1)
            prev_offset = curr_offset - 1;

        if (data_independent) {
            if (i % ARGON2_ADDRESSES_IN_BLOCK == 0)
                next_addresses(input_block, zero_block, address_block);
            pseudo_rand = address_block[i % ARGON2_ADDRESSES_IN_BLOCK];
        } else {
            pseudo_rand = ctx->memory[prev_offset
                * ARGON2_BLOCK_WORDS];
        }

        ref_lane = (uint32_t)(pseudo_rand >> 32) % ctx->lanes;
        if (pass == 0 && slice == 0)
            ref_lane = lane;

        ref_index = index_alpha(ctx, pass, slice, i,
                                (uint32_t)(pseudo_rand & 0xffffffffULL),
                                ref_lane == lane);

        fill_block(ctx->memory + prev_offset * ARGON2_BLOCK_WORDS,
                   ctx->memory + (ref_lane * ctx->lane_length
                       + ref_index) * ARGON2_BLOCK_WORDS,
                   ctx->memory + curr_offset * ARGON2_BLOCK_WORDS,
                   pass != 0);
    }

    secure_wipe(input_block, sizeof(input_block));
    secure_wipe(address_block, sizeof(address_block));
}

int arora_argon2id(void *out, size_t outlen,
                   const void *password, size_t passwordLen,
                   const void *salt, size_t saltLen,
                   uint32_t timeCost, uint32_t memoryKiB,
                   uint32_t lanes)
{
    argon2_ctx ctx;
    unsigned char h0[64];
    unsigned char blockhash_bytes[72];
    uint64_t final_block[ARGON2_BLOCK_WORDS];
    blake2b_ctx blake;
    uint32_t pass, slice, lane;

    if (!out || !password || !salt
        || outlen < 4 || saltLen < 8
        || timeCost < 1 || lanes < 1
        || memoryKiB < 8U * lanes) {
        secure_wipe(out, out ? outlen : 0);
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.memory_blocks = memoryKiB
        - (memoryKiB % (lanes * ARGON2_SYNC_POINTS));
    ctx.segment_length = ctx.memory_blocks / (lanes * ARGON2_SYNC_POINTS);
    ctx.lane_length = ctx.segment_length * ARGON2_SYNC_POINTS;
    ctx.lanes = lanes;
    ctx.passes = timeCost;

    ctx.memory = (uint64_t *)malloc((size_t)ctx.memory_blocks * 1024);
    if (!ctx.memory) {
        secure_wipe(out, outlen);
        return -1;
    }

    /* H0 = Blake2b-64 over the parameter block. */
    blake2b_init(&blake, 64);
    {
        uint32_t fields[6] = {
            lanes, (uint32_t)outlen, memoryKiB, timeCost,
            ARGON2_VERSION_13, ARGON2_TYPE_ID
        };
        unsigned char le[4];
        int i;
        for (i = 0; i < 6; ++i) {
            store32le(le, fields[i]);
            blake2b_update(&blake, le, 4);
        }
        store32le(le, (uint32_t)passwordLen);
        blake2b_update(&blake, le, 4);
        blake2b_update(&blake, password, passwordLen);
        store32le(le, (uint32_t)saltLen);
        blake2b_update(&blake, le, 4);
        blake2b_update(&blake, salt, saltLen);
        /* Secret and associated-data fields stay length-0 (this API
           doesn't take them); their length words are still hashed. */
        store32le(le, 0);
        blake2b_update(&blake, le, 4);
        store32le(le, 0);
        blake2b_update(&blake, le, 4);
    }
    blake2b_final(&blake, h0);

    /* First two blocks of every lane. */
    memcpy(blockhash_bytes, h0, 64);
    for (lane = 0; lane < lanes; ++lane) {
        store32le(blockhash_bytes + 64 + 4, lane);
        store32le(blockhash_bytes + 64, 0);
        blake2b_long(ctx.memory
                         + lane * ctx.lane_length * ARGON2_BLOCK_WORDS,
                     1024, blockhash_bytes, 72);
        store32le(blockhash_bytes + 64, 1);
        blake2b_long(ctx.memory
                         + (lane * ctx.lane_length + 1)
                             * ARGON2_BLOCK_WORDS,
                     1024, blockhash_bytes, 72);
    }

    /* Fill all slices of all lanes, t passes. */
    for (pass = 0; pass < timeCost; ++pass) {
        for (slice = 0; slice < ARGON2_SYNC_POINTS; ++slice) {
            const int data_independent =
                (pass == 0 && slice < ARGON2_SYNC_POINTS / 2);
            for (lane = 0; lane < lanes; ++lane)
                fill_segment(&ctx, pass, lane, slice, data_independent);
        }
    }

    /* Final block = XOR of the last block of every lane. */
    memcpy(final_block,
           ctx.memory + (ctx.lane_length - 1) * ARGON2_BLOCK_WORDS,
           sizeof(final_block));
    for (lane = 1; lane < lanes; ++lane) {
        const uint64_t *last = ctx.memory
            + (lane * ctx.lane_length + ctx.lane_length - 1)
                * ARGON2_BLOCK_WORDS;
        int i;
        for (i = 0; i < ARGON2_BLOCK_WORDS; ++i)
            final_block[i] ^= last[i];
    }
    {
        unsigned char final_bytes[1024];
        int i;
        for (i = 0; i < ARGON2_BLOCK_WORDS; ++i)
            store64le(final_bytes + i * 8, final_block[i]);
        blake2b_long(out, outlen, final_bytes, 1024);
        secure_wipe(final_bytes, sizeof(final_bytes));
    }

    secure_wipe(ctx.memory, (size_t)ctx.memory_blocks * 1024);
    free(ctx.memory);
    secure_wipe(final_block, sizeof(final_block));
    secure_wipe(h0, sizeof(h0));
    secure_wipe(blockhash_bytes, sizeof(blockhash_bytes));
    return 0;
}
