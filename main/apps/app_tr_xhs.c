/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 小红书标签页（只读）：顶部分段 关注 / 推荐；点开笔记先一屏一张看图，再看正文和评论。数据来自用户自建的 rednote
 * 服务器，由 components/tomato 的后台任务取到 TF 卡 rednote/，本页只读缓存；缩略图和大图的缩放抖动交给 hupu_img。
 *
 * English: the Xiaohongshu tab (read only): Following / Recommended segments; a note opens on its images, one per
 * screen, then text and comments. Data comes from the user's own rednote server; the components/tomato task stores it
 * under rednote/ on the TF card and this page reads only that cache; hupu_img scales and dithers thumbnails and images.
 *
 * 冻结：只在用户点“刷新”、切到没缓存的推荐、或打开没缓存的笔记时联网，进页不自动刷新。推荐的“刷新”才让服务器去读
 * 小红书首页，次数和间隔由服务器面板控制。没设置服务器时只显示“去设置”。大块数据进页分配、离页释放。不点赞、不评论。
 * Frozen: network only on Refresh, switching to recommendations without a cache, or opening an uncached note; entering
 * never refreshes. Only the recommendations Refresh makes the server read Xiaohongshu; its quota and spacing belong to
 * the server panel. Without a server the page only offers "Set up". Large buffers are allocated on entry and freed on
 * exit. No likes, no comments.
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

#define TAG "app_xhs"
#define POLL_MS 400
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CW (TR_W - 2 * TR_MARGIN)

// 列表 / Lists
#define LIST_MAX 100
#define LIST_TOP 284
#define PAGER_Y 1036
// 两列卡片，每页 2×2。/ Two-column cards, 2x2 per page.
#define COLS 2
#define PER_PAGE 4
#define CELL_GAP 20
#define CELL_W ((TR_W - 2 * TR_MARGIN - CELL_GAP) / COLS)
#define CELL_H 376
#define COVER_H 262
#define STATUS_Y 240
#define REFRESH_RECT (EpdRect){TR_W - TR_MARGIN - 240, 70, 240, 80}
#define SETUP_RECT (EpdRect){TR_MARGIN, 420, 240, 72}
#define DIALOG_RECT (EpdRect){92, 500, TR_W - 184, 220}

// 笔记 / Note
#define NOTE_SEG (EpdRect){TR_MARGIN, 150, CW, 64}
#define TITLE_Y 240
#define TITLE_PX 38
#define TITLE_LH 50
#define BODY_TOP 240
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
typedef enum { SEG_FOLLOW, SEG_REC } seg_t;
typedef enum { T_BODY, T_COMMENTS } text_tab_t;

typedef struct {
    char id[28];
    char title[160];
    char author[48];
    char likes[16];
    int16_t images;
    bool seen;
} item_t;

