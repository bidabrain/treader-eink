/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * X 的数据层：只连用户自己部署的 x 服务器（仓库 x/ 目录），不直接访问 X。为你推荐和正在关注两个列表、单条推文
 * 的全文和图片、回复都存到 TF 卡 x/，页面只读这些缓存。服务器上“正在关注”的总开关记在 state.json，页面据此把
 * 这个分段变灰。图片服务器已经处理成灰度基线 JPEG。列表和推文的格式和微博一样，“来源”一栏换成 @账号、转推人、
 * 回复对象和视频标记，页面可以照微博的画法。
 *
 * English: X data layer. It talks only to the user's own x server (the repo's x/ directory), never to X itself.
 * The For You and Following lists, each post's full text and images, and replies go to x/ on the TF card and the page
 * reads only those caches. The server's Following master switch is kept in state.json so the page can grey that
 * segment out. The server already turned images into grayscale baseline JPEGs. Lists and posts use the Weibo format;
 * the "source" field carries the @handle, reposter, reply target and a video mark so the page draws them like Weibo.
 *
 * 冻结：只在后台同步任务里联网，由页面按用户操作启动；地址和令牌存在 NVS 的 x 命名空间，不写 TF 卡、不打日志。
 * 只读：不点赞、不回复、不转推。缓存有上限：推文目录超过 POST_KEEP 个时，只留两个列表里还有的。
 * Frozen: network only on the sync task, started by the page on user actions; the address and token live in NVS
 * namespace x, never on the TF card or in logs. Read only: no likes, replies or reposts. Caches are bounded: past
 * POST_KEEP post folders, only those still in the two lists stay.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tomato_internal.h"

#define NVS_NS "x"
#define HEADER "x-hub-token"
#define POST_KEEP 150
// 推文目录一共最多占这么多；超过就从最旧的删大图。/ Cap for all post folders; past it the oldest lose full images.
#define POSTS_BYTES_MAX (200ull * 1024 * 1024)
#define TIMEOUT_MS 15000

/* ---- 设置 / Settings ---- */

static uint32_t s_revision;

int tomato_x_list_limit(void) { return tomato_srv_list_limit(NVS_NS); }
esp_err_t tomato_x_list_limit_set(int n) { return tomato_srv_list_limit_set(NVS_NS, n); }
esp_err_t tomato_x_config_load(tomato_x_config_t* out) { return tomato_srv_config_load(NVS_NS, out); }

esp_err_t tomato_x_config_save(const tomato_x_config_t* cfg) {
    const esp_err_t err = tomato_srv_config_save(NVS_NS, cfg);
    if (err == ESP_OK) s_revision++;
    return err;
}

uint32_t tomato_x_config_revision(void) { return s_revision; }

/* ---- 路径 / Paths ---- */

void tomato_x_post_dir(const char* id, char* out, size_t cap) {
    char sid[28];
    tomato_safe_id(id, sid, sizeof(sid));
    snprintf(out, cap, TOMATO_X_DIR "/p/%s", sid);
}

void tomato_x_img_path(const char* id, int n, char* out, size_t cap) {
    char dir[64];
    tomato_x_post_dir(id, dir, sizeof(dir));
    if (n <= 0) snprintf(out, cap, "%s/t.jpg", dir);
    else snprintf(out, cap, "%s/%d.jpg", dir, n);
}

void tomato_x_comments_path(const char* id, char* out, size_t cap) {
    char dir[64];
    tomato_x_post_dir(id, dir, sizeof(dir));
    snprintf(out, cap, "%s/c.json", dir);
}

void tomato_x_square_path(const char* id, int k, char* out, size_t cap) {
    char dir[64];
    tomato_x_post_dir(id, dir, sizeof(dir));
    snprintf(out, cap, "%s/s%d.jpg", dir, k);
}

