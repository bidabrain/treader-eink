/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡上的加密文件。格式（小端）：
 *   0  "TRVA"  4  版本 1  5  KDF 1（PBKDF2-HMAC-SHA256）  6  保留 2 字节
 *   8  迭代次数 u32  12  盐 16 字节  28  GCM nonce 12 字节  40  明文长度 u32
 *   44 密文  然后 16 字节 GCM 标签；前 44 字节是附加数据（AAD）。
 * 钥匙 = HKDF-SHA256(PBKDF2(口令, 盐, 迭代), 盐 = 设备随机数, info = "treader-totp-v1")。
 * 明文：版本 u8、next_id u32、个数 u16，再逐个：id u32、算法、位数、周期、密钥长度各 u8、密钥、
 * 发行方长度 u8 + 文字、名称长度 u8 + 文字。
 * 写入先写 .tmp，再把正式文件换成 .bak，最后改名。读的时候按 正式 → .tmp → .bak 找第一个能解开的
 * （GCM 标签保证文件完整），只用盐相同的文件，所以换口令后旧口令解不开任何文件。
 *
 * English: the encrypted file on the TF card. Layout (little endian): magic "TRVA", version 1, KDF 1, 2 reserved
 * bytes, iterations u32, 16-byte salt, 12-byte nonce, plaintext length u32, ciphertext, 16-byte GCM tag; the first 44
 * bytes are the AAD. Key = HKDF-SHA256(PBKDF2(pass, salt, iters), salt = device secret, info "treader-totp-v1").
 * Plaintext: version u8, next_id u32, count u16, then per account id u32, algo, digits, period, secret length u8 each,
 * secret, issuer length u8 + text, name length u8 + text. Writes go to .tmp, the current file becomes .bak, then the
 * rename. Reads take the first of file, .tmp, .bak that decrypts (the GCM tag proves it is whole), only among files
 * with the same salt, so after a passphrase change the old one opens nothing.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "read_pico_totp.h"
#include "totp_crypto.h"

#define HDR 44
#define TAG 16
#define PLAIN_MAX (7 + TOTP_MAX_ACCOUNTS * (8 + TOTP_SECRET_MAX + 2 + 2 * TOTP_TEXT_MAX))
static const uint8_t MAGIC[4] = {'T', 'R', 'V', 'A'};
static const char INFO[] = "treader-totp-v1";

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void path_of(char* out, size_t cap, const char* path, const char* ext) {
    snprintf(out, cap, "%s%s", path, ext);
}

/* ---- 明文编码 / Plaintext ---- */

static size_t encode(const totp_table_t* t, uint8_t* p) {
    size_t n = 0;
    p[n++] = 1;
    put32(p + n, t->next_id); n += 4;
    p[n++] = (uint8_t)t->count; p[n++] = (uint8_t)(t->count >> 8);
    for (int i = 0; i < t->count; i++) {
        const totp_account_t* a = &t->items[i];
        put32(p + n, a->id); n += 4;
        p[n++] = a->algo; p[n++] = a->digits; p[n++] = a->period; p[n++] = a->secret_len;
        memcpy(p + n, a->secret, a->secret_len); n += a->secret_len;
        const size_t il = strlen(a->issuer), nl = strlen(a->name);
        p[n++] = (uint8_t)il; memcpy(p + n, a->issuer, il); n += il;
        p[n++] = (uint8_t)nl; memcpy(p + n, a->name, nl); n += nl;
    }
    return n;
}

static bool decode(const uint8_t* p, size_t len, totp_table_t* t) {
    memset(t, 0, sizeof(*t));
    if (len < 7 || p[0] != 1) return false;
    size_t n = 1;
    t->next_id = get32(p + n); n += 4;
    const int count = p[n] | (p[n + 1] << 8); n += 2;
    if (count > TOTP_MAX_ACCOUNTS) return false;
    for (int i = 0; i < count; i++) {
        totp_account_t* a = &t->items[i];
        if (n + 8 > len) return false;
        a->id = get32(p + n); n += 4;
        a->algo = p[n++]; a->digits = p[n++]; a->period = p[n++]; a->secret_len = p[n++];
        if (a->secret_len > TOTP_SECRET_MAX || n + a->secret_len + 1 > len) return false;
        memcpy(a->secret, p + n, a->secret_len); n += a->secret_len;
        size_t l = p[n++];
        if (l > TOTP_TEXT_MAX || n + l + 1 > len) return false;
        memcpy(a->issuer, p + n, l); a->issuer[l] = 0; n += l;
        l = p[n++];
        if (l > TOTP_TEXT_MAX || n + l > len) return false;
        memcpy(a->name, p + n, l); a->name[l] = 0; n += l;
        if (!totp_account_valid(a)) return false;
        t->count = i + 1;
    }
    return n == len;
}

