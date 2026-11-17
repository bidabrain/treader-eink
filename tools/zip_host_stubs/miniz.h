/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 仅为主机测试把 tinfl 适配到 zlib：ZIP 用的单次 raw inflate，以及 PNG 流式解码用的分段 zlib inflate。
 * Adapt tinfl to zlib for host tests only: one-shot raw inflate for ZIP, and chunked zlib inflate for streaming PNG.
 * 冻结：不替换 ZIP 解析，不用于验证 ROM 实现。/ Frozen: Do not replace ZIP parsing or claim ROM validation.
 */
#pragma once
#include <stddef.h>
#include <string.h>
#include <zlib.h>
typedef unsigned char mz_uint8;
typedef unsigned int mz_uint32;
typedef struct { int m_state; z_stream stream; } tinfl_decompressor;
typedef enum {
    TINFL_STATUS_FAILED = -1, TINFL_STATUS_DONE = 0, TINFL_STATUS_NEEDS_MORE_INPUT = 1, TINFL_STATUS_HAS_MORE_OUTPUT = 2
} tinfl_status;
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_HAS_MORE_INPUT 2
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
#define TINFL_LZ_DICT_SIZE 32768
#define tinfl_init(r) ((r)->m_state = 0)
static inline tinfl_status tinfl_decompress(tinfl_decompressor* state, const mz_uint8* input,
    size_t* in_size, mz_uint8* start, mz_uint8* output, size_t* out_size, mz_uint32 flags) {
    (void)start;
    if (!(flags & TINFL_FLAG_PARSE_ZLIB_HEADER)) {
        // ZIP：单次解完整条 raw deflate。/ ZIP: one-shot raw deflate of a whole entry.
        z_stream stream;
        memset(&stream, 0, sizeof(stream));
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return TINFL_STATUS_FAILED;
        unsigned char empty;
        stream.next_in = (Bytef*)input;
        stream.avail_in = (uInt)*in_size;
        stream.next_out = *out_size ? output : &empty;
        stream.avail_out = *out_size ? (uInt)*out_size : 1;
        int result = inflate(&stream, Z_FINISH);
        *in_size = stream.total_in; *out_size = stream.total_out;
        inflateEnd(&stream);
        return result == Z_STREAM_END ? TINFL_STATUS_DONE : TINFL_STATUS_FAILED;
    }
    // PNG：带 zlib 头的分段输入，状态留在 state 里。/ PNG: chunked zlib input with state kept in state.
    if (state->m_state == 0) {
        memset(&state->stream, 0, sizeof(state->stream));
        if (inflateInit(&state->stream) != Z_OK) return TINFL_STATUS_FAILED;
        state->m_state = 1;
    }
    z_stream* z = &state->stream;
    z->next_in = (Bytef*)input; z->avail_in = (uInt)*in_size;
    z->next_out = output; z->avail_out = (uInt)*out_size;
    int result = inflate(z, Z_NO_FLUSH);
    *in_size -= z->avail_in; *out_size -= z->avail_out;
    if (result == Z_STREAM_END) { inflateEnd(z); state->m_state = 2; return TINFL_STATUS_DONE; }
    if (result != Z_OK && result != Z_BUF_ERROR) { inflateEnd(z); state->m_state = 2; return TINFL_STATUS_FAILED; }
    if (z->avail_out == 0) return TINFL_STATUS_HAS_MORE_OUTPUT;
    if (z->avail_in == 0 && (flags & TINFL_FLAG_HAS_MORE_INPUT)) return TINFL_STATUS_NEEDS_MORE_INPUT;
    inflateEnd(z); state->m_state = 2;
    return TINFL_STATUS_FAILED;
}
