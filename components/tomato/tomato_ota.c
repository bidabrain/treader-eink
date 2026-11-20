/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在线升级的联网部分。读 GitHub 上 treader-latest 发布的升级包信息，把升级包下载到 TF 卡 update 文件夹：
 * 先写 .part，断了可以续传（同一次多次重试，换次也接着下），大小和 SHA-256 都对上才改名成正式文件。
 * 写进程序槽、切换启动和重启由系统升级页按 TF 卡升级的原流程做。
 *
 * English: Network side of the online upgrade. Read the package info of the treader-latest GitHub release and
 * download the package into the TF card's update folder: write a .part file that resumes after a break (several
 * retries per run, and across runs), renaming it into place only when size and SHA-256 match. Writing the app slot,
 * switching boot and restarting stay with the system upgrade page and its TF card flow.
 *
 * 冻结：只从这个仓库的 treader-latest 取；文件名只收 treader-<版本>-upgrade.bin；校验不对的文件删掉，绝不改名到位；
 * 不在这里写 flash。
 * Frozen: fetch only this repo's treader-latest; accept only treader-<version>-upgrade.bin names; delete a file that
 * fails verification and never rename it into place; never write flash here.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "tomato_internal.h"

#define TAG "tomato_ota"
#define RELEASE_URL "https://api.github.com/repos/bidabrain/treader-eink/releases/tags/treader-latest"
#define RELEASE_MAX (256 * 1024)
#define NAME_PREFIX "treader-"
#define NAME_SUFFIX "-upgrade.bin"
#define IO_CHUNK 4096
#define TX_BUF 2048
#define TIMEOUT_MS 20000
#define RETRY_MAX 5
#define RETRY_WAIT_MS 2000
#define PROGRESS_RESET 65536   // 一次尝试下了这么多就重新计重试次数 / An attempt this productive resets the retry count
#define SPACE_MARGIN (64 * 1024)

/* ---- 检查 / Check ---- */

// 文件名必须是 treader-<版本>-upgrade.bin，版本只含数字、字母、点、横线、下划线；版本写进 out。
// The name must be treader-<version>-upgrade.bin with a version of digits, letters, dots, dashes or underscores.
static bool parse_name(const char* name, char* version, size_t cap) {
    const size_t len = strlen(name), pre = strlen(NAME_PREFIX), suf = strlen(NAME_SUFFIX);
    if (len >= sizeof(((tomato_ota_info_t*)0)->name) || len <= pre + suf) return false;
    if (strncmp(name, NAME_PREFIX, pre) || strcmp(name + len - suf, NAME_SUFFIX)) return false;
    const size_t vlen = len - pre - suf;
    if (vlen >= cap) return false;
    for (size_t i = 0; i < vlen; i++) {
        const char ch = name[pre + i];
        if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-' && ch != '_') return false;
    }
    memcpy(version, name + pre, vlen);
    version[vlen] = 0;
    return true;
}

