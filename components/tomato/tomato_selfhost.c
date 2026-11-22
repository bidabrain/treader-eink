/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 自建内容服务器（rednote、weibo）共用的部分：NVS 里的地址和令牌、带令牌的 GET、缓存文件的写入和读取、按目录数
 * 和总大小清理条目目录。各来源的接口路径、列表格式和页面都在自己的文件里。
 *
 * English: pieces shared by the self-hosted content servers (rednote, weibo): address and token in NVS, GET with the
 * device token, cache file writes and reads, and pruning item folders by count and total size. Each source keeps its
 * endpoints, list format and page in its own files.
 *
 * 冻结：令牌只放请求头，不写 TF 卡、不打日志。清理不碰调用方指定保留的条目，也不碰列表里还有的条目。
 * Frozen: the token goes only into a request header, never to the TF card or logs. Pruning never touches the item the
 * caller keeps or items still listed.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "tomato_internal.h"

#define TAG "tomato_srv"
#define IO_CHUNK 4096
// 微博两个列表各 300 条时，列表里的目录最多 600 个。/ Two Weibo lists of 300 hold up to 600 listed folders.
#define DIRS_MAX 700
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define COMIC_TMP_PATH 320

/* ---- 设置 / Settings ---- */

esp_err_t tomato_srv_config_load(const char* ns, tomato_srv_config_t* out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t n = sizeof(out->url);
    if (nvs_get_str(h, "url", out->url, &n) != ESP_OK) out->url[0] = 0;
    n = sizeof(out->token);
    if (nvs_get_str(h, "token", out->token, &n) != ESP_OK) out->token[0] = 0;
    nvs_close(h);
    return out->url[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tomato_srv_config_save(const char* ns, const tomato_srv_config_t* cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // 地址去掉结尾的斜杠，拼接口路径时不会出现“//”。/ Drop trailing slashes so joined paths never get "//".
    char url[sizeof(cfg->url)];
    snprintf(url, sizeof(url), "%s", cfg->url);
    for (size_t n = strlen(url); n && url[n - 1] == '/'; n--) url[n - 1] = 0;
    err = nvs_set_str(h, "url", url);
    if (err == ESP_OK) err = nvs_set_str(h, "token", cfg->token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// 列表条数存在同一个命名空间的 "list" 键；只认 100 / 200 / 300。/ The list size lives under "list" in the same
// namespace; only 100 / 200 / 300 are accepted.
int tomato_srv_list_limit(const char* ns) {
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, "list", &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    return (v == 200 || v == 300) ? v : TOMATO_LIST_LIMIT_MIN;
}

esp_err_t tomato_srv_list_limit_set(const char* ns, int n) {
    if (n != 100 && n != 200 && n != 300) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, "list", (uint16_t)n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---- 小工具 / Small helpers ---- */

void tomato_safe_id(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++)
        if (isalnum((unsigned char)*in)) out[n++] = *in;
    out[n] = 0;
}

bool tomato_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* ---- HTTP ---- */

// 非 200 时把服务器给的原因（JSON 的 detail）写进 msg；返回对应的错误码。/ On a non-200 status put the server's
// reason (the JSON detail) into msg; returns the matching error.
static esp_err_t status_error(int status, const uint8_t* body, char* msg, size_t cap) {
    cJSON* j = body ? cJSON_Parse((const char*)body) : NULL;
    const cJSON* d = cJSON_GetObjectItemCaseSensitive(j, "detail");
    if (cJSON_IsString(d) && d->valuestring[0]) tomato_copy_utf8(msg, cap, d->valuestring);
    else if (status == 401) snprintf(msg, cap, "设备令牌不对");
    else snprintf(msg, cap, "服务器返回 HTTP %d", status);
    cJSON_Delete(j);
    return status == 401 ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_FOUND;
}

// body 为 NULL 时 GET，否则把它当 JSON POST 出去（POST 不跟随跳转）。/ GET when body is NULL; otherwise POST it as
// JSON (POST does not follow redirects).
static esp_err_t srv_request(const tomato_srv_t* srv, const char* path, const char* body, size_t max, int timeout_ms,
                             const volatile bool* cancel, uint8_t** out, size_t* out_len, char* msg, size_t cap) {
    *out = NULL;
    *out_len = 0;
    char url[sizeof(srv->cfg->url) + 160];
    snprintf(url, sizeof(url), "%s%s", srv->cfg->url, path);
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = IO_CHUNK,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { snprintf(msg, cap, "服务器地址格式不对"); return ESP_ERR_INVALID_ARG; }
    esp_http_client_set_header(c, srv->header, srv->cfg->token);
    const int body_len = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_err_t err = ESP_FAIL;
    int status = 0;
    int64_t length = -1;
    // 跟随跳转（比如 http 被跳到 https）。/ Follow redirects (say, http forced to https).
    for (int hop = 0; hop < 4; hop++) {
        err = tomato_http_open(c, body_len);
        if (err != ESP_OK) break;
        if (body_len && esp_http_client_write(c, body, body_len) != body_len) { err = ESP_FAIL; break; }
        length = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400 || body) break;
        if (esp_http_client_set_redirection(c) != ESP_OK) { err = ESP_FAIL; break; }
        esp_http_client_close(c);
    }
    // 知道长度就按长度开，不知道就从 64 KB 起按需加倍，最多 max。/ Size from Content-Length when known; otherwise
    // start at 64 KB and double as needed, up to max.
    uint8_t* buf = NULL;
    size_t len = 0, room = 0;
    if (err == ESP_OK) {
        if (length > (int64_t)max) err = ESP_ERR_INVALID_SIZE;
        else room = length > 0 ? (size_t)length : (max < 65536 ? max : 65536);
    }
    if (err == ESP_OK && !(buf = heap_caps_malloc(room + 1, PSRAM_CAPS))) err = ESP_ERR_NO_MEM;
    while (err == ESP_OK) {
        if (cancel && *cancel) { err = ESP_ERR_INVALID_STATE; break; }
        if (len >= room) {
            if (room >= max) { err = ESP_ERR_INVALID_SIZE; break; }
            const size_t grow = room * 2 < max ? room * 2 : max;
            uint8_t* bigger = heap_caps_realloc(buf, grow + 1, PSRAM_CAPS);
            if (!bigger) { err = ESP_ERR_NO_MEM; break; }
            buf = bigger;
            room = grow;
        }
        int n = esp_http_client_read(c, (char*)buf + len, (int)(room - len));
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        len += (size_t)n;
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(c)) err = ESP_FAIL;
    if (err == ESP_OK && status != 200) {
        buf[len] = 0;
        err = status_error(status, buf, msg, cap);
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (tomato_http_detail()[0] == 0) tomato_http_set_detail(esp_err_to_name(err));
        snprintf(msg, cap, "连不上 %s 服务器（%s）", srv->name, tomato_http_detail());
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) { heap_caps_free(buf); return err; }
    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return ESP_OK;
}

esp_err_t tomato_srv_get(const tomato_srv_t* srv, const char* path, size_t max, int timeout_ms,
                         const volatile bool* cancel, uint8_t** out, size_t* out_len, char* msg, size_t cap) {
    return srv_request(srv, path, NULL, max, timeout_ms, cancel, out, out_len, msg, cap);
}

esp_err_t tomato_srv_post(const tomato_srv_t* srv, const char* path, const char* body, int timeout_ms,
                          const volatile bool* cancel, cJSON** out, char* msg, size_t cap) {
    uint8_t* buf;
    size_t len;
    *out = NULL;
    esp_err_t err = srv_request(srv, path, body ? body : "{}", TOMATO_SRV_JSON_MAX, timeout_ms, cancel, &buf, &len,
                                msg, cap);
    if (err != ESP_OK) return err;
    tomato_json_use_psram();
    *out = cJSON_Parse((const char*)buf);
    heap_caps_free(buf);
    if (!*out) { snprintf(msg, cap, "服务器返回的内容看不懂"); return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t tomato_srv_download(const tomato_srv_t* srv, const char* path, const char* dest, size_t max, int timeout_ms,
                              const volatile bool* cancel, char* msg, size_t cap) {
    char url[sizeof(srv->cfg->url) + 160], part[COMIC_TMP_PATH];
    snprintf(url, sizeof(url), "%s%s", srv->cfg->url, path);
    snprintf(part, sizeof(part), "%s.part", dest);
    if (cap) msg[0] = 0;
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = IO_CHUNK,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { snprintf(msg, cap, "服务器地址格式不对"); return ESP_ERR_INVALID_ARG; }
    esp_http_client_set_header(c, srv->header, srv->cfg->token);
    esp_err_t err = tomato_http_open(c, 0);
    const int64_t length = err == ESP_OK ? esp_http_client_fetch_headers(c) : -1;
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    uint8_t* buf = heap_caps_malloc(IO_CHUNK, PSRAM_CAPS);
    FILE* f = NULL;
    size_t total = 0;
    if (err == ESP_OK && status != 200) {
        int n = buf ? esp_http_client_read(c, (char*)buf, IO_CHUNK - 1) : 0;
        if (buf) buf[n > 0 ? n : 0] = 0;
        err = status_error(status, n > 0 ? buf : NULL, msg, cap);
    } else if (err == ESP_OK && (length > (int64_t)max || !buf)) {
        err = buf ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
    } else if (err == ESP_OK && !(f = fopen(part, "wb"))) {
        err = ESP_FAIL;
        snprintf(msg, cap, "写不进 TF 卡");
    }
    while (err == ESP_OK) {
        if (cancel && *cancel) { err = ESP_ERR_INVALID_STATE; break; }
        int n = esp_http_client_read(c, (char*)buf, IO_CHUNK);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        total += (size_t)n;
        if (total > max) { err = ESP_ERR_INVALID_SIZE; break; }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { err = ESP_FAIL; snprintf(msg, cap, "写不进 TF 卡"); break; }
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(c)) err = ESP_FAIL;
    if (f && fclose(f) != 0 && err == ESP_OK) { err = ESP_FAIL; snprintf(msg, cap, "写不进 TF 卡"); }
    if (err == ESP_OK) {
        remove(dest);
        if (rename(part, dest) != 0) { err = ESP_FAIL; snprintf(msg, cap, "写不进 TF 卡"); }
    }
    if (err != ESP_OK) {
        remove(part);
        if (!msg[0] && err != ESP_ERR_INVALID_STATE) {
            if (tomato_http_detail()[0] == 0) tomato_http_set_detail(esp_err_to_name(err));
            snprintf(msg, cap, "下载失败（%s）", tomato_http_detail());
        }
    }
    heap_caps_free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t tomato_srv_json(const tomato_srv_t* srv, const char* path, int timeout_ms, const volatile bool* cancel,
                          cJSON** out, char* msg, size_t cap) {
    uint8_t* buf;
    size_t len;
    esp_err_t err = tomato_srv_get(srv, path, TOMATO_SRV_JSON_MAX, timeout_ms, cancel, &buf, &len, msg, cap);
    if (err != ESP_OK) return err;
    tomato_json_use_psram();
    *out = cJSON_Parse((const char*)buf);
    heap_caps_free(buf);
    if (!*out) { snprintf(msg, cap, "服务器返回的内容看不懂"); return ESP_FAIL; }
    return ESP_OK;
}

#define POLL_GAP_MS 2500

esp_err_t tomato_srv_poll(const tomato_srv_t* srv, const char* first, const char* again, const char* key,
                          int timeout_ms, int max_ms, const volatile bool* cancel, cJSON** out, char* msg, size_t cap) {
    *out = NULL;
    const int64_t start = esp_timer_get_time() / 1000;
    for (const char* path = first;; path = again ? again : first) {
        cJSON* root = NULL;
        esp_err_t err = tomato_srv_json(srv, path, timeout_ms, cancel, &root, msg, cap);
        if (err != ESP_OK) return err;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, key))) {
            *out = root;
            return ESP_OK;
        }
        cJSON_Delete(root);
        if (esp_timer_get_time() / 1000 - start > max_ms) {
            snprintf(msg, cap, "服务器处理太久了，稍后再试");
            return ESP_ERR_TIMEOUT;
        }
        for (int t = 0; t < POLL_GAP_MS; t += 100) {
            if (cancel && *cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t tomato_srv_image(const tomato_srv_t* srv, const char* path, const char* dest, int timeout_ms,
                           const volatile bool* cancel) {
    uint8_t* buf;
    size_t len;
    char ignore[64];
    esp_err_t err = tomato_srv_get(srv, path, TOMATO_SRV_IMG_MAX, timeout_ms, cancel, &buf, &len, ignore, sizeof(ignore));
    if (err != ESP_OK) return err;
    const bool jpeg = len > 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF;
    err = jpeg ? tomato_cache_write(dest, buf, len) : ESP_ERR_NOT_SUPPORTED;
    heap_caps_free(buf);
    return err;
}

/* ---- 缓存文件 / Cache files ---- */

esp_err_t tomato_cache_write(const char* dest, const void* data, size_t len) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    FILE* f = fopen(tmp, "wb");
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return ESP_FAIL; }
    tomato_hupu_cache_lock();
    remove(dest);
    const bool renamed = rename(tmp, dest) == 0;
    tomato_hupu_cache_unlock();
    if (!renamed) { remove(tmp); return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t tomato_cache_write_json(const char* dest, const cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = tomato_cache_write(dest, text, strlen(text));
    cJSON_free(text);
    return err;
}

cJSON* tomato_cache_read_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < TOMATO_SRV_JSON_MAX ? heap_caps_malloc((size_t)n + 1, PSRAM_CAPS) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    cJSON* root = NULL;
    if (ok) {
        buf[n] = 0;
        tomato_json_use_psram();
        root = cJSON_Parse(buf);
    }
    heap_caps_free(buf);
    return root;
}

/* ---- 清理 / Pruning ---- */

static void remove_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* e;
        char path[300];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%.100s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

static bool listed_in(const cJSON* items, const char* id) {
    const cJSON* it;
    cJSON_ArrayForEach(it, items) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(v) && !strcmp(v->valuestring, id)) return true;
    }
    return false;
}

typedef struct {
    char name[32];
    time_t mtime;
    uint64_t bytes;
    bool listed;
} item_dir_t;

// 一个条目目录的总字节和最新修改时间。/ Total bytes and newest modification time of an item folder.
static void dir_stat(const char* dir, uint64_t* bytes, time_t* mtime) {
    *bytes = 0;
    *mtime = 0;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    char path[300];
    struct stat st;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%.100s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        *bytes += (uint64_t)st.st_size;
        if (st.st_mtime > *mtime) *mtime = st.st_mtime;
    }
    closedir(d);
}

// 只删大图和它们的灰度缓存，留下 .json 和缩略图（t.jpg、t_*）；返回删掉的字节。/ Delete only full images and their
// gray caches, keeping .json files and the thumbnail (t.jpg, t_*); returns the bytes freed.
static uint64_t strip_images(const char* dir) {
    uint64_t freed = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    char path[300];
    struct stat st;
    while ((e = readdir(d))) {
        const size_t n = strlen(e->d_name);
        // 留下正文、封面缩略图（t.*、t_*）和微博 / X 列表卡片用的小方图（s1.jpg、s1_宽x高.g 这类），只删大图：
        // 否则还在列表里的多图卡片会变成灰块，下次刷新又全部重下。
        // Keep the text, cover thumbnails (t.*, t_*) and the small squares Weibo / X list cards use (s1.jpg,
        // s1_WxH.g and the like); delete only large images, or listed multi-image cards turn gray and are all
        // downloaded again on the next refresh.
        const bool square = e->d_name[0] == 's' && e->d_name[1] >= '0' && e->d_name[1] <= '9' &&
                            (e->d_name[2] == '.' || e->d_name[2] == '_');
        if (e->d_name[0] == '.' || (n > 5 && !strcmp(e->d_name + n - 5, ".json")) || !strncmp(e->d_name, "t.", 2) ||
            !strncmp(e->d_name, "t_", 2) || square) continue;
        snprintf(path, sizeof(path), "%s/%.100s", dir, e->d_name);
        if (stat(path, &st) == 0 && remove(path) == 0) freed += (uint64_t)st.st_size;
    }
    closedir(d);
    return freed;
}

static int cmp_oldest(const void* a, const void* b) {
    const time_t x = ((const item_dir_t*)a)->mtime, y = ((const item_dir_t*)b)->mtime;
    return x < y ? -1 : x > y;
}

void tomato_cache_prune(const char* root, const char* const* lists, int list_count, const char* keep, int keep_dirs,
                        uint64_t max_bytes) {
    item_dir_t* dirs = heap_caps_calloc(DIRS_MAX, sizeof(item_dir_t), PSRAM_CAPS);
    if (!dirs) return;
    DIR* d = opendir(root);
    if (!d) { heap_caps_free(dirs); return; }
    // 列表文件在锁里读：写文件的 remove 和 rename 之间读到“没有”会把所有条目当成不在列表里。
    // Read the lists under the lock: a read between a writer's remove and rename would see no list at all.
    cJSON* roots[4] = {0};
    if (list_count > 4) list_count = 4;
    tomato_hupu_cache_lock();
    for (int i = 0; i < list_count; i++) roots[i] = tomato_cache_read_json(lists[i]);
    tomato_hupu_cache_unlock();
    int n = 0;
    uint64_t total = 0;
    char dir[128];
    struct dirent* e;
    // 扫描和统计不拿锁，界面照常画图；只在删的时候短暂拿锁。/ Scan without the lock so the UI keeps drawing; take it
    // briefly only around each delete.
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(dirs[0].name)) continue;
        snprintf(dir, sizeof(dir), "%s/%s", root, e->d_name);
        bool listed = keep && !strcmp(e->d_name, keep);
        for (int i = 0; i < list_count && !listed; i++)
            listed = listed_in(cJSON_GetObjectItemCaseSensitive(roots[i], "items"), e->d_name);
        if (n >= DIRS_MAX) {
            // 多出来的不在列表里就直接删。/ Overflow folders go at once unless still listed.
            if (!listed) { tomato_hupu_cache_lock(); remove_dir(dir); tomato_hupu_cache_unlock(); }
            continue;
        }
        item_dir_t* x = &dirs[n++];
        snprintf(x->name, sizeof(x->name), "%s", e->d_name);
        dir_stat(dir, &x->bytes, &x->mtime);
        x->listed = listed;
        total += x->bytes;
    }
    closedir(d);
    qsort(dirs, (size_t)n, sizeof(*dirs), cmp_oldest);
    int count = n;
    for (int i = 0; i < n && count > keep_dirs; i++) {
        if (dirs[i].listed) continue;
        snprintf(dir, sizeof(dir), "%s/%s", root, dirs[i].name);
        tomato_hupu_cache_lock();
        remove_dir(dir);
        tomato_hupu_cache_unlock();
        total -= dirs[i].bytes;
        dirs[i].bytes = 0;
        dirs[i].name[0] = 0;
        count--;
    }
    for (int i = 0; i < n && total > max_bytes; i++) {
        if (!dirs[i].name[0] || (keep && !strcmp(dirs[i].name, keep))) continue;
        snprintf(dir, sizeof(dir), "%s/%s", root, dirs[i].name);
        tomato_hupu_cache_lock();
        const uint64_t freed = strip_images(dir);
        tomato_hupu_cache_unlock();
        total = total > freed ? total - freed : 0;
    }
    for (int i = 0; i < list_count; i++) cJSON_Delete(roots[i]);
    heap_caps_free(dirs);
    ESP_LOGI(TAG, "%s: %d items, %.1f MB", root, count, total / 1048576.0);
}

void tomato_cache_mark_seen(const char* list_file, const char* id) {
    cJSON* root = tomato_cache_read_json(list_file);
    if (!root) return;
    bool changed = false;
    cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (!cJSON_IsString(v) || strcmp(v->valuestring, id)) continue;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "seen"))) {
            cJSON_ReplaceItemInObjectCaseSensitive(it, "seen", cJSON_CreateTrue());
            changed = true;
        }
        break;
    }
    if (changed) tomato_cache_write_json(list_file, root);
    cJSON_Delete(root);
}