/* ---- 钥匙 / Keys ---- */

totp_err_t totp_vault_derive(const char* pass, const uint8_t dev[TOTP_DEV_SECRET_LEN], totp_key_t* k) {
    if (!pass || !dev || !k || k->iters < 1) return TOTP_ERR_ARG;
    uint8_t pk[32];
    totp_err_t err = totp_pbkdf2((const uint8_t*)pass, strlen(pass), k->salt, TOTP_SALT_LEN, k->iters, pk);
    if (err == TOTP_OK)
        err = totp_hkdf(pk, sizeof(pk), dev, TOTP_DEV_SECRET_LEN, (const uint8_t*)INFO, sizeof(INFO) - 1, k->key);
    totp_wipe(pk, sizeof(pk));
    return err;
}

totp_err_t totp_key_new(const char* pass, const uint8_t dev[TOTP_DEV_SECRET_LEN], uint32_t iters, totp_key_t* k) {
    if (!k) return TOTP_ERR_ARG;
    memset(k, 0, sizeof(*k));
    if (!totp_random(k->salt, TOTP_SALT_LEN)) return TOTP_ERR_CRYPTO;
    k->iters = iters < TOTP_MIN_ITERS ? TOTP_MIN_ITERS : iters;
    return totp_vault_derive(pass, dev, k);
}

int64_t totp_pbkdf2_time(uint32_t n, int64_t (*now_us)(void)) {
    static const uint8_t salt[TOTP_SALT_LEN] = {0};
    uint8_t out[32];
    const int64_t t0 = now_us();
    const totp_err_t err = totp_pbkdf2((const uint8_t*)"calibrate", 9, salt, sizeof(salt), n, out);
    const int64_t dt = now_us() - t0;
    return err == TOTP_OK ? dt : -1;
}

/* ---- 文件 / Files ---- */

// 读整个文件；返回字节数，失败返回负的错误码。/ Read a whole file; bytes read, or a negative error code.
static long read_file(const char* path, uint8_t** out) {
    *out = NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? -TOTP_ERR_NOENT : -TOTP_ERR_IO;
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size < HDR + TAG || st.st_size > HDR + PLAIN_MAX + TAG) {
        fclose(f);
        return -TOTP_ERR_FORMAT;
    }
    uint8_t* buf = malloc((size_t)st.st_size);
    if (!buf) {
        fclose(f);
        return -TOTP_ERR_NOMEM;
    }
    const size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) {
        free(buf);
        return -TOTP_ERR_IO;
    }
    *out = buf;
    return (long)got;
}

static totp_err_t check_header(const uint8_t* h, long size) {
    // 只认第 1 版、KDF 1。/ Only version 1 with KDF 1 is known.
    if (memcmp(h, MAGIC, 4) || h[4] != 1 || h[5] != 1) return TOTP_ERR_FORMAT;
    if (get32(h + 8) < 1 || (long)get32(h + 40) + HDR + TAG != size) return TOTP_ERR_FORMAT;
    return TOTP_OK;
}

totp_err_t totp_vault_header(const char* path, uint8_t salt[TOTP_SALT_LEN], uint32_t* iters) {
    static const char* const EXT[] = {"", ".tmp", ".bak"};
    totp_err_t last = TOTP_ERR_NOENT;
    for (int i = 0; i < 3; i++) {
        char p[160];
        path_of(p, sizeof(p), path, EXT[i]);
        uint8_t* buf;
        const long n = read_file(p, &buf);
        if (n < 0) {
            if (-n != TOTP_ERR_NOENT) last = (totp_err_t)-n;
            continue;
        }
        const totp_err_t err = check_header(buf, n);
        if (err == TOTP_OK) {
            memcpy(salt, buf + 12, TOTP_SALT_LEN);
            *iters = get32(buf + 8);
        }
        free(buf);
        if (err == TOTP_OK) return TOTP_OK;
        last = err;
    }
    return last;
}

static totp_err_t open_one(const char* p, const totp_key_t* k, totp_table_t* out) {
    uint8_t* buf;
    const long n = read_file(p, &buf);
    if (n < 0) return (totp_err_t)-n;
    totp_err_t err = check_header(buf, n);
    if (err == TOTP_OK && (memcmp(buf + 12, k->salt, TOTP_SALT_LEN) || get32(buf + 8) != k->iters))
        err = TOTP_ERR_AUTH;  // 别的口令写的 / Written under another passphrase
    if (err == TOTP_OK) {
        const size_t len = get32(buf + 40);
        uint8_t* plain = malloc(len ? len : 1);
        if (!plain) err = TOTP_ERR_NOMEM;
        else {
            err = totp_gcm_open(k->key, buf + 28, buf, HDR, buf + HDR, len, plain);
            if (err == TOTP_OK && !decode(plain, len, out)) err = TOTP_ERR_FORMAT;
            totp_wipe(plain, len);
            free(plain);
        }
    }
    free(buf);
    return err;
}

