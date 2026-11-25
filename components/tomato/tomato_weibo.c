/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 微博的数据层：只连用户自己部署的 weibo 服务器（仓库 weibo/ 目录），不直接访问微博。关注和热门两个列表、单条微博
 * 的全文和图片、评论都存到 TF 卡 weibo/，页面只读这些缓存。图片服务器已经处理成灰度基线 JPEG（宽不超过 648）。
 *
 * English: Weibo data layer. It talks only to the user's own weibo server (the repo's weibo/ directory), never to
 * Weibo itself. The following and hot lists, each post's full text and images, and comments go to weibo/ on the TF
 * card and the page reads only those caches. The server already turned images into grayscale baseline JPEGs.
 *
 * 冻结：只在后台同步任务里联网，由页面按用户操作启动；地址和令牌存在 NVS 的 weibo 命名空间，不写 TF 卡、不打日志。
 * 只读：不点赞、不评论、不转发。缓存有上限：微博目录超过 POST_KEEP 个时，只留两个列表里还有的。
 * Frozen: network only on the sync task, started by the page on user actions; the address and token live in NVS
 * namespace weibo, never on the TF card or in logs. Read only: no likes, comments or reposts. Caches are bounded: past
 * POST_KEEP post folders, only those still in the two lists stay.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tomato_internal.h"

#define NVS_NS "weibo"
#define HEADER "x-weibo-token"
#define POST_KEEP 150
// 微博目录（大图、灰度缓存）一共最多占这么多；超过就从最旧的删大图。/ Cap for all post folders; past it the oldest
// posts lose their full images.
#define POSTS_BYTES_MAX (200ull * 1024 * 1024)
#define TIMEOUT_MS 15000
// 评论第一次请求时服务器要去微博取：设备用 wait=0 让它在后台取，自己隔几秒问一次（每次马上回话，前台随时能插队），
// 最多等这么久。/ The first comments request makes the server ask Weibo: the device passes wait=0 so it fetches in
// the background and asks every few seconds (each call answers at once, so the foreground can always cut in), for
// at most this long.
#define LIVE_MAX_MS 120000
// 每次请求的超时：新服务器马上回话；旧服务器不认 wait=0、还会照旧等着取完，留足时间。/ Per-request timeout: new
// servers answer at once; old ones ignore wait=0 and still answer when done, so leave room.
#define LIVE_TIMEOUT_MS 45000

/* ---- 设置 / Settings ---- */

static uint32_t s_revision;

int tomato_wb_list_limit(void) { return tomato_srv_list_limit(NVS_NS); }
esp_err_t tomato_wb_list_limit_set(int n) { return tomato_srv_list_limit_set(NVS_NS, n); }
esp_err_t tomato_wb_config_load(tomato_wb_config_t* out) { return tomato_srv_config_load(NVS_NS, out); }

esp_err_t tomato_wb_config_save(const tomato_wb_config_t* cfg) {
    const esp_err_t err = tomato_srv_config_save(NVS_NS, cfg);
    if (err == ESP_OK) s_revision++;
    return err;
}

uint32_t tomato_wb_config_revision(void) { return s_revision; }

/* ---- 路径 / Paths ---- */

void tomato_wb_post_dir(const char* id, char* out, size_t cap) {
    char sid[24];
    tomato_safe_id(id, sid, sizeof(sid));
    snprintf(out, cap, TOMATO_WB_DIR "/p/%s", sid);
}

void tomato_wb_img_path(const char* id, int n, char* out, size_t cap) {
    char dir[64];
    tomato_wb_post_dir(id, dir, sizeof(dir));
    if (n <= 0) snprintf(out, cap, "%s/t.jpg", dir);
    else snprintf(out, cap, "%s/%d.jpg", dir, n);
}

void tomato_wb_comments_path(const char* id, char* out, size_t cap) {
    char dir[64];
    tomato_wb_post_dir(id, dir, sizeof(dir));
    snprintf(out, cap, "%s/c.json", dir);
}

void tomato_wb_square_path(const char* id, int k, char* out, size_t cap) {
    char dir[64];
    tomato_wb_post_dir(id, dir, sizeof(dir));
    snprintf(out, cap, "%s/s%d.jpg", dir, k);
}

