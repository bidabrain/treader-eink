/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 小红书的数据层：只连用户自己部署的 rednote 服务器（仓库 rednote/ 目录），不直接访问小红书。关注列表、推荐列表、
 * 笔记正文和图片存到 TF 卡 rednote/，页面只读这些缓存。图片服务器已经处理成灰度基线 JPEG（宽不超过 648）。
 *
 * English: Xiaohongshu data layer. It talks only to the user's own rednote server (the repo's rednote/ directory),
 * never to Xiaohongshu itself. Following and recommendation lists, note text and images go to rednote/ on the TF card
 * and the page reads only those caches. The server already turned images into grayscale baseline JPEGs (<= 648 wide).
 *
 * 冻结：只在后台同步任务里联网，由页面按用户操作启动；地址和令牌存在 NVS 的 rednote 命名空间，不写 TF 卡、不打日志。
 * 只读：不点赞、不评论。缓存有上限：笔记目录超过 NOTE_KEEP 个时，只留当前两个列表里还有的。
 * Frozen: network only on the sync task, started by the page on user actions; the address and token live in NVS
 * namespace rednote, never on the TF card or in logs. Read only: no likes or comments. Caches are bounded: past
 * NOTE_KEEP note folders, only those still in the two lists stay.
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
#include "nvs.h"
#include "tomato_internal.h"

#define TAG "tomato_red"
#define NVS_NS "rednote"
#define JSON_MAX (512 * 1024)
#define IMG_MAX (2 * 1024 * 1024)
#define FEED_LIMIT 100
#define NOTE_KEEP 80
// 笔记目录（大图、灰度缓存）一共最多占这么多；超过就从最旧的删大图。/ Cap for all note folders (images and gray
// caches); past it the oldest notes lose their full images.
#define NOTES_BYTES_MAX (200ull * 1024 * 1024)
#define DIRS_MAX 300
#define IO_CHUNK 4096
#define TIMEOUT_MS 15000
// 点开推荐时服务器要现开小红书，最长要等半分钟以上。/ Opening a recommendation makes the server visit Xiaohongshu live.
#define SLOW_TIMEOUT_MS 60000
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* ---- 设置 / Settings ---- */

static uint32_t s_revision;

