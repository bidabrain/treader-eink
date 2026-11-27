/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * X 标签页（只读），照微博标签页的样子：顶部分段 为你推荐 / 正在关注；列表一条一张卡片（头像圆、名字、时间和
 * @账号、正文、引用推文的灰框、图片、转推回复赞三栏），每页按高度装满；点开先看全文，往后翻是图片（一屏一张），
 * 再往后是回复。数据来自用户自建的 x 服务器，由 components/tomato 的后台任务取到 TF 卡 x/，本页只读缓存；缩略图和
 * 大图的缩放抖动交给 hupu_img。服务器关了“正在关注”时，这个分段变灰、点不动。
 *
 * English: the X tab (read only), shaped like the Weibo tab: For You / Following segments; the list shows one card
 * per post (avatar circle, name, time and @handle, text, a gray box for a quoted post, images, and a reposts / replies
 * / likes bar), each page filled by height. A post opens on its full text, then images one per screen, then replies.
 * Data comes from the user's own x server; the components/tomato task stores it under x/ on the TF card and this page
 * reads only that cache; hupu_img scales and dithers images. When the server has Following off, that segment is grey
 * and cannot be chosen.
 *
 * 冻结：只在用户点“刷新”、切到没缓存的分段、打开没缓存的推文或回复时联网，进页不自动刷新；“正在关注”开关跟着每次
 * 刷新更新。服务器自己按面板设置去 X 拉取，设备只取服务器上已有的；只有回复第一次打开时服务器才去 X 取一次。没设置
 * 服务器时只显示“去设置”。大块数据进页分配、离页释放。不点赞、不回复、不转推。
 * Frozen: network only on Refresh, switching to a segment without a cache, or opening an uncached post or replies;
 * entering never refreshes, and the Following switch updates with each refresh. The server fetches X on its own
 * schedule and the device takes only what the server has; only a post's first replies request makes the server ask X.
 * Without a server the page only offers "Set up". Large buffers are allocated on entry and freed on exit. No likes,
 * replies or reposts.
 * Treader 修订（1.2.1，用户要求）：列表每页按紧凑高度分页（图片先按最小算），剩下的空白分回给本页卡片：先把图片放回
 * 正常大小，再多显示正文和原文，最后放大图片，不再留大片空白。列表区像书页一样分左中右：左右各三成翻页，中间打开；
 * 底部翻页按钮保留。
 * Treader revision (1.2.1, user request): the list pages by compact card heights (images counted at their smallest)
 * and hands each page's leftover space back to its cards: images back to normal size first, then more text and repost
 * lines, then bigger images, so no large blank is left. The list area splits like a book page: the outer 30% on each
 * side turns the page and the middle opens; the bottom pager stays.
 * Treader 修订（1.2.8，用户要求）：联网时机多一条：设置里“往后翻接着拉”开着时，翻过已经拉到的最后一页，向服务器要
 * 更早的一批（关注类和 X 为你推荐），直到接上卡上缓存；拉不到就先接卡上缓存。原因：两次刷新之间漏掉的帖子要能补上。
 * Treader revision (1.2.8, user request): one more time to go online: with Settings "keep pulling" on, paging past the
 * last fetched page asks the server for the next older batch (following lists and X For You) until it joins the
 * card's cache; on failure the card's cache follows. Reason: posts missed between refreshes should be recoverable.
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
#include "esp_timer.h"
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

#define TAG "app_x"
#define POLL_MS 400
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CW (TR_W - 2 * TR_MARGIN)

// 长度都随显示大小放大（页边距和细线不放大）；列表底边跟着标签栏走，正文和评论的底边跟着屏幕底走。
// Lengths scale with the display size (margins and hairlines do not); the list bottom follows the tab bar, the text
// and comment bottoms follow the screen bottom.

// 列表：一条一张卡片，每页按高度装满。/ List: one card per post, each page filled by height.
#define LIST_MAX TOMATO_LIST_CAP
#define LIST_TOP TR_S(288)
#define LIST_BOTTOM (TR_TABS_Y - TR_S(64))
#define PAGER_Y (TR_TABS_Y - TR_S(58))
#define AVATAR TR_S(60)
#define CARD_PX TR_S(30)
#define CARD_LH TR_S(42)
#define CARD_LINES 3
#define CARD_LINES_MAX 10   // 页面有空时最多显示到这么多行 / At most this many lines when the page has room
#define QUOTE_PX TR_S(26)
#define QUOTE_LH TR_S(38)
#define QUOTE_LINES 2
#define QUOTE_LINES_MAX 5
#define STRIP_GAP TR_S(11)
#define STRIP_W ((CW - 2 * STRIP_GAP) / 3)
#define STRIP_H TR_S(164)
#define SINGLE_MAX TR_S(300)
// 页面有空时单张图最多放大到这么大（缩略图只有 300 宽，再大就糊了）。/ With room on the page a single image grows to
// at most this size (thumbnails are only 300 wide; larger turns blurry).
#define SINGLE_BIG_W TR_S(420)
#define SINGLE_BIG_H TR_S(480)
// 分页时图片先按这么小算，好让一页多装一条；剩下的空白再把它们放回正常大小。/ Paging counts images this small so a
// page holds one more post; the leftover space then brings them back to normal size.
#define SINGLE_MIN_H TR_S(200)
#define STRIP_MIN_H TR_S(120)
#define FOOT_H TR_S(44)
#define CARD_GAP TR_S(16)
#define QUOTE_GRAY 0xE0
#define STATUS_Y TR_S(240)
#define REFRESH_RECT (EpdRect){TR_W - TR_MARGIN - TR_S(240), TR_S(70), TR_S(240), TR_S(80)}
#define EMPTY_PX TR_S(30)
#define EMPTY_LH TR_S(46)
#define EMPTY_LINES 8
#define SETUP_TEXT "还没连接 x 服务器。先在电脑或 NAS 上部署 x（见项目 x/ 目录），" \
                   "再到“设置 › 内容来源 › X”填服务器地址和设备令牌。"
// 对话框两边留白随放大收窄（正常 92，特大到页边距），一行的提示才放得下。/ The dialog's side inset narrows as the
// size grows (92 at normal, down to the margin at the largest) so its one-line hints still fit.
#define DIALOG_X (92 - (ui_scale_pct - 100) * 52 / 30)
#define DIALOG_RECT (EpdRect){DIALOG_X, TR_S(500), TR_W - 2 * DIALOG_X, TR_S(220)}

// 推文 / Post
#define HEAD_Y TR_S(150)
#define POST_SEG (EpdRect){TR_MARGIN, TR_S(232), CW, TR_S(56)}
#define BODY_TOP TR_S(316)
#define BODY_BOTTOM (TR_H - TR_S(116))
#define QUOTE_INSET TR_S(20)
#define POST_SQUARE post_square()
#define COMMENT_AVATAR TR_S(48)
#define COMMENT_X (TR_MARGIN + TR_S(64))
#define COMMENT_W (CW - TR_S(64))
#define BODY_PX TR_S(32)
#define BODY_LH TR_S(52)
#define TEXT_PAGER_Y (TR_H - TR_S(92))
#define LINES_MAX 1600
#define PAGES_MAX 200
#define COMMENTS_MAX 20
#define COMMENT_LINES 5
#define COMMENT_PX TR_S(30)
#define COMMENT_LH TR_S(44)
#define IMAGE_BAND TR_S(96)     // 看图底部的白条 / White strip under the image

// 正文最后一页一排四张小图：放大时方块缩到一排放得下（正常档保持原样）。/ Four small images in a row on the last text
// page: at larger sizes a square shrinks to what one row holds (unchanged at the normal size).
static int post_square(void) {
    const int fit = (CW - 3 * STRIP_GAP) / 4;
    return ui_scale_pct == 100 || TR_S(144) <= fit ? TR_S(144) : fit;
}

typedef enum { V_LIST, V_IMAGE, V_TEXT } view_t;
typedef enum { SEG_FORYOU, SEG_FOLLOWING } seg_t;
typedef enum { T_BODY, T_COMMENTS } text_tab_t;

typedef struct {
    char id[24];
    char user[48];
    char text[440];
    char rt_user[48];
    char rt_text[248];
    char source[160];     // @账号 · 转推人 · 回复谁 · 视频 / @handle · reposter · reply target · video
    double time;          // 毫秒或秒 / Milliseconds or seconds
    float ratio;          // 封面高宽比 / Cover height/width
    int16_t images;       // 大图页数 / Full-image pages
    int16_t pics;         // 有方形小图的原图张数（旧服务器为 0）/ Source images with squares (0 from older servers)
    int16_t height;       // 卡片高度 / Card height
    // 卡片在它那一页上的样子（剩下的空白分给它以后）。/ The card's shape on its page, after it took its share of
    // the leftover space.
    int8_t lines, qlines; // 正文、转发原文显示几行 / Text and repost lines shown
    int8_t media;         // 0 没图，1 一张放大，2 一排方形小图 / 0 none, 1 one enlarged, 2 a strip of squares
    int16_t mw, mh;       // 图片区宽高 / Image area size
    int16_t nw, nh;       // 图片的正常大小 / The images' normal size
    int32_t likes, comments, reposts;
    bool seen;
    bool next;            // 服务器在这条后面可能还有更早的 / The server may hold older entries after this one
} item_t;

typedef struct {
    const char* user;
    const char* text;
    int likes;
} comment_t;

typedef struct {
    uint32_t off;
    uint16_t len;
    bool rt;              // 转发原文的行（画在灰框里）/ A line of the reposted original (drawn in the gray box)
} line_t;

/* ---- 状态 / State ---- */

