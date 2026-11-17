/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 与下载服务器通信：JSON 请求与流式文件下载。每次请求带 x-tomato-password。
 * Downloader server I/O: JSON requests and streamed file download. Every request carries x-tomato-password.
 *
 * 冻结：JSON 响应最多 256 KB，按 Content-Length 分配在 PSRAM；文件边收边写，不整本进内存。
 * 非 2xx 一律返回错误并带回状态码，调用方据此区分密码错误、服务器忙等。
 * Frozen: JSON replies at most 256 KB, sized from Content-Length in PSRAM; files stream to disk.
 * Any non-2xx status is an error with the status reported, so callers can tell bad passwords from busy servers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "tomato_internal.h"

#define TAG "tomato_http"
#define JSON_MAX (256 * 1024)
#define JSON_FIRST (32 * 1024)
#define IO_CHUNK 4096
#define TX_BUF 2048
#define TIMEOUT_MS 15000

/* ---- 小工具 / Helpers ---- */

void tomato_copy_utf8(char* out, size_t cap, const char* in) {
    if (!cap) return;
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        // 退到字符开头，不留半个汉字。/ Back up to a character start; never leave half a character.
        while (n && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = 0;
}

void tomato_url_escape(const char* in, char* out, size_t cap, bool keep_slash) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 3 < cap; p++) {
        unsigned char c = *p;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/');
        if (plain) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

int tomato_json_int(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring) return atoi(v->valuestring);
    return 0;
}

void tomato_json_str(const cJSON* obj, const char* key, char* out, size_t cap) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    tomato_copy_utf8(out, cap, cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}

// cJSON 的小节点默认进内部 RAM，联网时内部 RAM 很紧，统一放 PSRAM。
// cJSON's small nodes would land in internal RAM, which is tight while Wi-Fi is up; keep them in PSRAM.
static void* psram_malloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void tomato_json_use_psram(void) {
    static bool done;
    if (done) return;
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = heap_caps_free};
    cJSON_InitHooks(&hooks);
    done = true;
}

/* ---- 请求 / Requests ---- */

// URL 按实际长度分配，长书名转义后也不会被截断。/ Size the URL to fit so long escaped titles never truncate.
static esp_http_client_handle_t open_client(const tomato_config_t* cfg, const char* method, const char* path) {
    size_t len = strlen(cfg->url) + strlen(path) + 1;
    char* url = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!url) return NULL;
    snprintf(url, len, "%s%s", cfg->url, path);
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = IO_CHUNK,
        .buffer_size_tx = TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = !strcmp(method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    heap_caps_free(url);   // init 会复制 URL / init copies the URL
    if (!c) return NULL;
    if (cfg->password[0]) esp_http_client_set_header(c, "x-tomato-password", cfg->password);
    return c;
}

// 读完整个响应体到 PSRAM。/ Read the whole body into PSRAM.
static esp_err_t read_body(esp_http_client_handle_t c, int64_t content_length, char** out, int* out_len) {
    size_t cap = content_length > 0 && content_length < JSON_MAX ? (size_t)content_length + 1 : JSON_FIRST;
    char* buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = 0;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= JSON_MAX) { heap_caps_free(buf); return ESP_ERR_INVALID_SIZE; }
            size_t next = cap * 2 > JSON_MAX ? JSON_MAX : cap * 2;
            char* grown = heap_caps_realloc(buf, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { heap_caps_free(buf); return ESP_ERR_NO_MEM; }
            buf = grown;
            cap = next;
        }
        int n = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
        if (n < 0) { heap_caps_free(buf); return ESP_FAIL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = 0;
    *out = buf;
    *out_len = (int)len;
    return ESP_OK;
}