esp_err_t tomato_red_config_load(tomato_red_config_t* out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t n = sizeof(out->url);
    if (nvs_get_str(h, "url", out->url, &n) != ESP_OK) out->url[0] = 0;
    n = sizeof(out->token);
    if (nvs_get_str(h, "token", out->token, &n) != ESP_OK) out->token[0] = 0;
    nvs_close(h);
    return out->url[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tomato_red_config_save(const tomato_red_config_t* cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // 地址去掉结尾的斜杠，拼接口路径时不会出现“//”。/ Drop trailing slashes so joined paths never get "//".
    char url[sizeof(cfg->url)];
    snprintf(url, sizeof(url), "%s", cfg->url);
    for (size_t n = strlen(url); n && url[n - 1] == '/'; n--) url[n - 1] = 0;
    err = nvs_set_str(h, "url", url);
    if (err == ESP_OK) err = nvs_set_str(h, "token", cfg->token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_revision++;
    return err;
}

uint32_t tomato_red_config_revision(void) { return s_revision; }

/* ---- 路径 / Paths ---- */

// 笔记编号只留字母和数字，免得拼进路径时出现 ../。/ Keep only letters and digits from note ids for paths.
static void safe_id(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++)
        if (isalnum((unsigned char)*in)) out[n++] = *in;
    out[n] = 0;
}

void tomato_red_note_dir(const char* id, char* out, size_t cap) {
    char sid[32];
    safe_id(id, sid, sizeof(sid));
    snprintf(out, cap, TOMATO_RED_DIR "/n/%s", sid);
}

void tomato_red_img_path(const char* id, int n, char* out, size_t cap) {
    char dir[64];
    tomato_red_note_dir(id, dir, sizeof(dir));
    if (n <= 0) snprintf(out, cap, "%s/t.jpg", dir);
    else snprintf(out, cap, "%s/%d.jpg", dir, n);
}

void tomato_red_cover_path(const char* id, char* out, size_t cap) {
    char sid[32];
    safe_id(id, sid, sizeof(sid));
    snprintf(out, cap, TOMATO_RED_DIR "/rc/%s.jpg", sid);
}

static bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* ---- HTTP ---- */

// GET 服务器上的一个接口，带设备令牌。非 200 时把服务器给的原因（{"detail": "..."}）写进 msg。
// GET one server endpoint with the device token. On a non-200 status the server's reason ({"detail"}) goes to msg.
static esp_err_t red_get(const tomato_red_config_t* cfg, const char* path, size_t max, int timeout_ms,
                         const volatile bool* cancel, uint8_t** out, size_t* out_len, char* msg, size_t cap) {
    *out = NULL;
    *out_len = 0;
    char url[sizeof(cfg->url) + 96];
    snprintf(url, sizeof(url), "%s%s", cfg->url, path);
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = IO_CHUNK,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { snprintf(msg, cap, "服务器地址格式不对"); return ESP_ERR_INVALID_ARG; }
    esp_http_client_set_header(c, "x-rednote-token", cfg->token);
    esp_err_t err = ESP_FAIL;
    int status = 0;
    int64_t length = -1;
    // 跟随跳转（比如 http 被跳到 https）。/ Follow redirects (say, http forced to https).
    for (int hop = 0; hop < 4; hop++) {
        err = tomato_http_open(c, 0);
        if (err != ESP_OK) break;
        length = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400) break;
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
        cJSON* j = cJSON_Parse((const char*)buf);
        const cJSON* d = cJSON_GetObjectItemCaseSensitive(j, "detail");
        if (cJSON_IsString(d) && d->valuestring[0]) tomato_copy_utf8(msg, cap, d->valuestring);
        else if (status == 401) snprintf(msg, cap, "设备令牌不对");
        else snprintf(msg, cap, "服务器返回 HTTP %d", status);
        cJSON_Delete(j);
        err = status == 401 ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_FOUND;
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (tomato_http_detail()[0] == 0) tomato_http_set_detail(esp_err_to_name(err));
        snprintf(msg, cap, "连不上 rednote 服务器（%s）", tomato_http_detail());
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) { heap_caps_free(buf); return err; }
    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return ESP_OK;
}

static esp_err_t red_json(const tomato_red_config_t* cfg, const char* path, int timeout_ms,
                          const volatile bool* cancel, cJSON** out, char* msg, size_t cap) {
    uint8_t* buf;
    size_t len;
    esp_err_t err = red_get(cfg, path, JSON_MAX, timeout_ms, cancel, &buf, &len, msg, cap);
    if (err != ESP_OK) return err;
    tomato_json_use_psram();
    *out = cJSON_Parse((const char*)buf);
    heap_caps_free(buf);
    if (!*out) { snprintf(msg, cap, "服务器返回的内容看不懂"); return ESP_FAIL; }
    return ESP_OK;
}

// 先写 .part 再改名，断电不会留下半个文件。/ Write .part then rename so a power cut never leaves half a file.
static esp_err_t write_file(const char* dest, const void* data, size_t len) {
    char tmp[112];
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

static esp_err_t write_json(const char* dest, const cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = write_file(dest, text, strlen(text));
    cJSON_free(text);
    return err;
}

static esp_err_t fetch_image(const tomato_red_config_t* cfg, const char* path, const char* dest,
                             const volatile bool* cancel, char* msg, size_t cap) {
    uint8_t* buf;
    size_t len;
    esp_err_t err = red_get(cfg, path, IMG_MAX, TIMEOUT_MS, cancel, &buf, &len, msg, cap);
    if (err != ESP_OK) return err;
    const bool jpeg = len > 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF;
    err = jpeg ? write_file(dest, buf, len) : ESP_ERR_NOT_SUPPORTED;
    heap_caps_free(buf);
    return err;
}

static cJSON* read_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < JSON_MAX ? heap_caps_malloc((size_t)n + 1, PSRAM_CAPS) : NULL;
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

static void mkdirs(void) {
    mkdir(TOMATO_RED_DIR, 0775);
    mkdir(TOMATO_RED_DIR "/n", 0775);
    mkdir(TOMATO_RED_DIR "/rc", 0775);
}

static void copy_str(cJSON* to, const char* key, const cJSON* from, const char* from_key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(from, from_key);
    if (cJSON_IsString(v)) cJSON_AddStringToObject(to, key, v->valuestring);
    else if (cJSON_IsNumber(v)) {
        char num[24];
        snprintf(num, sizeof(num), "%.0f", v->valuedouble);
        cJSON_AddStringToObject(to, key, num);
    } else cJSON_AddStringToObject(to, key, "");
}

static int json_int(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
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
} note_dir_t;

// 一篇笔记目录的总字节和最新修改时间。/ Total bytes and newest modification time of a note folder.
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

// 只删大图和它们的灰度缓存，留下正文和缩略图；返回删掉的字节。/ Delete only full images and their gray caches,
// keeping the text and the thumbnail; returns the bytes freed.
static uint64_t strip_images(const char* dir) {
    uint64_t freed = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    char path[300];
    struct stat st;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "note.json") || !strncmp(e->d_name, "t.", 2) ||
            !strncmp(e->d_name, "t_", 2)) continue;
        snprintf(path, sizeof(path), "%s/%.100s", dir, e->d_name);
        if (stat(path, &st) == 0 && remove(path) == 0) freed += (uint64_t)st.st_size;
    }
    closedir(d);
    return freed;
}