static void mkdirs(void) {
    mkdir(TOMATO_X_DIR, 0775);
    mkdir(TOMATO_X_DIR "/p", 0775);
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

static const char* json_str(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// 头部第二行的“来源”：@账号 · 谁转推 · 回复谁 · 视频。/ The header's "source": @handle · reposter · reply target ·
// video.
static void add_source(cJSON* to, const cJSON* from) {
    // 名字先按 UTF-8 截短，拼起来最多约 150 字节，放得进设备的 160。/ Names are cut on UTF-8 boundaries first;
    // the joined line stays near 150 bytes, within the device's 160.
    char src[160], screen[32], rt_by[52], reply[32];
    int n = 0;
    tomato_copy_utf8(screen, sizeof(screen), json_str(from, "screen"));
    tomato_copy_utf8(rt_by, sizeof(rt_by), json_str(from, "rt_by"));
    tomato_copy_utf8(reply, sizeof(reply), json_str(from, "reply_to"));
    if (screen[0]) n += snprintf(src + n, sizeof(src) - (size_t)n, "@%s", screen);
    if (rt_by[0] && n < (int)sizeof(src))
        n += snprintf(src + n, sizeof(src) - (size_t)n, "%s%s 转推", n ? " · " : "", rt_by);
    if (reply[0] && n < (int)sizeof(src))
        n += snprintf(src + n, sizeof(src) - (size_t)n, "%s回复 @%s", n ? " · " : "", reply);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(from, "video")) && n < (int)sizeof(src))
        snprintf(src + n, sizeof(src) - (size_t)n, "%s视频", n ? " · " : "");
    cJSON_AddStringToObject(to, "source", src);
}

static void prune_posts(const char* keep) {
    static const char* const LISTS[] = {TOMATO_X_FORYOU_FILE, TOMATO_X_FOLLOWING_FILE};
    tomato_cache_prune(TOMATO_X_DIR "/p", LISTS, 2, keep, POST_KEEP, POSTS_BYTES_MAX);
}

/* ---- 服务器上的开关 / Server switches ---- */

bool tomato_x_following_on(void) {
    cJSON* st = tomato_cache_read_json(TOMATO_X_STATE_FILE);
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(st, "following");
    const bool on = !cJSON_IsBool(v) || cJSON_IsTrue(v);
    cJSON_Delete(st);
    return on;
}

// 记下服务器说的“正在关注”开关（feed 的 feeds_on 或 status 的 feeds.following.on）。/ Keep the server's Following
// switch (feeds_on from a feed, or feeds.following.on from status).
static void save_following_on(bool on) {
    if (on == tomato_x_following_on() && tomato_file_exists(TOMATO_X_STATE_FILE)) return;
    cJSON* st = cJSON_CreateObject();
    cJSON_AddBoolToObject(st, "following", on);
    mkdirs();
    tomato_cache_write_json(TOMATO_X_STATE_FILE, st);
    cJSON_Delete(st);
}

static void state_from_feed(const cJSON* root) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "feeds_on"), "following");
    if (cJSON_IsBool(v)) save_following_on(cJSON_IsTrue(v));
}

/* ---- 任务 / Jobs ---- */

