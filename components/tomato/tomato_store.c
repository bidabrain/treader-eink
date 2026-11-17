/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TF 卡上的番茄配置与书架元数据，JSON 文本，电脑上也能直接改。
 * Tomato config and shelf metadata on TF as JSON text, editable on a computer too.
 *
 * 冻结：先写 .tmp 再改名，写失败不破坏旧文件。
 * Frozen: write .tmp then rename; a failed write never damages the old file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "tomato_internal.h"

#define FILE_MAX (64 * 1024)

static cJSON* read_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* buf = heap_caps_malloc(FILE_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = buf ? fread(buf, 1, FILE_MAX, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    cJSON* root = cJSON_ParseWithLength(buf, n);
    heap_caps_free(buf);
    return root;
}

static esp_err_t write_json(const char* path, const cJSON* root) {
    char* text = cJSON_Print(root);
    if (!text) return ESP_ERR_NO_MEM;
    mkdir(TOMATO_DIR, 0775);
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    size_t len = strlen(text);
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    cJSON_free(text);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t tomato_config_load(tomato_config_t* out) {
    memset(out, 0, sizeof(*out));
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    if (!root) return ESP_ERR_NOT_FOUND;
    tomato_json_str(root, "server", out->url, sizeof(out->url));
    tomato_json_str(root, "password", out->password, sizeof(out->password));
    cJSON_Delete(root);
    // 去掉末尾斜杠，避免拼出 //api。/ Strip trailing slashes so paths never become //api.
    size_t n = strlen(out->url);
    while (n && out->url[n - 1] == '/') out->url[--n] = 0;
    return out->url[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// 改写或新增一个字符串字段。/ Replace or add one string field.
static void set_str(cJSON* root, const char* key, const char* value) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, value);
}

// 读旧配置再改，保留不认识的字段。/ Edit the existing config so unknown fields survive.
static cJSON* config_root(void) {
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    return root ? root : cJSON_CreateObject();
}

esp_err_t tomato_config_save(const tomato_config_t* cfg) {
    cJSON* root = config_root();
    if (!root) return ESP_ERR_NO_MEM;
    set_str(root, "server", cfg->url);
    set_str(root, "password", cfg->password);
    esp_err_t err = write_json(TOMATO_CONFIG_PATH, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t tomato_webdav_config_load(tomato_webdav_config_t* out) {
    memset(out, 0, sizeof(*out));
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    if (!root) return ESP_ERR_NOT_FOUND;
    tomato_json_str(root, "webdav_url", out->url, sizeof(out->url));
    tomato_json_str(root, "webdav_user", out->user, sizeof(out->user));
    tomato_json_str(root, "webdav_password", out->password, sizeof(out->password));
    cJSON_Delete(root);
    size_t n = strlen(out->url);
    while (n && out->url[n - 1] == '/') out->url[--n] = 0;
    return out->url[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tomato_webdav_config_save(const tomato_webdav_config_t* cfg) {
    cJSON* root = config_root();
    if (!root) return ESP_ERR_NO_MEM;
    set_str(root, "webdav_url", cfg->url);
    set_str(root, "webdav_user", cfg->user);
    set_str(root, "webdav_password", cfg->password);
    esp_err_t err = write_json(TOMATO_CONFIG_PATH, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t tomato_shelf_load(tomato_book_t* books, int max, int* count) {
    *count = 0;
    cJSON* root = read_json(TOMATO_SHELF_PATH);
    if (!root) return ESP_ERR_NOT_FOUND;
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "books");
    const cJSON* it;
    cJSON_ArrayForEach(it, arr) {
        if (*count >= max) break;
        tomato_book_t* b = &books[*count];
        memset(b, 0, sizeof(*b));
        tomato_json_str(it, "book_id", b->book_id, sizeof(b->book_id));
        if (!b->book_id[0]) continue;
        tomato_json_str(it, "title", b->title, sizeof(b->title));
        tomato_json_str(it, "author", b->author, sizeof(b->author));
        tomato_json_str(it, "file", b->file, sizeof(b->file));
        b->chapters = (uint16_t)tomato_json_int(it, "chapters");
        b->remote = (uint16_t)tomato_json_int(it, "remote");
        (*count)++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tomato_shelf_save(const tomato_book_t* books, int count) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = root ? cJSON_AddArrayToObject(root, "books") : NULL;
    if (!arr) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    for (int i = 0; i < count; i++) {
        cJSON* o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "book_id", books[i].book_id);
        cJSON_AddStringToObject(o, "title", books[i].title);
        cJSON_AddStringToObject(o, "author", books[i].author);
        cJSON_AddStringToObject(o, "file", books[i].file);
        cJSON_AddNumberToObject(o, "chapters", books[i].chapters);
        cJSON_AddNumberToObject(o, "remote", books[i].remote);
        cJSON_AddItemToArray(arr, o);
    }
    esp_err_t err = write_json(TOMATO_SHELF_PATH, root);
    cJSON_Delete(root);
    return err;
}