typedef struct {
    const char* user;
    const char* text;
    const char* likes;
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
// 上次点当前分段的时间（双击回第一页）。/ Last tap on the current segment (a double tap goes to the first page).
static int64_t s_seg_tap_ms;
static bool s_has_cfg;

// 后台任务 / Background job
static bool s_job_active;
static uint32_t s_job_seq;           // 本页任务的序号，用来认出“是不是自己的”/ Our job's sequence number
static tomato_red_req_t s_job;
static tomato_status_t s_status;
static bool s_step_loaded;
static tomato_red_req_t s_pending;
static bool s_has_pending;

// 列表 / List
static item_t* s_items;
static int s_item_count;
static int s_list_page;
static long s_rec_time;
static int s_rec_left = -1;

// 关注列表刷新期间点开的笔记：刷新会覆盖 feed.json，等它结束再写“看过”。
// Notes opened while the following list refreshes: the refresh rewrites feed.json, so mark them seen afterwards.
#define SEEN_PENDING_MAX 16
static char s_seen_pending[SEEN_PENDING_MAX][28];
static int s_seen_pending_count;

// 正在等服务器拉的推荐笔记（等待框）。/ A recommended note the server is fetching (the wait dialog).
static bool s_wait;
static char s_wait_id[28];

// 笔记 / Note
static char* s_arena;
static size_t s_arena_len, s_arena_cap;
static char s_id[28];
static bool s_note_loaded;
static const char *s_title, *s_author, *s_likes, *s_comment_total, *s_body;
static double s_time;               // 秒或毫秒 / Seconds or milliseconds
static int s_images;
static comment_t* s_comments;
static int s_comment_count;
static line_t* s_lines;
static int s_line_count;
static uint16_t* s_pages;
static int s_page_count;
static uint8_t s_comment_pages[COMMENTS_MAX + 1];
static int s_comment_page_count;
static int s_title_lines;
static text_tab_t s_tab;
static int s_page;
static int s_image;                  // 从 0 开始 / 0-based
static tr_wallsave_t s_ws;             // 看图时的存为壁纸工具条 / Save-as-wallpaper toolbar
static bool s_image_missing;         // 当前图还没下好，任务进行中要回头看 / Current image not downloaded yet

/* ---- 内存 / Memory ---- */

#define FREE(p) do { heap_caps_free(p); (p) = NULL; } while (0)

static void free_note(void) {
    FREE(s_arena);
    s_arena_len = s_arena_cap = 0;
    s_note_loaded = false;
    s_comment_count = s_line_count = s_page_count = s_comment_page_count = 0;
    s_images = 0;
}

static void free_all(void) {
    free_note();
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
    else if (cJSON_IsNumber(v)) snprintf(out, cap, "%.0f", v->valuedouble);
    else if (cap) out[0] = 0;
}

static int num(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
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

/* ---- 后台任务 / Background jobs ---- */

static bool start_req(const tomato_red_req_t* r) {
    esp_err_t err = tomato_sync_start_red(r);
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
static bool start_job(tomato_red_job_t job, const char* id, bool rec, bool refresh, bool images_only) {
    tomato_red_req_t req = {.job = job, .rec = rec, .refresh = refresh, .images_only = images_only};
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

static void drop_job(tomato_red_job_t job) {
    if (s_has_pending && s_pending.job == job) s_has_pending = false;
    if (s_job.job == job && ours_running()) tomato_sync_cancel();
}

static bool job_running(tomato_red_job_t job) {
    return (s_has_pending && s_pending.job == job) || (s_job.job == job && ours_running());
}

static void flush_seen(void) {
    for (int i = 0; i < s_seen_pending_count; i++) tomato_red_mark_seen(s_seen_pending[i]);
    s_seen_pending_count = 0;
}

static const char* loading_text(const char* what) {
    return s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : what;
}

/* ---- 列表数据 / List data ---- */

static int list_pages(void) { return s_item_count ? (s_item_count + PER_PAGE - 1) / PER_PAGE : 1; }

static void load_list(void) {
    s_item_count = 0;
    cJSON* root = read_json(s_seg == SEG_FOLLOW ? TOMATO_RED_FEED_FILE : TOMATO_RED_REC_FILE);
    if (s_seg == SEG_REC) {
        s_rec_time = root ? num(root, "fetched_at") : 0;
        s_rec_left = root ? num(root, "left") : -1;
    }
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (s_item_count >= LIST_MAX) break;
        item_t* x = &s_items[s_item_count];
        memset(x, 0, sizeof(*x));
        str(x->id, sizeof(x->id), it, "id");
        if (!x->id[0]) continue;
        str(x->title, sizeof(x->title), it, "title");
        str(x->author, sizeof(x->author), it, "author");
        str(x->likes, sizeof(x->likes), it, "likes");
        x->images = (int16_t)num(it, "images");
        x->seen = s_seg == SEG_REC || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "seen"));
        s_item_count++;
    }
    cJSON_Delete(root);
    if (s_list_page >= list_pages()) s_list_page = list_pages() - 1;
}

static void thumb_path(const item_t* x, char* out, size_t cap) {
    if (s_seg == SEG_FOLLOW) tomato_red_img_path(x->id, 0, out, cap);
    else tomato_red_cover_path(x->id, out, cap);
}

static bool note_cached(const char* id) {
    char dir[64], file[80];
    tomato_red_note_dir(id, dir, sizeof(dir));
    snprintf(file, sizeof(file), "%s/note.json", dir);
    return exists(file);
}

/* ---- 笔记数据 / Note data ---- */

static const char* keep(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    const char* s = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    const size_t n = strlen(s) + 1;
    if (s_arena_len + n > s_arena_cap) return "";
    char* out = s_arena + s_arena_len;
    memcpy(out, s, n);
    s_arena_len += n;
    return out;
}

static int title_lines(void) {
    const char* p = s_title;
    int n = 0;
    while (*p && n < 2) { p += tr_line_bytes(p, TITLE_PX, CW); if (*p == '\n') p++; n++; }
    return n ? n : 1;
}

static int body_top(int page) {
    return page ? BODY_TOP : TITLE_Y + s_title_lines * TITLE_LH + 10 + 44 + 18;
}

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

static bool load_note(const char* id) {
    free_note();
    char dir[64], path[80];
    tomato_red_note_dir(id, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/note.json", dir);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    cJSON* root = read_json(path);
    if (!root) return false;
    // 内存池按文件大小开：里面的字符串加起来不会比文件长。/ Size the arena by the file: its strings never exceed it.
    s_arena_cap = (size_t)st.st_size + 512;
    s_arena = heap_caps_malloc(s_arena_cap, PSRAM_CAPS);
    if (!s_arena) { cJSON_Delete(root); s_arena_cap = 0; return false; }
    s_title = keep(root, "title");
    if (!s_title[0]) s_title = "（无标题）";
    s_author = keep(root, "author");
    s_likes = keep(root, "likes");
    s_comment_total = keep(root, "comments");
    s_body = keep(root, "body");
    const cJSON* tv = cJSON_GetObjectItemCaseSensitive(root, "time");
    s_time = cJSON_IsNumber(tv) ? tv->valuedouble : 0;
    s_images = num(root, "images");
    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "list")) {
        if (s_comment_count >= COMMENTS_MAX) break;
        comment_t* x = &s_comments[s_comment_count++];
        x->user = keep(c, "u");
        x->text = keep(c, "t");
        x->likes = keep(c, "l");
    }
    cJSON_Delete(root);
    snprintf(s_id, sizeof(s_id), "%s", id);
    s_title_lines = title_lines();
    layout_body();
    layout_comments();
    s_note_loaded = true;
    return true;
}

