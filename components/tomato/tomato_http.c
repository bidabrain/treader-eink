/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 与下载服务器通信：JSON 请求与流式文件下载。每次请求带 x-tomato-password。
 * Downloader server I/O: JSON requests and streamed file download. Every request carries x-tomato-password.
 *
 * 冻结：JSON 响应最多 256 KB，放 PSRAM；文件边收边写，不整本进内存。
 * Frozen: JSON replies at most 256 KB in PSRAM; files stream to disk, never whole in RAM.
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
#define IO_CHUNK 4096
#define TIMEOUT_MS 15000

void tomato_url_escape(const char* in, char* out, size_t cap, bool keep_slash) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 4 < cap; p++) {
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
    snprintf(out, cap, "%s", cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}

static esp_http_client_handle_t open_client(const tomato_config_t* cfg, const char* method, const char* path) {
    char url[384];
    snprintf(url, sizeof(url), "%s%s", cfg->url, path);
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = IO_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = !strcmp(method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return NULL;
    if (cfg->password[0]) esp_http_client_set_header(c, "x-tomato-password", cfg->password);
    return c;
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
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err != ESP_OK) goto done;
    if (body_len && esp_http_client_write(c, body, body_len) != body_len) { err = ESP_FAIL; goto done; }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    buf = heap_caps_malloc(JSON_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { err = ESP_ERR_NO_MEM; goto done; }
    int len = 0;
    for (;;) {
        int n = esp_http_client_read(c, buf + len, JSON_MAX - len);
        if (n < 0) { err = ESP_FAIL; goto done; }
        if (n == 0) break;
        len += n;
        if (len >= JSON_MAX) { err = ESP_ERR_INVALID_SIZE; goto done; }
    }
    buf[len] = 0;
    if (*status >= 200 && *status < 300) {
        *out = cJSON_ParseWithLength(buf, (size_t)len);
        if (!*out) err = ESP_ERR_INVALID_RESPONSE;
    }
done:
    if (err != ESP_OK) ESP_LOGW(TAG, "%s %s: %s http=%d", method, path, esp_err_to_name(err), *status);
    heap_caps_free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t tomato_http_download(const tomato_config_t* cfg, const char* path, const char* dest,
                               const volatile bool* cancel, volatile uint32_t* bytes) {
    char part[192];
    snprintf(part, sizeof(part), "%s.part", dest);
    *bytes = 0;
    esp_http_client_handle_t c = open_client(cfg, "GET", path);
    if (!c) return ESP_ERR_NO_MEM;
    FILE* f = NULL;
    char* buf = NULL;
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
    // FAT 不能覆盖改名，先删旧文件。/ FAT cannot rename over a file; remove the old one first.
    remove(dest);
    if (rename(part, dest) != 0) err = ESP_FAIL;
done:
    if (f) fclose(f);
    if (err != ESP_OK) remove(part);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}
