/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：read_pico_wifi_vault.h 的实现。文件格式（小端）：
 *   0  "RPWF"  4  版本 1  5  个数 u8  6  GCM nonce 12 字节
 *   18 密文（每个网络 102 字节：网络名 33、密码 65、last_ok u32）  然后 16 字节 GCM 标签；前 18 字节是附加数据（AAD）。
 * 文件钥匙 = HKDF-SHA256(设备钥匙, 盐 = 空, info = "rp-wifi-v1")。每次写入换一个随机 nonce。
 * 写入先写 .tmp，再把正式文件换成 .bak，改名后删掉 .bak。读的时候按 正式 → .tmp → .bak 找第一个能解开的。
 *
 * English: implementation of read_pico_wifi_vault.h. Layout (little endian): magic "RPWF", version 1, count u8,
 * 12-byte GCM nonce, ciphertext (102 bytes per network: name 33, password 65, last_ok u32), then the 16-byte GCM tag;
 * the first 18 bytes are the AAD. File key = HKDF-SHA256(device key, empty salt, info "rp-wifi-v1"), with a fresh
 * random nonce on every write. Writes go to .tmp, the current file becomes .bak, the rename follows and .bak is
 * deleted. Reads take the first of file, .tmp, .bak that decrypts.
 */

#include "read_pico_wifi_vault.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psa/crypto.h"

#define HDR 18
#define TAG 16
#define ENTRY 102
#define FILE_MAX (HDR + WIFI_VAULT_MAX * ENTRY + TAG)
static const uint8_t MAGIC[4] = {'R', 'P', 'W', 'F'};
static const char INFO[] = "rp-wifi-v1";

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void path_of(char* out, size_t cap, const char* path, const char* ext) {
    snprintf(out, cap, "%s%s", path, ext);
}

void wifi_vault_wipe(void* p, size_t n) {
    volatile uint8_t* b = p;
    while (n--) *b++ = 0;
}

/* ---- 列表 / List ---- */

bool wifi_vault_valid(const char* ssid, const char* password) {
    if (!ssid || !password) return false;
    const size_t ssid_len = strnlen(ssid, 33), pass_len = strnlen(password, 65);
    if (!ssid_len || ssid_len > 32 || pass_len > 64 || (pass_len && pass_len < 8)) return false;
    for (size_t i = 0; i < ssid_len; ++i)
        if ((unsigned char)ssid[i] < 32 || (unsigned char)ssid[i] == 127) return false;
    for (size_t i = 0; i < pass_len; ++i) {
        const unsigned char c = (unsigned char)password[i];
        if (c < 32 || c > 126) return false;
        // 64 个字符的是十六进制 PSK。/ Sixty-four characters is a hex PSK.
        if (pass_len == 64 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

int wifi_vault_find(const wifi_vault_t* v, const char* ssid) {
    if (!v || !ssid) return -1;
    for (int i = 0; i < v->count; ++i)
        if (!strcmp(v->items[i].ssid, ssid)) return i;
    return -1;
}

bool wifi_vault_put(wifi_vault_t* v, const char* ssid, const char* password, uint32_t last_ok) {
    if (!v || v->count < 0 || v->count > WIFI_VAULT_MAX || !wifi_vault_valid(ssid, password)) return false;
    int i = wifi_vault_find(v, ssid);
    if (i < 0 && v->count < WIFI_VAULT_MAX) i = v->count++;
    else if (i < 0) {
        i = 0;
        for (int j = 1; j < v->count; ++j)
            if (v->items[j].last_ok < v->items[i].last_ok) i = j;
        wifi_vault_wipe(&v->items[i], sizeof(v->items[i]));
    }
    wifi_vault_entry_t* e = &v->items[i];
    const uint32_t old = !strcmp(e->ssid, ssid) ? e->last_ok : 0;
    wifi_vault_wipe(e, sizeof(*e));
    strcpy(e->ssid, ssid);
    strcpy(e->password, password);
    e->last_ok = last_ok ? last_ok : old;
    return true;
}

bool wifi_vault_remove(wifi_vault_t* v, const char* ssid) {
    const int i = wifi_vault_find(v, ssid);
    if (i < 0) return false;
    memmove(&v->items[i], &v->items[i + 1], (size_t)(v->count - i - 1) * sizeof(v->items[0]));
    v->count--;
    wifi_vault_wipe(&v->items[v->count], sizeof(v->items[0]));
    return true;
}

void wifi_vault_sort(wifi_vault_t* v) {
    // 最多 16 个，插入排序就够；时间相同保持原顺序。/ At most 16, so insertion sort; ties keep their order.
    for (int i = 1; i < v->count; ++i) {
        wifi_vault_entry_t e = v->items[i];
        int j = i;
        while (j > 0 && v->items[j - 1].last_ok < e.last_ok) {
            v->items[j] = v->items[j - 1];
            --j;
        }
        v->items[j] = e;
        wifi_vault_wipe(&e, sizeof(e));
    }
}

/* ---- 密码学 / Crypto ---- */

static bool ready(void) {
    // 重复调用无害。/ Repeated calls are harmless.
    return psa_crypto_init() == PSA_SUCCESS;
}

bool wifi_vault_random(uint8_t* out, size_t n) {
    if (ready() && psa_generate_random(out, n) == PSA_SUCCESS) return true;
    // 拿不到随机数时不给出可预测的数。/ Never hand out predictable bytes.
    memset(out, 0, n);
    return false;
}

wifi_vault_err_t wifi_vault_derive(const uint8_t dev[WIFI_VAULT_KEY_LEN], uint8_t key[WIFI_VAULT_KEY_LEN]) {
    if (!dev || !key) return WIFI_VAULT_ERR_ARG;
    if (!ready()) return WIFI_VAULT_ERR_CRYPTO;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, NULL, 0);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, dev, WIFI_VAULT_KEY_LEN);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, (const uint8_t*)INFO,
                                            sizeof(INFO) - 1);
    if (st == PSA_SUCCESS) st = psa_key_derivation_output_bytes(&op, key, WIFI_VAULT_KEY_LEN);
    psa_key_derivation_abort(&op);
    if (st != PSA_SUCCESS) wifi_vault_wipe(key, WIFI_VAULT_KEY_LEN);
    return st == PSA_SUCCESS ? WIFI_VAULT_OK : WIFI_VAULT_ERR_CRYPTO;
}