static view_t s_view;
static seg_t s_seg;
static char s_msg[96];
static int64_t s_poll_ms;
// 上次点当前分段的时间（双击回第一页）。/ Last tap on the current segment (a double tap goes to the first page).
static int64_t s_seg_tap_ms;
static bool s_has_cfg;
// 服务器上的“正在关注”开关（上次刷新时知道的）。/ The server's Following switch as of the last refresh.
static bool s_following_on = true;

// 后台任务 / Background job
static bool s_job_active;
static uint32_t s_job_seq;           // 本页任务的序号，用来认出“是不是自己的”/ Our job's sequence number
static tomato_x_req_t s_job;
static tomato_status_t s_status;
static bool s_step_loaded;
// 刷新只先下前几条的缩略图，翻到的页再补；进页后成功刷新过才自动补，免得没网时一翻页就去连 Wi-Fi。
// A refresh fetches thumbnails for the first posts only and the pages being viewed fill in later; that happens only
// after a successful refresh in this visit, so paging offline never tries to join Wi-Fi.
#define THUMB_FIRST 6
#define THUMB_PAGES 2
static bool s_lazy_ok;
static int s_thumbs_tried = -1;   // 这一页已经补过一次（失败了也不再反复试）/ This page was already filled once
// 已经提前显示过缩略图的列表页（-1 是还没有）。/ The list page whose thumbnails were already shown early (-1: none).
static int s_thumbs_page = -1;
static tomato_x_req_t s_pending;
static bool s_has_pending;

// 列表 / List
static item_t* s_items;
static int s_item_count;
static int s_list_page;
static double s_list_time;           // 服务器给列表的时间（秒）/ Server time of the list (seconds)
static uint16_t s_page_first[LIST_MAX + 1];   // 每页第一条 / First item of each page
static int s_list_page_count;
// 往后翻接着拉服务器（设置里的开关）。/ Keep pulling from the server (the Settings switch).
static bool s_more_on;     // 开着且这一段支持 / On, and this segment supports it
static bool s_gap_skip;    // 这次拉不到，先接卡上缓存 / A pull failed; show the card's cache for now
static bool s_turn_after;  // 拉到以后自动翻到下一页 / Turn to the next page once the pull lands
static int s_vis;          // 显示到第几条（第一个断点为止）/ Entries shown (up to the first break)

// 正在关注刷新期间点开的推文：刷新会覆盖 following.json，等它结束再写“看过”。
// Posts opened while the Following list refreshes: the refresh rewrites following.json, so mark them seen afterwards.
#define SEEN_PENDING_MAX 16
static char s_seen_pending[SEEN_PENDING_MAX][24];
static int s_seen_pending_count;

// 正在等服务器给的推文（等待框）。/ A post being fetched from the server (the wait dialog).
static bool s_wait;
static char s_wait_id[24];

// 推文 / Post
static char* s_arena;
static size_t s_arena_len, s_arena_cap;
static char s_id[24];
static bool s_post_loaded;
static const char *s_user, *s_source, *s_body, *s_rt;
static double s_time;
static int s_images, s_likes, s_comment_total, s_reposts;
static int s_pics;                   // 有方形小图的原图张数 / Source images with squares
static int16_t s_starts[TOMATO_WB_POST_SQUARES];   // 每张原图的第一页 / Each source image's first page
static int s_tail_y;                 // 最后一页小图和计数的位置 / Where the strip and counts go on the last page
static comment_t* s_comments;
static int s_comment_count;
static bool s_comments_loaded;       // c.json 读到了 / c.json was read
static char s_comment_msg[96];       // 评论没取到的原因 / Why comments failed
// 服务器还在后台取回复：从什么时候开始等、上次问的时间。/ The server is still fetching replies in the background:
// when the wait began and when we last asked.
#define REPLY_ASK_MS 3000
#define REPLY_GIVE_UP_MS 180000
static bool s_reply_wait;
static int64_t s_reply_wait_start, s_reply_asked;
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
    s_reply_wait = false;
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

static cJSON* read_json(const char* path) { return tomato_cache_read_json(path); }

// 截断后去掉结尾不完整的 UTF-8 字（X 的名字常带表情和中日文）。/ Drop a partial UTF-8 character left at the end by
// truncation (X names often carry emoji and CJK).
static void utf8_trim(char* s) {
    size_t n = strlen(s), k = 0;
    while (k < n && k < 4 && ((unsigned char)s[n - 1 - k] & 0xC0) == 0x80) k++;
    if (k == n) { s[0] = 0; return; }
    const unsigned char lead = (unsigned char)s[n - 1 - k];
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need != k + 1) s[n - 1 - k] = 0;
}

static void str(char* out, size_t cap, const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(out, cap, "%s", v->valuestring);
        utf8_trim(out);
    } else if (cap) {
        out[0] = 0;
    }
}