static int cmp_oldest(const void* a, const void* b) {
    const time_t x = ((const note_dir_t*)a)->mtime, y = ((const note_dir_t*)b)->mtime;
    return x < y ? -1 : x > y;
}

// 两道上限：目录数超过 NOTE_KEEP 时先删不在两个列表里的旧笔记；总大小超过 NOTES_BYTES_MAX 时，从最旧的开始只删
// 大图（再打开会补下）。keep 这一篇两样都不动。
// Two caps: past NOTE_KEEP folders, delete the oldest notes no longer in either list; past NOTES_BYTES_MAX in total,
// strip full images from the oldest notes (reopening fetches them again). The keep note is never touched.
static void prune_notes(const char* keep) {
    note_dir_t* dirs = heap_caps_calloc(DIRS_MAX, sizeof(note_dir_t), PSRAM_CAPS);
    if (!dirs) return;
    DIR* d = opendir(TOMATO_RED_DIR "/n");
    if (!d) { heap_caps_free(dirs); return; }
    // 列表文件在锁里读：写文件的 remove 和 rename 之间读到“没有”会把所有笔记当成不在列表里。
    // Read the lists under the lock: a read between a writer's remove and rename would see no list at all.
    tomato_hupu_cache_lock();
    cJSON* feed = read_json(TOMATO_RED_FEED_FILE);
    cJSON* rec = read_json(TOMATO_RED_REC_FILE);
    tomato_hupu_cache_unlock();
    const cJSON* fi = cJSON_GetObjectItemCaseSensitive(feed, "items");
    const cJSON* ri = cJSON_GetObjectItemCaseSensitive(rec, "items");
    int n = 0;
    uint64_t total = 0;
    char dir[96];
    struct dirent* e;
    // 扫描和统计不拿锁，界面照常画图；只在删的时候短暂拿锁。/ Scan without the lock so the UI keeps drawing; take it
    // briefly only around each delete.
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(dirs[0].name)) continue;
        snprintf(dir, sizeof(dir), TOMATO_RED_DIR "/n/%s", e->d_name);
        const bool listed = (keep && !strcmp(e->d_name, keep)) || listed_in(fi, e->d_name) || listed_in(ri, e->d_name);
        if (n >= DIRS_MAX) {
            // 多出来的不在列表里就直接删。/ Overflow folders go at once unless still listed.
            if (!listed) { tomato_hupu_cache_lock(); remove_dir(dir); tomato_hupu_cache_unlock(); }
            continue;
        }
        note_dir_t* x = &dirs[n++];
        snprintf(x->name, sizeof(x->name), "%s", e->d_name);
        dir_stat(dir, &x->bytes, &x->mtime);
        x->listed = listed;
        total += x->bytes;
    }
    closedir(d);
    qsort(dirs, (size_t)n, sizeof(*dirs), cmp_oldest);
    int count = n;
    for (int i = 0; i < n && count > NOTE_KEEP; i++) {
        if (dirs[i].listed) continue;
        snprintf(dir, sizeof(dir), TOMATO_RED_DIR "/n/%s", dirs[i].name);
        tomato_hupu_cache_lock();
        remove_dir(dir);
        tomato_hupu_cache_unlock();
        total -= dirs[i].bytes;
        dirs[i].bytes = 0;
        dirs[i].name[0] = 0;
        count--;
    }
    for (int i = 0; i < n && total > NOTES_BYTES_MAX; i++) {
        if (!dirs[i].name[0] || (keep && !strcmp(dirs[i].name, keep))) continue;
        snprintf(dir, sizeof(dir), TOMATO_RED_DIR "/n/%s", dirs[i].name);
        tomato_hupu_cache_lock();
        const uint64_t freed = strip_images(dir);
        tomato_hupu_cache_unlock();
        total = total > freed ? total - freed : 0;
    }
    cJSON_Delete(feed);
    cJSON_Delete(rec);
    heap_caps_free(dirs);
    ESP_LOGI(TAG, "notes %d, %.1f MB", count, total / 1048576.0);
}

