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
#define NOTE_KEEP 80
// 笔记目录（大图、灰度缓存）一共最多占这么多；超过就从最旧的删大图。/ Cap for all note folders (images and gray
// caches); past it the oldest notes lose their full images.
#define NOTES_BYTES_MAX (200ull * 1024 * 1024)
#define TIMEOUT_MS 15000
// 刷新推荐、点开推荐、加载更多评论时服务器要现去小红书：设备用 wait=0 让它在后台做，自己隔几秒问一次（每次马上回话，
// 前台随时能插队），最多等这么久。/ Refreshing or opening recommendations and loading more comments make the server
// visit Xiaohongshu live: the device passes wait=0 so it works in the background and asks every few seconds (each call
// answers at once, so the foreground can always cut in), for at most this long.
#define LIVE_MAX_MS 180000
// 每次请求的超时：新服务器马上回话；旧服务器不认 wait=0、还会照旧等着做完，留足时间。/ Per-request timeout: new
// servers answer at once; old ones ignore wait=0 and still answer when done, so leave room.
#define LIVE_TIMEOUT_MS 60000

/* ---- 设置 / Settings ---- */

static uint32_t s_revision;

int tomato_red_list_limit(void) { return tomato_srv_list_limit(NVS_NS); }
esp_err_t tomato_red_list_limit_set(int n) { return tomato_srv_list_limit_set(NVS_NS, n); }
esp_err_t tomato_red_config_load(tomato_red_config_t* out) { return tomato_srv_config_load(NVS_NS, out); }

esp_err_t tomato_red_config_save(const tomato_red_config_t* cfg) {
    const esp_err_t err = tomato_srv_config_save(NVS_NS, cfg);
    if (err == ESP_OK) s_revision++;
    return err;
}

uint32_t tomato_red_config_revision(void) { return s_revision; }

/* ---- 路径 / Paths ---- */

#define safe_id tomato_safe_id
#define exists tomato_file_exists
#define read_json tomato_cache_read_json
#define write_json tomato_cache_write_json

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

/* ---- HTTP ---- */

static esp_err_t red_json(const tomato_red_config_t* cfg, const char* path, int timeout_ms,
                          const volatile bool* cancel, cJSON** out, char* msg, size_t cap) {
    const tomato_srv_t srv = {.cfg = cfg, .header = "x-rednote-token", .name = "rednote"};
    return tomato_srv_json(&srv, path, timeout_ms, cancel, out, msg, cap);
}

static esp_err_t red_poll(const tomato_red_config_t* cfg, const char* first, const char* again, const char* key,
                          const volatile bool* cancel, cJSON** out, char* msg, size_t cap) {
    const tomato_srv_t srv = {.cfg = cfg, .header = "x-rednote-token", .name = "rednote"};
    return tomato_srv_poll(&srv, first, again, key, LIVE_TIMEOUT_MS, LIVE_MAX_MS, cancel, out, msg, cap);
}

static esp_err_t fetch_image(const tomato_red_config_t* cfg, const char* path, const char* dest,
                             const volatile bool* cancel) {
    const tomato_srv_t srv = {.cfg = cfg, .header = "x-rednote-token", .name = "rednote"};
    return tomato_srv_image(&srv, path, dest, TIMEOUT_MS, cancel);
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

// 两道上限见 tomato_cache_prune：目录数超过 NOTE_KEEP、总大小超过 NOTES_BYTES_MAX。/ Two caps, see
// tomato_cache_prune: NOTE_KEEP folders and NOTES_BYTES_MAX in total.
static void prune_notes(const char* keep) {
    static const char* const LISTS[] = {TOMATO_RED_FEED_FILE, TOMATO_RED_REC_FILE};
    tomato_cache_prune(TOMATO_RED_DIR "/n", LISTS, 2, keep, NOTE_KEEP, NOTES_BYTES_MAX);
}

static bool listed_in(const cJSON* items, const char* id) {
    const cJSON* it;
    cJSON_ArrayForEach(it, items) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(v) && !strcmp(v->valuestring, id)) return true;
    }
    return false;
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

