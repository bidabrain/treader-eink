/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证码计算（RFC 4226 截断、RFC 6238 时间步）、base32 和账号表操作。
 *
 * English: code math (RFC 4226 truncation, RFC 6238 time steps), base32 and account-table operations.
 */

#include <stdio.h>
#include <string.h>

#include "read_pico_totp.h"
#include "totp_crypto.h"

/* ---- 验证码 / Codes ---- */

bool totp_account_valid(const totp_account_t* a) {
    return a && a->secret_len >= 1 && a->secret_len <= TOTP_SECRET_MAX && a->algo <= TOTP_SHA512 &&
           (a->digits == 6 || a->digits == 8) && (a->period == 30 || a->period == 60) && a->name[0] &&
           strlen(a->name) <= TOTP_TEXT_MAX && strlen(a->issuer) <= TOTP_TEXT_MAX;
}

totp_err_t totp_code(const totp_account_t* a, int64_t unix_time, uint32_t* out) {
    if (!totp_account_valid(a) || !out || unix_time < 0) return TOTP_ERR_ARG;
    uint64_t counter = (uint64_t)unix_time / a->period;
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (uint8_t)counter;
        counter >>= 8;
    }
    uint8_t h[64];
    size_t n = 0;
    const totp_err_t err = totp_hmac((totp_algo_t)a->algo, a->secret, a->secret_len, msg, sizeof(msg), h, &n);
    if (err != TOTP_OK || n < 20) {
        totp_wipe(h, sizeof(h));
        return err != TOTP_OK ? err : TOTP_ERR_CRYPTO;
    }
    const int off = h[n - 1] & 0x0f;
    const uint32_t bin = ((uint32_t)(h[off] & 0x7f) << 24) | ((uint32_t)h[off + 1] << 16) |
                         ((uint32_t)h[off + 2] << 8) | h[off + 3];
    totp_wipe(h, sizeof(h));
    *out = bin % (a->digits == 8 ? 100000000u : 1000000u);
    return TOTP_OK;
}

void totp_format(uint32_t code, int digits, char* out, size_t cap) {
    if (digits == 8) snprintf(out, cap, "%04u %04u", (unsigned)(code / 10000), (unsigned)(code % 10000));
    else snprintf(out, cap, "%03u %03u", (unsigned)(code / 1000 % 1000), (unsigned)(code % 1000));
}

int totp_remaining(const totp_account_t* a, int64_t unix_time) {
    const int p = a && a->period ? a->period : 30;
    if (unix_time < 0) return p;
    return p - (int)(unix_time % p);
}

bool totp_base32_decode(const char* in, uint8_t* out, size_t cap, size_t* out_len) {
    if (!in || !out || !out_len) return false;
    uint32_t buf = 0;
    int bits = 0;
    size_t n = 0;
    for (const char* p = in; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '-' || c == '=' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return false;
        buf = (buf << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return false;
            out[n++] = (uint8_t)(buf >> bits);
        }
    }
    buf = 0;
    *out_len = n;
    return n > 0;
}

/* ---- 账号表 / Table ---- */

static bool same(const totp_account_t* a, const totp_account_t* b) {
    return !strcmp(a->issuer, b->issuer) && !strcmp(a->name, b->name) && a->secret_len == b->secret_len &&
           !memcmp(a->secret, b->secret, a->secret_len);
}

bool totp_table_add(totp_table_t* t, const totp_account_t* a, bool* dup) {
    if (dup) *dup = false;
    if (!t || !totp_account_valid(a)) return false;
    for (int i = 0; i < t->count; i++)
        if (same(&t->items[i], a)) {
            if (dup) *dup = true;
            return false;
        }
    if (t->count >= TOTP_MAX_ACCOUNTS) return false;
    totp_account_t* d = &t->items[t->count++];
    *d = *a;
    if (!t->next_id) t->next_id = 1;
    d->id = t->next_id++;
    return true;
}

int totp_table_find(const totp_table_t* t, uint32_t id) {
    for (int i = 0; t && i < t->count; i++)
        if (t->items[i].id == id) return i;
    return -1;
}

void totp_table_remove(totp_table_t* t, int i) {
    if (!t || i < 0 || i >= t->count) return;
    memmove(&t->items[i], &t->items[i + 1], (size_t)(t->count - i - 1) * sizeof(t->items[0]));
    t->count--;
    totp_wipe(&t->items[t->count], sizeof(t->items[0]));
}

void totp_table_move(totp_table_t* t, int from, int to) {
    if (!t || from < 0 || from >= t->count) return;
    if (to < 0) to = 0;
    if (to >= t->count) to = t->count - 1;
    if (from == to) return;
    totp_account_t tmp = t->items[from];
    if (from < to) memmove(&t->items[from], &t->items[from + 1], (size_t)(to - from) * sizeof(tmp));
    else memmove(&t->items[to + 1], &t->items[to], (size_t)(from - to) * sizeof(tmp));
    t->items[to] = tmp;
    totp_wipe(&tmp, sizeof(tmp));
}

void totp_wipe(void* p, size_t n) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n--) *v++ = 0;
}