static psa_key_id_t aes_key(const uint8_t key[WIFI_VAULT_KEY_LEN]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_key_id_t id = 0;
    return psa_import_key(&attr, key, WIFI_VAULT_KEY_LEN, &id) == PSA_SUCCESS ? id : 0;
}

/* ---- 文件 / Files ---- */

// 一个文件解成列表；buf 是整个文件。/ Decode one whole file into the list.
static wifi_vault_err_t decode(const uint8_t key[WIFI_VAULT_KEY_LEN], const uint8_t* buf, size_t size,
                               wifi_vault_t* out) {
    if (size < HDR + TAG || memcmp(buf, MAGIC, 4) || buf[4] != 1 || buf[5] > WIFI_VAULT_MAX ||
        size != HDR + (size_t)buf[5] * ENTRY + TAG)
        return WIFI_VAULT_ERR_FORMAT;
    const size_t len = (size_t)buf[5] * ENTRY;
    if (!ready()) return WIFI_VAULT_ERR_CRYPTO;
    // 放堆上：后台任务的栈不大。/ On the heap: background task stacks are small.
    uint8_t* plain = malloc(WIFI_VAULT_MAX * ENTRY);
    if (!plain) return WIFI_VAULT_ERR_IO;
    const psa_key_id_t id = aes_key(key);
    size_t n = 0;
    const psa_status_t st = !id ? PSA_ERROR_GENERIC_ERROR :
        psa_aead_decrypt(id, PSA_ALG_GCM, buf + 6, 12, buf, HDR, buf + HDR, len + TAG, plain, WIFI_VAULT_MAX * ENTRY, &n);
    if (id) psa_destroy_key(id);
    if (st != PSA_SUCCESS || n != len) {
        wifi_vault_wipe(plain, WIFI_VAULT_MAX * ENTRY);
        free(plain);
        return st == PSA_ERROR_INVALID_SIGNATURE ? WIFI_VAULT_ERR_AUTH : WIFI_VAULT_ERR_CRYPTO;
    }
    memset(out, 0, sizeof(*out));
    wifi_vault_err_t err = WIFI_VAULT_OK;
    for (int i = 0; i < buf[5]; ++i) {
        const uint8_t* p = plain + i * ENTRY;
        wifi_vault_entry_t* e = &out->items[i];
        memcpy(e->ssid, p, 33);
        memcpy(e->password, p + 33, 65);
        e->last_ok = get32(p + 98);
        // 解开了但内容不对（带结尾的字符串、重名）也算坏文件。/ Decrypted but malformed (unterminated text, a
        // duplicate name) still counts as a bad file.
        if (e->ssid[32] || e->password[64] || !wifi_vault_valid(e->ssid, e->password) ||
            wifi_vault_find(out, e->ssid) >= 0) {
            err = WIFI_VAULT_ERR_FORMAT;
            break;
        }
        out->count = i + 1;
    }
    wifi_vault_wipe(plain, WIFI_VAULT_MAX * ENTRY);
    free(plain);
    if (err != WIFI_VAULT_OK) wifi_vault_wipe(out, sizeof(*out));
    return err;
}

