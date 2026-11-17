/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TF 卡上的番茄配置、书架元数据和“已删除”清单，JSON 文本，电脑上也能直接改。
 * Tomato config, shelf metadata and the removed-books list on TF as JSON text, editable on a computer too.
 *
 * 冻结：先写 .tmp 再改名；改名失败保留 .tmp，下次读取时用它恢复。界面任务和同步任务共用一把锁。
 * 配置文件存在但解析不了时拒绝保存，免得把另一半配置抹掉。
 * Frozen: write .tmp then rename; on rename failure keep .tmp and recover from it on the next read. The UI and
 * sync tasks share one lock. Refuse to save over a config file that exists but does not parse.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tomato_builtin.h"
#include "tomato_internal.h"

#define FILE_MAX (64 * 1024)
#define REMOVED_PATH TOMATO_DIR "/removed.json"

/* ---- 锁 / Lock ---- */

static SemaphoreHandle_t s_io;
static StaticSemaphore_t s_io_buf;
static portMUX_TYPE s_io_init = portMUX_INITIALIZER_UNLOCKED;

static void io_lock(void) {
    portENTER_CRITICAL(&s_io_init);
    if (!s_io) s_io = xSemaphoreCreateRecursiveMutexStatic(&s_io_buf);
    portEXIT_CRITICAL(&s_io_init);
    xSemaphoreTakeRecursive(s_io, portMAX_DELAY);
}

static void io_unlock(void) { xSemaphoreGiveRecursive(s_io); }

/* ---- 文件 / Files ---- */

static cJSON* read_file_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    size_t cap = fstat(fileno(f), &st) == 0 && st.st_size > 0 && st.st_size < FILE_MAX ? (size_t)st.st_size : FILE_MAX;
    char* buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    cJSON* root = cJSON_ParseWithLength(buf, n);
    heap_caps_free(buf);
    return root;
}

// 主文件丢了（掉电卡在删旧和改名之间）就用 .tmp。/ Fall back to .tmp if power was lost between remove and rename.
static cJSON* read_json(const char* path) {
    cJSON* root = read_file_json(path);
    if (root) return root;
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    struct stat st;
    if (stat(path, &st) == 0) return NULL;   // 主文件在但坏了，不用旧草稿 / Main file exists but is bad; skip the draft
    return read_file_json(tmp);
}

static esp_err_t write_json(const char* path, const cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (!text) return ESP_ERR_NO_MEM;
    mkdir(TOMATO_DIR, 0775);
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    size_t len = strlen(text);
    bool written = f && fwrite(text, 1, len, f) == len;
    if (f && fclose(f) != 0) written = false;
    cJSON_free(text);
    if (!written) { remove(tmp); return ESP_FAIL; }
    remove(path);
    // 改名失败保留 .tmp，read_json 会用它恢复。/ On rename failure keep .tmp; read_json recovers from it.
    return rename(tmp, path) == 0 ? ESP_OK : ESP_FAIL;
}

/* ---- 配置 / Config ---- */

bool tomato_config_has_builtin(void) {
    return TOMATO_BUILTIN_URL_LEN > 0 && TOMATO_BUILTIN_URL_LEN < sizeof(((tomato_config_t*)0)->url) &&
           TOMATO_BUILTIN_PASS_LEN < sizeof(((tomato_config_t*)0)->password);
}

// 还原编译时混淆的默认服务器。/ Recover the default server obfuscated at build time.
static bool load_builtin(tomato_config_t* out) {
#if TOMATO_BUILTIN_URL_LEN == 0
    (void)out;
    return false;
#else
    if (!tomato_config_has_builtin()) return false;
    for (size_t i = 0; i < TOMATO_BUILTIN_URL_LEN; i++) out->url[i] = (char)(TOMATO_BUILTIN_URL[i] ^ TOMATO_BUILTIN_KEY[i % 32]);
    out->url[TOMATO_BUILTIN_URL_LEN] = 0;
#if TOMATO_BUILTIN_PASS_LEN > 0
    for (size_t i = 0; i < TOMATO_BUILTIN_PASS_LEN; i++)
        out->password[i] = (char)(TOMATO_BUILTIN_PASS[i] ^ TOMATO_BUILTIN_KEY[i % 32]);
#endif
    out->password[TOMATO_BUILTIN_PASS_LEN] = 0;
    out->builtin = true;
    return true;
#endif
}