static double dnum(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static bool exists(const char* path) {
    size_t n = 0;
    return tomato_cache_size(path, &n) && n > 0;
}

// 从 JPEG 文件头读宽高（服务器出的是基线 JPEG，SOF 在开头不远）。/ Read width and height from the JPEG header
// (the server writes baseline JPEGs with the SOF near the start).
static bool jpeg_size(const char* path, int* w, int* h) { return tomato_cache_jpeg_size(path, w, h); }

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

static bool start_req(const tomato_x_req_t* r) {
    esp_err_t err = tomato_sync_start_x(r);
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "%s", err == ESP_ERR_INVALID_STATE ? "后台正在同步，稍后再试" : "无法开始加载");
        return false;
    }
    s_job = *r;
    s_job_seq = tomato_sync_seq();
    s_job_active = true;
    // 只补缩略图时列表早在卡上，不算“第 1 步写好”，免得页码被拉回第一页。/ Thumbnail fills reuse the list already on
    // the card, so they never count as "step 1 written", which would reset the page.
    s_step_loaded = r->thumbs_only;
    s_thumbs_page = -1;
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
static bool queue_req(const tomato_x_req_t* r) {
    const tomato_x_req_t req = *r;
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

static bool start_job(tomato_x_job_t job, const char* id, bool following, bool images_only) {
    tomato_x_req_t req = {.job = job, .following = following, .images_only = images_only};
    if (id) snprintf(req.id, sizeof(req.id), "%s", id);
    return queue_req(&req);
}

// 刷新列表：只先下前 THUMB_FIRST 条的缩略图。/ Refresh the list, fetching thumbnails for the first THUMB_FIRST posts only.
static void start_feed(bool following) {
    const tomato_x_req_t req = {.job = TOMATO_X_FEED, .following = following, .thumbs_count = THUMB_FIRST};
    queue_req(&req);
}

static void drop_job(tomato_x_job_t job) {
    if (s_has_pending && s_pending.job == job) s_has_pending = false;
    if (s_job.job == job && ours_running()) tomato_sync_cancel();
}

static bool job_running(tomato_x_job_t job) {
    return (s_has_pending && s_pending.job == job) || (s_job.job == job && ours_running());
}

// 当前分段的列表任务在跑吗。/ Whether the list job for the current segment runs.
static bool list_job_running(void) {
    if (s_has_pending && s_pending.job == TOMATO_X_FEED && s_pending.following == (s_seg == SEG_FOLLOWING))
        return true;
    return s_job.job == TOMATO_X_FEED && s_job.following == (s_seg == SEG_FOLLOWING) && ours_running();
}

static void flush_seen(void) {
    for (int i = 0; i < s_seen_pending_count; i++) tomato_x_mark_seen(s_seen_pending[i]);
    s_seen_pending_count = 0;
}

static bool follow_job_running(void) {
    if (s_has_pending && s_pending.job == TOMATO_X_FEED && s_pending.following) return true;
    return s_job.job == TOMATO_X_FEED && s_job.following && ours_running();
}

static const char* loading_text(const char* what) {
    return s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : what;
}

/* ---- 列表数据 / List data ---- */

static const char* list_file(void) {
    return s_seg == SEG_FOLLOWING ? TOMATO_X_FOLLOWING_FILE : TOMATO_X_FORYOU_FILE;
}

static int list_pages(void) { return s_list_page_count ? s_list_page_count : 1; }

// 有几行（最多 max）。/ How many lines (at most max).
static int count_lines(const char* text, int px, int w, int max) {
    int n = 0;
    for (const char* p = text; *p && n < max; n++) {
        p += tr_line_bytes(p, px, w);
        if (*p == '\n') p++;
    }
    return n;
}

// 卡片里正文和引用框各显示什么。/ What a card shows as text and in the quote box.
static const char* card_text(const item_t* x) { return x->text[0] || x->rt_text[0] ? x->text : "（没有文字）"; }

static void card_quote(const item_t* x, char* out, size_t cap) {
    if (x->rt_text[0]) snprintf(out, cap, "引用 @%s：%s", x->rt_user[0] ? x->rt_user : "原推文", x->rt_text);
    else out[0] = 0;
}

// 卡片里的图：0 没有，1 一张放大（w×h），2 一排方形小图。/ Card images: 0 none, 1 one enlarged (w x h), 2 a strip.
// 卡片里图片以外的高度。/ Card height without the images.
static int card_text_height(const item_t* x) {
    int h = TR_S(80) + x->lines * CARD_LH;
    if (x->qlines) h += TR_S(12) + TR_S(20) + x->qlines * QUOTE_LH;
    return h + TR_S(10) + FOOT_H + CARD_GAP;
}

static int card_media(const item_t* x, int* w, int* h) {
    if (x->images <= 0) return 0;
    int kind = 2;
    if (x->pics >= 2) {
        *w = CW;
        *h = STRIP_H;
    } else {
        kind = 1;
        const float r = x->ratio > 0.5f && x->ratio < 2.0f ? x->ratio : 1.0f;
        *w = SINGLE_MAX;
        *h = (int)(SINGLE_MAX * r);
        if (*h > SINGLE_MAX) { *h = SINGLE_MAX; *w = (int)(SINGLE_MAX / r); }
    }
    // 放大后一页可能装不下整张卡片：图片压矮到放得下（正常档不会发生）。/ At larger sizes a card may not fit one
    // page: shrink the images until it does (never happens at the normal size).
    int room = LIST_BOTTOM - LIST_TOP - card_text_height(x) - TR_S(12);
    if (room < TR_S(80)) room = TR_S(80);
    if (*h > room) {
        if (kind == 1) *w = *w * room / *h;
        *h = room;
    }
    return kind;
}

static int card_height(const item_t* x) { return card_text_height(x) + (x->media ? TR_S(12) + x->mh : 0); }

// 单张图按高宽比放进 wmax×hmax。/ Fit the single image into wmax x hmax by its aspect ratio.
static void single_fit(const item_t* x, int wmax, int hmax, int* w, int* h) {
    const float r = x->ratio > 0.5f && x->ratio < 2.0f ? x->ratio : 1.0f;
    *h = hmax;
    *w = (int)(hmax / r);
    if (*w > wmax) { *w = wmax; *h = (int)(wmax * r); }
}

// 紧凑的样子，用来分页：正文 CARD_LINES 行、原文 QUOTE_LINES 行、图片最小。/ The compact shape used for paging:
// CARD_LINES of text, QUOTE_LINES of repost, images at their smallest.
static void card_base(item_t* x) {
    char quote[320];
    card_quote(x, quote, sizeof(quote));
    x->lines = (int8_t)count_lines(card_text(x), CARD_PX, CW, CARD_LINES);
    x->qlines = quote[0] ? (int8_t)count_lines(quote, QUOTE_PX, CW - 2 * QUOTE_INSET, QUOTE_LINES) : 0;
    int w = 0, h = 0;
    x->media = (int8_t)card_media(x, &w, &h);
    x->nw = x->mw = (int16_t)w;
    x->nh = x->mh = (int16_t)h;
    if (x->media == 1 && h > SINGLE_MIN_H) {
        single_fit(x, w, SINGLE_MIN_H, &w, &h);
        x->mw = (int16_t)w;
        x->mh = (int16_t)h;
    } else if (x->media == 2 && h > STRIP_MIN_H) {
        x->mh = (int16_t)STRIP_MIN_H;
    }
    x->height = (int16_t)card_height(x);
}

// 图片往 wmax×hmax 长，最多用掉 *left。/ Grow the images toward wmax x hmax, using at most *left.
static void grow_media(item_t* x, int* left, int wmax, int hmax) {
    if (*left <= 0) return;
    int w = x->mw, h = x->mh + *left < hmax ? x->mh + *left : hmax;
    if (x->media == 1) single_fit(x, wmax, h, &w, &h);
    else if (x->media != 2) return;
    if (h <= x->mh) return;
    *left -= h - x->mh;
    x->mw = (int16_t)w;
    x->mh = (int16_t)h;
}

// 把最多 budget 的空白用在这张卡片上：先把图片放回正常大小，再多显示正文和原文，最后放大图片；返回用掉多少。
// Spend up to budget of spare height on this card: images back to normal size first, then more text and repost
// lines, then bigger images; returns what it used.
static int card_grow(item_t* x, int budget) {
    int left = budget;
    grow_media(x, &left, x->nw, x->nh);
    const int full = count_lines(card_text(x), CARD_PX, CW, CARD_LINES_MAX);
    while (x->lines < full && left >= CARD_LH) { x->lines++; left -= CARD_LH; }
    if (x->qlines) {
        char quote[320];
        card_quote(x, quote, sizeof(quote));
        const int qfull = count_lines(quote, QUOTE_PX, CW - 2 * QUOTE_INSET, QUOTE_LINES_MAX);
        while (x->qlines < qfull && left >= QUOTE_LH) { x->qlines++; left -= QUOTE_LH; }
    }
    // 一排小图最多长成正方形。/ The strip grows up to squares.
    if (x->media == 1) grow_media(x, &left, SINGLE_BIG_W, SINGLE_BIG_H);
    else grow_media(x, &left, CW, STRIP_W);
    x->height = (int16_t)card_height(x);
    return budget - left;
}

// 开关开着时列表只显示到第一个断点（服务器在那条后面可能还有），翻过去再拉。/ With the switch on, the list shows
// up to the first break (the server may hold more after it), pulled when paged past.
static void set_visible(void) {
    s_more_on = true && tomato_feed_more_on();
    s_vis = s_item_count;
    if (!s_more_on || s_gap_skip) return;
    for (int i = 0; i < s_item_count; i++)
        if (s_items[i].next) { s_vis = i + 1; return; }
}

// 翻过最后一页还能向服务器要。/ Paging past the last page can still ask the server.
static bool more_left(void) { return s_more_on && !s_gap_skip && s_vis > 0 && s_items[s_vis - 1].next; }

// 按卡片高度分页：放不下的整张挪到下一页。/ Page by card height: a card that does not fit moves to the next page.
static void layout_list(void) {
    s_list_page_count = 0;
    int y = LIST_BOTTOM;
    for (int i = 0; i < s_vis; i++) {
        card_base(&s_items[i]);
        if (y + s_items[i].height > LIST_BOTTOM && y != LIST_TOP) {
            s_page_first[s_list_page_count++] = (uint16_t)i;
            y = LIST_TOP;
        }
        y += s_items[i].height;
    }
    s_page_first[s_list_page_count] = (uint16_t)s_vis;
    // 空列表也把第一页的结尾写成 0：否则沿用上一份列表的结尾，点空白处会打开看不见的旧帖子。
    // An empty list also ends page one at 0; otherwise the previous list's end stays and a tap on blank space opens an
    // invisible old post.
    if (s_list_page_count == 0) s_page_first[1] = 0;
    // 每页剩下的空白分给本页的卡片：先平分，谁用不完再按顺序给后面的。
    // Each page's leftover space goes to its cards: an even share first, then whatever is unused, in order.
    for (int p = 0; p < s_list_page_count; p++) {
        const int a = s_page_first[p], b = s_page_first[p + 1];
        int left = LIST_BOTTOM - LIST_TOP;
        for (int i = a; i < b; i++) left -= s_items[i].height;
        for (int i = a; i < b && left > 0; i++) left -= card_grow(&s_items[i], left / (b - i));
        for (int i = a; i < b && left > 0; i++) left -= card_grow(&s_items[i], left);
    }
}

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
        str(x->source, sizeof(x->source), it, "source");
        x->time = dnum(it, "time");
        x->ratio = (float)dnum(it, "ratio");
        x->images = (int16_t)dnum(it, "images");
        x->pics = (int16_t)dnum(it, "pics");
        x->likes = (int32_t)dnum(it, "likes");
        x->comments = (int32_t)dnum(it, "comments");
        x->reposts = (int32_t)dnum(it, "reposts");
        x->seen = s_seg == SEG_FORYOU || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "seen"));
        x->next = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(it, "next"));
        s_item_count++;
    }
    cJSON_Delete(root);
    set_visible();
    layout_list();
    if (s_list_page >= list_pages()) s_list_page = list_pages() - 1;
}

static bool post_cached(const char* id) {
    char dir[64], file[80];
    tomato_x_post_dir(id, dir, sizeof(dir));
    snprintf(file, sizeof(file), "%s/post.json", dir);
    return exists(file);
}

/* ---- 推文数据 / Post data ---- */

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

static int tail_height(void) {
    return (s_pics > 0 || s_images > 0 ? POST_SQUARE + TR_S(20) : 0) + FOOT_H + TR_S(20);
}

static void add_lines(const char* base, const char* text, int w, bool rt) {
    for (const char* p = text; *p && s_line_count < LINES_MAX;) {
        size_t n = tr_line_bytes(p, BODY_PX, w);
        if (n > 0xFFFF) n = 0xFFFF;
        s_lines[s_line_count++] = (line_t){.off = (uint32_t)(p - base), .len = (uint16_t)n, .rt = rt};
        p += n;
        if (*p == '\n') p++;
    }
}

// 正文的行，空一行，转发原文的行（窄一点，画在灰框里）；再按页切，最后一页留出小图和计数的位置。
// Text lines, a blank line, then the repost's lines (narrower, in the gray box); paged, leaving room on the last page
// for the image strip and counts.
static void layout_body(void) {
    s_line_count = 0;
    add_lines(s_body, s_body, CW, false);
    if (s_rt[0]) {
        if (s_line_count && s_line_count < LINES_MAX) s_lines[s_line_count++] = (line_t){0};
        add_lines(s_rt, s_rt, CW - 2 * QUOTE_INSET, true);
    }
    s_pages[0] = 0;
    s_page_count = 1;
    int y = BODY_TOP;
    for (int i = 0; i < s_line_count; i++) {
        if (y + BODY_LH > BODY_BOTTOM) {
            if (s_page_count >= PAGES_MAX) break;
            s_pages[s_page_count++] = (uint16_t)i;
            y = BODY_TOP;
        }
        y += BODY_LH;
    }
    if (y + TR_S(16) + tail_height() > BODY_BOTTOM && s_page_count < PAGES_MAX) {
        s_pages[s_page_count++] = (uint16_t)s_line_count;
        y = BODY_TOP;
    } else if (s_line_count) {
        y += TR_S(16);
    }
    s_tail_y = y;
}

static int comment_lines(const comment_t* c) {
    const char* p = c->text;
    int n = 0;
    while (*p && n < COMMENT_LINES) { p += tr_line_bytes(p, COMMENT_PX, COMMENT_W); if (*p == '\n') p++; n++; }
    return n ? n : 1;
}

