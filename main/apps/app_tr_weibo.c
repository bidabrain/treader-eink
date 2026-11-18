/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 微博标签页（只读）：顶部分段 关注 / 热门；列表一行一条（作者、时间、正文前几行、转评赞，有图的右边一张缩略图）；
 * 点开先看全文（含转发原文），往后翻是图片（一屏一张），再往后是评论。数据来自用户自建的 weibo 服务器，由
 * components/tomato 的后台任务取到 TF 卡 weibo/，本页只读缓存；缩略图和大图的缩放抖动交给 hupu_img。
 *
 * English: the Weibo tab (read only): Following / Hot segments; the list shows one post per row (author, time, the
 * first lines of text, counts, and a thumbnail on the right when there are images). A post opens on its full text
 * (with the reposted original), then images one per screen, then comments. Data comes from the user's own weibo
 * server; the components/tomato task stores it under weibo/ on the TF card and this page reads only that cache;
 * hupu_img scales and dithers thumbnails and images.
 *
 * 冻结：只在用户点“刷新”、切到没缓存的分段、打开没缓存的微博或评论时联网，进页不自动刷新。服务器自己按面板设置定时
 * 去微博拉取，设备只取服务器上已有的；只有评论第一次打开时服务器才去微博取一次。没设置服务器时只显示“去设置”。大块
 * 数据进页分配、离页释放。不点赞、不评论、不转发。
 * Frozen: network only on Refresh, switching to a segment without a cache, or opening an uncached post or comments;
 * entering never refreshes. The server fetches Weibo on its own schedule and the device takes only what the server
 * has; only a post's first comments request makes the server ask Weibo. Without a server the page only offers "Set
 * up". Large buffers are allocated on entry and freed on exit. No likes, comments or reposts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hupu_img.h"
#include "tomato.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "tr_wallsave.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_weibo"
#define POLL_MS 400
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CW (TR_W - 2 * TR_MARGIN)

// 列表：每页 4 行。/ List: 4 rows per page.
#define LIST_MAX 100
#define LIST_TOP 284
#define PAGER_Y 1036
#define PER_PAGE 4
#define ROW_H 188
#define THUMB 150
#define ROW_TEXT_LINES 3
#define STATUS_Y 240
#define REFRESH_RECT (EpdRect){TR_W - TR_MARGIN - 240, 70, 240, 80}
#define SETUP_RECT (EpdRect){TR_MARGIN, 420, 240, 72}
#define DIALOG_RECT (EpdRect){92, 500, TR_W - 184, 220}

// 微博 / Post
#define POST_SEG (EpdRect){TR_MARGIN, 150, CW, 64}
#define BODY_TOP 240
#define BODY_FIRST (BODY_TOP + 64)   // 第一页先写一行时间来源 / Page one starts with a time and source line
#define BODY_BOTTOM 1100
#define BODY_PX 32
#define BODY_LH 52
#define TEXT_PAGER_Y 1124
#define LINES_MAX 1600
#define PAGES_MAX 200
#define COMMENTS_MAX 20
#define COMMENT_LINES 5
#define COMMENT_PX 30
#define COMMENT_LH 44
#define IMAGE_BAND 96           // 看图底部的白条 / White strip under the image

typedef enum { V_LIST, V_IMAGE, V_TEXT } view_t;
typedef enum { SEG_FOLLOW, SEG_HOT } seg_t;
typedef enum { T_BODY, T_COMMENTS } text_tab_t;

typedef struct {
    char id[24];
    char user[48];
    char text[440];
    char rt_user[48];
    char rt_text[248];
    double time;          // 毫秒或秒 / Milliseconds or seconds
    int16_t images;
    int32_t likes, comments, reposts;
    bool seen;
} item_t;

typedef struct {
    const char* user;
    const char* text;
    int likes;
} comment_t;

typedef struct {
    uint32_t off;
    uint16_t len;
} line_t;

/* ---- 状态 / State ---- */

static view_t s_view;
static seg_t s_seg;
static char s_msg[96];
static int64_t s_poll_ms;
static bool s_has_cfg;

// 后台任务 / Background job
static bool s_job_active;
static uint32_t s_job_seq;           // 本页任务的序号，用来认出“是不是自己的”/ Our job's sequence number
static tomato_wb_req_t s_job;
static tomato_status_t s_status;
static bool s_step_loaded;
static tomato_wb_req_t s_pending;
static bool s_has_pending;

// 列表 / List
static item_t* s_items;
static int s_item_count;
static int s_list_page;
static double s_list_time;           // 服务器给列表的时间（秒）/ Server time of the list (seconds)

// 关注列表刷新期间点开的微博：刷新会覆盖 follow.json，等它结束再写“看过”。
// Posts opened while the following list refreshes: the refresh rewrites follow.json, so mark them seen afterwards.
#define SEEN_PENDING_MAX 16
static char s_seen_pending[SEEN_PENDING_MAX][24];
static int s_seen_pending_count;

// 正在等服务器给的微博（等待框）。/ A post being fetched from the server (the wait dialog).
static bool s_wait;
static char s_wait_id[24];

// 微博 / Post
static char* s_arena;
static size_t s_arena_len, s_arena_cap;
static char s_id[24];
static bool s_post_loaded;
static const char *s_user, *s_source, *s_body;
static double s_time;
static int s_images, s_likes, s_comment_total, s_reposts;
static comment_t* s_comments;
static int s_comment_count;
static bool s_comments_loaded;       // c.json 读到了 / c.json was read
static char s_comment_msg[96];       // 评论没取到的原因 / Why comments failed
static line_t* s_lines;
static int s_line_count;
static uint16_t* s_pages;
static int s_page_count;
static uint8_t s_comment_pages[COMMENTS_MAX + 1];
static int s_comment_page_count;
static text_tab_t s_tab;
static int s_page;
static int s_image;                  // 从 0 开始 / 0-based
static tr_wallsave_t s_ws;             // 看图时的存为壁纸工具条 / Save-as-wallpaper toolbar
static bool s_image_missing;         // 当前图还没下好，任务进行中要回头看 / Current image not downloaded yet

/* ---- 内存 / Memory ---- */

#define FREE(p) do { heap_caps_free(p); (p) = NULL; } while (0)

static void free_post(void) {
    FREE(s_arena);
    s_arena_len = s_arena_cap = 0;
    s_post_loaded = false;
    s_comments_loaded = false;
    s_comment_msg[0] = 0;
    s_comment_count = s_line_count = s_page_count = s_comment_page_count = 0;
    s_images = 0;
}