static int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// "sha256:<64 位十六进制>" → 32 字节。/ "sha256:<64 hex>" to 32 bytes.
static bool parse_digest(const char* text, uint8_t out[32]) {
    if (!text || strncmp(text, "sha256:", 7) || strlen(text) != 7 + 64) return false;
    for (int i = 0; i < 32; i++) {
        const int hi = hex_nibble(text[7 + 2 * i]), lo = hex_nibble(text[8 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

esp_err_t tomato_ota_check(tomato_ota_info_t* out, const volatile bool* cancel, char* msg, size_t cap) {
    memset(out, 0, sizeof(*out));
    uint8_t* body = NULL;
    size_t len = 0;
    esp_err_t err = tomato_http_get(RELEASE_URL, RELEASE_MAX, false, &body, &len, cancel);
    if (err != ESP_OK) {
        snprintf(msg, cap, "读不到线上版本信息");
        return err;
    }
    cJSON* root = cJSON_ParseWithLength((const char*)body, len);
    heap_caps_free(body);
    const cJSON* assets = root ? cJSON_GetObjectItemCaseSensitive(root, "assets") : NULL;
    err = ESP_ERR_NOT_FOUND;
    const cJSON* a;
    cJSON_ArrayForEach(a, assets) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(a, "name");
        const cJSON* size = cJSON_GetObjectItemCaseSensitive(a, "size");
        const cJSON* url = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
        const cJSON* digest = cJSON_GetObjectItemCaseSensitive(a, "digest");
        if (!cJSON_IsString(name) || !cJSON_IsNumber(size) || !cJSON_IsString(url)) continue;
        if (!parse_name(name->valuestring, out->version, sizeof(out->version))) continue;
        if (size->valuedouble <= 0 || size->valuedouble > 64.0 * 1024 * 1024) continue;
        if (strncmp(url->valuestring, "https://", 8) || strlen(url->valuestring) >= sizeof(out->url)) continue;
        snprintf(out->name, sizeof(out->name), "%s", name->valuestring);
        snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
        out->size = (uint32_t)size->valuedouble;
        out->has_sha256 = parse_digest(cJSON_IsString(digest) ? digest->valuestring : NULL, out->sha256);
        err = ESP_OK;
        break;
    }
    const bool parsed = root != NULL;
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(msg, cap, parsed ? "线上没有找到升级包" : "线上版本信息格式不对");
        return err;
    }
    ESP_LOGI(TAG, "online %s, %lu bytes, sha256 %s", out->name, (unsigned long)out->size, out->has_sha256 ? "yes" : "no");
    snprintf(msg, cap, "线上版本 %s", out->version);
    return ESP_OK;
}

/* ---- 下载 / Download ---- */

typedef struct {
    psa_hash_operation_t hash;
    FILE* f;
    uint32_t have;
    uint8_t* buf;
} dl_t;

static void hash_restart(dl_t* d) {
    psa_hash_abort(&d->hash);
    d->hash = psa_hash_operation_init();
    psa_hash_setup(&d->hash, PSA_ALG_SHA_256);
}

// 整个文件的 SHA-256；读不了返回 false。/ SHA-256 of a whole file; false if it cannot be read.
static bool hash_file(const char* path, uint8_t* buf, uint8_t out[32]) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    psa_hash_operation_t op = psa_hash_operation_init();
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (size_t n; ok && (n = fread(buf, 1, IO_CHUNK, f)) > 0;) ok = psa_hash_update(&op, buf, n) == PSA_SUCCESS;
    ok = ok && !ferror(f);
    fclose(f);
    size_t len = 0;
    ok = ok && psa_hash_finish(&op, out, 32, &len) == PSA_SUCCESS && len == 32;
    psa_hash_abort(&op);
    return ok;
}

// 把已有的 .part 读一遍续上摘要；读不了或比目标还大就从头来。
// Re-hash an existing .part to continue the digest; start over if it cannot be read or is too large.
static void resume_part(dl_t* d, const char* part, uint32_t size) {
    struct stat st;
    d->have = 0;
    hash_restart(d);
    if (stat(part, &st) != 0 || st.st_size <= 0) return;
    if ((uint64_t)st.st_size > size) { remove(part); return; }
    FILE* f = fopen(part, "rb");
    if (!f) { remove(part); return; }
    uint32_t done = 0;
    for (;;) {
        size_t n = fread(d->buf, 1, IO_CHUNK, f);
        if (!n) break;
        psa_hash_update(&d->hash, d->buf, n);
        done += (uint32_t)n;
    }
    const bool ok = !ferror(f) && done == (uint32_t)st.st_size;
    fclose(f);
    if (!ok) { remove(part); hash_restart(d); return; }
    d->have = done;
}

// 一次 HTTP 尝试：从 d->have 接着下，返回 ESP_OK 表示收全。/ One HTTP attempt from d->have; ESP_OK when complete.
static esp_err_t attempt(dl_t* d, const tomato_ota_info_t* info, const char* part, const volatile bool* cancel,
                         void (*progress)(int, int)) {
    esp_http_client_config_t hc = {
        .url = info->url,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = IO_CHUNK,
        .buffer_size_tx = TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(c, "Accept", "application/octet-stream");
    char range[40];
    if (d->have) {
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)d->have);
        esp_http_client_set_header(c, "Range", range);
    }
    esp_err_t err = ESP_FAIL;
    int status = 0;
    int64_t length = -1;
    // 下载地址会跳转到 GitHub 的文件服务器，Range 头一起带过去。/ The URL redirects to GitHub's file host, Range included.
    for (int hop = 0; hop < 5; hop++) {
        err = tomato_http_open(c, 0);
        if (err != ESP_OK) break;
        length = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400) break;
        if (esp_http_client_set_redirection(c) != ESP_OK) { err = ESP_FAIL; break; }
        esp_http_client_close(c);
    }
    if (err != ESP_OK) goto done;
    if (status == 200 && d->have) {
        // 服务器不认 Range，只能从头下。/ The server ignored Range; start over.
        ESP_LOGW(TAG, "range ignored; restarting");
        if (d->f) { fclose(d->f); d->f = NULL; }
        remove(part);
        d->have = 0;
        hash_restart(d);
    } else if (!(status == 206 && d->have) && !(status == 200 && !d->have)) {
        ESP_LOGW(TAG, "download http=%d", status);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (length > 0 && (uint64_t)length != (uint64_t)info->size - d->have) {
        ESP_LOGW(TAG, "length %lld, expected %lu", (long long)length, (unsigned long)(info->size - d->have));
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    if (!d->f) d->f = fopen(part, d->have ? "ab" : "wb");
    if (!d->f) { err = ESP_ERR_INVALID_STATE; goto done; }
    while (d->have < info->size) {
        if (*cancel) { err = ESP_ERR_TIMEOUT; goto done; }
        const int want = info->size - d->have < (uint32_t)IO_CHUNK ? (int)(info->size - d->have) : IO_CHUNK;
        int n = esp_http_client_read(c, (char*)d->buf, want);
        if (n <= 0) { err = ESP_FAIL; goto done; }   // 断开或超时 / Dropped or timed out
        if (fwrite(d->buf, 1, (size_t)n, d->f) != (size_t)n) { err = ESP_ERR_INVALID_STATE; goto done; }
        psa_hash_update(&d->hash, d->buf, (size_t)n);
        d->have += (uint32_t)n;
        if (progress) progress((int)d->have, (int)info->size);
    }
    err = ESP_OK;
done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

static void wait_or_cancel(int ms, const volatile bool* cancel) {
    for (int t = 0; t < ms && !*cancel; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t tomato_ota_download(const tomato_ota_info_t* info, const volatile bool* cancel,
                              void (*progress)(int, int), char* msg, size_t cap) {
    char version[sizeof(info->version)];
    if (!parse_name(info->name, version, sizeof(version)) || !info->size || strncmp(info->url, "https://", 8)) {
        snprintf(msg, cap, "升级包信息不对，请重新检查");
        return ESP_ERR_INVALID_ARG;
    }
    if (mkdir(TOMATO_OTA_DIR, 0777) != 0 && errno != EEXIST) {
        tomato_http_clear_detail();
        snprintf(msg, cap, "无法在 TF 卡上建 update 文件夹");
        return ESP_FAIL;
    }
    char part[sizeof(TOMATO_OTA_DIR) + sizeof(info->name) + 8], final[sizeof(TOMATO_OTA_DIR) + sizeof(info->name) + 2];
    snprintf(final, sizeof(final), "%s/%s", TOMATO_OTA_DIR, info->name);
    snprintf(part, sizeof(part), "%s.part", final);
    psa_crypto_init();
    dl_t d = {.hash = psa_hash_operation_init()};
    d.buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!d.buf) { snprintf(msg, cap, "内存不足"); return ESP_ERR_NO_MEM; }
    // 上次已经下好、校验过的正式文件还在（比如写入前电量不够），核对一遍就直接用。
    // A verified package from an earlier run (say, battery was low before writing) is reused after a re-check.
    struct stat st;
    uint8_t sum[32];
    if (info->has_sha256 && stat(final, &st) == 0 && (uint64_t)st.st_size == info->size &&
        hash_file(final, d.buf, sum) && !memcmp(sum, info->sha256, sizeof(sum))) {
        remove(part);
        if (progress) progress((int)info->size, (int)info->size);
        snprintf(msg, cap, "已下载过，校验通过");
        heap_caps_free(d.buf);
        return ESP_OK;
    }
    resume_part(&d, part, info->size);
    if (progress) progress((int)d.have, (int)info->size);

    // 还差的部分加一点余量要放得下。/ The remaining bytes plus a margin must fit.
    uint64_t total = 0, free_bytes = 0;
    esp_err_t err = ESP_OK;
    if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK &&
        free_bytes < (uint64_t)(info->size - d.have) + SPACE_MARGIN) {
        tomato_http_clear_detail();
        snprintf(msg, cap, "TF 卡空间不足，还需要 %.1f MB",
                 ((uint64_t)(info->size - d.have) + SPACE_MARGIN - free_bytes) / 1048576.0);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    for (int fails = 0; d.have < info->size;) {
        const uint32_t before = d.have;
        err = attempt(&d, info, part, cancel, progress);
        if (err == ESP_OK || *cancel) break;
        // 写卡失败不重试：多半是卡满或被拔。/ Do not retry a card write failure: likely full or pulled.
        if (err == ESP_ERR_INVALID_STATE) break;
        if (d.have - before >= PROGRESS_RESET) fails = 0;
        if (++fails >= RETRY_MAX) break;
        ESP_LOGW(TAG, "attempt failed (%s) at %lu; retry %d", esp_err_to_name(err), (unsigned long)d.have, fails);
        wait_or_cancel(RETRY_WAIT_MS, cancel);
        if (*cancel) break;
    }
    if (d.f) {
        if (fclose(d.f) != 0 && err == ESP_OK) err = ESP_ERR_INVALID_STATE;
        d.f = NULL;
    }
    if (*cancel) {
        snprintf(msg, cap, "已暂停，已下载 %.1f MB，再点会接着下", d.have / 1048576.0);
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        // 卡的问题与网络无关，不带网络原因，也不能和“暂停”混在一起。/ A card failure: no network cause, and not a pause.
        tomato_http_clear_detail();
        snprintf(msg, cap, "写 TF 卡失败，检查卡和剩余空间");
        err = ESP_FAIL;
        goto out;
    }
    if (err != ESP_OK) {
        snprintf(msg, cap, "下载中断，已下载 %.1f MB，再点会接着下", d.have / 1048576.0);
        goto out;
    }

    size_t sum_len = 0;
    const bool hashed = psa_hash_finish(&d.hash, sum, sizeof(sum), &sum_len) == PSA_SUCCESS && sum_len == sizeof(sum);
    if (d.have != info->size || !hashed || (info->has_sha256 && memcmp(sum, info->sha256, sizeof(sum)))) {
        ESP_LOGW(TAG, "verify failed: %lu of %lu bytes", (unsigned long)d.have, (unsigned long)info->size);
        remove(part);
        snprintf(msg, cap, "文件校验失败，已删除，请重新下载");
        err = ESP_ERR_INVALID_CRC;
        goto out;
    }
    // FAT 不能覆盖改名，先删同名旧文件。/ FAT cannot rename over a file; remove an old one first.
    remove(final);
    if (rename(part, final) != 0) {
        tomato_http_clear_detail();
        snprintf(msg, cap, "下载好了但改名失败，检查 TF 卡");
        err = ESP_FAIL;
        goto out;
    }
    ESP_LOGI(TAG, "downloaded %s", final);
    snprintf(msg, cap, "下载完成，校验通过");
out:
    if (d.f) fclose(d.f);
    psa_hash_abort(&d.hash);
    heap_caps_free(d.buf);
    return err;
}
