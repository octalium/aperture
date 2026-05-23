/*
 * BLAKE3 reference implementation (portable C).
 * Derived from the official BLAKE3 reference implementation,
 * https://github.com/BLAKE3-team/BLAKE3, which is dual-licensed
 * CC0 / Apache 2.0.
 */

#include "blake3.h"

#include <stdint.h>
#include <string.h>

#define BLOCK_LEN  64
#define CHUNK_LEN  1024
#define OUT_LEN    32
#define KEY_LEN    32

#define CHUNK_START         (1u << 0)
#define CHUNK_END           (1u << 1)
#define PARENT              (1u << 2)
#define ROOT                (1u << 3)
#define KEYED_HASH          (1u << 4)
#define DERIVE_KEY_CONTEXT  (1u << 5)
#define DERIVE_KEY_MATERIAL (1u << 6)

static const uint32_t IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static const uint8_t MSG_PERMUTATION[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

static inline uint32_t rotate_right(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static inline void g(uint32_t *state, size_t a, size_t b, size_t c, size_t d,
                     uint32_t mx, uint32_t my)
{
    state[a] = state[a] + state[b] + mx;
    state[d] = rotate_right(state[d] ^ state[a], 16);
    state[c] = state[c] + state[d];
    state[b] = rotate_right(state[b] ^ state[c], 12);
    state[a] = state[a] + state[b] + my;
    state[d] = rotate_right(state[d] ^ state[a], 8);
    state[c] = state[c] + state[d];
    state[b] = rotate_right(state[b] ^ state[c], 7);
}

static inline void round_fn(uint32_t state[16], const uint32_t *msg)
{
    g(state, 0, 4,  8, 12, msg[0],  msg[1]);
    g(state, 1, 5,  9, 13, msg[2],  msg[3]);
    g(state, 2, 6, 10, 14, msg[4],  msg[5]);
    g(state, 3, 7, 11, 15, msg[6],  msg[7]);
    g(state, 0, 5, 10, 15, msg[8],  msg[9]);
    g(state, 1, 6, 11, 12, msg[10], msg[11]);
    g(state, 2, 7,  8, 13, msg[12], msg[13]);
    g(state, 3, 4,  9, 14, msg[14], msg[15]);
}

static void compress_in_place(uint32_t cv[8], const uint8_t block[BLOCK_LEN],
                              uint8_t block_len, uint64_t counter,
                              uint8_t flags)
{
    uint32_t msg[16];
    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        v |= (uint32_t)block[4 * i + 0];
        v |= (uint32_t)block[4 * i + 1] << 8;
        v |= (uint32_t)block[4 * i + 2] << 16;
        v |= (uint32_t)block[4 * i + 3] << 24;
        msg[i] = v;
    }

    uint32_t state[16] = {
        cv[0], cv[1], cv[2], cv[3],
        cv[4], cv[5], cv[6], cv[7],
        IV[0], IV[1], IV[2], IV[3],
        (uint32_t)counter,
        (uint32_t)(counter >> 32),
        (uint32_t)block_len,
        (uint32_t)flags,
    };

    for (int r = 0; r < 7; r++) {
        round_fn(state, msg);
        uint32_t permuted[16];
        for (int i = 0; i < 16; i++) {
            permuted[i] = msg[MSG_PERMUTATION[i]];
        }
        for (int i = 0; i < 16; i++) {
            msg[i] = permuted[i];
        }
    }

    for (int i = 0; i < 8; i++) {
        cv[i] = state[i] ^ state[i + 8];
    }
}

static void compress_xof(const uint32_t cv[8], const uint8_t block[BLOCK_LEN],
                         uint8_t block_len, uint64_t counter, uint8_t flags,
                         uint8_t out[64])
{
    uint32_t msg[16];
    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        v |= (uint32_t)block[4 * i + 0];
        v |= (uint32_t)block[4 * i + 1] << 8;
        v |= (uint32_t)block[4 * i + 2] << 16;
        v |= (uint32_t)block[4 * i + 3] << 24;
        msg[i] = v;
    }

    uint32_t state[16] = {
        cv[0], cv[1], cv[2], cv[3],
        cv[4], cv[5], cv[6], cv[7],
        IV[0], IV[1], IV[2], IV[3],
        (uint32_t)counter,
        (uint32_t)(counter >> 32),
        (uint32_t)block_len,
        (uint32_t)flags,
    };

    for (int r = 0; r < 7; r++) {
        round_fn(state, msg);
        uint32_t permuted[16];
        for (int i = 0; i < 16; i++) {
            permuted[i] = msg[MSG_PERMUTATION[i]];
        }
        for (int i = 0; i < 16; i++) {
            msg[i] = permuted[i];
        }
    }

    uint32_t full[16];
    for (int i = 0; i < 8; i++) {
        full[i]     = state[i] ^ state[i + 8];
        full[i + 8] = state[i + 8] ^ cv[i];
    }

    for (int i = 0; i < 16; i++) {
        out[4 * i + 0] = (uint8_t)(full[i]);
        out[4 * i + 1] = (uint8_t)(full[i] >> 8);
        out[4 * i + 2] = (uint8_t)(full[i] >> 16);
        out[4 * i + 3] = (uint8_t)(full[i] >> 24);
    }
}

static void chunk_state_init(blake3_chunk_state *self, const uint32_t key[8],
                              uint64_t chunk_counter, uint8_t flags)
{
    memcpy(self->cv, key, KEY_LEN);
    self->chunk_counter     = chunk_counter;
    memset(self->buf, 0, BLOCK_LEN);
    self->buf_len           = 0;
    self->blocks_compressed = 0;
    self->flags             = flags;
}