static void free_all(void) {
    free_post();
    FREE(s_items);
    FREE(s_comments);
    FREE(s_lines);
    FREE(s_pages);
    s_item_count = 0;
    hupu_img_release();
}

static bool alloc_all(void) {
    if (!s_items) s_items = heap_caps_calloc(LIST_MAX, sizeof(item_t), PSRAM_CAPS);
    if (!s_comments) s_comments = heap_caps_calloc(COMMENTS_MAX, sizeof(comment_t), PSRAM_CAPS);
    if (!s_lines) s_lines = heap_caps_calloc(LINES_MAX, sizeof(line_t), PSRAM_CAPS);
    if (!s_pages) s_pages = heap_caps_calloc(PAGES_MAX, sizeof(uint16_t), PSRAM_CAPS);
    const bool ok = s_items && s_comments && s_lines && s_pages;
    if (!ok) { ESP_LOGE(TAG, "out of memory"); free_all(); }
    return ok;
}

/* ---- 文件 / Files ---- */

static cJSON* read_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < 1024 * 1024 ? heap_caps_malloc((size_t)n + 1, PSRAM_CAPS) : NULL;
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

static void str(char* out, size_t cap, const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
    else if (cap) out[0] = 0;
}

static double dnum(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

// 从 JPEG 文件头读宽高（服务器出的是基线 JPEG，SOF 在开头不远）。/ Read width and height from the JPEG header
// (the server writes baseline JPEGs with the SOF near the start).
static bool jpeg_size(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t b[4];
    bool ok = false;
    if (fread(b, 1, 2, f) == 2 && b[0] == 0xFF && b[1] == 0xD8) {
        for (int guard = 0; guard < 64; guard++) {
            int c = fgetc(f);
            while (c == 0xFF) c = fgetc(f);
            if (c == EOF) break;
            if (fread(b, 1, 2, f) != 2) break;
            const int len = (b[0] << 8) | b[1];
            if ((c >= 0xC0 && c <= 0xC3) || (c >= 0xC5 && c <= 0xC7) || (c >= 0xC9 && c <= 0xCB)) {
                uint8_t s[5];
                if (fread(s, 1, 5, f) == 5) {
                    *h = (s[1] << 8) | s[2];
                    *w = (s[3] << 8) | s[4];
                    ok = *w > 0 && *h > 0;
                }
                break;
            }
            if (len < 2 || fseek(f, len - 2, SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    return ok;
}

// 数字缩成“1.2万”。/ Shorten counts to "1.2万".
static void fmt_count(char* out, size_t cap, int n) {
    if (n >= 100000000) snprintf(out, cap, "%.1f亿", n / 100000000.0);
    else if (n >= 10000) snprintf(out, cap, "%.1f万", n / 10000.0);
    else snprintf(out, cap, "%d", n);
}

// 今天的写时分，别的写月日。/ Today shows hours and minutes; other days show month and day.
static void fmt_time(char* out, size_t cap, double t, bool long_form) {
    out[0] = 0;
    if (t < 1e9) return;
    const time_t when = (time_t)(t > 1e11 ? t / 1000 : t);
    struct tm tm, now_tm;
    localtime_r(&when, &tm);
    const time_t now = time(NULL);
    localtime_r(&now, &now_tm);
    const bool today = now > 1735689600 && tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday;
    if (long_form) snprintf(out, cap, "%02d-%02d %02d:%02d", (tm.tm_mon + 1) % 100, tm.tm_mday % 100, tm.tm_hour % 100,
                            tm.tm_min % 100);
    else if (today) snprintf(out, cap, "%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100);
    else snprintf(out, cap, "%02d-%02d", (tm.tm_mon + 1) % 100, tm.tm_mday % 100);
}

/* ---- 后台任务 / Background jobs ---- */

static bool start_req(const tomato_wb_req_t* r) {
    esp_err_t err = tomato_sync_start_wb(r);
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "%s", err == ESP_ERR_INVALID_STATE ? "后台正在同步，稍后再试" : "无法开始加载");
        return false;
    }
    s_job = *r;
    s_job_seq = tomato_sync_seq();
    s_job_active = true;
    s_step_loaded = false;
    s_msg[0] = 0;
    tomato_sync_get_status(&s_status);
    return true;
}

// 后台正在跑的是不是本页发起的任务（别的页的任务不能当成自己的去取消）。/ Whether the running job is the one this
// page started (another page's job must never be cancelled as ours).
static bool ours_running(void) {
    return s_job_active && tomato_sync_busy() && tomato_sync_seq() == s_job_seq;
}

// 前台优先：本页的旧任务和别的页留下的浏览任务立刻取消；下载和同步不打断，排在它后面。
// Foreground first: cancel this page's old job and browsing jobs left by other pages at once; downloads and sync are
// left to finish and this request waits behind them.
static bool start_job(tomato_wb_job_t job, const char* id, bool hot, bool images_only) {
    tomato_wb_req_t req = {.job = job, .hot = hot, .images_only = images_only};
    if (id) snprintf(req.id, sizeof(req.id), "%s", id);
    if (tomato_sync_busy()) {
        const bool yields = ours_running() || tomato_sync_preemptible();
        if (yields) tomato_sync_cancel();
        s_pending = req;
        s_has_pending = true;
        if (yields) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "后台正在下载或同步，完成后自动开始");
        return true;
    }
    s_has_pending = false;
    return start_req(&req);
}

static void drop_job(tomato_wb_job_t job) {
    if (s_has_pending && s_pending.job == job) s_has_pending = false;
    if (s_job.job == job && ours_running()) tomato_sync_cancel();
}

static bool job_running(tomato_wb_job_t job) {
    return (s_has_pending && s_pending.job == job) || (s_job.job == job && ours_running());
}

// 当前分段的列表任务在跑吗。/ Whether the list job for the current segment runs.
static bool list_job_running(void) {
    if (s_has_pending && s_pending.job == TOMATO_WB_FEED && s_pending.hot == (s_seg == SEG_HOT)) return true;
    return s_job.job == TOMATO_WB_FEED && s_job.hot == (s_seg == SEG_HOT) && ours_running();
}

static void flush_seen(void) {
    for (int i = 0; i < s_seen_pending_count; i++) tomato_wb_mark_seen(s_seen_pending[i]);
    s_seen_pending_count = 0;
}

static bool follow_job_running(void) {
    if (s_has_pending && s_pending.job == TOMATO_WB_FEED && !s_pending.hot) return true;
    return s_job.job == TOMATO_WB_FEED && !s_job.hot && ours_running();
}

static const char* loading_text(const char* what) {
    return s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : what;
}

/* ---- 列表数据 / List data ---- */

static const char* list_file(void) { return s_seg == SEG_FOLLOW ? TOMATO_WB_FOLLOW_FILE : TOMATO_WB_HOT_FILE; }

static int list_pages(void) { return s_item_count ? (s_item_count + PER_PAGE - 1) / PER_PAGE : 1; }

static void load_list(void) {
    s_item_count = 0;
    cJSON* root = read_json(list_file());
    s_list_time = root ? dnum(root, "now") : 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (s_item_count >= LIST_MAX) break;
        item_t* x = &s_items[s_item_count];
        memset(x, 0, sizeof(*x));
        str(x->id, sizeof(x->id), it, "id");
        if (!x->id[0]) continue;
        str(x->user, sizeof(x->user), it, "user");
        str(x->text, sizeof(x->text), it, "text");
        str(x->rt_user, sizeof(x->rt_user), it, "rt_user");
        str(x->rt_text, sizeof(x->rt_text), it, "rt_text");
        x->time = dnum(it, "time");
        x->images = (int16_t)dnum(it, "images");
        x->likes = (int32_t)dnum(it, "likes");
        x->comments = (int32_t)dnum(it, "comments");
        x->reposts = (int32_t)dnum(it, "reposts");
        x->seen = s_seg == SEG_HOT || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "seen"));
        s_item_count++;
    }
    cJSON_Delete(root);
    if (s_list_page >= list_pages()) s_list_page = list_pages() - 1;
}