// 关注列表：取最近“列表条数”篇，保留旧文件里的“看过”标记，再补缩略图。
// Following list: fetch the latest "list size" notes, keep "seen" marks from the old file, then fill thumbnails.
static esp_err_t run_feed(const tomato_red_config_t* cfg, const volatile bool* cancel, tomato_hupu_progress_t progress,
                          char* msg, size_t cap) {
    cJSON* root = NULL;
    char path[96];
    snprintf(path, sizeof(path), "/api/device/v1/feed?limit=%d", tomato_red_list_limit());
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
        // wait=0：服务器还在做的图不等，下次刷新再取；老服务器不认这个参数，照旧等。/ wait=0: skip images the
        // server is still making until the next refresh; older servers ignore it and wait as before.
        snprintf(path, sizeof(path), "/api/device/v1/img/%s/thumb.jpg?wait=0", sid);
        if (fetch_image(cfg, path, dest, cancel) == ESP_OK) got++;
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
    esp_err_t err = red_poll(cfg, refresh ? "/api/device/v1/rec?refresh=1&wait=0" : "/api/device/v1/rec?wait=0",
                             "/api/device/v1/rec?wait=0", "pending", cancel, &root, msg, cap);
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
    char path[96], dest[96], sid[32];
    cJSON_ArrayForEach(n, items) {
        if (*cancel) break;
        i++;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(n, "thumb"))) continue;
        const char* id = cJSON_GetObjectItemCaseSensitive(n, "id")->valuestring;
        tomato_red_cover_path(id, dest, sizeof(dest));
        if (exists(dest)) continue;
        safe_id(id, sid, sizeof(sid));
        snprintf(path, sizeof(path), "/api/device/v1/rec/cover/%s.jpg", sid);
        fetch_image(cfg, path, dest, cancel);
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
    if (r->images_only && !r->more_comments && exists(file)) {
        cJSON* note = read_json(file);
        images = json_int(note, "images");
        cJSON_Delete(note);
    } else {
        cJSON* root = NULL;
        esp_err_t err;
        if (r->more_comments) {
            // 先让服务器开始拉，再隔几秒问到拉完。/ Ask the server to start, then ask every few seconds until done.
            char again[112];
            snprintf(path, sizeof(path), "/api/device/v1/note/%s/more_comments?start=1&wait=0", sid);
            snprintf(again, sizeof(again), "/api/device/v1/note/%s/more_comments?wait=0", sid);
            err = red_poll(cfg, path, again, "loading", cancel, &root, msg, cap);
            if (err != ESP_OK) return err;
        } else {
            snprintf(path, sizeof(path), r->rec ? "/api/device/v1/rec/note/%s?wait=0" : "/api/device/v1/note/%s", sid);
            err = r->rec ? red_poll(cfg, path, NULL, "pending", cancel, &root, msg, cap)
                         : red_json(cfg, path, TIMEOUT_MS, cancel, &root, msg, cap);
            if (err != ESP_OK) return err;
        }
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
        // 加载过更多评论就记下，按钮变灰。/ Remember that more comments were loaded, so the button turns gray.
        if (r->more_comments) cJSON_AddBoolToObject(out, "more_done", true);
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
        char dest[96];
        tomato_red_img_path(sid, k, dest, sizeof(dest));
        if (!exists(dest)) {
            snprintf(path, sizeof(path), "/api/device/v1/img/%s/%d.jpg", sid, k);
            if (fetch_image(cfg, path, dest, cancel) != ESP_OK) failed++;
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
    snprintf(path, sizeof(path), "/api/device/v1/feed?limit=%d", tomato_red_list_limit());
    esp_err_t err = red_json(cfg, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    const int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "notes"));
    cJSON_Delete(root);
    snprintf(msg, cap, n >= tomato_red_list_limit() ? "正常 · 关注 %d+ 篇" : "正常 · 关注 %d 篇", n);
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
void tomato_red_mark_seen(const char* id) { tomato_cache_mark_seen(TOMATO_RED_FEED_FILE, id); }