static void image_path(int k, char* out, size_t cap) { tomato_red_img_path(s_id, k + 1, out, cap); }

static bool note_missing_images(void) {
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
    tr_title(fb, "小红书");
    if (s_has_cfg) {
        const bool busy = job_running(s_seg == SEG_FOLLOW ? TOMATO_RED_FEED : TOMATO_RED_REC);
        if (busy) {
            tr_text(fb, TR_W - TR_MARGIN, 98, 30, "拉取中…", EPD_DRAW_ALIGN_RIGHT, false);
        } else if (!tomato_sync_busy()) {
            const char* label = s_seg == SEG_FOLLOW ? "刷新" : "刷新推荐";
            tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - ttf_text_width_px(30, label) - 26, 116, false);
            tr_text(fb, TR_W - TR_MARGIN, 98, 30, label, EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    static const char* const SEG[2] = {"关注", "推荐"};
    tr_segment(fb, TR_SEGMENT_RECT, SEG, 2, (int)s_seg);
}

// 两列封面卡片：封面（铺满裁切）、标题两行、作者 · 赞；没看过的封面左上角黑底“新”，推荐里下好的右下角“已下载”。
// Two-column cover cards: cover (fill-cropped), two-line title, author · likes; unseen notes get a black "new" tag
// at the cover's top-left, and downloaded recommendations a "downloaded" tag at its bottom-right.
static void draw_cards(uint8_t* fb) {
    char line[200];
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = s_list_page * PER_PAGE + k;
        if (i >= s_item_count) break;
        const item_t* x = &s_items[i];
        const int cx = TR_MARGIN + (k % COLS) * (CELL_W + CELL_GAP);
        const int cy = LIST_TOP + (k / COLS) * CELL_H;
        char thumb[96];
        thumb_path(x, thumb, sizeof(thumb));
        const uint8_t* px = exists(thumb) ? hupu_img_get(thumb, CELL_W, COVER_H) : NULL;
        if (px) hupu_img_draw(fb, cx, cy, CELL_W, COVER_H, px, true);
        else epd_draw_rect((EpdRect){cx, cy, CELL_W, COVER_H}, TR_GRAY_LINE, fb);
        if (!x->seen) {
            epd_fill_rect((EpdRect){cx + 10, cy + 10, 48, 34}, UI_GRAY_BLACK, fb);
            tr_text_inv(fb, cx + 34, cy + 13, 22, "新", EPD_DRAW_ALIGN_CENTER, true);
        }
        if (s_seg == SEG_REC && note_cached(x->id)) {
            const EpdRect r = {cx + CELL_W - 98, cy + COVER_H - 44, 88, 34};
            epd_fill_rect(r, UI_GRAY_WHITE, fb);
            epd_draw_rect(r, UI_GRAY_BLACK, fb);
            tr_text(fb, r.x + r.width / 2, r.y + 4, 20, "已下载", EPD_DRAW_ALIGN_CENTER, true);
        }
        tr_wrap(fb, cx, cy + COVER_H + 10, CELL_W, 26, 34, x->title[0] ? x->title : "（无标题）", 2);
        snprintf(line, sizeof(line), "%s%s%s", x->author, x->likes[0] ? " · 赞 " : "", x->likes);
        tr_fit(line, 22, CELL_W);
        tr_text(fb, cx, cy + COVER_H + 84, 22, line, EPD_DRAW_ALIGN_LEFT, false);
    }
}

static void draw_list(uint8_t* fb) {
    draw_header(fb);
    if (!s_has_cfg) {
        draw_empty(fb, LIST_TOP, "还没连接 rednote 服务器。先在电脑或 NAS 上部署 rednote（见项目 rednote/ 目录），"
                                 "再到“设置 › 内容来源”填服务器地址和设备令牌。");
        tr_pill(fb, SETUP_RECT, "去设置", true, 30);
        return;
    }
    char line[160];
    const bool loading = job_running(s_seg == SEG_FOLLOW ? TOMATO_RED_FEED : TOMATO_RED_REC);
    if (loading) {
        snprintf(line, sizeof(line), "%s", loading_text(s_step_loaded ? "正在下载封面…" :
                 s_seg == SEG_REC && s_job.refresh ? "服务器正在读小红书首页…" : "正在加载…"));
    } else if (s_msg[0]) {
        snprintf(line, sizeof(line), "%s", s_msg);
    } else if (s_seg == SEG_FOLLOW) {
        snprintf(line, sizeof(line), "关注的博主 · %d 篇", s_item_count);
    } else {
        char when[16] = "";
        if (s_rec_time > 1735689600) {
            time_t t = (time_t)s_rec_time;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(when, sizeof(when), " · %02d:%02d", tm.tm_hour, tm.tm_min);
        }
        if (s_rec_left >= 0) snprintf(line, sizeof(line), "首页推荐%s · 今天还能拉 %d 次", when, s_rec_left);
        else snprintf(line, sizeof(line), "首页推荐%s", when);
    }
    tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - 12, CW, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_item_count) {
        draw_empty(fb, LIST_TOP + 20, loading ? "加载中，请稍候。" : s_seg == SEG_FOLLOW ?
                   "还没有缓存。联网后点右上角“刷新”，从服务器取关注博主的笔记；之后不联网也能看。" :
                   "还没有推荐。点右上角“刷新推荐”，服务器会去读一次小红书首页（只留图文）。");
        return;
    }
    draw_cards(fb);
    tr_pager(fb, PAGER_Y, s_list_page, list_pages());
}