static size_t chunk_state_len(const blake3_chunk_state *self)
{
    return BLOCK_LEN * (size_t)self->blocks_compressed + (size_t)self->buf_len;
}

static uint8_t chunk_state_start_flag(const blake3_chunk_state *self)
{
    return self->blocks_compressed == 0 ? CHUNK_START : 0;
}

static void chunk_state_update(blake3_chunk_state *self, const uint8_t *input,
                               size_t input_len)
{
    while (input_len > 0) {
        if (self->buf_len == BLOCK_LEN) {
            uint8_t flags = self->flags | chunk_state_start_flag(self);
            compress_in_place(self->cv, self->buf, BLOCK_LEN,
                              self->chunk_counter, flags);
            self->blocks_compressed++;
            self->buf_len = 0;
            memset(self->buf, 0, BLOCK_LEN);
        }
        size_t want = BLOCK_LEN - self->buf_len;
        size_t take = want < input_len ? want : input_len;
        memcpy(self->buf + self->buf_len, input, take);
        self->buf_len += (uint8_t)take;
        input     += take;
        input_len -= take;
    }
}

static void chunk_state_output(const blake3_chunk_state *self,
                                uint8_t out_cv[32])
{
    uint8_t flags = self->flags | chunk_state_start_flag(self) | CHUNK_END;
    uint8_t xof[64];
    compress_xof(self->cv, self->buf, self->buf_len, self->chunk_counter,
                 flags, xof);
    memcpy(out_cv, xof, 32);
}

static void parent_cv(const uint8_t left_cv[32], const uint8_t right_cv[32],
                      const uint32_t key[8], uint8_t flags, uint8_t out[32])
{
    uint8_t block[BLOCK_LEN];
    memcpy(block,      left_cv,  32);
    memcpy(block + 32, right_cv, 32);

    uint32_t cv[8];
    memcpy(cv, key, KEY_LEN);
    compress_in_place(cv, block, BLOCK_LEN, 0, PARENT | flags);
    for (int i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(cv[i]);
        out[4 * i + 1] = (uint8_t)(cv[i] >> 8);
        out[4 * i + 2] = (uint8_t)(cv[i] >> 16);
        out[4 * i + 3] = (uint8_t)(cv[i] >> 24);
    }
}

static void parent_output_xof(const uint8_t left_cv[32],
                               const uint8_t right_cv[32],
                               const uint32_t key[8], uint8_t flags,
                               uint8_t out[64])
{
    uint8_t block[BLOCK_LEN];
    memcpy(block,      left_cv,  32);
    memcpy(block + 32, right_cv, 32);
    compress_xof(key, block, BLOCK_LEN, 0, PARENT | ROOT | flags, out);
}

static void hasher_push_cv(blake3_hasher *self, uint8_t new_cv[32],
                           uint64_t chunk_counter)
{
    while (chunk_counter & 1) {
        size_t sp = (size_t)(self->cv_stack_len - 1);
        uint8_t out_cv[32];
        parent_cv(self->cv_stack[sp], new_cv, self->key, self->chunk.flags, out_cv);
        memcpy(new_cv, out_cv, 32);
        self->cv_stack_len--;
        chunk_counter >>= 1;
    }
    memcpy(self->cv_stack[self->cv_stack_len], new_cv, 32);
    self->cv_stack_len++;
}

void blake3_hasher_init(blake3_hasher *self)
{
    memcpy(self->key, IV, KEY_LEN);
    chunk_state_init(&self->chunk, IV, 0, 0);
    self->cv_stack_len = 0;
}

void blake3_hasher_update(blake3_hasher *self, const void *input,
                          size_t input_len)
{
    const uint8_t *in = (const uint8_t *)input;

    while (input_len > 0) {
        if (chunk_state_len(&self->chunk) == CHUNK_LEN) {
            uint8_t chunk_cv[32];
            chunk_state_output(&self->chunk, chunk_cv);
            uint64_t counter = self->chunk.chunk_counter;
            hasher_push_cv(self, chunk_cv, counter);
            chunk_state_init(&self->chunk, self->key,
                             counter + 1, self->chunk.flags);
        }
        size_t want  = CHUNK_LEN - chunk_state_len(&self->chunk);
        size_t take  = want < input_len ? want : input_len;
        chunk_state_update(&self->chunk, in, take);
        in        += take;
        input_len -= take;
    }
}

void blake3_hasher_finalize(const blake3_hasher *self, uint8_t *out,
                            size_t out_len)
{
    if (out_len == 0) return;

    if (self->cv_stack_len == 0) {
        uint8_t flags = self->chunk.flags
                      | chunk_state_start_flag(&self->chunk)
                      | CHUNK_END | ROOT;
        uint8_t xof[64];
        compress_xof(self->chunk.cv, self->chunk.buf, self->chunk.buf_len,
                     self->chunk.chunk_counter, flags, xof);
        size_t n = out_len < 64 ? out_len : 64;
        memcpy(out, xof, n);
        return;
    }

    uint8_t cur[32];
    chunk_state_output(&self->chunk, cur);

    int sp = (int)self->cv_stack_len - 1;
    while (sp > 0) {
        uint8_t tmp[32];
        parent_cv(self->cv_stack[sp], cur, self->key, self->chunk.flags, tmp);
        memcpy(cur, tmp, 32);
        sp--;
    }

    uint8_t xof[64];
    parent_output_xof(self->cv_stack[0], cur, self->key,
                      self->chunk.flags, xof);
    size_t n = out_len < 64 ? out_len : 64;
    memcpy(out, xof, n);
}