static void mkdirs(void) {
    mkdir(TOMATO_WB_DIR, 0775);
    mkdir(TOMATO_WB_DIR "/p", 0775);
}

/* ---- JSON 小工具 / JSON helpers ---- */

static void copy_str(cJSON* to, const char* key, const cJSON* from, const char* from_key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(from, from_key);
    cJSON_AddStringToObject(to, key, cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}

static void copy_num(cJSON* to, const cJSON* from, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(from, key);
    cJSON_AddNumberToObject(to, key, cJSON_IsNumber(v) ? v->valuedouble : 0);
}

static int json_int(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static void prune_posts(const char* keep) {
    static const char* const LISTS[] = {TOMATO_WB_FOLLOW_FILE, TOMATO_WB_HOT_FILE};
    tomato_cache_prune(TOMATO_WB_DIR "/p", LISTS, 2, keep, POST_KEEP, POSTS_BYTES_MAX);
}

/* ---- 任务 / Jobs ---- */

// 列表：取最近“列表条数”条，关注列表保留旧文件里的“看过”标记，再补缩略图。
// List: fetch the latest "list size" posts, keep "seen" marks from the old following file, then fill thumbnails.
static esp_err_t run_feed(const tomato_srv_t* srv, bool hot, const volatile bool* cancel,
                          tomato_hupu_progress_t progress, char* msg, size_t cap) {
    const char* file = hot ? TOMATO_WB_HOT_FILE : TOMATO_WB_FOLLOW_FILE;
    char path[112];
    snprintf(path, sizeof(path), "/api/device/v1/feed?seg=%s&limit=%d", hot ? "hot" : "follow", tomato_wb_list_limit());
    cJSON* root = NULL;
    esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    cJSON* old = hot ? NULL : tomato_cache_read_json(file);
    const cJSON* old_items = cJSON_GetObjectItemCaseSensitive(old, "items");
    cJSON* out = cJSON_CreateObject();
    copy_num(out, root, "now");
    cJSON* items = cJSON_AddArrayToObject(out, "items");
    const cJSON* n;
    int count = 0;
    cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(root, "posts")) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(n, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        cJSON* x = cJSON_CreateObject();
        copy_str(x, "id", n, "id");
        copy_str(x, "user", n, "user");
        copy_str(x, "text", n, "text");
        copy_str(x, "rt_user", n, "rt_user");
        copy_str(x, "rt_text", n, "rt_text");
        copy_str(x, "source", n, "source");
        copy_num(x, n, "time");
        copy_num(x, n, "images");
        copy_num(x, n, "pics");
        copy_num(x, n, "ratio");
        copy_num(x, n, "likes");
        copy_num(x, n, "comments");
        copy_num(x, n, "reposts");
        // 新到的标“新”；以前看过的保持；第一次取到的都算看过。热门不标。/ New arrivals are unseen and earlier marks
        // stay; a first fetch counts as seen. Hot posts carry no marks.
        bool seen = hot || !old;
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
    err = tomato_cache_write_json(file, out);
    if (err != ESP_OK) { cJSON_Delete(out); snprintf(msg, cap, "写不进 TF 卡"); return err; }
    progress(1, count + 1);
    // 图：两张以上取前几张方形小图（列表里排一排），一张或旧服务器取封面缩略图。失败的跳过，下次刷新再补。
    // Images: with two or more, fetch the first square images (a strip in the list); with one, or from an older
    // server, the cover thumbnail. Failures wait for the next refresh.
    int i = 0;
    char dir[64], dest[96], sid[24];
    cJSON_ArrayForEach(n, items) {
        if (*cancel) break;
        i++;
        const char* id = cJSON_GetObjectItemCaseSensitive(n, "id")->valuestring;
        const int pics = json_int(n, "pics");
        if (json_int(n, "images") <= 0) continue;
        tomato_wb_post_dir(id, dir, sizeof(dir));
        mkdir(dir, 0775);
        tomato_safe_id(id, sid, sizeof(sid));
        if (pics >= 2) {
            for (int k = 1; k <= pics && k <= TOMATO_WB_LIST_SQUARES && !*cancel; k++) {
                tomato_wb_square_path(id, k, dest, sizeof(dest));
                if (tomato_file_exists(dest)) continue;
                snprintf(path, sizeof(path), "/api/device/v1/img/%s/s%d.jpg", sid, k);
                tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel);
            }
        } else {
            tomato_wb_img_path(id, 0, dest, sizeof(dest));
            if (!tomato_file_exists(dest)) {
                snprintf(path, sizeof(path), "/api/device/v1/img/%s/thumb.jpg", sid);
                tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel);
            }
        }
        progress(1 + i, count + 1);
    }
    cJSON_Delete(out);
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    prune_posts(NULL);
    snprintf(msg, cap, "%s %d 条", hot ? "热门" : "关注", count);
    return ESP_OK;
}

