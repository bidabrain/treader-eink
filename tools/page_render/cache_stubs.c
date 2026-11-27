// SPDX-FileCopyrightText: 2026 Treader contributors
// SPDX-License-Identifier: Apache-2.0
// 页面渲染用的缓存读写桩：直接读写普通文件（固件里归缓存包管的路径进包）。/ Cache access stubs for page rendering:
// plain files (the firmware puts pack-owned paths into the cache pack).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "tomato.h"

bool tomato_cache_size(const char* path, size_t* len) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (len) *len = (size_t)st.st_size;
    return true;
}

esp_err_t tomato_cache_load(const char* path, size_t max, uint8_t** out, size_t* len) {
    *out = NULL;
    *len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n >= 0 && (size_t)n <= max ? malloc((size_t)n + 1) : NULL;
    const bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(buf); return ESP_FAIL; }
    buf[n] = 0;
    *out = buf;
    *len = (size_t)n;
    return ESP_OK;
}

esp_err_t tomato_cache_store2(const char* path, const void* a, size_t alen, const void* b, size_t blen) {
    FILE* f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    const bool ok = fwrite(a, 1, alen, f) == alen && fwrite(b, 1, blen, f) == blen;
    return fclose(f) == 0 && ok ? ESP_OK : ESP_FAIL;
}

cJSON* tomato_cache_read_json(const char* path) {
    uint8_t* buf = NULL;
    size_t n = 0;
    cJSON* root = tomato_cache_load(path, 1024 * 1024, &buf, &n) == ESP_OK && n ? cJSON_Parse((const char*)buf) : NULL;
    free(buf);
    return root;
}

bool tomato_cache_jpeg_size(const char* path, int* w, int* h) {
    (void)path;
    (void)w;
    (void)h;
    return false;
}