static bool post_cached(const char* id) {
    char dir[64], file[80];
    tomato_wb_post_dir(id, dir, sizeof(dir));
    snprintf(file, sizeof(file), "%s/post.json", dir);
    return exists(file);
}

/* ---- 微博数据 / Post data ---- */

static const char* keep(const char* s) {
    const size_t n = strlen(s) + 1;
    if (s_arena_len + n > s_arena_cap) return "";
    char* out = s_arena + s_arena_len;
    memcpy(out, s, n);
    s_arena_len += n;
    return out;
}

static const char* keep_key(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return keep(cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}

// 正文后面接转发原文：空一行，“转发 @作者：”，再原文。/ The reposted original follows the text: a blank line,
// "转发 @author:", then the original.
static const char* keep_body(const cJSON* root) {
    const cJSON* t = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON* ru = cJSON_GetObjectItemCaseSensitive(root, "rt_user");
    const cJSON* rt = cJSON_GetObjectItemCaseSensitive(root, "rt_text");
    const char* text = cJSON_IsString(t) && t->valuestring ? t->valuestring : "";
    const char* rt_user = cJSON_IsString(ru) && ru->valuestring ? ru->valuestring : "";
    const char* rt_text = cJSON_IsString(rt) && rt->valuestring ? rt->valuestring : "";
    if (!rt_text[0]) return keep(text);
    const size_t n = strlen(text) + strlen(rt_user) + strlen(rt_text) + 32;
    if (s_arena_len + n > s_arena_cap) return keep(text);
    char* out = s_arena + s_arena_len;
    const int w = snprintf(out, n, "%s%s转发 @%s：\n%s", text, text[0] ? "\n\n" : "", rt_user[0] ? rt_user : "原微博",
                           rt_text);
    s_arena_len += (size_t)(w > 0 ? w : 0) + 1;
    return out;
}

static int body_top(int page) { return page ? BODY_TOP : BODY_FIRST; }

static void layout_body(void) {
    s_line_count = 0;
    const char* p = s_body;
    while (*p && s_line_count < LINES_MAX) {
        size_t n = tr_line_bytes(p, BODY_PX, CW);
        if (n > 0xFFFF) n = 0xFFFF;
        s_lines[s_line_count++] = (line_t){.off = (uint32_t)(p - s_body), .len = (uint16_t)n};
        p += n;
        if (*p == '\n') p++;
    }
    s_pages[0] = 0;
    s_page_count = 1;
    int y = body_top(0);
    for (int i = 0; i < s_line_count; i++) {
        if (y + BODY_LH > BODY_BOTTOM) {
            if (s_page_count >= PAGES_MAX) break;
            s_pages[s_page_count++] = (uint16_t)i;
            y = body_top(1);
        }
        y += BODY_LH;
    }
}

static int comment_lines(const comment_t* c) {
    const char* p = c->text;
    int n = 0;
    while (*p && n < COMMENT_LINES) { p += tr_line_bytes(p, COMMENT_PX, CW); if (*p == '\n') p++; n++; }
    return n ? n : 1;
}

static int comment_height(const comment_t* c) { return 40 + comment_lines(c) * COMMENT_LH + 32; }

static void layout_comments(void) {
    int pages = 0, y = BODY_BOTTOM;
    for (int i = 0; i < s_comment_count; i++) {
        const int h = comment_height(&s_comments[i]);
        if (y + h > BODY_BOTTOM && y != BODY_TOP) { s_comment_pages[pages++] = (uint8_t)i; y = BODY_TOP; }
        y += h;
    }
    if (!pages) s_comment_pages[pages++] = 0;
    s_comment_pages[pages] = (uint8_t)s_comment_count;
    s_comment_page_count = pages;
}

// 读评论缓存；字符串放进同一个内存池（池按两个文件的大小开）。/ Read the cached comments into the same arena (sized
// for both files).
static void load_comments(void) {
    s_comment_count = 0;
    char path[96];
    tomato_wb_comments_path(s_id, path, sizeof(path));
    cJSON* root = read_json(path);
    s_comments_loaded = root != NULL;
    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "list")) {
        if (s_comment_count >= COMMENTS_MAX) break;
        comment_t* x = &s_comments[s_comment_count++];
        x->user = keep_key(c, "u");
        x->text = keep_key(c, "t");
        x->likes = (int)dnum(c, "l");
    }
    cJSON_Delete(root);
    layout_comments();
}