// 一条微博：全文存成 post.json，图片一张张取。images_only 时只补缺的图。
// One post: the full text goes to post.json, then images one by one. images_only fills missing images only.
static esp_err_t run_post(const tomato_srv_t* srv, const tomato_wb_req_t* r, const volatile bool* cancel,
                          tomato_hupu_progress_t progress, char* msg, size_t cap) {
    char sid[24], dir[64], file[96], path[112];
    tomato_safe_id(r->id, sid, sizeof(sid));
    if (!sid[0]) { snprintf(msg, cap, "微博编号不对"); return ESP_ERR_INVALID_ARG; }
    tomato_wb_post_dir(sid, dir, sizeof(dir));
    snprintf(file, sizeof(file), "%s/post.json", dir);
    int images = 0, pics = 0;
    if (r->images_only && tomato_file_exists(file)) {
        cJSON* post = tomato_cache_read_json(file);
        images = json_int(post, "images");
        pics = json_int(post, "pics");
        cJSON_Delete(post);
    } else {
        cJSON* root = NULL;
        snprintf(path, sizeof(path), "/api/device/v1/post/%s", sid);
        esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
        if (err != ESP_OK) return err;
        cJSON* out = cJSON_CreateObject();
        copy_str(out, "id", root, "id");
        copy_str(out, "user", root, "user");
        copy_str(out, "text", root, "text");
        copy_str(out, "rt_user", root, "rt_user");
        copy_str(out, "rt_text", root, "rt_text");
        copy_str(out, "source", root, "source");
        copy_num(out, root, "time");
        copy_num(out, root, "images");
        copy_num(out, root, "pics");
        copy_num(out, root, "ratio");
        const cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "starts");
        cJSON_AddItemToObject(out, "starts", cJSON_IsArray(st) ? cJSON_Duplicate(st, true) : cJSON_CreateArray());
        copy_num(out, root, "likes");
        copy_num(out, root, "comments");
        copy_num(out, root, "reposts");
        images = json_int(root, "images");
        pics = json_int(root, "pics");
        cJSON_Delete(root);
        mkdirs();
        mkdir(dir, 0775);
        err = tomato_cache_write_json(file, out);
        cJSON_Delete(out);
        if (err != ESP_OK) { snprintf(msg, cap, "写不进 TF 卡"); return err; }
    }
    // 正文页下面那排方形小图先取（很小），再一张张取大图。/ Fetch the small squares for the post page first (they are
    // tiny), then the full images one by one.
    // 连不上服务器就不再一张张等超时。/ Once the server is unreachable, stop waiting out a timeout per image.
    bool down = false;
    for (int k = 1; k <= pics && k <= TOMATO_WB_POST_SQUARES && !*cancel; k++) {
        char dest[96];
        tomato_wb_square_path(sid, k, dest, sizeof(dest));
        if (tomato_file_exists(dest)) continue;
        snprintf(path, sizeof(path), "/api/device/v1/img/%s/s%d.jpg", sid, k);
        if (tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel) == ESP_ERR_HTTP_CONNECT) { down = true; break; }
    }
    progress(1, images + 1);
    int failed = 0;
    for (int k = 1; k <= images && !*cancel && !down; k++) {
        char dest[96];
        tomato_wb_img_path(sid, k, dest, sizeof(dest));
        if (!tomato_file_exists(dest)) {
            snprintf(path, sizeof(path), "/api/device/v1/img/%s/%d.jpg", sid, k);
            const esp_err_t err = tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel);
            if (err != ESP_OK) failed++;
            if (err == ESP_ERR_HTTP_CONNECT) down = true;
        }
        progress(1 + k, images + 1);
    }
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    prune_posts(sid);
    if (down) snprintf(msg, cap, "连不上服务器，图片没下载完");
    else if (failed) snprintf(msg, cap, "%d 张图没下载成功", failed);
    else msg[0] = 0;
    return ESP_OK;
}