// 推荐封面只留当前列表的。/ Keep only the current list's recommendation covers.
static void prune_covers(const cJSON* items) {
    DIR* d = opendir(TOMATO_RED_DIR "/rc");
    if (!d) return;
    struct dirent* e;
    char path[96], id[48];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        // 封面 id.jpg 和它的灰度缓存 id_120x160.g 都按 id 判断。/ Both id.jpg and its gray cache id_120x160.g go by id.
        snprintf(id, sizeof(id), "%.40s", e->d_name);
        id[strcspn(id, "._")] = 0;
        if (listed_in(items, id)) continue;
        snprintf(path, sizeof(path), TOMATO_RED_DIR "/rc/%.60s", e->d_name);
        tomato_hupu_cache_lock();
        remove(path);
        tomato_hupu_cache_unlock();
    }
    closedir(d);
}

/* ---- 任务 / Jobs ---- */

// 关注列表：取最近 FEED_LIMIT 篇，保留旧文件里的“看过”标记，再补缩略图。
// Following list: fetch the latest FEED_LIMIT notes, keep "seen" marks from the old file, then fill thumbnails.
static esp_err_t run_feed(const tomato_red_config_t* cfg, const volatile bool* cancel, tomato_hupu_progress_t progress,
                          char* msg, size_t cap) {
    cJSON* root = NULL;
    char path[96];
    snprintf(path, sizeof(path), "/api/device/v1/feed?limit=%d", FEED_LIMIT);
    esp_err_t err = red_json(cfg, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    cJSON* old = read_json(TOMATO_RED_FEED_FILE);
    const cJSON* old_items = cJSON_GetObjectItemCaseSensitive(old, "items");
    cJSON* out = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(out, "items");
    const cJSON* n;
    int count = 0;
    cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(root, "notes")) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(n, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        cJSON* x = cJSON_CreateObject();
        copy_str(x, "id", n, "id");
        copy_str(x, "title", n, "title");
        copy_str(x, "author", n, "author");
        copy_str(x, "likes", n, "likes");
        cJSON_AddNumberToObject(x, "images", json_int(n, "images"));
        const cJSON* tv = cJSON_GetObjectItemCaseSensitive(n, "time");
        cJSON_AddNumberToObject(x, "time", cJSON_IsNumber(tv) ? tv->valuedouble : 0);
        // 新到的标“新”；以前看过的保持。/ New arrivals are unseen; earlier seen marks stay.
        bool seen = !old;   // 第一次取到的都算看过，不满屏“新” / A first fetch marks all as seen
        const cJSON* o;
        cJSON_ArrayForEach(o, old_items) {
            const cJSON* oid = cJSON_GetObjectItemCaseSensitive(o, "id");
            if (cJSON_IsString(oid) && !strcmp(oid->valuestring, id->valuestring)) {
                seen = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "seen"));
                break;
            }
        }
        cJSON_AddBoolToObject(x, "seen", seen);
        cJSON_AddItemToArray(items, x);
        count++;
    }
    cJSON_Delete(root);
    cJSON_Delete(old);
    mkdirs();
    err = write_json(TOMATO_RED_FEED_FILE, out);
    if (err != ESP_OK) { cJSON_Delete(out); snprintf(msg, cap, "写不进 TF 卡"); return err; }
    progress(1, count + 1);
    // 缩略图：一篇一张，失败的跳过，下次刷新再补。/ One thumbnail per note; failures are skipped until the next refresh.
    int i = 0, got = 0;
    char dir[64], dest[96];
    cJSON_ArrayForEach(n, items) {
        if (*cancel) break;
        i++;
        const char* id = cJSON_GetObjectItemCaseSensitive(n, "id")->valuestring;
        tomato_red_img_path(id, 0, dest, sizeof(dest));
        if (exists(dest)) continue;
        tomato_red_note_dir(id, dir, sizeof(dir));
        mkdir(dir, 0775);
        char sid[32];
        safe_id(id, sid, sizeof(sid));
        snprintf(path, sizeof(path), "/api/device/v1/img/%s/thumb.jpg", sid);
        char ignore[64];
        if (fetch_image(cfg, path, dest, cancel, ignore, sizeof(ignore)) == ESP_OK) got++;
        progress(1 + i, count + 1);
    }
    cJSON_Delete(out);
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    prune_notes(NULL);
    snprintf(msg, cap, "关注 %d 篇", count);
    return ESP_OK;
}

