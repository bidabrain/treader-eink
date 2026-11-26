// 主机测试桩：够用的 Base64 编码。/ Host-test stub: a Base64 encoder that is enough here.
#pragma once
#include <stddef.h>
static inline int mbedtls_base64_encode(unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src, size_t slen) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        unsigned v = (unsigned)src[i] << 16 | (i + 1 < slen ? (unsigned)src[i + 1] << 8 : 0) | (i + 2 < slen ? src[i + 2] : 0);
        if (o + 4 >= dlen) return -1;
        dst[o++] = (unsigned char)T[v >> 18 & 63];
        dst[o++] = (unsigned char)T[v >> 12 & 63];
        dst[o++] = i + 1 < slen ? (unsigned char)T[v >> 6 & 63] : '=';
        dst[o++] = i + 2 < slen ? (unsigned char)T[v & 63] : '=';
    }
    dst[o] = 0;
    *olen = o;
    return 0;
}