// 列表：取最近“列表条数”条，正在关注保留旧文件里的“看过”标记，再补缩略图。服务器关了正在关注时只记开关，不动旧缓存。
// List: fetch the latest "list size" posts, keep "seen" marks from the old Following file, then fill thumbnails. When
// the server has Following off, only the switch is kept and the old cache stays.
static esp_err_t run_feed(const tomato_srv_t* srv, bool following, const volatile bool* cancel,
                          tomato_hupu_progress_t progress, char* msg, size_t cap) {
    const char* file = following ? TOMATO_X_FOLLOWING_FILE : TOMATO_X_FORYOU_FILE;
    char path[112];
    snprintf(path, sizeof(path), "/api/device/v1/feed?seg=%s&limit=%d", following ? "following" : "foryou",
             tomato_x_list_limit());
    cJSON* root = NULL;
    esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    state_from_feed(root);
    if (following && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "on"))) {
        cJSON_Delete(root);
        snprintf(msg, cap, "服务器关了正在关注");
        return ESP_OK;
    }
    cJSON* old = following ? tomato_cache_read_json(file) : NULL;
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
        add_source(x, n);
        copy_num(x, n, "time");
        copy_num(x, n, "images");
        copy_num(x, n, "pics");
        copy_num(x, n, "ratio");
        copy_num(x, n, "likes");
        copy_num(x, n, "comments");
        copy_num(x, n, "reposts");
        // 新到的标“新”；以前看过的保持；第一次取到的都算看过。为你推荐不标。/ New arrivals are unseen and earlier
        // marks stay; a first fetch counts as seen. For You carries no marks.
        bool seen = !following || !old;
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
    // 图：两张以上取前几张方形小图，一张取封面缩略图。失败的跳过，下次刷新再补。/ Images: with two or more, the first
    // square images; with one, the cover thumbnail. Failures wait for the next refresh.
    int i = 0;
    char dir[64], dest[96], sid[28];
    cJSON_ArrayForEach(n, items) {
        if (*cancel) break;
        i++;
        const char* id = cJSON_GetObjectItemCaseSensitive(n, "id")->valuestring;
        const int pics = json_int(n, "pics");
        if (json_int(n, "images") <= 0) continue;
        tomato_x_post_dir(id, dir, sizeof(dir));
        mkdir(dir, 0775);
        tomato_safe_id(id, sid, sizeof(sid));
        if (pics >= 2) {
            for (int k = 1; k <= pics && k <= TOMATO_WB_LIST_SQUARES && !*cancel; k++) {
                tomato_x_square_path(id, k, dest, sizeof(dest));
                if (tomato_file_exists(dest)) continue;
                snprintf(path, sizeof(path), "/api/device/v1/img/%s/s%d.jpg", sid, k);
                tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel);
            }
        } else {
            tomato_x_img_path(id, 0, dest, sizeof(dest));
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
    snprintf(msg, cap, "%s %d 条", following ? "正在关注" : "为你推荐", count);
    return ESP_OK;
}

// 一条推文：全文存成 post.json，图片一张张取。images_only 时只补缺的图。
// One post: the full text goes to post.json, then images one by one. images_only fills missing images only.
static esp_err_t run_post(const tomato_srv_t* srv, const tomato_x_req_t* r, const volatile bool* cancel,
                          tomato_hupu_progress_t progress, char* msg, size_t cap) {
    char sid[28], dir[64], file[96], path[112];
    tomato_safe_id(r->id, sid, sizeof(sid));
    if (!sid[0]) { snprintf(msg, cap, "推文编号不对"); return ESP_ERR_INVALID_ARG; }
    tomato_x_post_dir(sid, dir, sizeof(dir));
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
        add_source(out, root);
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
    // 正文页下面那排方形小图先取（很小），再一张张取大图。/ Fetch the small squares for the post page first, then the
    // full images one by one.
    for (int k = 1; k <= pics && k <= TOMATO_WB_POST_SQUARES && !*cancel; k++) {
        char dest[96];
        tomato_x_square_path(sid, k, dest, sizeof(dest));
        if (tomato_file_exists(dest)) continue;
        snprintf(path, sizeof(path), "/api/device/v1/img/%s/s%d.jpg", sid, k);
        tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel);
    }
    progress(1, images + 1);
    int failed = 0;
    for (int k = 1; k <= images && !*cancel; k++) {
        char dest[96];
        tomato_x_img_path(sid, k, dest, sizeof(dest));
        if (!tomato_file_exists(dest)) {
            snprintf(path, sizeof(path), "/api/device/v1/img/%s/%d.jpg", sid, k);
            if (tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel) != ESP_OK) failed++;
        }
        progress(1 + k, images + 1);
    }
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    prune_posts(sid);
    if (failed) snprintf(msg, cap, "%d 张图没下载成功", failed);
    else msg[0] = 0;
    return ESP_OK;
}