// 评论：服务器第一次会去微博取一次（前 20 条）并存下来。/ Comments: the server fetches them from Weibo once (top
// 20) and keeps them.
static esp_err_t run_comments(const tomato_srv_t* srv, const tomato_wb_req_t* r, const volatile bool* cancel,
                              char* msg, size_t cap) {
    char sid[24], dir[64], file[96], path[112];
    tomato_safe_id(r->id, sid, sizeof(sid));
    if (!sid[0]) { snprintf(msg, cap, "微博编号不对"); return ESP_ERR_INVALID_ARG; }
    snprintf(path, sizeof(path), "/api/device/v1/post/%s/comments?wait=0", sid);
    cJSON* root = NULL;
    esp_err_t err = tomato_srv_poll(srv, path, NULL, "pending", LIVE_TIMEOUT_MS, LIVE_MAX_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    cJSON* out = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(out, "list");
    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "comments")) {
        cJSON* x = cJSON_CreateObject();
        copy_str(x, "u", c, "user");
        copy_str(x, "t", c, "text");
        const cJSON* l = cJSON_GetObjectItemCaseSensitive(c, "likes");
        cJSON_AddNumberToObject(x, "l", cJSON_IsNumber(l) ? l->valuedouble : 0);
        cJSON_AddItemToArray(list, x);
    }
    cJSON_Delete(root);
    mkdirs();
    tomato_wb_post_dir(sid, dir, sizeof(dir));
    mkdir(dir, 0775);
    tomato_wb_comments_path(sid, file, sizeof(file));
    err = tomato_cache_write_json(file, out);
    cJSON_Delete(out);
    if (err != ESP_OK) { snprintf(msg, cap, "写不进 TF 卡"); return err; }
    msg[0] = 0;
    return ESP_OK;
}

static esp_err_t run_test(const tomato_srv_t* srv, const volatile bool* cancel, char* msg, size_t cap) {
    cJSON* root = NULL;
    char path[80];
    snprintf(path, sizeof(path), "/api/device/v1/feed?seg=follow&limit=%d", tomato_wb_list_limit());
    esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    const int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "posts"));
    cJSON_Delete(root);
    snprintf(msg, cap, n >= tomato_wb_list_limit() ? "正常 · 关注 %d+ 条" : "正常 · 关注 %d 条", n);
    return ESP_OK;
}

esp_err_t tomato_wb_run(const tomato_wb_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                        char* msg, size_t cap) {
    tomato_wb_config_t cfg;
    if (tomato_wb_config_load(&cfg) != ESP_OK) { snprintf(msg, cap, "还没设置 weibo 服务器"); return ESP_ERR_NOT_FOUND; }
    const tomato_srv_t srv = {.cfg = &cfg, .header = HEADER, .name = "weibo"};
    esp_err_t err;
    switch (r->job) {
    case TOMATO_WB_FEED: err = run_feed(&srv, r->hot, cancel, progress, msg, cap); break;
    case TOMATO_WB_POST: err = run_post(&srv, r, cancel, progress, msg, cap); break;
    case TOMATO_WB_COMMENTS: err = run_comments(&srv, r, cancel, msg, cap); break;
    case TOMATO_WB_TEST: err = run_test(&srv, cancel, msg, cap); break;
    default: err = ESP_ERR_INVALID_ARG; break;
    }
    memset(&cfg, 0, sizeof(cfg));
    return err;
}

// 关注列表里标记看过（页面在界面任务里调用）。/ Mark a following post as seen (UI task).
void tomato_wb_mark_seen(const char* id) { tomato_cache_mark_seen(TOMATO_WB_FOLLOW_FILE, id); }
