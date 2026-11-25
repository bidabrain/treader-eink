/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 小红书标签页（只读）：顶部分段 关注 / 推荐；列表照微博标签一行一篇，每页两篇（左边大封面，右边头像、作者和时间、
 * 标题、赞和评论），左右三成点按翻页；点开笔记先一屏一张看图，再看正文和评论。数据来自用户自建的 rednote
 * 服务器，由 components/tomato 的后台任务取到 TF 卡 rednote/，本页只读缓存；缩略图和大图的缩放抖动交给 hupu_img。
 *
 * English: the Xiaohongshu tab (read only): Following / Recommended segments; like the Weibo tab the list shows one
 * note per row, two per page (a large cover on the left; avatar, author and time, title, likes and comments on the
 * right), with the outer 30% on each side turning pages; a note opens on its images, one per
 * screen, then text and comments. Data comes from the user's own rednote server; the components/tomato task stores it
 * under rednote/ on the TF card and this page reads only that cache; hupu_img scales and dithers thumbnails and images.
 *
 * 冻结：只在用户点“刷新”、切到没缓存的推荐、或打开没缓存的笔记时联网，进页不自动刷新。推荐的“刷新”才让服务器去读
 * 小红书首页，次数和间隔由服务器面板控制。没设置服务器时只显示“去设置”。大块数据进页分配、离页释放。不点赞、不评论。
 * Frozen: network only on Refresh, switching to recommendations without a cache, or opening an uncached note; entering
 * never refreshes. Only the recommendations Refresh makes the server read Xiaohongshu; its quota and spacing belong to
 * the server panel. Without a server the page only offers "Set up". Large buffers are allocated on entry and freed on
 * exit. No likes, no comments.
 * Treader 修订（1.2.7，用户要求）：列表从两列封面网格改成和微博、X 一样的一行一篇（每页两篇、突出封面），好用同样的左右点按翻页；刷新
 * 拉到新列表后停在原来那页，回第一页用双击分段（微博、X 同样）。
 * Treader revision (1.2.7, user request): the list changed from a two-column cover grid to one note per row like
 * Weibo and X (two per page, cover first), so the same left / right tap zones turn pages; a refresh keeps the current page and a double tap on the
 * segment goes to the first (Weibo and X likewise).
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

// 长度都随显示大小放大（页边距和细线不放大）；列表底边跟着标签栏走，笔记和评论的底边跟着屏幕底走。
// Lengths scale with the display size (margins and hairlines do not); the list bottom follows the tab bar, the note
// and comment bottoms follow the screen bottom.

// 列表 / Lists
#define LIST_MAX TOMATO_LIST_CAP
#define LIST_TOP TR_S(288)
#define PAGER_Y (TR_TABS_Y - TR_S(58))
// 一条一张卡片（照微博标签的一行一条），每页固定两篇：左边大封面占满一行的高，右边头像、作者和时间、标题、赞和评论。
// One card per note (one per row like the Weibo tab), two per page: a large cover filling the row height on the left;
// avatar, author and time, title, likes and comments on the right.
#define LIST_BOTTOM (TR_TABS_Y - TR_S(64))
#define PER_PAGE 2
#define ROW_H ((LIST_BOTTOM - LIST_TOP) / PER_PAGE)
#define AVATAR TR_S(60)
#define CARD_PX TR_S(30)
#define CARD_LH TR_S(42)
#define CARD_LINES_MAX 6
#define COVER_MAX_W (CW * 48 / 100)   // 横图封面最宽到这里，右边文字才放得下 / Landscape covers stop here so the text fits
#define COVER_GAP TR_S(22)
#define FOOT_H TR_S(44)
#define CARD_GAP TR_S(16)
#define HEAD_H TR_S(72)     // 头像和作者两行 / Avatar with the author's two lines
#define STATUS_Y TR_S(240)
#define REFRESH_RECT (EpdRect){TR_W - TR_MARGIN - TR_S(240), TR_S(70), TR_S(240), TR_S(80)}
#define EMPTY_PX TR_S(30)
#define EMPTY_LH TR_S(46)
#define EMPTY_LINES 8
#define SETUP_TEXT "还没连接 rednote 服务器。先在电脑或 NAS 上部署 rednote（见项目 rednote/ 目录），" \
                   "再到“设置 › 内容来源”填服务器地址和设备令牌。"
// 对话框两边留白随放大收窄（正常 92，特大到页边距），一行的提示才放得下。/ The dialog's side inset narrows as the
// size grows (92 at normal, down to the margin at the largest) so its one-line hints still fit.
#define DIALOG_X (92 - (ui_scale_pct - 100) * 52 / 30)
#define DIALOG_RECT (EpdRect){DIALOG_X, TR_S(500), TR_W - 2 * DIALOG_X, TR_S(220)}