static int comment_height(const comment_t* c) { return TR_S(52) + comment_lines(c) * COMMENT_LH + TR_S(26); }

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
    tomato_x_comments_path(s_id, path, sizeof(path));
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
    tomato_x_post_dir(id, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/post.json", dir);
    size_t file_len = 0, comments_len = 0;
    if (!tomato_cache_size(path, &file_len) || !file_len) return false;
    cJSON* root = read_json(path);
    if (!root) return false;
    // 内存池：正文文件加评论文件（评论可能稍后才到，按 32 KB 预留）。/ Arena: the post file plus the comments file
    // (comments may arrive later, so reserve 32 KB).
    tomato_x_comments_path(id, cpath, sizeof(cpath));
    const size_t comments = tomato_cache_size(cpath, &comments_len) && comments_len > 32768 ? comments_len : 32768;
    s_arena_cap = file_len + comments + 1024;
    s_arena = heap_caps_malloc(s_arena_cap, PSRAM_CAPS);
    if (!s_arena) { cJSON_Delete(root); s_arena_cap = 0; return false; }
    s_user = keep_key(root, "user");
    s_source = keep_key(root, "source");
    s_body = keep_key(root, "text");
    // 引用的推文写成“引用 @原作者：原文”放进灰框。/ A quoted post becomes "quote @author: original" in the gray box.
    const cJSON* ru = cJSON_GetObjectItemCaseSensitive(root, "rt_user");
    const cJSON* rt = cJSON_GetObjectItemCaseSensitive(root, "rt_text");
    if (cJSON_IsString(rt) && rt->valuestring && rt->valuestring[0]) {
        char who[64], head[96];
        snprintf(who, sizeof(who), "%s", cJSON_IsString(ru) && ru->valuestring[0] ? ru->valuestring : "原推文");
        utf8_trim(who);
        snprintf(head, sizeof(head), "引用 @%s：", who);
        const size_t n = strlen(head) + strlen(rt->valuestring) + 1;
        if (s_arena_len + n <= s_arena_cap) {
            char* out = s_arena + s_arena_len;
            snprintf(out, n, "%s%s", head, rt->valuestring);
            s_arena_len += n;
            s_rt = out;
        } else {
            s_rt = "";
        }
    } else {
        s_rt = "";
    }
    s_pics = (int)dnum(root, "pics");
    memset(s_starts, 0, sizeof(s_starts));
    int k = 0;
    const cJSON* sv;
    cJSON_ArrayForEach(sv, cJSON_GetObjectItemCaseSensitive(root, "starts")) {
        if (k >= TOMATO_WB_POST_SQUARES) break;
        s_starts[k++] = cJSON_IsNumber(sv) ? (int16_t)sv->valueint : 0;
    }
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

static void image_path(int k, char* out, size_t cap) { tomato_x_img_path(s_id, k + 1, out, cap); }

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

// 固定提示最多 8 行，放大后也写得完。/ Fixed hints get up to 8 lines so they still show whole at larger sizes.
static void draw_empty(uint8_t* fb, int y, const char* text) {
    tr_wrap(fb, TR_MARGIN, y, CW, EMPTY_PX, EMPTY_LH, text, EMPTY_LINES);
}

// “去设置”按钮放在说明文字下面（正常档仍在 420）。/ The Set up button sits below the hint (still at 420 at normal).
static EpdRect setup_rect(void) {
    const int lines = count_lines(SETUP_TEXT, EMPTY_PX, CW, EMPTY_LINES);
    const int below = LIST_TOP + (lines - 1) * EMPTY_LH + EMPTY_PX + TR_S(10);
    return (EpdRect){TR_MARGIN, below > TR_S(420) ? below : TR_S(420), TR_S(240), TR_S(72)};
}

static void draw_header(uint8_t* fb) {
    tr_title(fb, "X");
    if (s_has_cfg) {
        if (list_job_running()) {
            // 拉取中点一下就取消，下好的留着。/ Tap while fetching to cancel; what arrived stays.
            tr_text(fb, TR_W - TR_MARGIN, TR_S(98), TR_S(30), "拉取中 · 取消", EPD_DRAW_ALIGN_RIGHT, false);
        } else if (!tomato_sync_busy() || tomato_sync_preemptible()) {
            // 别的页在浏览类拉取也能点：start_job 会先请它让路。/ Also tappable while another page's browsing job
            // runs: start_job asks it to make way first.
            tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - ttf_text_width_px(TR_S(30), "刷新") - TR_S(26), TR_S(116),
                    false);
            tr_text(fb, TR_W - TR_MARGIN, TR_S(98), TR_S(30), "刷新", EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    static const char* const SEG[2] = {"为你推荐", "正在关注"};
    tr_segment(fb, TR_SEGMENT_RECT, SEG, 2, (int)s_seg);
    // 服务器关了正在关注：这一格铺浅灰、写明已关。/ Following off on the server: that cell is light gray and says so.
    if (!s_following_on) {
        const EpdRect r = TR_SEGMENT_RECT;
        const int w = r.width / 2;
        const EpdRect cell = {r.x + w + TR_S(4), r.y + TR_S(4), r.width - w - TR_S(8), r.height - TR_S(8)};
        ui_fill_round_rect(fb, cell, cell.height / 2, QUOTE_GRAY);
        // 特大档这一格放不下整句：字号往下收到放得下为止。/ At the largest size the label does not fit the cell:
        // step the font down until it does.
        int px = TR_S(30);
        while (px > 24 && ttf_text_width_px(px, "正在关注（已关）") > cell.width - TR_S(16)) px--;
        tr_text(fb, cell.x + cell.width / 2, r.y + (r.height - px) / 2 - TR_S(2), px, "正在关注（已关）",
                EPD_DRAW_ALIGN_CENTER, false);
    }
}

// 头像：圆圈里写名字的第一个字。/ Avatar: the first character of the name in a circle.
static void draw_avatar(uint8_t* fb, int x, int y, int d, const char* name) {
    // Material 3 头像：浅灰实心圆里写首字（这些页只用整页刷新）。/ Material 3 avatar: the first character on a
    // filled light gray circle (these pages refresh whole pages only).
    const int cx = x + d / 2, cy = y + d / 2;
    epd_fill_circle(cx, cy, d / 2, TR_GRAY_SELECTED, fb);
    char first[8] = "X";
    if (name && name[0]) {
        const unsigned char c = (unsigned char)name[0];
        const size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (strlen(name) >= n) { memcpy(first, name, n); first[n] = 0; }
    }
    const int px = d * 28 / 60;
    tr_text_on(fb, cx, cy - px / 2 - TR_S(3), px, first, EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SELECTED);
}

// 卡片和正文页共用的头部：头像、粗体名字、下一行时间和来源。/ Header shared by cards and the post page: avatar,
// bold name, then time and source.
static void draw_head(uint8_t* fb, int y, const char* user, double t, const char* source, bool long_time, bool dot) {
    draw_avatar(fb, TR_MARGIN, y, AVATAR, user);
    if (dot) {
        epd_fill_circle(TR_MARGIN + AVATAR - TR_S(6), y + TR_S(6), TR_S(10), UI_GRAY_WHITE, fb);
        epd_fill_circle(TR_MARGIN + AVATAR - TR_S(6), y + TR_S(6), TR_S(7), UI_GRAY_BLACK, fb);
    }
    char line[200], when[24];
    snprintf(line, sizeof(line), "%s", user && user[0] ? user : "X 用户");
    tr_fit(line, TR_S(30), CW - TR_S(78));
    tr_text(fb, TR_MARGIN + TR_S(78), y + TR_S(2), TR_S(30), line, EPD_DRAW_ALIGN_LEFT, true);
    fmt_time(when, sizeof(when), t, long_time);
    snprintf(line, sizeof(line), "%s%s%s", when, source && source[0] ? " · " : "", source ? source : "");
    utf8_trim(line);
    tr_fit(line, TR_S(22), CW - TR_S(78));
    tr_text_dim(fb, TR_MARGIN + TR_S(78), y + TR_S(40), TR_S(22), line, EPD_DRAW_ALIGN_LEFT);
}

// 转发、评论、赞的小图标（约 28 像素）。/ Small repost, comment and like glyphs (about 28 px).
static void draw_glyph(uint8_t* fb, int kind, int cx, int cy) {
    const uint8_t c = TR_GRAY_SECONDARY;
#define G TR_S
    if (kind == 0) {
        for (int d = 0; d < 2; d++) {
            epd_draw_line(cx - G(12), cy - G(5) + d, cx + G(10), cy - G(5) + d, c, fb);
            epd_draw_line(cx + G(12), cy + G(5) + d, cx - G(10), cy + G(5) + d, c, fb);
        }
        epd_fill_triangle(cx + G(13), cy - G(4), cx + G(6), cy - G(10), cx + G(6), cy + G(2), c, fb);
        epd_fill_triangle(cx - G(13), cy + G(6), cx - G(6), cy, cx - G(6), cy + G(12), c, fb);
    } else if (kind == 1) {
        for (int d = 0; d < 2; d++)
            epd_draw_rect((EpdRect){cx - G(12) + d, cy - G(10) + d, G(24) - 2 * d, G(17) - 2 * d}, c, fb);
        epd_fill_triangle(cx - G(8), cy + G(6), cx - G(1), cy + G(6), cx - G(9), cy + G(13), c, fb);
    } else {
        epd_fill_circle(cx - G(5), cy - G(3), G(6), c, fb);
        epd_fill_circle(cx + G(5), cy - G(3), G(6), c, fb);
        epd_fill_triangle(cx - G(11), cy - G(1), cx + G(11), cy - G(1), cx, cy + G(11), c, fb);
    }
#undef G
}

// 底部三栏：转发、评论、赞。/ The three-column count bar: reposts, comments, likes.
static void draw_counts(uint8_t* fb, int y, int reposts, int comments, int likes) {
    const int vals[3] = {reposts, comments, likes};
    for (int k = 0; k < 3; k++) {
        char n[16];
        fmt_count(n, sizeof(n), vals[k]);
        const int col = TR_MARGIN + k * CW / 3, w = ttf_text_width_px(TR_S(24), n) + TR_S(40);
        const int x0 = col + (CW / 3 - w) / 2;
        draw_glyph(fb, k, x0 + TR_S(14), y + FOOT_H / 2);
        tr_text_dim(fb, x0 + TR_S(40), y + (FOOT_H - TR_S(24)) / 2 - TR_S(2), TR_S(24), n, EPD_DRAW_ALIGN_LEFT);
    }
}

// 一张小图：有就画，没有画个浅灰圆角块。/ One small image: drawn when present, a light gray block otherwise.
static void draw_pic(uint8_t* fb, const char* path, int x, int y, int w, int h) {
    const uint8_t* px = exists(path) ? hupu_img_get(path, w, h) : NULL;
    if (px) hupu_img_draw(fb, x, y, w, h, px, true);
    else ui_fill_round_rect(fb, (EpdRect){x, y, w, h}, TR_S(12), TR_GRAY_SURFACE);
}

// 黑底白字的小角标（“+3”）。/ A small black badge with white text ("+3").
static void draw_badge(uint8_t* fb, int right, int bottom, const char* text) {
    const int w = ttf_text_width_px(TR_S(24), text) + TR_S(20);
    ui_fill_round_rect(fb, (EpdRect){right - w - TR_S(6), bottom - TR_S(40), w, TR_S(34)}, TR_S(17), UI_GRAY_BLACK);
    tr_text_inv(fb, right - TR_S(6) - w / 2, bottom - TR_S(36), TR_S(24), text, EPD_DRAW_ALIGN_CENTER, true);
}

static void draw_card(uint8_t* fb, const item_t* x, int y) {
    draw_head(fb, y, x->user, x->time, x->source, false, !x->seen);
    int cy = y + TR_S(80);
    if (x->lines) tr_wrap(fb, TR_MARGIN, cy, CW, CARD_PX, CARD_LH, card_text(x), x->lines);
    cy += x->lines * CARD_LH;
    char quote[320];
    card_quote(x, quote, sizeof(quote));
    if (quote[0] && x->qlines) {
        const EpdRect box = {TR_MARGIN, cy + TR_S(12), CW, TR_S(20) + x->qlines * QUOTE_LH};
        ui_fill_round_rect(fb, box, TR_S(14), QUOTE_GRAY);
        tr_wrap(fb, TR_MARGIN + QUOTE_INSET, box.y + TR_S(10), CW - 2 * QUOTE_INSET, QUOTE_PX, QUOTE_LH, quote, x->qlines);
        cy = box.y + box.height;
    }
    const int media = x->media, mw = x->mw, mh = x->mh;
    char path[96];
    if (media == 1) {
        tomato_x_img_path(x->id, 0, path, sizeof(path));
        draw_pic(fb, path, TR_MARGIN, cy + TR_S(12), mw, mh);
        cy += TR_S(12) + mh;
    } else if (media == 2) {
        const int n = x->pics < TOMATO_WB_LIST_SQUARES ? x->pics : TOMATO_WB_LIST_SQUARES;
        for (int k = 0; k < n; k++) {
            const int px = TR_MARGIN + k * (STRIP_W + STRIP_GAP);
            tomato_x_square_path(x->id, k + 1, path, sizeof(path));
            draw_pic(fb, path, px, cy + TR_S(12), STRIP_W, mh);
            if (k == n - 1 && x->pics > n) {
                char more[12];
                snprintf(more, sizeof(more), "+%d", x->pics - n);
                draw_badge(fb, px + STRIP_W, cy + TR_S(12) + mh, more);
            }
        }
        cy += TR_S(12) + mh;
    }
    draw_counts(fb, cy + TR_S(10), x->reposts, x->comments, x->likes);
}

static void draw_cards(uint8_t* fb) {
    int y = LIST_TOP;
    for (int i = s_page_first[s_list_page]; i < s_page_first[s_list_page + 1]; i++) {
        draw_card(fb, &s_items[i], y);
        y += s_items[i].height;
        if (i + 1 < s_page_first[s_list_page + 1]) tr_hline(fb, y - CARD_GAP / 2 - 1, TR_MARGIN, CW, true);
    }
}

static void draw_list(uint8_t* fb) {
    draw_header(fb);
    if (!s_has_cfg) {
        draw_empty(fb, LIST_TOP, SETUP_TEXT);
        tr_pill(fb, setup_rect(), "去设置", true, TR_S(30));
        return;
    }
    char line[160];
    const bool loading = list_job_running();
    if (loading) {
        snprintf(line, sizeof(line), "%s", loading_text(s_job.older ? "正在向服务器要更早的…" : s_step_loaded ? "正在下载缩略图…" : "正在加载…"));
    } else if (s_msg[0]) {
        snprintf(line, sizeof(line), "%s", s_msg);
    } else {
        char when[16] = "";
        if (s_list_time > 1735689600) fmt_time(when, sizeof(when), s_list_time, true);
        snprintf(line, sizeof(line), "%s · %d 条%s%s", s_seg == SEG_FOLLOWING ? "正在关注" : "为你推荐", s_item_count,
                 when[0] ? " · 刷新于 " : "", when);
    }
    // 平常的条数用深灰，加载和出错的提示保持黑字。/ The plain count is dark gray; loading and errors stay black.
    if (!loading && !s_msg[0]) tr_text_dim(fb, TR_MARGIN, STATUS_Y, TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
    else tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - TR_S(12), CW, TR_S(26), line, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_item_count) {
        draw_empty(fb, LIST_TOP + TR_S(20), loading ? "加载中，请稍候。" : s_seg == SEG_FOLLOWING ?
                   "还没有缓存。联网后点右上角“刷新”，从服务器取正在关注的推文；之后不联网也能看。" :
                   "还没有缓存。联网后点右上角“刷新”，从服务器取为你推荐的推文；之后不联网也能看。");
        return;
    }
    draw_cards(fb);
    if (more_left()) tr_pager_more(fb, PAGER_Y, s_list_page, list_pages());
    else tr_pager(fb, PAGER_Y, s_list_page, list_pages());
}

static void draw_wait(uint8_t* fb) {
    const EpdRect r = DIALOG_RECT;
    // Material 3 对话框：白底大圆角、深灰细框。/ Material 3 dialog: white, large radius, thin dark outline.
    epd_fill_rect(r, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, r, TR_S(28), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), TR_S(28) - 1, TR_GRAY_OUTLINE);
    const int cx = r.x + r.width / 2;
    const bool failed = !job_running(TOMATO_X_POST) && s_msg[0];
    if (failed) {
        tr_text(fb, cx, r.y + TR_S(40), TR_S(32), "没能打开这条", EPD_DRAW_ALIGN_CENTER, true);
        tr_wrap(fb, r.x + TR_S(30), r.y + TR_S(96), r.width - TR_S(60), TR_S(26), TR_S(36), s_msg, 2);
        tr_text_dim(fb, cx, r.y + TR_S(168), TR_S(24), "点任意处关闭", EPD_DRAW_ALIGN_CENTER);
        return;
    }
    tr_text(fb, cx, r.y + TR_S(44), TR_S(34), "正在加载这条推文…", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, cx, r.y + TR_S(104), TR_S(26), loading_text("从服务器取全文和图片"), EPD_DRAW_ALIGN_CENTER, false);
    tr_text_dim(fb, cx, r.y + TR_S(156), TR_S(26), "点框外取消", EPD_DRAW_ALIGN_CENTER);
}

static void draw_image(uint8_t* fb) {
    s_image_missing = false;
    char path[96];
    image_path(s_image, path, sizeof(path));
    int w, h;
    if (exists(path) && image_fit(s_image, &w, &h)) {
        const uint8_t* px = hupu_img_get(path, w, h);
        if (px) hupu_img_draw(fb, (TR_W - w) / 2, (TR_H - IMAGE_BAND - h) / 2, w, h, px, false);
        else tr_text(fb, TR_W / 2, (TR_H - IMAGE_BAND) / 2, TR_S(30),
                     hupu_img_failed(path, w, h) ? "这张图打不开" : "正在处理图片…", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        const bool loading = job_running(TOMATO_X_POST);
        s_image_missing = loading;
        if (loading) {
            tr_text(fb, TR_W / 2, (TR_H - IMAGE_BAND) / 2, TR_S(30), loading_text("图片下载中…"), EPD_DRAW_ALIGN_CENTER,
                    false);
        } else {
            // 下载停了这张还没有：写原因，点中间重下。/ The download stopped without this image: say why; a tap in the
            // middle downloads again.
            const int cy = (TR_H - IMAGE_BAND) / 2;
            tr_text(fb, TR_W / 2, cy - TR_S(50), TR_S(30), "图片没下载成功", EPD_DRAW_ALIGN_CENTER, true);
            if (s_msg[0]) {
                char why[96];
                snprintf(why, sizeof(why), "%s", s_msg);
                tr_fit(why, TR_S(26), TR_W - 2 * TR_MARGIN);
                tr_text_dim(fb, TR_W / 2, cy + TR_S(4), TR_S(26), why, EPD_DRAW_ALIGN_CENTER);
            }
            tr_text_dim(fb, TR_W / 2, cy + (s_msg[0] ? TR_S(50) : TR_S(4)), TR_S(26), "点这里重试", EPD_DRAW_ALIGN_CENTER);
        }
    }
    epd_fill_rect((EpdRect){0, TR_H - IMAGE_BAND, TR_W, IMAGE_BAND}, UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_H - IMAGE_BAND, 0, TR_W, true);
    char label[32];
    snprintf(label, sizeof(label), "%d / %d", s_image + 1, s_images);
    tr_text(fb, TR_MARGIN, TR_H - IMAGE_BAND + TR_S(28), TR_S(30), "‹ 正文", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W / 2, TR_H - IMAGE_BAND + TR_S(28), TR_S(30), label, EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, TR_W - TR_MARGIN, TR_H - IMAGE_BAND + TR_S(28), TR_S(30), "回复 ›", EPD_DRAW_ALIGN_RIGHT, false);
    tr_wallsave_draw(fb, &s_ws);
}

static void draw_text_header(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(30), "‹ 返回", EPD_DRAW_ALIGN_LEFT, false);
    draw_head(fb, HEAD_Y, s_user, s_time, s_source, true, false);
    char l1[24], l2[24], n[16];
    snprintf(l1, sizeof(l1), "图片 %d", s_images);
    fmt_count(n, sizeof(n), s_comment_total);
    snprintf(l2, sizeof(l2), "回复 %s", n);
    const char* labels[3] = {"正文", l1, l2};
    tr_segment(fb, POST_SEG, labels, 3, s_tab == T_BODY ? 0 : 2);
}

// 正文最后一页下面那排小图有几张，第 k 张是哪个文件。/ How many small images the last page shows, and image k's file.
static int tail_count(void) {
    if (s_pics > 0) return s_pics < TOMATO_WB_POST_SQUARES ? s_pics : TOMATO_WB_POST_SQUARES;
    return s_images > 0 ? 1 : 0;
}

static void tail_path(int k, char* out, size_t cap) {
    if (s_pics > 0) tomato_x_square_path(s_id, k + 1, out, cap);
    else tomato_x_img_path(s_id, 0, out, cap);
}

static void draw_body(uint8_t* fb) {
    int y = BODY_TOP;
    if (!s_line_count && s_page == 0 && !tail_count()) {
        tr_text_dim(fb, TR_MARGIN, y, TR_S(30), "这条没有文字。", EPD_DRAW_ALIGN_LEFT);
        return;
    }
    const int start = s_pages[s_page];
    const int end = s_page + 1 < s_page_count ? s_pages[s_page + 1] : s_line_count;
    // 这一页上转发原文的行先铺灰框。/ Lay the gray box under this page's repost lines first.
    int first_rt = -1, last_rt = -1;
    for (int i = start; i < end; i++)
        if (s_lines[i].rt) { if (first_rt < 0) first_rt = i; last_rt = i; }
    if (first_rt >= 0) {
        const EpdRect box = {TR_MARGIN, y + (first_rt - start) * BODY_LH - TR_S(8), CW,
                             (last_rt - first_rt + 1) * BODY_LH + TR_S(16)};
        ui_fill_round_rect(fb, box, TR_S(14), QUOTE_GRAY);
    }
    char buf[512];
    for (int i = start; i < end; i++, y += BODY_LH) {
        const char* base = s_lines[i].rt ? s_rt : s_body;
        size_t n = s_lines[i].len < sizeof(buf) - 1 ? s_lines[i].len : sizeof(buf) - 1;
        memcpy(buf, base + s_lines[i].off, n);
        buf[n] = 0;
        tr_text(fb, TR_MARGIN + (s_lines[i].rt ? QUOTE_INSET : 0), y, s_lines[i].rt ? TR_S(30) : BODY_PX, buf,
                EPD_DRAW_ALIGN_LEFT, false);
    }
    if (s_page + 1 < s_page_count) return;
    // 最后一页：一排小图（多出来的在最后一张上写“+N”），再是三栏计数。/ Last page: a strip of small images ("+N" on
    // the last one for the rest), then the count bar.
    int ty = s_tail_y;
    const int n = tail_count();
    char path[96];
    for (int k = 0; k < n; k++) {
        const int px = TR_MARGIN + k * (POST_SQUARE + STRIP_GAP);
        tail_path(k, path, sizeof(path));
        draw_pic(fb, path, px, ty, POST_SQUARE, POST_SQUARE);
        if (k == n - 1 && s_pics > n) {
            char more[12];
            snprintf(more, sizeof(more), "+%d", s_pics - n);
            draw_badge(fb, px + POST_SQUARE, ty + POST_SQUARE, more);
        }
    }
    if (n) ty += POST_SQUARE + TR_S(20);
    tr_hline(fb, ty, TR_MARGIN, CW, true);
    draw_counts(fb, ty + TR_S(10), s_reposts, s_comment_total, s_likes);
}

static void draw_comments(uint8_t* fb) {
    if (!s_comments_loaded) {
        const char* text = job_running(TOMATO_X_COMMENTS) ? loading_text("正在取回复…（服务器第一次要去 X 取，约半分钟）")
                           : s_comment_msg[0] ? s_comment_msg : "回复还没取到";
        tr_wrap(fb, TR_MARGIN, BODY_TOP + TR_S(20), CW, TR_S(30), TR_S(44), text, 4);
        return;
    }
    if (!s_comment_count) {
        tr_text_dim(fb, TR_MARGIN, BODY_TOP + TR_S(20), TR_S(30), "还没有回复", EPD_DRAW_ALIGN_LEFT);
        return;
    }
    // 每条：小头像圆、粗体名字、右边赞数（心形图标），文字从头像右边开始。/ Each: a small avatar circle, bold name,
    // likes with a heart on the right, and the text starting right of the avatar.
    int y = BODY_TOP;
    char line[160];
    for (int i = s_comment_pages[s_page]; i < s_comment_pages[s_page + 1]; i++) {
        const comment_t* c = &s_comments[i];
        draw_avatar(fb, TR_MARGIN, y, COMMENT_AVATAR, c->user);
        snprintf(line, sizeof(line), "%s", c->user[0] ? c->user : "X 用户");
        tr_fit(line, TR_S(26), COMMENT_W - TR_S(140));
        tr_text(fb, COMMENT_X, y + TR_S(8), TR_S(26), line, EPD_DRAW_ALIGN_LEFT, true);
        if (c->likes > 0) {
            char n[16];
            fmt_count(n, sizeof(n), c->likes);
            tr_text_dim(fb, TR_W - TR_MARGIN, y + TR_S(8), TR_S(24), n, EPD_DRAW_ALIGN_RIGHT);
            draw_glyph(fb, 2, TR_W - TR_MARGIN - ttf_text_width_px(TR_S(24), n) - TR_S(20), y + TR_S(22));
        }
        tr_wrap(fb, COMMENT_X, y + TR_S(52), COMMENT_W, COMMENT_PX, COMMENT_LH, c->text[0] ? c->text : "（空）", COMMENT_LINES);
        y += comment_height(c);
        tr_hline(fb, y - TR_S(12), COMMENT_X, COMMENT_W, true);
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
        tr_title(fb, "X");
        draw_empty(fb, TR_S(240), "内存不足，打不开 X 页。");
        tr_tab_bar(fb, TR_TAB_X);
        return;
    }
    if (s_view == V_IMAGE && s_post_loaded) { draw_image(fb); return; }
    tr_status_bar(fb);
    if (s_view == V_TEXT && s_post_loaded) { draw_text(fb); return; }
    draw_list(fb);
    tr_tab_bar(fb, TR_TAB_X);
    if (s_wait) draw_wait(fb);
}

/* ---- 动作 / Actions ---- */

static app_redraw_t show_seg(seg_t seg) {
    if (seg == SEG_FOLLOWING && !s_following_on) {
        snprintf(s_msg, sizeof(s_msg), "服务器关了正在关注，到 x 面板设置里打开");
        return APP_REDRAW_PAGE;
    }
    s_seg = seg;
    s_gap_skip = false;
    s_turn_after = false;
    s_thumbs_tried = -1;
    s_list_page = 0;
    s_msg[0] = 0;
    hupu_img_drop_pending();
    load_list();
    // 这一段还没缓存：从服务器取一次（服务器上已有的，不会让它去 X）。/ No cache for this segment yet: take the
    // server's list once (what it already has; the server does not ask X).
    if (s_has_cfg && !s_item_count && !exists(list_file())) start_feed(seg == SEG_FOLLOWING);
    return APP_REDRAW_PAGE;
}

static app_redraw_t refresh(void) {
    s_gap_skip = false;
    s_turn_after = false;
    start_feed(s_seg == SEG_FOLLOWING);
    return APP_REDRAW_PAGE;
}

// 切到评论：没缓存就去取。/ Switch to comments, fetching them when not cached.
static app_redraw_t show_comments(void) {
    s_view = V_TEXT;
    s_tab = T_COMMENTS;
    s_page = 0;
    hupu_img_drop_pending();
    if (!s_comments_loaded && !job_running(TOMATO_X_COMMENTS)) {
        s_comment_msg[0] = 0;
        start_job(TOMATO_X_COMMENTS, s_id, false, false);
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
    if (post_missing_images() && !job_running(TOMATO_X_POST) && !job_running(TOMATO_X_COMMENTS))
        start_job(TOMATO_X_POST, s_id, false, true);
    return APP_REDRAW_PAGE;
}

// 推文已经在卡上：直接看正文；有图没下好就在后台补。/ A cached post opens on its text at once; missing images fill in
// behind it.
static app_redraw_t enter_post(const char* id) {
    if (!load_post(id)) return APP_REDRAW_NONE;
    s_wait = false;
    s_tab = T_BODY;
    s_page = 0;
    s_image = 0;
    s_view = V_TEXT;
    if (post_missing_images() && !(job_running(TOMATO_X_POST) && !strcmp(s_job.id, id)))
        start_job(TOMATO_X_POST, id, false, true);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_item(int i) {
    item_t* x = &s_items[i];
    hupu_img_drop_pending();
    s_msg[0] = 0;
    if (s_seg == SEG_FOLLOWING && !x->seen) {
        x->seen = true;
        if (!follow_job_running()) tomato_x_mark_seen(x->id);
        else if (s_seen_pending_count < SEEN_PENDING_MAX)
            snprintf(s_seen_pending[s_seen_pending_count++], sizeof(s_seen_pending[0]), "%s", x->id);
    }
    if (post_cached(x->id)) return enter_post(x->id);
    snprintf(s_wait_id, sizeof(s_wait_id), "%s", x->id);
    s_wait = true;
    start_job(TOMATO_X_POST, x->id, false, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t back_to_list(void) {
    tr_wallsave_close(&s_ws);
    hupu_img_drop_pending();
    drop_job(TOMATO_X_POST);
    drop_job(TOMATO_X_COMMENTS);
    free_post();
    s_view = V_LIST;
    s_wait = false;
    s_msg[0] = 0;
    load_list();
    return APP_REDRAW_PAGE;
}

// 向服务器要断点后面更早的一批，拉到后翻到下一页。/ Ask the server for the older batch after the break and turn
// the page once it lands.
static app_redraw_t pull_older(void) {
    if (job_running(TOMATO_X_FEED)) return APP_REDRAW_NONE;
    tomato_x_req_t req = {.job = TOMATO_X_FEED, .following = s_seg == SEG_FOLLOWING, .older = true, .thumbs_count = THUMB_FIRST};
    snprintf(req.id, sizeof(req.id), "%s", s_items[s_vis - 1].id);
    s_turn_after = true;
    s_msg[0] = 0;
    queue_req(&req);
    return APP_REDRAW_PAGE;
}

// 往后拉的一批写好了：等着翻页的就翻过去。/ The older batch is written: turn the page if a turn was waiting.
static void after_older(void) {
    if (s_turn_after && s_list_page + 1 < list_pages()) {
        s_list_page++;
        s_thumbs_page = -1;
        hupu_img_drop_pending();
    }
    s_turn_after = false;
}

// 翻页一条线：正文 → 图片 → 评论，往回同理。/ Paging runs one line: text, then images, then comments, and back.
static app_redraw_t turn(int dir) {
    tr_wallsave_close(&s_ws);
    if (s_view == V_LIST) {
        const int next = s_list_page + dir;
        if (dir > 0 && next >= list_pages() && more_left()) return pull_older();
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
        if (job_running(TOMATO_X_POST) && ui_rect_hit(DIALOG_RECT, x, y)) return APP_REDRAW_NONE;
        drop_job(TOMATO_X_POST);
        s_wait = false;
        s_msg[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (tr_nav_touch(ctx, x, y, TR_TAB_X)) return APP_REDRAW_NONE;
    if (!s_has_cfg) {
        if (ui_rect_hit(setup_rect(), x, y)) {
            app_tr_settings_request_source(TR_TAB_X);
            ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
        }
        const int seg = tr_segment_hit(TR_SEGMENT_RECT, 2, x, y);
        return seg >= 0 && seg != (int)s_seg ? show_seg((seg_t)seg) : APP_REDRAW_NONE;
    }
    if (ui_rect_hit(REFRESH_RECT, x, y)) {
        // 点了取消就别再自动补缩略图，等下次刷新。/ After a cancel, stop filling thumbnails until the next refresh.
        if (list_job_running()) { drop_job(TOMATO_X_FEED); s_lazy_ok = false; return APP_REDRAW_PAGE; }
        if (!tomato_sync_busy() || tomato_sync_preemptible()) return refresh();
    }
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
    if (y < LIST_TOP || y >= LIST_BOTTOM) return APP_REDRAW_NONE;
    // 列表区像书页一样分左中右：左右各三成翻页，中间打开点到的那条。/ The list splits like a book page: the outer 30%
    // on each side turns the page, the middle opens the post under the finger.
    if (x < TR_W * 3 / 10) return turn(-1);
    if (x >= TR_W * 7 / 10) return turn(1);
    int cy = LIST_TOP;
    for (int i = s_page_first[s_list_page]; i < s_page_first[s_list_page + 1]; i++) {
        if (y < cy + s_items[i].height) return open_item(i);
        cy += s_items[i].height;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t tap_image(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    char path[96];
    image_path(s_image, path, sizeof(path));
    if (tr_wallsave_tap(&s_ws, x, y, path, "X", s_user, s_image + 1, ctx->now_ms)) return APP_REDRAW_PAGE;
    if (y >= TR_H - IMAGE_BAND) {
        tr_wallsave_close(&s_ws);
        if (x < TR_W / 3) return show_body(0);
        if (x >= TR_W * 2 / 3) return show_comments();
        return APP_REDRAW_NONE;
    }
    if (s_ws.shown && y >= TR_WALLSAVE_TOP) return APP_REDRAW_NONE;   // 工具条空白处 / Blank toolbar area
    if (x < TR_W * 3 / 10) return turn(-1);
    if (x >= TR_W * 7 / 10) return turn(1);
    // 这张没下好：点中间重下缺的图，下载中点了不管。/ This image is missing: a tap in the middle fetches the
    // missing images again, and does nothing while a download runs.
    if (!exists(path)) {
        if (job_running(TOMATO_X_POST)) return APP_REDRAW_NONE;
        s_msg[0] = 0;
        start_job(TOMATO_X_POST, s_id, false, true);
        return APP_REDRAW_PAGE;
    }
    // 点中间开关存为壁纸工具条。/ A tap in the middle toggles the save-as-wallpaper toolbar.
    if (s_ws.shown) tr_wallsave_close(&s_ws);
    else tr_wallsave_open(&s_ws);
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap_text(uint16_t x, uint16_t y) {
    if (y < TR_S(140) && x < 420) return back_to_list();
    const int seg = tr_segment_hit(POST_SEG, 3, x, y);
    if (seg == 0) return s_tab == T_BODY ? APP_REDRAW_NONE : show_body(0);
    if (seg == 1) return show_image(0);
    if (seg == 2) return s_tab == T_COMMENTS ? APP_REDRAW_NONE : show_comments();
    const int pager = tr_pager_hit(TEXT_PAGER_Y, x, y);
    if (pager) return turn(pager);
    // 正文最后一页的小图：从那张图的第一页开始看。/ The last page's small images open at that image's first page.
    if (s_tab == T_BODY && s_page + 1 == s_page_count && y >= s_tail_y && y < s_tail_y + POST_SQUARE) {
        const int k = (x - TR_MARGIN) / (POST_SQUARE + STRIP_GAP);
        if (x >= TR_MARGIN && k < tail_count()) return show_image(s_pics > 0 ? s_starts[k] : 0);
    }
    if (y < BODY_TOP || y >= BODY_BOTTOM) return APP_REDRAW_NONE;
    if (x < TR_W * 3 / 10) return turn(-1);
    if (x >= TR_W * 7 / 10) return turn(1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (!s_items) {
        if (ev->type == UI_GESTURE_TAP) tr_nav_touch(ctx, ev->x0, ev->y0, TR_TAB_X);
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
    if (s_wait) { drop_job(TOMATO_X_POST); s_wait = false; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

/* ---- 后台进度 / Background progress ---- */

static void queue_visible_images(void) {
    char path[96];
    if (s_view == V_LIST) {
        for (int i = s_page_first[s_list_page]; i < s_page_first[s_list_page + 1]; i++) {
            const item_t* x = &s_items[i];
            const int media = x->media, w = x->mw, h = x->mh;
            if (media == 1) {
                tomato_x_img_path(x->id, 0, path, sizeof(path));
                if (exists(path)) hupu_img_get(path, w, h);
            }
            for (int k = 0; media == 2 && k < x->pics && k < TOMATO_WB_LIST_SQUARES; k++) {
                tomato_x_square_path(x->id, k + 1, path, sizeof(path));
                if (exists(path)) hupu_img_get(path, STRIP_W, h);
            }
        }
    } else if (s_view == V_TEXT && s_post_loaded && s_tab == T_BODY && s_page + 1 == s_page_count) {
        for (int k = 0; k < tail_count(); k++) {
            tail_path(k, path, sizeof(path));
            if (exists(path)) hupu_img_get(path, POST_SQUARE, POST_SQUARE);
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

// 读服务器的正在关注开关；关了而正停在这一段，就回到为你推荐。返回是否换了段。/ Read the server's Following
// switch; when it is off while that segment shows, go back to For You. Returns whether the segment changed.
static bool sync_following(void) {
    s_following_on = tomato_x_following_on();
    if (s_following_on || s_seg != SEG_FOLLOWING) return false;
    snprintf(s_msg, sizeof(s_msg), "服务器关了正在关注，已回到为你推荐");
    s_seg = SEG_FORYOU;
    s_list_page = 0;
    load_list();
    return true;
}

static app_redraw_t job_done(bool ok) {
    switch (s_job.job) {
    case TOMATO_X_FEED:
        if (sync_following() && s_view == V_LIST) {
            if (ok) queue_visible_images();
            return APP_REDRAW_PAGE;
        }
        if (s_view == V_LIST && s_job.following == (s_seg == SEG_FOLLOWING)) {
            load_list();
            if (s_job.older) after_older();
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_X_POST:
        if (s_wait && !strcmp(s_job.id, s_wait_id)) {
            // 失败：等待框里写原因；成功：进入正文。/ Failure: the dialog shows why; success: open the text.
            if (ok && enter_post(s_wait_id) == APP_REDRAW_PAGE) return APP_REDRAW_PAGE;
            if (ok) snprintf(s_msg, sizeof(s_msg), "推文存不下来，检查 TF 卡");
            return APP_REDRAW_PAGE;
        }
        if (s_view != V_LIST && s_post_loaded && !strcmp(s_job.id, s_id)) {
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_X_COMMENTS:
        if (s_post_loaded && !strcmp(s_job.id, s_id)) {
            const int64_t now = esp_timer_get_time() / 1000;
            // 记下原来的提示：还在等、字没变就不重画，免得每 3 秒整页刷一次。/ Remember the old hint: while still
            // waiting with unchanged text, skip the redraw instead of refreshing the page every 3 s.
            char before[sizeof(s_comment_msg)];
            snprintf(before, sizeof(before), "%s", s_comment_msg);
            if (ok) {
                s_reply_wait = false;
                load_comments();
            } else if (s_status.error == ESP_ERR_NOT_FINISHED) {
                // 服务器在后台取：过几秒再问，最多等 3 分钟。/ The server fetches in the background: ask again in a few
                // seconds, for at most 3 minutes.
                if (!s_reply_wait) s_reply_wait_start = now;
                s_reply_wait = now - s_reply_wait_start < REPLY_GIVE_UP_MS;
                s_reply_asked = now;
                snprintf(s_comment_msg, sizeof(s_comment_msg), "%s", s_reply_wait ?
                         "服务器正在取回复…（第一次要去 X 打开这条，约半分钟）" : "服务器取回复太久，稍后再点进来");
            } else {
                s_reply_wait = false;
                snprintf(s_comment_msg, sizeof(s_comment_msg), "%s", s_msg[0] ? s_msg : "回复没取到");
            }
            s_msg[0] = 0;
            // 取评论会顶掉同一条还在下的图：评论到了再把缺的图补上。/ Fetching comments preempts this post's image
            // download; once comments arrive, fill in the missing images again.
            // 还在等服务器取回复时先不补图，否则补图和问回复轮流进行。/ While the server is still fetching replies, do
            // not refill images, or image refills and reply polls alternate.
            if (!s_reply_wait && post_missing_images() && !s_has_pending) start_job(TOMATO_X_POST, s_id, false, true);
            if (s_reply_wait && !strcmp(before, s_comment_msg)) return APP_REDRAW_NONE;
            return s_view == V_TEXT && s_tab == T_COMMENTS ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_X_STATE:
    case TOMATO_X_TEST:
        sync_following();
        return s_view == V_LIST ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

// 这条的列表小图都在卡上（没有图的也算）。/ Whether this post's list images are all on the card (none counts too).
static bool thumbs_present(const item_t* x) {
    char path[96];
    if (x->images <= 0) return true;
    if (x->pics >= 2) {
        for (int k = 1; k <= x->pics && k <= TOMATO_WB_LIST_SQUARES; k++) {
            tomato_x_square_path(x->id, k, path, sizeof(path));
            if (!exists(path)) return false;
        }
        return true;
    }
    tomato_x_img_path(x->id, 0, path, sizeof(path));
    return exists(path);
}

// 列表停在一页上：这页和下一页缺缩略图就在后台补（别的页的任务在跑时不抢）。/ The list rests on a page: fill missing
// thumbnails for it and the next page in the background (never taking the worker from another page's job).
static app_redraw_t fill_thumbs(void) {
    if (!s_lazy_ok || s_view != V_LIST || s_has_pending || tomato_sync_busy() || s_thumbs_tried == s_list_page ||
        s_list_page >= s_list_page_count)
        return APP_REDRAW_NONE;
    s_thumbs_tried = s_list_page;
    const int last = s_list_page + THUMB_PAGES < s_list_page_count ? s_list_page + THUMB_PAGES : s_list_page_count;
    const int a = s_page_first[s_list_page], b = s_page_first[last];
    for (int i = a; i < b; i++) {
        if (thumbs_present(&s_items[i])) continue;
        const tomato_x_req_t req = {.job = TOMATO_X_FEED, .following = s_seg == SEG_FOLLOWING, .thumbs_only = true, .thumbs_from = (uint16_t)a,
                         .thumbs_count = (uint16_t)(b - a)};
        return start_req(&req) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
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
        const tomato_x_req_t r = s_pending;
        start_req(&r);
        return APP_REDRAW_PAGE;
    }
    // 排着队时又有浏览任务占了后台（别的页留下的）：让它让路。/ While queued, a browsing job from elsewhere took the
    // worker: make it yield.
    if (s_has_pending && !s_job_active && tomato_sync_preemptible()) tomato_sync_cancel();
    // 停在回复页、服务器还在取：后台空闲时再问一次（每次请求马上返回，不占着后台）。/ On the replies tab while the
    // server still fetches: ask again when the worker is idle (each request returns at once and never holds it).
    if (s_reply_wait && !s_job_active && !s_has_pending && !tomato_sync_busy() && s_post_loaded &&
        s_view == V_TEXT && s_tab == T_COMMENTS && esp_timer_get_time() / 1000 - s_reply_asked >= REPLY_ASK_MS) {
        start_job(TOMATO_X_COMMENTS, s_id, false, false);
        return APP_REDRAW_NONE;
    }
    if (!s_job_active) return fill_thumbs();
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
        if (s_job.job == TOMATO_X_FEED) {
            // 留在原来那页，回第一页用双击分段；往后拉的一批等着翻页就翻过去。/ Stay on the current page (a double tap
            // on the segment goes to the first); an older batch turns the page if a turn was waiting.
            if (s_view == V_LIST && s_job.following == (s_seg == SEG_FOLLOWING)) {
                load_list();
                if (s_job.older) after_older();
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (s_job.job == TOMATO_X_POST && s_wait && !strcmp(s_job.id, s_wait_id)) return enter_post(s_wait_id);
        return APP_REDRAW_NONE;
    }
    // 正在看的图刚下好：重画（会排队缩放）。/ The image on screen just arrived: redraw, which queues its scaling.
    if (busy && s_view == V_IMAGE && s_image_missing) {
        char path[96];
        image_path(s_image, path, sizeof(path));
        if (exists(path)) return APP_REDRAW_PAGE;
    }
    // 列表缩略图按列表顺序下：本页最后一条也轮过了，就先解码显示，不等后面一两百张；解码完 hupu_img_tick 刷一次。
    // List thumbnails download in list order: once this page's last item has had its turn, decode and show them
    // without waiting for the hundreds after it; hupu_img_tick redraws once decoding is done.
    if (busy && s_view == V_LIST && s_job.job == TOMATO_X_FEED && s_job.following == (s_seg == SEG_FOLLOWING) &&
        s_thumbs_page != s_list_page && s_status.current >= 1 + s_page_first[s_list_page + 1]) {
        s_thumbs_page = s_list_page;
        queue_visible_images();
    }
    if (busy) return APP_REDRAW_NONE;
    s_job_active = false;
    if (s_job.job == TOMATO_X_FEED && s_job.following) flush_seen();
    const bool ok = s_status.phase != TOMATO_FAILED;
    if (s_job.job == TOMATO_X_FEED) {
        // 刷新成功才开始按页补缩略图；联网失败就停，免得每翻一页都等一次；被别的任务打断的那页回来再补。
        // Paged thumbnail fills start only after a successful refresh and stop on a network failure, so not every page
        // turn waits on it; a page interrupted by another job fills again on return.
        if (ok && !s_job.thumbs_only) { s_lazy_ok = true; s_thumbs_tried = -1; }
        else if (!ok && s_status.error != ESP_ERR_INVALID_STATE) s_lazy_ok = false;
        else if (!ok) s_thumbs_tried = -1;
    }
    if (!ok && s_job.job == TOMATO_X_FEED && s_job.older) {
        // 拉不到更早的：这次先接卡上缓存；取消了就只是不翻。/ The older pull failed: show the card's cache for now;
        // a cancel just skips the turn.
        if (s_status.error != ESP_ERR_INVALID_STATE) {
            s_gap_skip = true;
            snprintf(s_msg, sizeof(s_msg), "拉不到更早的，下面接卡上缓存");
            if (s_view == V_LIST) { load_list(); after_older(); }
            return APP_REDRAW_PAGE;
        }
        s_turn_after = false;
    }
    if (!ok) {
        if (s_status.error == ESP_ERR_INVALID_STATE) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    } else if (s_job.job == TOMATO_X_POST && s_status.message[0]) {
        snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    }
    if (s_has_pending) {
        if (ok) job_done(ok);
        s_has_pending = false;
        const tomato_x_req_t next = s_pending;
        start_req(&next);
        return APP_REDRAW_PAGE;
    }
    const app_redraw_t r = job_done(ok);
    return !ok && r == APP_REDRAW_NONE && s_view == V_LIST ? APP_REDRAW_PAGE : r;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    hupu_img_drop_pending();
    tomato_x_config_t cfg;
    s_has_cfg = tomato_x_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    if (!alloc_all()) return;
    s_msg[0] = 0;
    s_view = V_LIST;
    s_wait = false;
    s_lazy_ok = false;
    s_gap_skip = false;
    s_turn_after = false;
    s_thumbs_tried = -1;
    s_list_page = 0;
    // 离开期间的任务：不是自己的、或已经结束，都不再跟踪。/ A job from before: stop tracking it unless ours still runs.
    if (s_job_active && !ours_running()) s_job_active = false;
    if (!follow_job_running()) flush_seen();
    s_following_on = tomato_x_following_on();
    if (!s_following_on) s_seg = SEG_FORYOU;
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

const app_desc_t app_tr_x = {
    .title = "X",
    .detail = "为你推荐、正在关注（经自建 x 服务器，只读）",
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