// 推荐列表：refresh 时服务器才去读小红书首页，否则给上次的。/ Recommendations: refresh makes the server read
// Xiaohongshu; otherwise it returns the previous list.
static esp_err_t run_rec(const tomato_red_config_t* cfg, bool refresh, const volatile bool* cancel,
                         tomato_hupu_progress_t progress, char* msg, size_t cap) {
    cJSON* root = NULL;
    esp_err_t err = red_json(cfg, refresh ? "/api/device/v1/rec?refresh=1" : "/api/device/v1/rec", SLOW_TIMEOUT_MS,
                             cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    cJSON* out = cJSON_CreateObject();
    const cJSON* fa = cJSON_GetObjectItemCaseSensitive(root, "fetched_at");
    cJSON_AddNumberToObject(out, "fetched_at", cJSON_IsNumber(fa) ? fa->valuedouble : 0);
    cJSON_AddNumberToObject(out, "left", json_int(root, "left_today"));
    cJSON* items = cJSON_AddArrayToObject(out, "items");
    const cJSON* n;
    int count = 0;
    cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(root, "notes")) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(n, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        cJSON* x = cJSON_CreateObject();
        copy_str(x, "id", n, "id");
        copy_str(x, "title", n, "title");
        copy_str(x, "author", n, "author");
        copy_str(x, "likes", n, "likes");
        cJSON_AddBoolToObject(x, "thumb", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(n, "thumb")));
        cJSON_AddItemToArray(items, x);
        count++;
    }
    cJSON_Delete(root);
    mkdirs();
    err = write_json(TOMATO_RED_REC_FILE, out);
    if (err != ESP_OK) { cJSON_Delete(out); snprintf(msg, cap, "写不进 TF 卡"); return err; }
    progress(1, count + 1);
    int i = 0;
    char path[96], dest[96], sid[32], ignore[64];
    cJSON_ArrayForEach(n, items) {
        if (*cancel) break;
        i++;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(n, "thumb"))) continue;
        const char* id = cJSON_GetObjectItemCaseSensitive(n, "id")->valuestring;
        tomato_red_cover_path(id, dest, sizeof(dest));
        if (exists(dest)) continue;
        safe_id(id, sid, sizeof(sid));
        snprintf(path, sizeof(path), "/api/device/v1/rec/cover/%s.jpg", sid);
        fetch_image(cfg, path, dest, cancel, ignore, sizeof(ignore));
        progress(1 + i, count + 1);
    }
    prune_covers(items);
    cJSON_Delete(out);
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    snprintf(msg, cap, "推荐 %d 篇", count);
    return ESP_OK;
}