// 笔记 / Note
#define NOTE_SEG (EpdRect){TR_MARGIN, TR_S(150), CW, TR_S(64)}
#define TITLE_Y TR_S(240)
#define TITLE_PX TR_S(38)
#define TITLE_LH TR_S(50)
#define BODY_TOP TR_S(240)
#define BODY_BOTTOM (TR_H - TR_S(116))
#define BODY_PX TR_S(32)
#define BODY_LH TR_S(52)
#define TEXT_PAGER_Y (TR_H - TR_S(92))
#define LINES_MAX 1600
#define PAGES_MAX 200
#define COMMENTS_MAX 60
// 评论最后一页末尾的“加载更多评论”按钮。/ The "load more comments" button at the end of the last comment page.
#define MORE_BTN_W TR_S(400)
#define MORE_BTN_H TR_S(76)
#define MORE_BLOCK_H TR_S(120)
#define COMMENT_LINES 5
#define COMMENT_PX TR_S(30)
#define COMMENT_LH TR_S(44)
#define IMAGE_BAND TR_S(96)     // 看图底部的白条 / White strip under the image

typedef enum { V_LIST, V_IMAGE, V_TEXT } view_t;
typedef enum { SEG_FOLLOW, SEG_REC } seg_t;
typedef enum { T_BODY, T_COMMENTS } text_tab_t;

typedef struct {
    char id[28];
    char title[160];
    char author[48];
    char likes[16];
    char comments[16];
    double time;            // 秒或毫秒，推荐没有 / Seconds or milliseconds; none for recommendations
    float ratio;            // 封面高宽比 / Cover height/width
    int16_t images;
    bool seen;
    bool next;            // 服务器在这条后面可能还有更早的 / The server may hold older entries after this one
    // 排版结果：标题行数、封面大小、卡片高。/ Layout: title lines, cover size, card height.
    int8_t lines;
    int16_t mw, mh;
    int16_t height;
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
// 刷新只先下前几页的缩略图，翻到的页再补；进页后成功刷新过才自动补，免得没网时一翻页就去连 Wi-Fi。
// A refresh fetches thumbnails for the first pages only and the pages being viewed fill in later; that happens only
// after a successful refresh in this visit, so paging offline never tries to join Wi-Fi.
#define THUMB_FIRST 6
#define THUMB_PAGES 2
static bool s_lazy_ok;
static int s_thumbs_tried = -1;   // 这一页已经补过一次（失败了也不再反复试）/ This page was already filled once
// 已经提前显示过缩略图的列表页（-1 是还没有）。/ The list page whose thumbnails were already shown early (-1: none).
static int s_thumbs_page = -1;
static tomato_red_req_t s_pending;
static bool s_has_pending;

// 列表 / List
static item_t* s_items;
static int s_item_count;
static int s_list_page;
static uint16_t s_page_first[LIST_MAX + 1];   // 每页第一条 / First item of each page
static int s_list_page_count;
// 往后翻接着拉服务器（设置里的开关）。/ Keep pulling from the server (the Settings switch).
static bool s_more_on;     // 开着且这一段支持 / On, and this segment supports it
static bool s_gap_skip;    // 这次拉不到，先接卡上缓存 / A pull failed; show the card's cache for now
static bool s_turn_after;  // 拉到以后自动翻到下一页 / Turn to the next page once the pull lands
static int s_vis;          // 显示到第几条（第一个断点为止）/ Entries shown (up to the first break)
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
static bool s_comments_more;         // 还能加载更多评论 / More comments can be loaded
static int s_more_y;                 // 按钮所在块的顶（最后一页）/ Top of the button block (last page)
// 等服务器拉更多评论（等待框）；s_more_failed 时框里写原因。/ Waiting for the server to fetch more comments (dialog);
// with s_more_failed it shows why.
static bool s_more_wait, s_more_failed;
static int s_title_lines;
static text_tab_t s_tab;
static int s_page;
static int s_image;                  // 从 0 开始 / 0-based
static tr_wallsave_t s_ws;             // 看图时的存为壁纸工具条 / Save-as-wallpaper toolbar
static bool s_note_rec;              // 打开的这篇来自推荐 / The open note came from recommendations
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
static bool queue_req(const tomato_red_req_t* r) {
    const tomato_red_req_t req = *r;
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

static bool start_job(tomato_red_job_t job, const char* id, bool rec, bool refresh, bool images_only) {
    tomato_red_req_t req = {.job = job, .rec = rec, .refresh = refresh, .images_only = images_only};
    if (id) snprintf(req.id, sizeof(req.id), "%s", id);
    return queue_req(&req);
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

static const char* card_title(const item_t* x) { return x->title[0] ? x->title : "（无标题）"; }

// 封面按高宽比放进 wmax×hmax。/ Fit the cover into wmax x hmax by its aspect ratio.
static void cover_fit(const item_t* x, int wmax, int hmax, int* w, int* h) {
    *h = hmax;
    *w = (int)(hmax / x->ratio);
    if (*w > wmax) { *w = wmax; *h = (int)(wmax * x->ratio); }
}

// 右边文字的起点和宽度（跟着封面宽）。/ Left edge and width of the text on the right (following the cover width).
static int text_x(const item_t* x) { return TR_MARGIN + x->mw + COVER_GAP; }
static int text_w(const item_t* x) { return CW - x->mw - COVER_GAP; }

// 封面尽量占满一行的高，标题用完封面旁边剩下的行。/ The cover fills as much of the row height as it can and the title
// takes the lines left beside it.
static void card_layout(item_t* x) {
    int w, h;
    cover_fit(x, COVER_MAX_W, ROW_H - CARD_GAP, &w, &h);
    x->mw = (int16_t)w;
    x->mh = (int16_t)h;
    int room = (ROW_H - CARD_GAP - HEAD_H - TR_S(10) - FOOT_H) / CARD_LH;
    if (room > CARD_LINES_MAX) room = CARD_LINES_MAX;
    if (room < 1) room = 1;
    x->lines = (int8_t)count_lines(card_title(x), CARD_PX, text_w(x), room);
    x->height = (int16_t)ROW_H;
}

// 开关开着时列表只显示到第一个断点（服务器在那条后面可能还有），翻过去再拉。/ With the switch on, the list shows
// up to the first break (the server may hold more after it), pulled when paged past.
static void set_visible(void) {
    s_more_on = s_seg == SEG_FOLLOW && tomato_feed_more_on();
    s_vis = s_item_count;
    if (!s_more_on || s_gap_skip) return;
    for (int i = 0; i < s_item_count; i++)
        if (s_items[i].next) { s_vis = i + 1; return; }
}

// 翻过最后一页还能向服务器要。/ Paging past the last page can still ask the server.
static bool more_left(void) { return s_more_on && !s_gap_skip && s_vis > 0 && s_items[s_vis - 1].next; }

static void layout_list(void) {
    s_list_page_count = (s_vis + PER_PAGE - 1) / PER_PAGE;
    for (int i = 0; i < s_vis; i++) card_layout(&s_items[i]);
    for (int p = 0; p <= s_list_page_count; p++)
        s_page_first[p] = (uint16_t)(p * PER_PAGE < s_vis ? p * PER_PAGE : s_vis);
    // 空列表也把第一页的结尾写成 0，点空白处才不会打开看不见的旧笔记。/ An empty list also ends page one at 0, so a
    // tap on blank space never opens an invisible old note.
    if (s_list_page_count == 0) s_page_first[1] = 0;
}

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
        str(x->comments, sizeof(x->comments), it, "comments");
        const cJSON* tv = cJSON_GetObjectItemCaseSensitive(it, "time");
        x->time = cJSON_IsNumber(tv) ? tv->valuedouble : 0;
        // 老缓存没有比例：按 4:3 竖图。/ Older caches have no ratio: assume a 4:3 portrait.
        const cJSON* rv = cJSON_GetObjectItemCaseSensitive(it, "ratio");
        x->ratio = cJSON_IsNumber(rv) && rv->valuedouble >= 0.5 && rv->valuedouble <= 2.0 ? (float)rv->valuedouble
                                                                                            : 4.0f / 3;
        x->images = (int16_t)num(it, "images");
        x->seen = s_seg == SEG_REC || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "seen"));
        x->next = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(it, "next"));
        s_item_count++;
    }
    cJSON_Delete(root);
    set_visible();
    layout_list();
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
    return page ? BODY_TOP : TITLE_Y + s_title_lines * TITLE_LH + TR_S(10) + TR_S(44) + TR_S(18);
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