esp_err_t tomato_http_json(const tomato_config_t* cfg, const char* method, const char* path,
                           const char* body, cJSON** out, int* status) {
    *out = NULL;
    *status = 0;
    esp_http_client_handle_t c = open_client(cfg, method, path);
    if (!c) return ESP_ERR_NO_MEM;
    int body_len = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    char* buf = NULL;
    int len = 0;
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err != ESP_OK) goto done;
    if (body_len && esp_http_client_write(c, body, body_len) != body_len) { err = ESP_FAIL; goto done; }
    int64_t content_length = esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    if (*status < 200 || *status >= 300) { err = ESP_FAIL; goto done; }
    err = read_body(c, content_length, &buf, &len);
    if (err != ESP_OK) goto done;
    *out = cJSON_ParseWithLength(buf, (size_t)len);
    if (!*out) err = ESP_ERR_INVALID_RESPONSE;
done:
    if (err != ESP_OK) ESP_LOGW(TAG, "%s %s: %s http=%d", method, path, esp_err_to_name(err), *status);
    heap_caps_free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t tomato_http_download(const tomato_config_t* cfg, const char* path, const char* dest,
                               const volatile bool* cancel, volatile uint32_t* bytes) {
    char part[200];
    snprintf(part, sizeof(part), "%s.part", dest);
    *bytes = 0;
    esp_http_client_handle_t c = open_client(cfg, "GET", path);
    if (!c) return ESP_ERR_NO_MEM;
    FILE* f = NULL;
    char* buf = NULL;
    bool keep_part = false;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) goto done;
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) { ESP_LOGW(TAG, "download http=%d", status); err = ESP_ERR_NOT_FOUND; goto done; }
    buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_DEFAULT);
    f = fopen(part, "wb");
    if (!buf || !f) { err = buf ? ESP_FAIL : ESP_ERR_NO_MEM; goto done; }
    for (;;) {
        if (*cancel) { err = ESP_ERR_INVALID_STATE; goto done; }
        int n = esp_http_client_read(c, buf, IO_CHUNK);
        if (n < 0) { err = ESP_FAIL; goto done; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { err = ESP_FAIL; goto done; }
        *bytes += (uint32_t)n;
    }
    if (!esp_http_client_is_complete_data_received(c)) { err = ESP_FAIL; goto done; }
    if (fclose(f) != 0) { f = NULL; err = ESP_FAIL; goto done; }
    f = NULL;
    // FAT 不能覆盖改名，先删旧文件；改名失败时保留 .part，不能两份都丢。
    // FAT cannot rename over a file; remove the old one first. If the rename fails, keep .part so nothing is lost.
    remove(dest);
    if (rename(part, dest) != 0) {
        ESP_LOGE(TAG, "rename %s failed; kept %s", dest, part);
        keep_part = true;
        err = ESP_FAIL;
    }
done:
    if (f) fclose(f);
    if (err != ESP_OK && !keep_part) remove(part);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

// 边收边数两个键名出现的次数，取较大者；不缓存整个响应。只数结尾落在新数据里的匹配，跨块不重不漏。
// Count two key names while streaming and keep the larger; never buffers the reply. Only matches ending in new
// data count, so chunk boundaries neither double-count nor miss.
esp_err_t tomato_http_count_keys(const char* url, const char* key_a, const char* key_b,
                                 const volatile bool* cancel, int* count) {
    *count = 0;
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = IO_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return ESP_ERR_NO_MEM;
    const size_t la = strlen(key_a), lb = strlen(key_b);
    const size_t keep = (la > lb ? la : lb) - 1;
    char* buf = heap_caps_malloc(IO_CHUNK + keep + 1, MALLOC_CAP_DEFAULT);
    int na = 0, nb = 0;
    esp_err_t err = buf ? esp_http_client_open(c, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) != 200) err = ESP_ERR_NOT_FOUND;
    }
    size_t carry = 0;
    while (err == ESP_OK) {
        if (cancel && *cancel) { err = ESP_ERR_INVALID_STATE; break; }
        int n = esp_http_client_read(c, buf + carry, IO_CHUNK);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        size_t len = carry + (size_t)n;
        for (size_t i = 0; i < len; i++) {
            if (i + la <= len && i + la > carry && !memcmp(buf + i, key_a, la)) na++;
            if (i + lb <= len && i + lb > carry && !memcmp(buf + i, key_b, lb)) nb++;
        }
        carry = len < keep ? len : keep;
        memmove(buf, buf + len - carry, carry);
    }
    *count = na > nb ? na : nb;
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}