// 回复：服务器第一次会在后台用浏览器去 X 取一次并存下来。这里用 wait=0，服务器马上回答；还在取就返回
// ESP_ERR_NOT_FINISHED，页面过几秒再问，后台任务不会被挂住，前台随时能插队。
// Replies: the server fetches them from X once in the background and keeps them. wait=0 makes it answer at once;
// while it is still fetching this returns ESP_ERR_NOT_FINISHED and the page asks again a few seconds later, so the
// worker is never held and the foreground can always cut in.
static esp_err_t run_comments(const tomato_srv_t* srv, const tomato_x_req_t* r, const volatile bool* cancel,
                              char* msg, size_t cap) {
    char sid[28], dir[64], file[96], path[112];
    tomato_safe_id(r->id, sid, sizeof(sid));
    if (!sid[0]) { snprintf(msg, cap, "推文编号不对"); return ESP_ERR_INVALID_ARG; }
    snprintf(path, sizeof(path), "/api/device/v1/post/%s/comments?wait=0", sid);
    cJSON* root = NULL;
    esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "pending"))) {
        cJSON_Delete(root);
        snprintf(msg, cap, "服务器正在取回复");
        return ESP_ERR_NOT_FINISHED;
    }
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
    tomato_x_post_dir(sid, dir, sizeof(dir));
    mkdir(dir, 0775);
    tomato_x_comments_path(sid, file, sizeof(file));
    err = tomato_cache_write_json(file, out);
    cJSON_Delete(out);
    if (err != ESP_OK) { snprintf(msg, cap, "写不进 TF 卡"); return err; }
    msg[0] = 0;
    return ESP_OK;
}

// 问服务器状态：记下正在关注开关，返回账号状态说明。/ Ask the server's status: keep the Following switch and describe
// the account state.
static esp_err_t run_state(const tomato_srv_t* srv, const volatile bool* cancel, bool* following, char* msg,
                           size_t cap) {
    cJSON* root = NULL;
    esp_err_t err = tomato_srv_json(srv, "/api/device/v1/status", TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    const cJSON* feeds = cJSON_GetObjectItemCaseSensitive(root, "feeds");
    const cJSON* on = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(feeds, "following"), "on");
    *following = !cJSON_IsBool(on) || cJSON_IsTrue(on);
    save_following_on(*following);
    const char* account = json_str(root, "account");
    if (!strcmp(account, "need_login")) snprintf(msg, cap, "服务器上的 X 需要重新登录");
    else if (!strcmp(account, "locked")) snprintf(msg, cap, "服务器上的 X 需要验证账号");
    else msg[0] = 0;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t run_test(const tomato_srv_t* srv, const volatile bool* cancel, char* msg, size_t cap) {
    bool following = true;
    char why[64];
    esp_err_t err = run_state(srv, cancel, &following, why, sizeof(why));
    if (err != ESP_OK) { snprintf(msg, cap, "%s", why); return err; }
    cJSON* root = NULL;
    char path[80];
    snprintf(path, sizeof(path), "/api/device/v1/feed?seg=foryou&limit=%d", tomato_x_list_limit());
    err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    const int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "posts"));
    cJSON_Delete(root);
    if (why[0]) snprintf(msg, cap, "已连上 · %s", why);
    else snprintf(msg, cap, n >= tomato_x_list_limit() ? "正常 · 推荐 %d+ 条%s" : "正常 · 推荐 %d 条%s", n,
                  following ? "" : " · 正在关注已关");
    return ESP_OK;
}

esp_err_t tomato_x_run(const tomato_x_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                       char* msg, size_t cap) {
    tomato_x_config_t cfg;
    if (tomato_x_config_load(&cfg) != ESP_OK) { snprintf(msg, cap, "还没设置 x 服务器"); return ESP_ERR_NOT_FOUND; }
    const tomato_srv_t srv = {.cfg = &cfg, .header = HEADER, .name = "x"};
    esp_err_t err;
    bool following;
    switch (r->job) {
    case TOMATO_X_FEED: err = run_feed(&srv, r->following, cancel, progress, msg, cap); break;
    case TOMATO_X_POST: err = run_post(&srv, r, cancel, progress, msg, cap); break;
    case TOMATO_X_COMMENTS: err = run_comments(&srv, r, cancel, msg, cap); break;
    case TOMATO_X_STATE: err = run_state(&srv, cancel, &following, msg, cap); break;
    case TOMATO_X_TEST: err = run_test(&srv, cancel, msg, cap); break;
    default: err = ESP_ERR_INVALID_ARG; break;
    }
    memset(&cfg, 0, sizeof(cfg));
    return err;
}

// 正在关注列表里标记看过（页面在界面任务里调用）。/ Mark a Following post as seen (UI task).
void tomato_x_mark_seen(const char* id) { tomato_cache_mark_seen(TOMATO_X_FOLLOWING_FILE, id); }