static int comment_height(const comment_t* c) { return TR_S(40) + comment_lines(c) * COMMENT_LH + TR_S(32); }

static void layout_comments(void) {
    int pages = 0, y = BODY_BOTTOM;
    for (int i = 0; i < s_comment_count; i++) {
        const int h = comment_height(&s_comments[i]);
        if (y + h > BODY_BOTTOM && y != BODY_TOP) { s_comment_pages[pages++] = (uint8_t)i; y = BODY_TOP; }
        y += h;
    }
    if (!pages) { s_comment_pages[pages++] = 0; y = BODY_TOP + TR_S(80); }
    // 按钮放不下就单独占一页。/ The button gets a page of its own when it does not fit.
    if (y + MORE_BLOCK_H > BODY_BOTTOM) { s_comment_pages[pages++] = (uint8_t)s_comment_count; y = BODY_TOP; }
    s_more_y = y;
    s_comment_pages[pages] = (uint8_t)s_comment_count;
    s_comment_page_count = pages;
}

static EpdRect more_btn(void) {
    return (EpdRect){(TR_W - MORE_BTN_W) / 2, s_more_y + TR_S(24), MORE_BTN_W, MORE_BTN_H};
}

// 小红书的计数文字（“12”“1.2万”“10+”）换成大约的数。/ Xiaohongshu count text ("12", "1.2万", "10+") as a number.
static long count_value(const char* s) {
    char* end = NULL;
    const double v = strtod(s, &end);
    if (end == s || v < 0) return 0;
    while (*end == ' ') end++;
    return !strncmp(end, "万", strlen("万")) || *end == 'w' || *end == 'W' ? (long)(v * 10000) : (long)v;
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
    const bool more_done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "more_done"));
    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "list")) {
        if (s_comment_count >= COMMENTS_MAX) break;
        comment_t* x = &s_comments[s_comment_count++];
        x->user = keep(c, "u");
        x->text = keep(c, "t");
        x->likes = keep(c, "l");
    }
    cJSON_Delete(root);
    // 按钮在设备上判断（升级前缓存的笔记也能用）：评论总数比显示的多、还没加载过。
    // Decided on the device so notes cached before an upgrade work too: more comments than shown, not loaded yet.
    s_comments_more = !more_done && count_value(s_comment_total) > s_comment_count;
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