static wifi_vault_err_t load_one(const char* p, const uint8_t key[WIFI_VAULT_KEY_LEN], wifi_vault_t* out) {
    FILE* f = fopen(p, "rb");
    if (!f) return errno == ENOENT ? WIFI_VAULT_ERR_NOENT : WIFI_VAULT_ERR_IO;
    uint8_t* buf = malloc(FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return WIFI_VAULT_ERR_IO;
    }
    const size_t got = fread(buf, 1, FILE_MAX + 1, f);
    const bool bad = ferror(f);
    fclose(f);
    const wifi_vault_err_t err = bad ? WIFI_VAULT_ERR_IO : got > FILE_MAX ? WIFI_VAULT_ERR_FORMAT : decode(key, buf, got, out);
    wifi_vault_wipe(buf, FILE_MAX + 1);
    free(buf);
    return err;
}

wifi_vault_err_t wifi_vault_load(const char* path, const uint8_t key[WIFI_VAULT_KEY_LEN], wifi_vault_t* out) {
    static const char* const EXT[] = {"", ".tmp", ".bak"};
    if (!path || !key || !out) return WIFI_VAULT_ERR_ARG;
    wifi_vault_err_t best = WIFI_VAULT_ERR_NOENT;
    for (int i = 0; i < 3; i++) {
        char p[160];
        path_of(p, sizeof(p), path, EXT[i]);
        const wifi_vault_err_t err = load_one(p, key, out);
        if (err == WIFI_VAULT_OK) return WIFI_VAULT_OK;
        // 记下最有用的错误：钥匙不对优先，其次损坏、读写失败。/ Keep the most useful error: auth, then format, then I/O.
        if (err == WIFI_VAULT_ERR_AUTH || (best != WIFI_VAULT_ERR_AUTH && err != WIFI_VAULT_ERR_NOENT)) best = err;
    }
    wifi_vault_wipe(out, sizeof(*out));
    return best;
}

wifi_vault_err_t wifi_vault_save(const char* path, const uint8_t key[WIFI_VAULT_KEY_LEN], const wifi_vault_t* v) {
    if (!path || !key || !v || v->count < 0 || v->count > WIFI_VAULT_MAX) return WIFI_VAULT_ERR_ARG;
    uint8_t* plain = calloc(1, WIFI_VAULT_MAX * ENTRY);
    uint8_t* file = malloc(FILE_MAX);
    if (!plain || !file) {
        free(plain);
        free(file);
        return WIFI_VAULT_ERR_IO;
    }
    for (int i = 0; i < v->count; ++i) {
        uint8_t* p = plain + i * ENTRY;
        memcpy(p, v->items[i].ssid, 33);
        memcpy(p + 33, v->items[i].password, 65);
        put32(p + 98, v->items[i].last_ok);
    }
    const size_t len = (size_t)v->count * ENTRY;
    memcpy(file, MAGIC, 4);
    file[4] = 1;
    file[5] = (uint8_t)v->count;
    wifi_vault_err_t err = wifi_vault_random(file + 6, 12) && ready() ? WIFI_VAULT_OK : WIFI_VAULT_ERR_CRYPTO;
    if (err == WIFI_VAULT_OK) {
        const psa_key_id_t id = aes_key(key);
        size_t n = 0;
        if (!id || psa_aead_encrypt(id, PSA_ALG_GCM, file + 6, 12, file, HDR, plain, len, file + HDR,
                                    FILE_MAX - HDR, &n) != PSA_SUCCESS || n != len + TAG)
            err = WIFI_VAULT_ERR_CRYPTO;
        if (id) psa_destroy_key(id);
    }
    wifi_vault_wipe(plain, WIFI_VAULT_MAX * ENTRY);
    free(plain);
    char tmp[160], bak[160];
    path_of(tmp, sizeof(tmp), path, ".tmp");
    path_of(bak, sizeof(bak), path, ".bak");
    if (err == WIFI_VAULT_OK) {
        FILE* f = fopen(tmp, "wb");
        if (!f) err = WIFI_VAULT_ERR_IO;
        else {
            const size_t total = HDR + len + TAG;
            if (fwrite(file, 1, total, f) != total || fflush(f) || fsync(fileno(f))) err = WIFI_VAULT_ERR_IO;
            if (fclose(f)) err = WIFI_VAULT_ERR_IO;
            if (err != WIFI_VAULT_OK) unlink(tmp);
        }
    }
    free(file);
    if (err != WIFI_VAULT_OK) return err;
    // FAT 的改名不能覆盖：先腾出 .bak，再把正式文件挪过去。/ FAT rename cannot overwrite: clear .bak, then move the file.
    struct stat st;
    if (stat(path, &st) == 0) {
        unlink(bak);
        if (rename(path, bak)) return WIFI_VAULT_ERR_IO;
    }
    if (rename(tmp, path)) return WIFI_VAULT_ERR_IO;
    unlink(bak);
    return WIFI_VAULT_OK;
}