// 一篇笔记：正文和评论存成 note.json，图片一张张取。images_only 时只补缺的图。
// One note: text and comments go to note.json, then images one by one. images_only fills missing images only.
static esp_err_t run_note(const tomato_red_config_t* cfg, const tomato_red_req_t* r, const volatile bool* cancel,
                          tomato_hupu_progress_t progress, char* msg, size_t cap) {
    char sid[32], dir[64], file[96], path[112];
    safe_id(r->id, sid, sizeof(sid));
    if (!sid[0]) { snprintf(msg, cap, "笔记编号不对"); return ESP_ERR_INVALID_ARG; }
    tomato_red_note_dir(sid, dir, sizeof(dir));
    snprintf(file, sizeof(file), "%s/note.json", dir);
    int images = 0;
    if (r->images_only && exists(file)) {
        cJSON* note = read_json(file);
        images = json_int(note, "images");
        cJSON_Delete(note);
    } else {
        cJSON* root = NULL;
        snprintf(path, sizeof(path), r->rec ? "/api/device/v1/rec/note/%s" : "/api/device/v1/note/%s", sid);
        esp_err_t err = red_json(cfg, path, r->rec ? SLOW_TIMEOUT_MS : TIMEOUT_MS, cancel, &root, msg, cap);
        if (err != ESP_OK) return err;
        cJSON* out = cJSON_CreateObject();
        copy_str(out, "id", root, "id");
        copy_str(out, "title", root, "title");
        copy_str(out, "author", root, "author");
        copy_str(out, "likes", root, "likes");
        copy_str(out, "comments", root, "comments");
        copy_str(out, "body", root, "body");
        images = json_int(root, "images");
        cJSON_AddNumberToObject(out, "images", images);
        const cJSON* tv = cJSON_GetObjectItemCaseSensitive(root, "time");
        cJSON_AddNumberToObject(out, "time", cJSON_IsNumber(tv) ? tv->valuedouble : 0);   // 可能是毫秒 / May be ms
        cJSON* list = cJSON_AddArrayToObject(out, "list");
        const cJSON* c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "comment_list")) {
            cJSON* x = cJSON_CreateObject();
            copy_str(x, "u", c, "user");
            copy_str(x, "t", c, "text");
            copy_str(x, "l", c, "likes");
            cJSON_AddItemToArray(list, x);
        }
        cJSON_Delete(root);
        mkdirs();
        mkdir(dir, 0775);
        err = write_json(file, out);
        cJSON_Delete(out);
        if (err != ESP_OK) { snprintf(msg, cap, "写不进 TF 卡"); return err; }
    }
    progress(1, images + 1);
    int failed = 0;
    for (int k = 1; k <= images && !*cancel; k++) {
        char dest[96], ignore[64];
        tomato_red_img_path(sid, k, dest, sizeof(dest));
        if (!exists(dest)) {
            snprintf(path, sizeof(path), "/api/device/v1/img/%s/%d.jpg", sid, k);
            if (fetch_image(cfg, path, dest, cancel, ignore, sizeof(ignore)) != ESP_OK) failed++;
        }
        progress(1 + k, images + 1);
    }
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    prune_notes(sid);
    if (failed) snprintf(msg, cap, "%d 张图没下载成功", failed);
    else msg[0] = 0;
    return ESP_OK;
}

static esp_err_t run_test(const tomato_red_config_t* cfg, const volatile bool* cancel, char* msg, size_t cap) {
    cJSON* root = NULL;
    char path[64];
    snprintf(path, sizeof(path), "/api/device/v1/feed?limit=%d", FEED_LIMIT);
    esp_err_t err = red_json(cfg, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    const int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "notes"));
    cJSON_Delete(root);
    snprintf(msg, cap, n >= FEED_LIMIT ? "正常 · 关注 %d+ 篇" : "正常 · 关注 %d 篇", n);
    return ESP_OK;
}

esp_err_t tomato_red_run(const tomato_red_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                         char* msg, size_t cap) {
    tomato_red_config_t cfg;
    if (tomato_red_config_load(&cfg) != ESP_OK) { snprintf(msg, cap, "还没设置 rednote 服务器"); return ESP_ERR_NOT_FOUND; }
    esp_err_t err;
    switch (r->job) {
    case TOMATO_RED_FEED: err = run_feed(&cfg, cancel, progress, msg, cap); break;
    case TOMATO_RED_REC: err = run_rec(&cfg, r->refresh, cancel, progress, msg, cap); break;
    case TOMATO_RED_NOTE: err = run_note(&cfg, r, cancel, progress, msg, cap); break;
    case TOMATO_RED_TEST: err = run_test(&cfg, cancel, msg, cap); break;
    default: err = ESP_ERR_INVALID_ARG; break;
    }
    memset(&cfg, 0, sizeof(cfg));
    return err;
}

// 关注列表里标记看过（页面在界面任务里调用）。/ Mark a following-list note as seen (UI task).
void tomato_red_mark_seen(const char* id) {
    cJSON* root = read_json(TOMATO_RED_FEED_FILE);
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
    if (changed) write_json(TOMATO_RED_FEED_FILE, root);
    cJSON_Delete(root);
}