esp_err_t tomato_config_load(tomato_config_t* out) {
    memset(out, 0, sizeof(*out));
    io_lock();
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    io_unlock();
    // 设置里填过就用填的，否则用内置默认。/ A server set in Settings wins; otherwise use the built-in default.
    if (root) {
        tomato_json_str(root, "server", out->url, sizeof(out->url));
        tomato_json_str(root, "password", out->password, sizeof(out->password));
        cJSON_Delete(root);
    }
    if (!out->url[0]) {
        memset(out, 0, sizeof(*out));
        return load_builtin(out) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
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

// 读旧配置再改，保留不认识的字段；文件在但解析失败时返回 NULL，不覆盖。
// Edit the existing config so unknown fields survive; NULL if the file exists but does not parse.
static cJSON* config_root(void) {
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    if (root) return root;
    struct stat st;
    if (stat(TOMATO_CONFIG_PATH, &st) == 0) return NULL;
    return cJSON_CreateObject();
}

static esp_err_t save_config_fields(const char* const keys[], const char* const values[], int n) {
    io_lock();
    cJSON* root = config_root();
    esp_err_t err = root ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    for (int i = 0; root && i < n; i++) set_str(root, keys[i], values[i]);
    if (root) err = write_json(TOMATO_CONFIG_PATH, root);
    io_unlock();
    cJSON_Delete(root);
    return err;
}

esp_err_t tomato_config_save(const tomato_config_t* cfg) {
    const char* const keys[] = {"server", "password"};
    const char* const values[] = {cfg->url, cfg->password};
    return save_config_fields(keys, values, 2);
}

esp_err_t tomato_store_url_load(char* out, size_t cap) {
    out[0] = 0;
    io_lock();
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    io_unlock();
    if (!root) return ESP_ERR_NOT_FOUND;
    tomato_json_str(root, "store_url", out, cap);
    cJSON_Delete(root);
    return out[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tomato_store_url_save(const char* url) {
    const char* const keys[] = {"store_url"};
    const char* const values[] = {url ? url : ""};
    return save_config_fields(keys, values, 1);
}

esp_err_t tomato_webdav_config_load(tomato_webdav_config_t* out) {
    memset(out, 0, sizeof(*out));
    io_lock();
    cJSON* root = read_json(TOMATO_CONFIG_PATH);
    io_unlock();
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
    const char* const keys[] = {"webdav_url", "webdav_user", "webdav_password"};
    const char* const values[] = {cfg->url, cfg->user, cfg->password};
    return save_config_fields(keys, values, 3);
}

/* ---- 书架 / Shelf ---- */

esp_err_t tomato_shelf_load(tomato_book_t* books, int max, int* count) {
    *count = 0;
    io_lock();
    cJSON* root = read_json(TOMATO_SHELF_PATH);
    io_unlock();
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
    io_lock();
    esp_err_t err = write_json(TOMATO_SHELF_PATH, root);
    io_unlock();
    cJSON_Delete(root);
    return err;
}

/* ---- 已删除清单 / Removed-books list ---- */

int tomato_removed_load(char (*ids)[TOMATO_ID_LEN], int max) {
    io_lock();
    cJSON* root = read_json(REMOVED_PATH);
    io_unlock();
    int n = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "removed")) {
        if (n >= max) break;
        if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) tomato_copy_utf8(ids[n++], TOMATO_ID_LEN, it->valuestring);
    }
    cJSON_Delete(root);
    return n;
}

esp_err_t tomato_removed_save(const char (*ids)[TOMATO_ID_LEN], int n) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = root ? cJSON_AddArrayToObject(root, "removed") : NULL;
    if (!arr) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(ids[i]));
    io_lock();
    esp_err_t err = write_json(REMOVED_PATH, root);
    io_unlock();
    cJSON_Delete(root);
    return err;
}

// 加入或移出已删除清单。/ Add to or drop from the removed list.
esp_err_t tomato_removed_mark(const char* book_id, bool removed) {
    EXT_RAM_BSS_ATTR static char ids[TOMATO_REMOVED_MAX][TOMATO_ID_LEN];
    io_lock();
    int n = tomato_removed_load(ids, TOMATO_REMOVED_MAX), at = -1;
    for (int i = 0; i < n; i++) if (!strcmp(ids[i], book_id)) at = i;
    esp_err_t err = ESP_OK;
    if (removed && at < 0) {
        // 满了就挤掉最旧的。/ When full, drop the oldest.
        if (n == TOMATO_REMOVED_MAX) { memmove(ids[0], ids[1], sizeof(ids[0]) * (TOMATO_REMOVED_MAX - 1)); n--; }
        tomato_copy_utf8(ids[n++], TOMATO_ID_LEN, book_id);
        err = tomato_removed_save((const char (*)[TOMATO_ID_LEN])ids, n);
    } else if (!removed && at >= 0) {
        memmove(ids[at], ids[at + 1], sizeof(ids[0]) * (size_t)(n - at - 1));
        err = tomato_removed_save((const char (*)[TOMATO_ID_LEN])ids, n - 1);
    }
    io_unlock();
    return err;
}

// 立即从书架去掉某个文件，并把它的 book_id 记进已删除清单。/ Drop a file from the shelf now and record its book_id as removed.
esp_err_t tomato_shelf_remove_now(const char* file) {
    tomato_book_t* books = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*books), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!books) return ESP_ERR_NO_MEM;
    io_lock();
    int count = 0, write = 0;
    esp_err_t err = tomato_shelf_load(books, TOMATO_SHELF_MAX, &count);
    for (int i = 0; err == ESP_OK && i < count; i++) {
        if (strcmp(books[i].file, file)) { books[write++] = books[i]; continue; }
        tomato_removed_mark(books[i].book_id, true);
        char cover[200];
        snprintf(cover, sizeof(cover), "%s/%.150s.img", TOMATO_COVER_DIR, file);
        remove(cover);
    }
    if (err == ESP_OK && write != count) err = tomato_shelf_save(books, write);
    io_unlock();
    heap_caps_free(books);
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

esp_err_t tomato_shelf_remove_file(const char* file) {
    // 同步任务手里有一份书架，等它结束再删，免得被它写回。/ The sync task holds a shelf copy; defer until it ends.
    if (tomato_sync_defer_remove(file)) return ESP_OK;
    return tomato_shelf_remove_now(file);
}