static void draw_wait(uint8_t* fb) {
    const EpdRect r = DIALOG_RECT;
    epd_fill_rect(r, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, r, 22, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), 21, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(r, 2), 20, UI_GRAY_BLACK);
    const int cx = r.x + r.width / 2;
    const bool failed = !job_running(TOMATO_RED_NOTE) && s_msg[0];
    if (failed) {
        tr_text(fb, cx, r.y + 40, 32, "没能打开这篇", EPD_DRAW_ALIGN_CENTER, true);
        tr_wrap(fb, r.x + 30, r.y + 96, r.width - 60, 26, 36, s_msg, 2);
        tr_text(fb, cx, r.y + 168, 24, "点任意处关闭", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    tr_text(fb, cx, r.y + 44, 34, "正在拉取这篇笔记…", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, cx, r.y + 104, 26, loading_text("服务器要打开小红书，约 10～20 秒"), EPD_DRAW_ALIGN_CENTER, false);
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
        const bool loading = job_running(TOMATO_RED_NOTE);
        s_image_missing = loading;
        tr_text(fb, TR_W / 2, (TR_H - IMAGE_BAND) / 2, 30, loading ? loading_text("图片下载中…") : "图片还没下载",
                EPD_DRAW_ALIGN_CENTER, false);
    }
    epd_fill_rect((EpdRect){0, TR_H - IMAGE_BAND, TR_W, IMAGE_BAND}, UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_H - IMAGE_BAND, 0, TR_W, false);
    char label[32];
    snprintf(label, sizeof(label), "%d / %d", s_image + 1, s_images);
    tr_text(fb, TR_MARGIN, TR_H - IMAGE_BAND + 28, 30, "‹ 返回", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W / 2, TR_H - IMAGE_BAND + 28, 30, label, EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, TR_W - TR_MARGIN, TR_H - IMAGE_BAND + 28, 30, "正文 ›", EPD_DRAW_ALIGN_RIGHT, false);
    tr_wallsave_draw(fb, &s_ws);
}