totp_err_t totp_vault_load(const char* path, const totp_key_t* k, totp_table_t* out) {
    static const char* const EXT[] = {"", ".tmp", ".bak"};
    if (!path || !k || !out) return TOTP_ERR_ARG;
    totp_err_t best = TOTP_ERR_NOENT;
    for (int i = 0; i < 3; i++) {
        char p[160];
        path_of(p, sizeof(p), path, EXT[i]);
        const totp_err_t err = open_one(p, k, out);
        if (err == TOTP_OK) return TOTP_OK;
        // 记下最有用的错误：口令不对优先，其次损坏、读写失败。/ Keep the most useful error: auth, then format, then I/O.
        if (err == TOTP_ERR_AUTH || (best != TOTP_ERR_AUTH && err != TOTP_ERR_NOENT)) best = err;
    }
    totp_wipe(out, sizeof(*out));
    return best;
}

totp_err_t totp_vault_unlock(const char* path, const char* pass, const uint8_t dev[TOTP_DEV_SECRET_LEN],
                             totp_key_t* k, totp_table_t* out) {
    if (!path || !pass || !dev || !k || !out) return TOTP_ERR_ARG;
    memset(k, 0, sizeof(*k));
    totp_err_t err = totp_vault_header(path, k->salt, &k->iters);
    if (err == TOTP_OK) err = totp_vault_derive(pass, dev, k);
    if (err == TOTP_OK) err = totp_vault_load(path, k, out);
    if (err != TOTP_OK) totp_wipe(k, sizeof(*k));
    return err;
}

totp_err_t totp_vault_save(const char* path, const totp_key_t* k, const totp_table_t* t) {
    if (!path || !k || !t || t->count < 0 || t->count > TOTP_MAX_ACCOUNTS) return TOTP_ERR_ARG;
    uint8_t* plain = malloc(PLAIN_MAX);
    uint8_t* file = malloc(HDR + PLAIN_MAX + TAG);
    if (!plain || !file) {
        free(plain);
        free(file);
        return TOTP_ERR_NOMEM;
    }
    const size_t len = encode(t, plain);
    memset(file, 0, HDR);
    memcpy(file, MAGIC, 4);
    file[4] = 1;
    file[5] = 1;
    put32(file + 8, k->iters);
    memcpy(file + 12, k->salt, TOTP_SALT_LEN);
    put32(file + 40, (uint32_t)len);
    totp_err_t err = totp_random(file + 28, 12) ? TOTP_OK : TOTP_ERR_CRYPTO;
    if (err == TOTP_OK) err = totp_gcm_seal(k->key, file + 28, file, HDR, plain, len, file + HDR);
    totp_wipe(plain, PLAIN_MAX);
    free(plain);
    char tmp[160], bak[160];
    path_of(tmp, sizeof(tmp), path, ".tmp");
    path_of(bak, sizeof(bak), path, ".bak");
    if (err == TOTP_OK) {
        FILE* f = fopen(tmp, "wb");
        if (!f) err = TOTP_ERR_IO;
        else {
            const size_t total = HDR + len + TAG;
            if (fwrite(file, 1, total, f) != total || fflush(f) || fsync(fileno(f))) err = TOTP_ERR_IO;
            if (fclose(f)) err = TOTP_ERR_IO;
            if (err != TOTP_OK) unlink(tmp);
        }
    }
    free(file);
    if (err != TOTP_OK) return err;
    // FAT 的改名不能覆盖：先腾出 .bak，再把正式文件挪过去。/ FAT rename cannot overwrite: clear .bak, then move the file.
    struct stat st;
    if (stat(path, &st) == 0) {
        unlink(bak);
        if (rename(path, bak)) return TOTP_ERR_IO;
    }
    return rename(tmp, path) ? TOTP_ERR_IO : TOTP_OK;
}

void totp_vault_drop_backup(const char* path) {
    char p[160];
    path_of(p, sizeof(p), path, ".bak");
    unlink(p);
    path_of(p, sizeof(p), path, ".tmp");
    unlink(p);
}

void totp_vault_remove(const char* path) {
    totp_vault_drop_backup(path);
    unlink(path);
}

bool totp_vault_exists(const char* path) {
    static const char* const EXT[] = {"", ".tmp", ".bak"};
    for (int i = 0; i < 3; i++) {
        char p[160];
        struct stat st;
        path_of(p, sizeof(p), path, EXT[i]);
        if (stat(p, &st) == 0) return true;
    }
    return false;
}