// 固定提示最多 8 行，放大后也写得完。/ Fixed hints get up to 8 lines so they still show whole at larger sizes.
static void draw_empty(uint8_t* fb, int y, const char* text) {
    tr_wrap(fb, TR_MARGIN, y, CW, EMPTY_PX, EMPTY_LH, text, EMPTY_LINES);
}

// “去设置”按钮放在说明文字下面（正常档仍在 420）。/ The Set up button sits below the hint (still at 420 at normal).
static EpdRect setup_rect(void) {
    int lines = 0;
    for (const char* p = SETUP_TEXT; *p && lines < EMPTY_LINES; lines++) {
        p += tr_line_bytes(p, EMPTY_PX, CW);
        if (*p == '\n') p++;
    }
    const int below = LIST_TOP + (lines - 1) * EMPTY_LH + EMPTY_PX + TR_S(10);
    return (EpdRect){TR_MARGIN, below > TR_S(420) ? below : TR_S(420), TR_S(240), TR_S(72)};
}

static void draw_header(uint8_t* fb) {
    tr_title(fb, "小红书");
    if (s_has_cfg) {
        const bool busy = job_running(s_seg == SEG_FOLLOW ? TOMATO_RED_FEED : TOMATO_RED_REC);
        if (busy) {
            // 拉取中点一下就取消，下好的留着。/ Tap while fetching to cancel; what arrived stays.
            tr_text(fb, TR_W - TR_MARGIN, TR_S(98), TR_S(30), "拉取中 · 取消", EPD_DRAW_ALIGN_RIGHT, false);
        } else if (!tomato_sync_busy() || tomato_sync_preemptible()) {
            // 别的页在浏览类拉取也能点：start_job 会先请它让路。/ Also tappable while another page's browsing job
            // runs: start_job asks it to make way first.
            const char* label = s_seg == SEG_FOLLOW ? "刷新" : "刷新推荐";
            tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - ttf_text_width_px(TR_S(30), label) - TR_S(26), TR_S(116),
                    false);
            tr_text(fb, TR_W - TR_MARGIN, TR_S(98), TR_S(30), label, EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    static const char* const SEG[2] = {"关注", "推荐"};
    tr_segment(fb, TR_SEGMENT_RECT, SEG, 2, (int)s_seg);
}

// 今天的写时分，别的写月日。/ Today shows hours and minutes; other days show month and day.
static void fmt_time(char* out, size_t cap, double t) {
    out[0] = 0;
    if (t < 1e9) return;
    // 服务器给的是毫秒或秒，都能认。/ The server gives milliseconds or seconds; accept both.
    const time_t when = (time_t)(t > 1e11 ? t / 1000 : t);
    struct tm tm, now_tm;
    localtime_r(&when, &tm);
    const time_t now = time(NULL);
    localtime_r(&now, &now_tm);
    if (now > 1735689600 && tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday)
        snprintf(out, cap, "%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100);
    else snprintf(out, cap, "%02d-%02d", (tm.tm_mon + 1) % 100, tm.tm_mday % 100);
}

// 头像：浅灰实心圆里写作者名字的第一个字（这页只用整页刷新）。/ Avatar: the author's first character on a filled
// light gray circle (this page refreshes whole pages only).
static void draw_avatar(uint8_t* fb, int x, int y, int d, const char* name) {
    const int cx = x + d / 2, cy = y + d / 2;
    epd_fill_circle(cx, cy, d / 2, TR_GRAY_SELECTED, fb);
    char first[8] = "红";
    if (name && name[0]) {
        const unsigned char c = (unsigned char)name[0];
        const size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (strlen(name) >= n) { memcpy(first, name, n); first[n] = 0; }
    }
    const int px = d * 28 / 60;
    tr_text_on(fb, cx, cy - px / 2 - TR_S(3), px, first, EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SELECTED);
}

// 一张卡片：左边封面（铺满裁切，还没到画浅灰圆角块）；右边头像（没看过的右上角黑点）、粗体作者、下一行时间和图数
// （推荐里下好的写“已下载”），标题，赞和评论贴着封面底边。
// One card: the cover on the left (fill-cropped; a light gray block until it arrives); on the right the avatar (a black
// dot at its top-right when unseen), bold author, then time and image count ("downloaded" for cached
// recommendations), the title, and likes and comments level with the cover's bottom.
static void draw_card(uint8_t* fb, const item_t* x, int y) {
    char thumb[96];
    thumb_path(x, thumb, sizeof(thumb));
    const uint8_t* px = exists(thumb) ? hupu_img_get(thumb, x->mw, x->mh) : NULL;
    if (px) hupu_img_draw(fb, TR_MARGIN, y, x->mw, x->mh, px, true);
    else ui_fill_round_rect(fb, (EpdRect){TR_MARGIN, y, x->mw, x->mh}, TR_S(12), TR_GRAY_SURFACE);
    const int tx = text_x(x), tw = text_w(x);
    draw_avatar(fb, tx, y, AVATAR, x->author);
    if (!x->seen) {
        epd_fill_circle(tx + AVATAR - TR_S(6), y + TR_S(6), TR_S(10), UI_GRAY_WHITE, fb);
        epd_fill_circle(tx + AVATAR - TR_S(6), y + TR_S(6), TR_S(7), UI_GRAY_BLACK, fb);
    }
    char line[200], when[16];
    snprintf(line, sizeof(line), "%s", x->author[0] ? x->author : "小红书用户");
    tr_fit(line, TR_S(30), tw - TR_S(78));
    tr_text(fb, tx + TR_S(78), y + TR_S(2), TR_S(30), line, EPD_DRAW_ALIGN_LEFT, true);
    fmt_time(when, sizeof(when), x->time);
    char pics[24] = "";
    if (x->images > 0) snprintf(pics, sizeof(pics), "%d 张图", x->images);
    const char* got = s_seg == SEG_REC && note_cached(x->id) ? "已下载" : "";
    snprintf(line, sizeof(line), "%s%s%s%s%s", when, when[0] && pics[0] ? " · " : "", pics,
             (when[0] || pics[0]) && got[0] ? " · " : "", got);
    tr_fit(line, TR_S(22), tw - TR_S(78));
    tr_text_dim(fb, tx + TR_S(78), y + TR_S(40), TR_S(22), line, EPD_DRAW_ALIGN_LEFT);
    if (x->lines) tr_wrap(fb, tx, y + HEAD_H, tw, CARD_PX, CARD_LH, card_title(x), x->lines);
    snprintf(line, sizeof(line), "赞 %s%s%s", x->likes[0] ? x->likes : "0", x->comments[0] ? " · 评论 " : "",
             x->comments);
    tr_fit(line, TR_S(24), tw);
    int fy = y + x->mh - FOOT_H;
    const int below = y + HEAD_H + x->lines * CARD_LH + TR_S(10);
    if (fy < below) fy = below;
    tr_text_dim(fb, tx, fy + (FOOT_H - TR_S(24)) / 2 - TR_S(2), TR_S(24), line, EPD_DRAW_ALIGN_LEFT);
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
    const bool loading = job_running(s_seg == SEG_FOLLOW ? TOMATO_RED_FEED : TOMATO_RED_REC);
    if (loading) {
        snprintf(line, sizeof(line), "%s", loading_text(s_job.older ? "正在向服务器要更早的…" : s_step_loaded ? "正在下载封面…" :
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
    // 平常的条数用深灰，加载和出错的提示保持黑字。/ The plain count is dark gray; loading and errors stay black.
    if (!loading && !s_msg[0]) tr_text_dim(fb, TR_MARGIN, STATUS_Y, TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
    else tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - TR_S(12), CW, TR_S(26), line, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_item_count) {
        draw_empty(fb, LIST_TOP + TR_S(20), loading ? "加载中，请稍候。" : s_seg == SEG_FOLLOW ?
                   "还没有缓存。联网后点右上角“刷新”，从服务器取关注博主的笔记；之后不联网也能看。" :
                   "还没有推荐。点右上角“刷新推荐”，服务器会去读一次小红书首页（只留图文）。");
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
    const bool failed = !job_running(TOMATO_RED_NOTE) && s_msg[0];
    if (failed) {
        tr_text(fb, cx, r.y + TR_S(40), TR_S(32), "没能打开这篇", EPD_DRAW_ALIGN_CENTER, true);
        tr_wrap(fb, r.x + TR_S(30), r.y + TR_S(96), r.width - TR_S(60), TR_S(26), TR_S(36), s_msg, 2);
        tr_text_dim(fb, cx, r.y + TR_S(168), TR_S(24), "点任意处关闭", EPD_DRAW_ALIGN_CENTER);
        return;
    }
    tr_text(fb, cx, r.y + TR_S(44), TR_S(34), "正在拉取这篇笔记…", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, cx, r.y + TR_S(104), TR_S(26), loading_text("服务器要打开小红书，约 10～20 秒"), EPD_DRAW_ALIGN_CENTER, false);
    tr_text_dim(fb, cx, r.y + TR_S(156), TR_S(26), "点框外取消", EPD_DRAW_ALIGN_CENTER);
}

static void draw_more_wait(uint8_t* fb) {
    const EpdRect r = DIALOG_RECT;
    // Material 3 对话框：白底大圆角、深灰细框。/ Material 3 dialog: white, large radius, thin dark outline.
    epd_fill_rect(r, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, r, TR_S(28), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), TR_S(28) - 1, TR_GRAY_OUTLINE);
    const int cx = r.x + r.width / 2;
    if (s_more_failed) {
        tr_text(fb, cx, r.y + TR_S(40), TR_S(32), "没能加载更多评论", EPD_DRAW_ALIGN_CENTER, true);
        tr_wrap(fb, r.x + TR_S(30), r.y + TR_S(96), r.width - TR_S(60), TR_S(26), TR_S(36), s_msg[0] ? s_msg : "稍后再试", 2);
        tr_text_dim(fb, cx, r.y + TR_S(168), TR_S(24), "点任意处关闭", EPD_DRAW_ALIGN_CENTER);
        return;
    }
    tr_text(fb, cx, r.y + TR_S(44), TR_S(34), "正在加载更多评论…", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, cx, r.y + TR_S(104), TR_S(26), loading_text("服务器要打开小红书往下翻，约 20～60 秒"), EPD_DRAW_ALIGN_CENTER, false);
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
        const bool loading = job_running(TOMATO_RED_NOTE);
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
    tr_text(fb, TR_MARGIN, TR_H - IMAGE_BAND + TR_S(28), TR_S(30), "‹ 返回", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W / 2, TR_H - IMAGE_BAND + TR_S(28), TR_S(30), label, EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, TR_W - TR_MARGIN, TR_H - IMAGE_BAND + TR_S(28), TR_S(30), "正文 ›", EPD_DRAW_ALIGN_RIGHT, false);
    tr_wallsave_draw(fb, &s_ws);
}

static void draw_text_header(uint8_t* fb) {
    char back[80];
    snprintf(back, sizeof(back), "‹ %s", s_author[0] ? s_author : "返回");
    tr_fit(back, TR_S(30), 400);
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(30), back, EPD_DRAW_ALIGN_LEFT, false);
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
        const int my = TITLE_Y + s_title_lines * TITLE_LH + TR_S(10);
        snprintf(meta, sizeof(meta), "%s%s%s%s%s%s", s_author, when, s_likes[0] ? " · 赞 " : "", s_likes,
                 s_comment_total[0] ? " · 评论 " : "", s_comment_total);
        tr_fit(meta, TR_S(24), CW);
        tr_text_dim(fb, TR_MARGIN, my, TR_S(24), meta, EPD_DRAW_ALIGN_LEFT);
        tr_hline(fb, my + TR_S(40), TR_MARGIN, CW, true);
    }
    if (!s_line_count) {
        tr_text_dim(fb, TR_MARGIN, y, TR_S(30), "这篇没有文字。", EPD_DRAW_ALIGN_LEFT);
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

// 能按时深灰描边、粗体黑字；不能按时浅灰底、深灰字。/ A dark outline with bold black text when it can be pressed; a
// light gray fill with dark gray text when it cannot.
static void draw_more_btn(uint8_t* fb) {
    const EpdRect b = more_btn();
    if (s_comments_more) {
        ui_draw_round_rect(fb, b, TR_S(38), TR_GRAY_OUTLINE);
        ui_draw_round_rect(fb, ui_inset_rect(b, 1), TR_S(38) - 1, TR_GRAY_OUTLINE);
        tr_text(fb, TR_W / 2, b.y + (MORE_BTN_H - TR_S(30)) / 2 - TR_S(2), TR_S(30), "加载更多评论", EPD_DRAW_ALIGN_CENTER, true);
    } else {
        ui_fill_round_rect(fb, b, TR_S(38), TR_GRAY_SURFACE);
        tr_text_on(fb, TR_W / 2, b.y + (MORE_BTN_H - TR_S(30)) / 2 - TR_S(2), TR_S(30), "加载更多评论", EPD_DRAW_ALIGN_CENTER, false,
                   TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    }
}

static void draw_comments(uint8_t* fb) {
    if (s_page + 1 == s_comment_page_count) draw_more_btn(fb);
    if (!s_comment_count) {
        tr_text_dim(fb, TR_MARGIN, BODY_TOP + TR_S(20), TR_S(30), "服务器没有存这篇的评论", EPD_DRAW_ALIGN_LEFT);
        return;
    }
    int y = BODY_TOP;
    char line[160];
    for (int i = s_comment_pages[s_page]; i < s_comment_pages[s_page + 1]; i++) {
        const comment_t* c = &s_comments[i];
        snprintf(line, sizeof(line), "%s", c->user[0] ? c->user : "小红书用户");
        tr_fit(line, TR_S(26), CW - TR_S(160));
        tr_text(fb, TR_MARGIN, y, TR_S(26), line, EPD_DRAW_ALIGN_LEFT, true);
        if (c->likes[0] && strcmp(c->likes, "0")) {
            snprintf(line, sizeof(line), "赞 %s", c->likes);
            tr_badge(fb, TR_W - TR_MARGIN, y - TR_S(4), line, false, TR_S(22));
        }
        tr_wrap(fb, TR_MARGIN, y + TR_S(40), CW, COMMENT_PX, COMMENT_LH, c->text[0] ? c->text : "（空）", COMMENT_LINES);
        y += comment_height(c);
        tr_hline(fb, y - TR_S(16), TR_MARGIN, CW, true);
    }
}

static void draw_text(uint8_t* fb) {
    draw_text_header(fb);
    if (s_tab == T_BODY) draw_body(fb);
    else draw_comments(fb);
    tr_pager(fb, TEXT_PAGER_Y, s_page, text_pages());
    if (s_more_wait) draw_more_wait(fb);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (!s_items) {
        tr_status_bar(fb);
        tr_title(fb, "小红书");
        draw_empty(fb, TR_S(240), "内存不足，打不开小红书页。");
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
    s_gap_skip = false;
    s_turn_after = false;
    s_seg = seg;
    s_thumbs_tried = -1;
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
    s_gap_skip = false;
    s_turn_after = false;
    if (s_seg == SEG_FOLLOW) {
        const tomato_red_req_t req = {.job = TOMATO_RED_FEED, .thumbs_count = THUMB_FIRST};
        queue_req(&req);
    }
    else start_job(TOMATO_RED_REC, NULL, false, true, false);
    return APP_REDRAW_PAGE;
}

// 笔记已经在卡上：直接看；有图没下好就在后台补。/ A cached note opens at once; missing images fill in behind it.
static app_redraw_t enter_note(const char* id, bool rec) {
    if (!load_note(id)) return APP_REDRAW_NONE;
    s_note_rec = rec;
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
    s_more_wait = false;
    s_msg[0] = 0;
    load_list();
    return APP_REDRAW_PAGE;
}

// 让服务器现拉这篇更多的评论，弹等待框。/ Have the server fetch more of this note's comments, behind a dialog.
static app_redraw_t load_more_comments(void) {
    tomato_red_req_t req = {.job = TOMATO_RED_NOTE, .more_comments = true};
    snprintf(req.id, sizeof(req.id), "%s", s_id);
    s_more_wait = true;
    s_more_failed = false;
    s_msg[0] = 0;
    if (tomato_sync_busy()) {
        const bool yields = ours_running() || tomato_sync_preemptible();
        if (yields) tomato_sync_cancel();
        s_pending = req;
        s_has_pending = true;
        if (!yields) snprintf(s_msg, sizeof(s_msg), "后台正在下载或同步，完成后自动开始");
    } else {
        s_has_pending = false;
        if (!start_req(&req)) s_more_failed = true;
    }
    return APP_REDRAW_PAGE;
}

// 关掉等待框；还在排队或在跑就取消（服务器那边照样会拉完存好）。/ Close the dialog; cancel if still queued or
// running (the server still finishes and keeps the comments).
static app_redraw_t close_more(void) {
    if (s_has_pending && s_pending.more_comments) s_has_pending = false;
    else if (s_job.more_comments && ours_running()) tomato_sync_cancel();
    s_more_wait = false;
    s_msg[0] = 0;
    return APP_REDRAW_PAGE;
}

// 向服务器要断点后面更早的一批，拉到后翻到下一页。/ Ask the server for the older batch after the break and turn
// the page once it lands.
static app_redraw_t pull_older(void) {
    if (job_running(TOMATO_RED_FEED)) return APP_REDRAW_NONE;
    tomato_red_req_t req = {.job = TOMATO_RED_FEED, .older = true, .thumbs_count = THUMB_FIRST};
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

static app_redraw_t turn(int dir) {
    // 工具条或提示框开着：就算没翻成也要重画把它擦掉。/ With the toolbar or notice up, redraw even when the page
    // does not change, so it is wiped.
    const app_redraw_t idle = s_ws.shown || s_ws.title[0] ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    tr_wallsave_close(&s_ws);
    int* page;
    int pages;
    if (s_view == V_LIST) {
        if (dir > 0 && s_list_page + 1 >= list_pages() && more_left()) return pull_older();
        page = &s_list_page;
        pages = list_pages();
    }
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
        if (ui_rect_hit(setup_rect(), x, y)) {
            app_tr_settings_request_source(TR_TAB_XHS);
            ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
        }
        const int seg = tr_segment_hit(TR_SEGMENT_RECT, 2, x, y);
        return seg >= 0 && seg != (int)s_seg ? show_seg((seg_t)seg) : APP_REDRAW_NONE;
    }
    if (ui_rect_hit(REFRESH_RECT, x, y)) {
        const tomato_red_job_t list = s_seg == SEG_FOLLOW ? TOMATO_RED_FEED : TOMATO_RED_REC;
        // 点了取消就别再自动补缩略图，等下次刷新。/ After a cancel, stop filling thumbnails until the next refresh.
        if (job_running(list)) { drop_job(list); s_lazy_ok = false; return APP_REDRAW_PAGE; }
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
    // on each side turns the page, the middle opens the note under the finger.
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
    // 这张没下好：点中间重下缺的图，下载中点了不管。/ This image is missing: a tap in the middle fetches the
    // missing images again, and does nothing while a download runs.
    if (!exists(path)) {
        if (job_running(TOMATO_RED_NOTE)) return APP_REDRAW_NONE;
        s_msg[0] = 0;
        start_job(TOMATO_RED_NOTE, s_id, s_note_rec, false, true);
        return APP_REDRAW_PAGE;
    }
    // 点中间开关存为壁纸工具条。/ A tap in the middle toggles the save-as-wallpaper toolbar.
    if (s_ws.shown) tr_wallsave_close(&s_ws);
    else tr_wallsave_open(&s_ws);
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap_text(uint16_t x, uint16_t y) {
    if (s_more_wait) return !s_more_failed && ui_rect_hit(DIALOG_RECT, x, y) ? APP_REDRAW_NONE : close_more();
    if (y < TR_S(140) && x < 420) return back_to_list();
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
    if (s_tab == T_COMMENTS && s_page + 1 == s_comment_page_count && ui_rect_hit(more_btn(), x, y))
        return s_comments_more ? load_more_comments() : APP_REDRAW_NONE;
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
    if (s_wait || s_more_wait) return APP_REDRAW_NONE;
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
    if (s_more_wait) return close_more();
    if (s_view != V_LIST) return back_to_list();
    if (s_wait) { drop_job(TOMATO_RED_NOTE); s_wait = false; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

/* ---- 后台进度 / Background progress ---- */

static void queue_visible_images(void) {
    if (s_view == V_LIST) {
        for (int i = s_page_first[s_list_page]; i < s_page_first[s_list_page + 1]; i++) {
            char thumb[96];
            thumb_path(&s_items[i], thumb, sizeof(thumb));
            if (exists(thumb)) hupu_img_get(thumb, s_items[i].mw, s_items[i].mh);
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
            if (s_job.older) after_older();
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_RED_NOTE:
        if (s_job.more_comments) {
            if (!s_more_wait || s_view != V_TEXT || !s_note_loaded || strcmp(s_job.id, s_id)) return APP_REDRAW_NONE;
            if (!ok) { s_more_failed = true; return APP_REDRAW_PAGE; }
            // 重新读这篇：留在评论页，页码不超出。/ Reload the note: stay on comments within the page range.
            char id[28];
            snprintf(id, sizeof(id), "%s", s_id);
            const int page = s_page;
            if (!load_note(id)) {
                s_view = V_LIST;
                s_more_wait = false;
                snprintf(s_msg, sizeof(s_msg), "笔记读不出来，检查 TF 卡");
                load_list();
                return APP_REDRAW_PAGE;
            }
            s_more_wait = false;
            s_tab = T_COMMENTS;
            s_page = page < s_comment_page_count ? page : s_comment_page_count - 1;
            return APP_REDRAW_PAGE;
        }
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

// 列表停在一页上：这页和下一页缺缩略图就在后台补（别的页的任务在跑时不抢）。/ The list rests on a page: fill missing
// thumbnails for it and the next page in the background (never taking the worker from another page's job).
static app_redraw_t fill_thumbs(void) {
    if (!s_lazy_ok || s_view != V_LIST || s_seg != SEG_FOLLOW || s_has_pending || tomato_sync_busy() ||
        s_thumbs_tried == s_list_page || s_list_page >= s_list_page_count)
        return APP_REDRAW_NONE;
    s_thumbs_tried = s_list_page;
    const int last = s_list_page + THUMB_PAGES < s_list_page_count ? s_list_page + THUMB_PAGES : s_list_page_count;
    const int a = s_page_first[s_list_page], b = s_page_first[last];
    for (int i = a; i < b; i++) {
        char path[96];
        thumb_path(&s_items[i], path, sizeof(path));
        if (exists(path)) continue;
        const tomato_red_req_t req = {.job = TOMATO_RED_FEED, .thumbs_only = true, .thumbs_from = (uint16_t)a,
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
        const tomato_red_req_t r = s_pending;
        start_req(&r);
        return APP_REDRAW_PAGE;
    }
    // 排着队时又有浏览任务占了后台（别的页留下的）：让它让路。/ While queued, a browsing job from elsewhere took the
    // worker: make it yield.
    if (s_has_pending && !s_job_active && tomato_sync_preemptible()) tomato_sync_cancel();
    if (!s_job_active) return fill_thumbs();
    // 序号变了：自己的任务早结束了，后台在跑别人的；不再跟踪，按缓存重画。/ The sequence moved on: our job ended and
    // another page's job runs; stop tracking and redraw from the caches.
    if (tomato_sync_seq() != s_job_seq) {
        s_job_active = false;
        flush_seen();
        if (s_more_wait) { s_more_failed = true; return APP_REDRAW_PAGE; }
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
            // 留在原来那页，回第一页用双击分段；往后拉的一批等着翻页就翻过去。/ Stay on the current page (a double tap
            // on the segment goes to the first); an older batch turns the page if a turn was waiting.
            if (s_view == V_LIST && (s_job.job == TOMATO_RED_FEED) == (s_seg == SEG_FOLLOW)) {
                load_list();
                if (s_job.older) after_older();
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
    // 列表缩略图按列表顺序下：本页最后一条也轮过了，就先解码显示，不等后面一两百张；解码完 hupu_img_tick 刷一次。
    // List thumbnails download in list order: once this page's last item has had its turn, decode and show them
    // without waiting for the hundreds after it; hupu_img_tick redraws once decoding is done.
    if (busy && s_view == V_LIST && s_job.job != TOMATO_RED_NOTE &&
        (s_job.job == TOMATO_RED_FEED) == (s_seg == SEG_FOLLOW) && s_thumbs_page != s_list_page) {
        if (s_status.current >= 1 + s_page_first[s_list_page + 1]) {
            s_thumbs_page = s_list_page;
            queue_visible_images();
        }
    }
    if (busy) return APP_REDRAW_NONE;
    s_job_active = false;
    if (s_job.job == TOMATO_RED_FEED) flush_seen();
    const bool ok = s_status.phase != TOMATO_FAILED;
    if (s_job.job == TOMATO_RED_FEED) {
        // 刷新成功才开始按页补缩略图；联网失败就停，免得每翻一页都等一次；被别的任务打断的那页回来再补。
        // Paged thumbnail fills start only after a successful refresh and stop on a network failure, so not every page
        // turn waits on it; a page interrupted by another job fills again on return.
        if (ok && !s_job.thumbs_only) { s_lazy_ok = true; s_thumbs_tried = -1; }
        else if (!ok && s_status.error != ESP_ERR_INVALID_STATE) s_lazy_ok = false;
        else if (!ok) s_thumbs_tried = -1;
    }
    if (!ok && s_job.job == TOMATO_RED_FEED && s_job.older) {
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
    s_more_wait = false;
    s_list_page = 0;
    s_lazy_ok = false;
    s_gap_skip = false;
    s_turn_after = false;
    s_thumbs_tried = -1;
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
    s_more_wait = false;
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