static void draw_text_header(uint8_t* fb) {
    char back[80];
    snprintf(back, sizeof(back), "‹ %s", s_author[0] ? s_author : "返回");
    tr_fit(back, 30, 400);
    tr_text(fb, TR_MARGIN, 92, 30, back, EPD_DRAW_ALIGN_LEFT, false);
    char l0[24], l2[24];
    snprintf(l0, sizeof(l0), "图片 %d", s_images);
    snprintf(l2, sizeof(l2), "评论 %d", s_comment_count);
    const char* labels[3] = {l0, "正文", l2};
    tr_segment(fb, NOTE_SEG, labels, 3, s_tab == T_BODY ? 1 : 2);
}

static void draw_body(uint8_t* fb) {
    int y = body_top(s_page);
    if (s_page == 0) {
        tr_wrap(fb, TR_MARGIN, TITLE_Y, CW, TITLE_PX, TITLE_LH, s_title, 2);
        char meta[200], when[32] = "";
        if (s_time > 1e9) {
            // 服务器给的是毫秒或秒，都能认。/ The server gives milliseconds or seconds; accept both.
            time_t t = (time_t)(s_time > 1e11 ? s_time / 1000 : s_time);
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(when, sizeof(when), " · %02d-%02d", (tm.tm_mon + 1) % 100, tm.tm_mday % 100);
        }
        const int my = TITLE_Y + s_title_lines * TITLE_LH + 10;
        snprintf(meta, sizeof(meta), "%s%s%s%s%s%s", s_author, when, s_likes[0] ? " · 赞 " : "", s_likes,
                 s_comment_total[0] ? " · 评论 " : "", s_comment_total);
        tr_fit(meta, 24, CW);
        tr_text(fb, TR_MARGIN, my, 24, meta, EPD_DRAW_ALIGN_LEFT, false);
        tr_hline(fb, my + 40, TR_MARGIN, CW, true);
    }
    if (!s_line_count) {
        tr_text(fb, TR_MARGIN, y, 30, "这篇没有文字。", EPD_DRAW_ALIGN_LEFT, false);
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
    if (!s_comment_count) {
        tr_text(fb, TR_MARGIN, BODY_TOP + 20, 30, "服务器没有存这篇的评论", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    int y = BODY_TOP;
    char line[160];
    for (int i = s_comment_pages[s_page]; i < s_comment_pages[s_page + 1]; i++) {
        const comment_t* c = &s_comments[i];
        snprintf(line, sizeof(line), "%s", c->user[0] ? c->user : "小红书用户");
        tr_fit(line, 26, CW - 160);
        tr_text(fb, TR_MARGIN, y, 26, line, EPD_DRAW_ALIGN_LEFT, true);
        if (c->likes[0] && strcmp(c->likes, "0")) {
            snprintf(line, sizeof(line), "赞 %s", c->likes);
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
        tr_title(fb, "小红书");
        draw_empty(fb, 240, "内存不足，打不开小红书页。");
        tr_tab_bar(fb, TR_TAB_XHS);
        return;
    }
    if (s_view == V_IMAGE && s_note_loaded) { draw_image(fb); return; }
    tr_status_bar(fb);
    if (s_view == V_TEXT && s_note_loaded) { draw_text(fb); return; }
    draw_list(fb);
    tr_tab_bar(fb, TR_TAB_XHS);
    if (s_wait) draw_wait(fb);
}

/* ---- 动作 / Actions ---- */

static app_redraw_t show_seg(seg_t seg) {
    s_seg = seg;
    s_list_page = 0;
    s_msg[0] = 0;
    hupu_img_drop_pending();
    load_list();
    // 推荐没缓存：先取服务器上已有的列表（不让服务器去读小红书）。/ No cached recommendations: fetch the server's
    // existing list without making it read Xiaohongshu.
    if (seg == SEG_REC && s_has_cfg && !s_item_count && !exists(TOMATO_RED_REC_FILE))
        start_job(TOMATO_RED_REC, NULL, false, false, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t refresh(void) {
    if (s_seg == SEG_FOLLOW) start_job(TOMATO_RED_FEED, NULL, false, false, false);
    else start_job(TOMATO_RED_REC, NULL, false, true, false);
    return APP_REDRAW_PAGE;
}

// 笔记已经在卡上：直接看；有图没下好就在后台补。/ A cached note opens at once; missing images fill in behind it.
static app_redraw_t enter_note(const char* id, bool rec) {
    if (!load_note(id)) return APP_REDRAW_NONE;
    s_wait = false;
    s_tab = T_BODY;
    s_page = 0;
    s_image = 0;
    s_view = s_images ? V_IMAGE : V_TEXT;
    if (note_missing_images() && !(job_running(TOMATO_RED_NOTE) && !strcmp(s_job.id, id)))
        start_job(TOMATO_RED_NOTE, id, rec, false, true);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_item(int i) {
    item_t* x = &s_items[i];
    hupu_img_drop_pending();
    s_msg[0] = 0;
    if (s_seg == SEG_FOLLOW && !x->seen) {
        x->seen = true;
        if (!job_running(TOMATO_RED_FEED)) tomato_red_mark_seen(x->id);
        else if (s_seen_pending_count < SEEN_PENDING_MAX)
            snprintf(s_seen_pending[s_seen_pending_count++], sizeof(s_seen_pending[0]), "%s", x->id);
    }
    const bool rec = s_seg == SEG_REC;
    if (note_cached(x->id)) return enter_note(x->id, rec);
    // 没缓存：关注的笔记服务器上已经有，很快；推荐的要现拉，弹等待框。
    // Not cached: following notes already sit on the server (fast); recommendations are fetched live behind a dialog.
    snprintf(s_wait_id, sizeof(s_wait_id), "%s", x->id);
    s_wait = true;
    start_job(TOMATO_RED_NOTE, x->id, rec, false, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t back_to_list(void) {
    tr_wallsave_close(&s_ws);
    hupu_img_drop_pending();
    drop_job(TOMATO_RED_NOTE);
    free_note();
    s_view = V_LIST;
    s_wait = false;
    s_msg[0] = 0;
    load_list();
    return APP_REDRAW_PAGE;
}

static app_redraw_t turn(int dir) {
    // 工具条或提示框开着：就算没翻成也要重画把它擦掉。/ With the toolbar or notice up, redraw even when the page
    // does not change, so it is wiped.
    const app_redraw_t idle = s_ws.shown || s_ws.title[0] ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    tr_wallsave_close(&s_ws);
    int* page;
    int pages;
    if (s_view == V_LIST) { page = &s_list_page; pages = list_pages(); }
    else if (s_view == V_TEXT) { page = &s_page; pages = text_pages(); }
    else {
        // 最后一张再往后进正文；第一张再往前回列表。/ Past the last image comes the text; before the first, the list.
        if (dir > 0 && s_image + 1 >= s_images) { s_view = V_TEXT; s_tab = T_BODY; s_page = 0; return APP_REDRAW_PAGE; }
        if (dir < 0 && s_image == 0) return idle;
        page = &s_image;
        pages = s_images;
    }
    const int next = *page + dir;
    if (next < 0 || next >= pages) return idle;
    *page = next;
    hupu_img_drop_pending();
    return APP_REDRAW_PAGE;
}

/* ---- 触摸 / Touch ---- */

static app_redraw_t tap_list(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_wait) {
        // 等待框外点一下取消（服务器那边照样会拉完存好）。/ A tap outside the dialog cancels (the server still finishes).
        if (job_running(TOMATO_RED_NOTE) && ui_rect_hit(DIALOG_RECT, x, y)) return APP_REDRAW_NONE;
        drop_job(TOMATO_RED_NOTE);
        s_wait = false;
        s_msg[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (tr_nav_touch(ctx, x, y, TR_TAB_XHS)) return APP_REDRAW_NONE;
    if (!s_has_cfg) {
        if (ui_rect_hit(SETUP_RECT, x, y)) {
            app_tr_settings_request_source(TR_TAB_XHS);
            ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
        }
        const int seg = tr_segment_hit(TR_SEGMENT_RECT, 2, x, y);
        return seg >= 0 && seg != (int)s_seg ? show_seg((seg_t)seg) : APP_REDRAW_NONE;
    }
    if (ui_rect_hit(REFRESH_RECT, x, y) && !tomato_sync_busy()) return refresh();
    const int seg = tr_segment_hit(TR_SEGMENT_RECT, 2, x, y);
    if (seg >= 0 && seg != (int)s_seg) return show_seg((seg_t)seg);
    if (seg >= 0) {
        // 双击当前分段：回到第一页（最新的）。/ Double tap on the current segment: back to the first (newest) page.
        const bool twice = ctx->now_ms - s_seg_tap_ms < 600;
        s_seg_tap_ms = twice ? 0 : ctx->now_ms;
        if (!twice || s_list_page == 0) return APP_REDRAW_NONE;
        s_list_page = 0;
        hupu_img_drop_pending();
        return APP_REDRAW_PAGE;
    }
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (y < LIST_TOP || y >= LIST_TOP + (PER_PAGE / COLS) * CELL_H || x < TR_MARGIN || x >= TR_W - TR_MARGIN)
        return APP_REDRAW_NONE;
    const int col = (x - TR_MARGIN) >= CELL_W + CELL_GAP / 2 ? 1 : 0;
    const int i = s_list_page * PER_PAGE + ((y - LIST_TOP) / CELL_H) * COLS + col;
    return i < s_item_count ? open_item(i) : APP_REDRAW_NONE;
}

static app_redraw_t tap_image(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    char path[96];
    image_path(s_image, path, sizeof(path));
    if (tr_wallsave_tap(&s_ws, x, y, path, "小红书", s_author, s_image + 1, ctx->now_ms)) return APP_REDRAW_PAGE;
    if (y >= TR_H - IMAGE_BAND) {
        tr_wallsave_close(&s_ws);
        if (x < TR_W / 3) return back_to_list();
        if (x >= TR_W * 2 / 3) { s_view = V_TEXT; s_tab = T_BODY; s_page = 0; hupu_img_drop_pending(); return APP_REDRAW_PAGE; }
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
    const int seg = tr_segment_hit(NOTE_SEG, 3, x, y);
    if (seg == 0) {
        if (!s_images) return APP_REDRAW_NONE;
        s_view = V_IMAGE;
        return APP_REDRAW_PAGE;
    }
    if (seg > 0) {
        const text_tab_t tab = seg == 1 ? T_BODY : T_COMMENTS;
        if (tab == s_tab) return APP_REDRAW_NONE;
        s_tab = tab;
        s_page = 0;
        return APP_REDRAW_PAGE;
    }
    const int pager = tr_pager_hit(TEXT_PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (y < BODY_TOP || y >= BODY_BOTTOM) return APP_REDRAW_NONE;
    if (x < TR_W * 3 / 10) return turn(-1);
    if (x >= TR_W * 7 / 10) return turn(1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (!s_items) {
        if (ev->type == UI_GESTURE_TAP) tr_nav_touch(ctx, ev->x0, ev->y0, TR_TAB_XHS);
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
    if (s_wait) { drop_job(TOMATO_RED_NOTE); s_wait = false; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

/* ---- 后台进度 / Background progress ---- */

static void queue_visible_images(void) {
    if (s_view == V_LIST) {
        for (int k = 0; k < PER_PAGE; k++) {
            const int i = s_list_page * PER_PAGE + k;
            if (i >= s_item_count) break;
            char thumb[96];
            thumb_path(&s_items[i], thumb, sizeof(thumb));
            if (exists(thumb)) hupu_img_get(thumb, CELL_W, COVER_H);
        }
    } else if (s_view == V_IMAGE && s_note_loaded) {
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
    case TOMATO_RED_FEED:
    case TOMATO_RED_REC:
        if (s_view == V_LIST && (s_job.job == TOMATO_RED_FEED) == (s_seg == SEG_FOLLOW)) {
            load_list();
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_RED_NOTE:
        if (s_wait && !strcmp(s_job.id, s_wait_id)) {
            // 失败：等待框里写原因；成功：进入笔记。/ Failure: the dialog shows why; success: open the note.
            if (ok && enter_note(s_wait_id, s_job.rec) == APP_REDRAW_PAGE) return APP_REDRAW_PAGE;
            if (ok) snprintf(s_msg, sizeof(s_msg), "笔记存不下来，检查 TF 卡");
            return APP_REDRAW_PAGE;
        }
        if (s_view != V_LIST && s_note_loaded && !strcmp(s_job.id, s_id)) {
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_RED_TEST:
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
        const tomato_red_req_t r = s_pending;
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
        if (s_job.job != TOMATO_RED_NOTE) {
            if (s_view == V_LIST && (s_job.job == TOMATO_RED_FEED) == (s_seg == SEG_FOLLOW)) {
                s_list_page = 0;
                load_list();
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (s_wait && !strcmp(s_job.id, s_wait_id)) return enter_note(s_wait_id, s_job.rec);
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
    if (s_job.job == TOMATO_RED_FEED) flush_seen();
    const bool ok = s_status.phase != TOMATO_FAILED;
    if (!ok) {
        if (s_status.error == ESP_ERR_INVALID_STATE) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    } else if (s_job.job == TOMATO_RED_NOTE && s_status.message[0]) {
        snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    }
    if (s_has_pending) {
        if (ok) job_done(ok);
        s_has_pending = false;
        const tomato_red_req_t next = s_pending;
        start_req(&next);
        return APP_REDRAW_PAGE;
    }
    const app_redraw_t r = job_done(ok);
    return !ok && r == APP_REDRAW_NONE && s_view == V_LIST ? APP_REDRAW_PAGE : r;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    hupu_img_drop_pending();
    tomato_red_config_t cfg;
    s_has_cfg = tomato_red_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    if (!alloc_all()) return;
    s_msg[0] = 0;
    s_view = V_LIST;
    s_wait = false;
    s_list_page = 0;
    // 离开期间的任务：不是自己的、或已经结束，都不再跟踪。/ A job from before: stop tracking it unless ours still runs.
    if (s_job_active && !ours_running()) s_job_active = false;
    if (!job_running(TOMATO_RED_FEED)) flush_seen();
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

const app_desc_t app_tr_xhs = {
    .title = "小红书",
    .detail = "关注、推荐（经自建 rednote 服务器，只读）",
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