static bool load_post(const char* id) {
    free_post();
    char dir[64], path[80], cpath[96];
    tomato_wb_post_dir(id, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/post.json", dir);
    struct stat st, cst;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    cJSON* root = read_json(path);
    if (!root) return false;
    // 内存池：正文文件加评论文件（评论可能稍后才到，按 32 KB 预留）。/ Arena: the post file plus the comments file
    // (comments may arrive later, so reserve 32 KB).
    tomato_wb_comments_path(id, cpath, sizeof(cpath));
    const size_t comments = stat(cpath, &cst) == 0 && cst.st_size > 32768 ? (size_t)cst.st_size : 32768;
    s_arena_cap = (size_t)st.st_size + comments + 1024;
    s_arena = heap_caps_malloc(s_arena_cap, PSRAM_CAPS);
    if (!s_arena) { cJSON_Delete(root); s_arena_cap = 0; return false; }
    s_user = keep_key(root, "user");
    s_source = keep_key(root, "source");
    s_body = keep_body(root);
    s_time = dnum(root, "time");
    s_images = (int)dnum(root, "images");
    s_likes = (int)dnum(root, "likes");
    s_comment_total = (int)dnum(root, "comments");
    s_reposts = (int)dnum(root, "reposts");
    cJSON_Delete(root);
    snprintf(s_id, sizeof(s_id), "%s", id);
    layout_body();
    load_comments();
    s_post_loaded = true;
    return true;
}

static void image_path(int k, char* out, size_t cap) { tomato_wb_img_path(s_id, k + 1, out, cap); }

static bool post_missing_images(void) {
    char path[96];
    for (int k = 0; k < s_images; k++) {
        image_path(k, path, sizeof(path));
        if (!exists(path)) return true;
    }
    return false;
}

// 第 k 张图放进看图区域的尺寸。/ Size of image k fitted to the viewing area.
static bool image_fit(int k, int* w, int* h) {
    char path[96];
    image_path(k, path, sizeof(path));
    int iw, ih;
    if (!jpeg_size(path, &iw, &ih)) return false;
    const int area_h = TR_H - IMAGE_BAND;
    *w = TR_W;
    *h = (int)((int64_t)TR_W * ih / iw);
    if (*h > area_h) { *h = area_h; *w = (int)((int64_t)area_h * iw / ih); }
    if (*w < 8) *w = 8;
    if (*h < 8) *h = 8;
    return true;
}

static int text_pages(void) { return s_tab == T_BODY ? s_page_count : s_comment_page_count; }

/* ---- 绘制 / Drawing ---- */

static void draw_empty(uint8_t* fb, int y, const char* text) { tr_wrap(fb, TR_MARGIN, y, CW, 30, 46, text, 5); }

static void draw_header(uint8_t* fb) {
    tr_title(fb, "微博");
    if (s_has_cfg) {
        if (list_job_running()) {
            tr_text(fb, TR_W - TR_MARGIN, 98, 30, "拉取中…", EPD_DRAW_ALIGN_RIGHT, false);
        } else if (!tomato_sync_busy()) {
            tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - ttf_text_width_px(30, "刷新") - 26, 116, false);
            tr_text(fb, TR_W - TR_MARGIN, 98, 30, "刷新", EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    static const char* const SEG[2] = {"关注", "热门"};
    tr_segment(fb, TR_SEGMENT_RECT, SEG, 2, (int)s_seg);
}

// 一行：作者（没看过的前面黑底“新”）、时间；正文前三行（转发的接原文）；转评赞；有图的右边一张缩略图。
// One row: author (a black "new" tag before unseen posts) and time; three lines of text (reposts continue with the
// original); counts; a thumbnail on the right when there are images.
static void draw_row(uint8_t* fb, const item_t* x, int y) {
    char thumb[96];
    tomato_wb_img_path(x->id, 0, thumb, sizeof(thumb));
    const bool has_thumb = x->images > 0 && exists(thumb);
    const int text_w = CW - (has_thumb ? THUMB + 18 : 0);
    if (has_thumb) {
        const int tx = TR_W - TR_MARGIN - THUMB, ty = y + 14;
        const uint8_t* px = hupu_img_get(thumb, THUMB, THUMB);
        if (px) hupu_img_draw(fb, tx, ty, THUMB, THUMB, px, true);
        else epd_draw_rect((EpdRect){tx, ty, THUMB, THUMB}, TR_GRAY_LINE, fb);
        if (x->images > 1) {
            char n[12];
            snprintf(n, sizeof(n), "%d 图", x->images);
            const int w = ttf_text_width_px(20, n) + 16;
            const EpdRect r = {tx + THUMB - w - 6, ty + THUMB - 36, w, 30};
            epd_fill_rect(r, UI_GRAY_BLACK, fb);
            tr_text_inv(fb, r.x + w / 2, r.y + 4, 20, n, EPD_DRAW_ALIGN_CENTER, true);
        }
    }
    int ux = TR_MARGIN;
    if (!x->seen) {
        epd_fill_rect((EpdRect){ux, y + 12, 44, 30}, UI_GRAY_BLACK, fb);
        tr_text_inv(fb, ux + 22, y + 14, 20, "新", EPD_DRAW_ALIGN_CENTER, true);
        ux += 54;
    }
    char when[16], line[800];
    fmt_time(when, sizeof(when), x->time, false);
    const int when_w = ttf_text_width_px(22, when);
    snprintf(line, sizeof(line), "%s", x->user[0] ? x->user : "微博用户");
    tr_fit(line, 26, TR_MARGIN + text_w - ux - when_w - 16);
    tr_text(fb, ux, y + 10, 26, line, EPD_DRAW_ALIGN_LEFT, true);
    tr_text(fb, TR_MARGIN + text_w, y + 13, 22, when, EPD_DRAW_ALIGN_RIGHT, false);
    if (x->rt_text[0])
        snprintf(line, sizeof(line), "%s%s// @%s：%s", x->text, x->text[0] ? " " : "", x->rt_user, x->rt_text);
    else
        snprintf(line, sizeof(line), "%s", x->text[0] ? x->text : "（没有文字）");
    tr_wrap(fb, TR_MARGIN, y + 48, text_w, 27, 36, line, ROW_TEXT_LINES);
    char a[16], b[16], c[16];
    fmt_count(a, sizeof(a), x->reposts);
    fmt_count(b, sizeof(b), x->comments);
    fmt_count(c, sizeof(c), x->likes);
    snprintf(line, sizeof(line), "转发 %s · 评论 %s · 赞 %s", a, b, c);
    tr_fit(line, 22, text_w);
    tr_text(fb, TR_MARGIN, y + 48 + ROW_TEXT_LINES * 36 + 4, 22, line, EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_rows(uint8_t* fb) {
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = s_list_page * PER_PAGE + k;
        if (i >= s_item_count) break;
        const int y = LIST_TOP + k * ROW_H;
        draw_row(fb, &s_items[i], y);
        if (k + 1 < PER_PAGE && i + 1 < s_item_count) tr_hline(fb, y + ROW_H - 4, TR_MARGIN, CW, true);
    }
}

static void draw_list(uint8_t* fb) {
    draw_header(fb);
    if (!s_has_cfg) {
        draw_empty(fb, LIST_TOP, "还没连接 weibo 服务器。先在电脑或 NAS 上部署 weibo（见项目 weibo/ 目录），"
                                 "再到“设置 › 内容来源 › 微博”填服务器地址和设备令牌。");
        tr_pill(fb, SETUP_RECT, "去设置", true, 30);
        return;
    }
    char line[160];
    const bool loading = list_job_running();
    if (loading) {
        snprintf(line, sizeof(line), "%s", loading_text(s_step_loaded ? "正在下载缩略图…" : "正在加载…"));
    } else if (s_msg[0]) {
        snprintf(line, sizeof(line), "%s", s_msg);
    } else {
        char when[16] = "";
        if (s_list_time > 1735689600) fmt_time(when, sizeof(when), s_list_time, true);
        snprintf(line, sizeof(line), "%s · %d 条%s%s", s_seg == SEG_FOLLOW ? "关注的人" : "热门", s_item_count,
                 when[0] ? " · 刷新于 " : "", when);
    }
    tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - 12, CW, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_item_count) {
        draw_empty(fb, LIST_TOP + 20, loading ? "加载中，请稍候。" : s_seg == SEG_FOLLOW ?
                   "还没有缓存。联网后点右上角“刷新”，从服务器取关注的人的微博；之后不联网也能看。" :
                   "还没有缓存。点右上角“刷新”，从服务器取热门微博。");
        return;
    }
    draw_rows(fb);
    tr_pager(fb, PAGER_Y, s_list_page, list_pages());
}

static void draw_wait(uint8_t* fb) {
    const EpdRect r = DIALOG_RECT;
    epd_fill_rect(r, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, r, 22, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), 21, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(r, 2), 20, UI_GRAY_BLACK);
    const int cx = r.x + r.width / 2;
    const bool failed = !job_running(TOMATO_WB_POST) && s_msg[0];
    if (failed) {
        tr_text(fb, cx, r.y + 40, 32, "没能打开这条", EPD_DRAW_ALIGN_CENTER, true);
        tr_wrap(fb, r.x + 30, r.y + 96, r.width - 60, 26, 36, s_msg, 2);
        tr_text(fb, cx, r.y + 168, 24, "点任意处关闭", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    tr_text(fb, cx, r.y + 44, 34, "正在加载这条微博…", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, cx, r.y + 104, 26, loading_text("从服务器取全文和图片"), EPD_DRAW_ALIGN_CENTER, false);
    tr_text(fb, cx, r.y + 156, 26, "点框外取消", EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_image(uint8_t* fb) {
    s_image_missing = false;
    char path[96];
    image_path(s_image, path, sizeof(path));
    int w, h;
    if (exists(path) && image_fit(s_image, &w, &h)) {
        const uint8_t* px = hupu_img_get(path, w, h);
        if (px) hupu_img_draw(fb, (TR_W - w) / 2, (TR_H - IMAGE_BAND - h) / 2, w, h, px, false);
        else tr_text(fb, TR_W / 2, (TR_H - IMAGE_BAND) / 2, 30,
                     hupu_img_failed(path, w, h) ? "这张图打不开" : "正在处理图片…", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        const bool loading = job_running(TOMATO_WB_POST);
        s_image_missing = loading;
        tr_text(fb, TR_W / 2, (TR_H - IMAGE_BAND) / 2, 30, loading ? loading_text("图片下载中…") : "图片还没下载",
                EPD_DRAW_ALIGN_CENTER, false);
    }
    epd_fill_rect((EpdRect){0, TR_H - IMAGE_BAND, TR_W, IMAGE_BAND}, UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_H - IMAGE_BAND, 0, TR_W, false);
    char label[32];
    snprintf(label, sizeof(label), "%d / %d", s_image + 1, s_images);
    tr_text(fb, TR_MARGIN, TR_H - IMAGE_BAND + 28, 30, "‹ 正文", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W / 2, TR_H - IMAGE_BAND + 28, 30, label, EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, TR_W - TR_MARGIN, TR_H - IMAGE_BAND + 28, 30, "评论 ›", EPD_DRAW_ALIGN_RIGHT, false);
    tr_wallsave_draw(fb, &s_ws);
}

static void draw_text_header(uint8_t* fb) {
    char back[80];
    snprintf(back, sizeof(back), "‹ %s", s_user[0] ? s_user : "返回");
    tr_fit(back, 30, 400);
    tr_text(fb, TR_MARGIN, 92, 30, back, EPD_DRAW_ALIGN_LEFT, false);
    char l1[24], l2[24], n[16];
    snprintf(l1, sizeof(l1), "图片 %d", s_images);
    fmt_count(n, sizeof(n), s_comment_total);
    snprintf(l2, sizeof(l2), "评论 %s", n);
    const char* labels[3] = {"正文", l1, l2};
    tr_segment(fb, POST_SEG, labels, 3, s_tab == T_BODY ? 0 : 2);
}

static void draw_body(uint8_t* fb) {
    int y = body_top(s_page);
    if (s_page == 0) {
        char meta[200], when[24], a[16], b[16];
        fmt_time(when, sizeof(when), s_time, true);
        fmt_count(a, sizeof(a), s_reposts);
        fmt_count(b, sizeof(b), s_likes);
        snprintf(meta, sizeof(meta), "%s%s%s · 转发 %s · 赞 %s", when, s_source[0] ? " · 来自 " : "", s_source, a, b);
        tr_fit(meta, 24, CW);
        tr_text(fb, TR_MARGIN, BODY_TOP, 24, meta, EPD_DRAW_ALIGN_LEFT, false);
        tr_hline(fb, BODY_TOP + 42, TR_MARGIN, CW, true);
    }
    if (!s_line_count) {
        tr_text(fb, TR_MARGIN, y, 30, "这条没有文字。", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    const int start = s_pages[s_page];
    const int end = s_page + 1 < s_page_count ? s_pages[s_page + 1] : s_line_count;
    char buf[512];
    for (int i = start; i < end; i++, y += BODY_LH) {
        size_t n = s_lines[i].len < sizeof(buf) - 1 ? s_lines[i].len : sizeof(buf) - 1;
        memcpy(buf, s_body + s_lines[i].off, n);
        buf[n] = 0;
        tr_text(fb, TR_MARGIN, y, BODY_PX, buf, EPD_DRAW_ALIGN_LEFT, false);
    }
}

static void draw_comments(uint8_t* fb) {
    if (!s_comments_loaded) {
        const char* text = job_running(TOMATO_WB_COMMENTS) ? loading_text("正在取评论…（服务器第一次要去微博取，约几秒）")
                           : s_comment_msg[0] ? s_comment_msg : "评论还没取到";
        tr_wrap(fb, TR_MARGIN, BODY_TOP + 20, CW, 30, 44, text, 3);
        return;
    }
    if (!s_comment_count) {
        tr_text(fb, TR_MARGIN, BODY_TOP + 20, 30, "还没有评论", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    int y = BODY_TOP;
    char line[160];
    for (int i = s_comment_pages[s_page]; i < s_comment_pages[s_page + 1]; i++) {
        const comment_t* c = &s_comments[i];
        snprintf(line, sizeof(line), "%s", c->user[0] ? c->user : "微博用户");
        tr_fit(line, 26, CW - 160);
        tr_text(fb, TR_MARGIN, y, 26, line, EPD_DRAW_ALIGN_LEFT, true);
        if (c->likes > 0) {
            char n[16];
            fmt_count(n, sizeof(n), c->likes);
            snprintf(line, sizeof(line), "赞 %s", n);
            tr_text(fb, TR_W - TR_MARGIN, y, 24, line, EPD_DRAW_ALIGN_RIGHT, true);
        }
        tr_wrap(fb, TR_MARGIN, y + 40, CW, COMMENT_PX, COMMENT_LH, c->text[0] ? c->text : "（空）", COMMENT_LINES);
        y += comment_height(c);
        tr_hline(fb, y - 16, TR_MARGIN, CW, true);
    }
}

static void draw_text(uint8_t* fb) {
    draw_text_header(fb);
    if (s_tab == T_BODY) draw_body(fb);
    else draw_comments(fb);
    tr_pager(fb, TEXT_PAGER_Y, s_page, text_pages());
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (!s_items) {
        tr_status_bar(fb);
        tr_title(fb, "微博");
        draw_empty(fb, 240, "内存不足，打不开微博页。");
        tr_tab_bar(fb, TR_TAB_WEIBO);
        return;
    }
    if (s_view == V_IMAGE && s_post_loaded) { draw_image(fb); return; }
    tr_status_bar(fb);
    if (s_view == V_TEXT && s_post_loaded) { draw_text(fb); return; }
    draw_list(fb);
    tr_tab_bar(fb, TR_TAB_WEIBO);
    if (s_wait) draw_wait(fb);
}

/* ---- 动作 / Actions ---- */

static app_redraw_t show_seg(seg_t seg) {
    s_seg = seg;
    s_list_page = 0;
    s_msg[0] = 0;
    hupu_img_drop_pending();
    load_list();
    // 这一段还没缓存：从服务器取一次（服务器上已有的，不会让它去微博）。/ No cache for this segment yet: take the
    // server's list once (what it already has; the server does not ask Weibo).
    if (s_has_cfg && !s_item_count && !exists(list_file())) start_job(TOMATO_WB_FEED, NULL, seg == SEG_HOT, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t refresh(void) {
    start_job(TOMATO_WB_FEED, NULL, s_seg == SEG_HOT, false);
    return APP_REDRAW_PAGE;
}

// 切到评论：没缓存就去取。/ Switch to comments, fetching them when not cached.
static app_redraw_t show_comments(void) {
    s_view = V_TEXT;
    s_tab = T_COMMENTS;
    s_page = 0;
    hupu_img_drop_pending();
    if (!s_comments_loaded && !job_running(TOMATO_WB_COMMENTS)) {
        s_comment_msg[0] = 0;
        start_job(TOMATO_WB_COMMENTS, s_id, false, false);
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t show_body(int page) {
    s_view = V_TEXT;
    s_tab = T_BODY;
    s_page = page < 0 ? 0 : page >= s_page_count ? s_page_count - 1 : page;
    hupu_img_drop_pending();
    return APP_REDRAW_PAGE;
}

static app_redraw_t show_image(int k) {
    if (!s_images) return APP_REDRAW_NONE;
    s_view = V_IMAGE;
    s_image = k < 0 ? 0 : k >= s_images ? s_images - 1 : k;
    hupu_img_drop_pending();
    // 图还缺、也没有任务在补（比如被别的请求顶掉了）：再补一次。/ Images missing and nothing filling them (say,
    // preempted by another request): fill them again.
    if (post_missing_images() && !job_running(TOMATO_WB_POST) && !job_running(TOMATO_WB_COMMENTS))
        start_job(TOMATO_WB_POST, s_id, false, true);
    return APP_REDRAW_PAGE;
}

// 微博已经在卡上：直接看正文；有图没下好就在后台补。/ A cached post opens on its text at once; missing images fill in
// behind it.
static app_redraw_t enter_post(const char* id) {
    if (!load_post(id)) return APP_REDRAW_NONE;
    s_wait = false;
    s_tab = T_BODY;
    s_page = 0;
    s_image = 0;
    s_view = V_TEXT;
    if (post_missing_images() && !(job_running(TOMATO_WB_POST) && !strcmp(s_job.id, id)))
        start_job(TOMATO_WB_POST, id, false, true);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_item(int i) {
    item_t* x = &s_items[i];
    hupu_img_drop_pending();
    s_msg[0] = 0;
    if (s_seg == SEG_FOLLOW && !x->seen) {
        x->seen = true;
        if (!follow_job_running()) tomato_wb_mark_seen(x->id);
        else if (s_seen_pending_count < SEEN_PENDING_MAX)
            snprintf(s_seen_pending[s_seen_pending_count++], sizeof(s_seen_pending[0]), "%s", x->id);
    }
    if (post_cached(x->id)) return enter_post(x->id);
    snprintf(s_wait_id, sizeof(s_wait_id), "%s", x->id);
    s_wait = true;
    start_job(TOMATO_WB_POST, x->id, false, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t back_to_list(void) {
    tr_wallsave_close(&s_ws);
    hupu_img_drop_pending();
    drop_job(TOMATO_WB_POST);
    drop_job(TOMATO_WB_COMMENTS);
    free_post();
    s_view = V_LIST;
    s_wait = false;
    s_msg[0] = 0;
    load_list();
    return APP_REDRAW_PAGE;
}

// 翻页一条线：正文 → 图片 → 评论，往回同理。/ Paging runs one line: text, then images, then comments, and back.
static app_redraw_t turn(int dir) {
    tr_wallsave_close(&s_ws);
    if (s_view == V_LIST) {
        const int next = s_list_page + dir;
        if (next < 0 || next >= list_pages()) return APP_REDRAW_NONE;
        s_list_page = next;
        hupu_img_drop_pending();
        return APP_REDRAW_PAGE;
    }
    if (s_view == V_IMAGE) {
        if (dir > 0 && s_image + 1 >= s_images) return show_comments();
        if (dir < 0 && s_image == 0) return show_body(s_page_count - 1);
        return show_image(s_image + dir);
    }
    const int next = s_page + dir;
    if (next >= 0 && next < text_pages()) {
        s_page = next;
        return APP_REDRAW_PAGE;
    }
    if (s_tab == T_BODY && dir > 0) return s_images ? show_image(0) : show_comments();
    if (s_tab == T_COMMENTS && dir < 0) return s_images ? show_image(s_images - 1) : show_body(s_page_count - 1);
    return APP_REDRAW_NONE;
}

/* ---- 触摸 / Touch ---- */

static app_redraw_t tap_list(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_wait) {
        // 等待框外点一下取消。/ A tap outside the dialog cancels.
        if (job_running(TOMATO_WB_POST) && ui_rect_hit(DIALOG_RECT, x, y)) return APP_REDRAW_NONE;
        drop_job(TOMATO_WB_POST);
        s_wait = false;
        s_msg[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (tr_nav_touch(ctx, x, y, TR_TAB_WEIBO)) return APP_REDRAW_NONE;
    if (!s_has_cfg) {
        if (ui_rect_hit(SETUP_RECT, x, y)) {
            app_tr_settings_request_source(TR_TAB_WEIBO);
            ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
        }
        const int seg = tr_segment_hit(TR_SEGMENT_RECT, 2, x, y);
        return seg >= 0 && seg != (int)s_seg ? show_seg((seg_t)seg) : APP_REDRAW_NONE;
    }
    if (ui_rect_hit(REFRESH_RECT, x, y) && !tomato_sync_busy()) return refresh();
    const int seg = tr_segment_hit(TR_SEGMENT_RECT, 2, x, y);
    if (seg >= 0) return seg == (int)s_seg ? APP_REDRAW_NONE : show_seg((seg_t)seg);
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (y < LIST_TOP || y >= LIST_TOP + PER_PAGE * ROW_H) return APP_REDRAW_NONE;
    const int i = s_list_page * PER_PAGE + (y - LIST_TOP) / ROW_H;
    return i < s_item_count ? open_item(i) : APP_REDRAW_NONE;
}

static app_redraw_t tap_image(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    char path[96];
    image_path(s_image, path, sizeof(path));
    if (tr_wallsave_tap(&s_ws, x, y, path, "微博", s_user, s_image + 1, ctx->now_ms)) return APP_REDRAW_PAGE;
    if (y >= TR_H - IMAGE_BAND) {
        tr_wallsave_close(&s_ws);
        if (x < TR_W / 3) return show_body(0);
        if (x >= TR_W * 2 / 3) return show_comments();
        return APP_REDRAW_NONE;
    }
    if (s_ws.shown && y >= TR_WALLSAVE_TOP) return APP_REDRAW_NONE;   // 工具条空白处 / Blank toolbar area
    if (x < TR_W * 3 / 10) return turn(-1);
    if (x >= TR_W * 7 / 10) return turn(1);
    // 点中间开关存为壁纸工具条。/ A tap in the middle toggles the save-as-wallpaper toolbar.
    if (s_ws.shown) tr_wallsave_close(&s_ws);
    else tr_wallsave_open(&s_ws);
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap_text(uint16_t x, uint16_t y) {
    if (y < 140 && x < 420) return back_to_list();
    const int seg = tr_segment_hit(POST_SEG, 3, x, y);
    if (seg == 0) return s_tab == T_BODY ? APP_REDRAW_NONE : show_body(0);
    if (seg == 1) return show_image(0);
    if (seg == 2) return s_tab == T_COMMENTS ? APP_REDRAW_NONE : show_comments();
    const int pager = tr_pager_hit(TEXT_PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (y < BODY_TOP || y >= BODY_BOTTOM) return APP_REDRAW_NONE;
    if (x < TR_W * 3 / 10) return turn(-1);
    if (x >= TR_W * 7 / 10) return turn(1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (!s_items) {
        if (ev->type == UI_GESTURE_TAP) tr_nav_touch(ctx, ev->x0, ev->y0, TR_TAB_WEIBO);
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP) {
        if (s_view == V_IMAGE) return tap_image(ctx, ev->x0, ev->y0);
        if (s_view == V_TEXT) return tap_text(ev->x0, ev->y0);
        return tap_list(ctx, ev->x0, ev->y0);
    }
    if (s_wait) return APP_REDRAW_NONE;
    // 长按图片也打开存为壁纸工具条。/ A long press on the image also opens the save-as-wallpaper toolbar.
    if (ev->type == UI_GESTURE_LONG_PRESS && s_view == V_IMAGE && ev->y0 < TR_WALLSAVE_TOP && !s_ws.shown) {
        tr_wallsave_open(&s_ws);
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U) return turn(1);
    if (ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D) return turn(-1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1 || !s_items) return APP_REDRAW_NONE;
    if (s_view != V_LIST) return back_to_list();
    if (s_wait) { drop_job(TOMATO_WB_POST); s_wait = false; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

/* ---- 后台进度 / Background progress ---- */

static void queue_visible_images(void) {
    if (s_view == V_LIST) {
        for (int k = 0; k < PER_PAGE; k++) {
            const int i = s_list_page * PER_PAGE + k;
            if (i >= s_item_count) break;
            if (s_items[i].images <= 0) continue;
            char thumb[96];
            tomato_wb_img_path(s_items[i].id, 0, thumb, sizeof(thumb));
            if (exists(thumb)) hupu_img_get(thumb, THUMB, THUMB);
        }
    } else if (s_view == V_IMAGE && s_post_loaded) {
        // 当前一张和下一张（预取）。/ The current image and the next one (prefetch).
        for (int k = s_image; k <= s_image + 1 && k < s_images; k++) {
            char path[96];
            int w, h;
            image_path(k, path, sizeof(path));
            if (exists(path) && image_fit(k, &w, &h)) hupu_img_get(path, w, h);
        }
    }
}

static app_redraw_t job_done(bool ok) {
    switch (s_job.job) {
    case TOMATO_WB_FEED:
        if (s_view == V_LIST && s_job.hot == (s_seg == SEG_HOT)) {
            load_list();
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_WB_POST:
        if (s_wait && !strcmp(s_job.id, s_wait_id)) {
            // 失败：等待框里写原因；成功：进入正文。/ Failure: the dialog shows why; success: open the text.
            if (ok && enter_post(s_wait_id) == APP_REDRAW_PAGE) return APP_REDRAW_PAGE;
            if (ok) snprintf(s_msg, sizeof(s_msg), "微博存不下来，检查 TF 卡");
            return APP_REDRAW_PAGE;
        }
        if (s_view != V_LIST && s_post_loaded && !strcmp(s_job.id, s_id)) {
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_WB_COMMENTS:
        if (s_post_loaded && !strcmp(s_job.id, s_id)) {
            if (ok) load_comments();
            else snprintf(s_comment_msg, sizeof(s_comment_msg), "%s", s_msg[0] ? s_msg : "评论没取到");
            s_msg[0] = 0;
            // 取评论会顶掉同一条还在下的图：评论到了再把缺的图补上。/ Fetching comments preempts this post's image
            // download; once comments arrive, fill in the missing images again.
            if (post_missing_images() && !s_has_pending) start_job(TOMATO_WB_POST, s_id, false, true);
            return s_view == V_TEXT && s_tab == T_COMMENTS ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_WB_TEST:
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (!s_items) return APP_REDRAW_NONE;
    if (tr_wallsave_tick(&s_ws, ctx->now_ms)) return APP_REDRAW_PAGE;
    if (!(ctx->touch && ctx->touch->touched) && hupu_img_tick()) return APP_REDRAW_PAGE;
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tr_time_poll();
    if (s_has_pending && !tomato_sync_busy() && !s_job_active) {
        s_has_pending = false;
        const tomato_wb_req_t r = s_pending;
        start_req(&r);
        return APP_REDRAW_PAGE;
    }
    // 排着队时又有浏览任务占了后台（别的页留下的）：让它让路。/ While queued, a browsing job from elsewhere took the
    // worker: make it yield.
    if (s_has_pending && !s_job_active && tomato_sync_preemptible()) tomato_sync_cancel();
    if (!s_job_active) return APP_REDRAW_NONE;
    // 序号变了：自己的任务早结束了，后台在跑别人的；不再跟踪，按缓存重画。/ The sequence moved on: our job ended and
    // another page's job runs; stop tracking and redraw from the caches.
    if (tomato_sync_seq() != s_job_seq) {
        s_job_active = false;
        flush_seen();
        if (s_view == V_LIST) { load_list(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    const bool busy = tomato_sync_busy();
    tomato_sync_get_status(&s_status);
    // 第 1 步（列表或正文）写好就先显示，图片接着在后台下。/ Show step 1 (list or text) as soon as it is written;
    // images keep downloading behind it.
    if (busy && !s_step_loaded && s_status.current >= 1 && s_status.phase != TOMATO_FAILED) {
        s_step_loaded = true;
        if (s_job.job == TOMATO_WB_FEED) {
            if (s_view == V_LIST && s_job.hot == (s_seg == SEG_HOT)) {
                s_list_page = 0;
                load_list();
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (s_job.job == TOMATO_WB_POST && s_wait && !strcmp(s_job.id, s_wait_id)) return enter_post(s_wait_id);
        return APP_REDRAW_NONE;
    }
    // 正在看的图刚下好：重画（会排队缩放）。/ The image on screen just arrived: redraw, which queues its scaling.
    if (busy && s_view == V_IMAGE && s_image_missing) {
        char path[96];
        image_path(s_image, path, sizeof(path));
        if (exists(path)) return APP_REDRAW_PAGE;
    }
    if (busy) return APP_REDRAW_NONE;
    s_job_active = false;
    if (s_job.job == TOMATO_WB_FEED && !s_job.hot) flush_seen();
    const bool ok = s_status.phase != TOMATO_FAILED;
    if (!ok) {
        if (s_status.error == ESP_ERR_INVALID_STATE) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    } else if (s_job.job == TOMATO_WB_POST && s_status.message[0]) {
        snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    }
    if (s_has_pending) {
        if (ok) job_done(ok);
        s_has_pending = false;
        const tomato_wb_req_t next = s_pending;
        start_req(&next);
        return APP_REDRAW_PAGE;
    }
    const app_redraw_t r = job_done(ok);
    return !ok && r == APP_REDRAW_NONE && s_view == V_LIST ? APP_REDRAW_PAGE : r;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    hupu_img_drop_pending();
    tomato_wb_config_t cfg;
    s_has_cfg = tomato_wb_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    if (!alloc_all()) return;
    s_msg[0] = 0;
    s_view = V_LIST;
    s_wait = false;
    s_list_page = 0;
    // 离开期间的任务：不是自己的、或已经结束，都不再跟踪。/ A job from before: stop tracking it unless ours still runs.
    if (s_job_active && !ours_running()) s_job_active = false;
    if (!follow_job_running()) flush_seen();
    load_list();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    tr_wallsave_close(&s_ws);
    s_has_pending = false;
    free_all();
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    tr_wallsave_close(&s_ws);
    tomato_sync_cancel();
    s_job_active = false;
    s_has_pending = false;
    s_wait = false;
    free_all();
    alloc_all();
    s_view = V_LIST;
}

const app_desc_t app_tr_weibo = {
    .title = "微博",
    .detail = "关注、热门（经自建 weibo 服务器，只读）",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
};
