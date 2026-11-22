/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 发现漫画（WEBTOON）：从我的漫画右上角进来。上面是搜索栏和分段 排行 / 类型 / 星期 / 完结，分段下面一排二级选项
 * （榜单、类型或星期几），再下面 3 列封面网格按页翻；点开是作品详情（封面、作者、类型、免费话数、简介），没订阅时
 * 三个订阅按钮（从第 1 话 / 只追新的 / 最近 5 话），订阅后在后台下载，已订阅的可以检查这部的更新或取消订阅。
 * 数据来自用户自建的 webtoon 服务器，components/tomato 的后台任务把列表、详情和灰度封面存到 TF 卡 webtoon/，本页
 * 只读这些缓存；订阅是我的漫画里作品文件夹里的 .webtoon 标记。
 *
 * English: the comic store (WEBTOON), opened from the top right of the comic shelf. A search bar and the segments
 * Rankings / Genres / Weekdays / Completed sit on top, a row of second-level choices (ranking, genre or weekday)
 * below, then a three-column cover grid turned by page. A title opens its details (cover, author, genre, free episode
 * count, summary) with three subscribe buttons (from episode 1 / new only / latest 5) when not subscribed; a
 * subscription downloads in the background, and a subscribed title can check its updates or unsubscribe. Data comes
 * from the user's own webtoon server; the components/tomato task stores lists, details and gray covers under webtoon/
 * on the TF card and this page reads only those caches; a subscription is the .webtoon marker in the title's folder
 * on the comic shelf.
 *
 * 冻结：只在用户点“刷新”、切到没缓存的列表、打开详情、搜索、订阅和检查更新时联网，进页不自动刷新。取消订阅只删标记，
 * 已下载的话留在我的漫画里。只拉公开免费的话，不登录 WEBTOON。大块数据进页分配、离页释放。
 * Frozen: network only on Refresh, opening an uncached list, opening details, searching, subscribing and checking
 * updates; entering never refreshes. Unsubscribing only removes the marker and downloaded episodes stay on the comic
 * shelf. Only free public episodes, never a WEBTOON sign-in. Large buffers are allocated on entry and freed on exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "book_cover.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tomato.h"
#include "tr_kbd.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_wt"
#define POLL_MS 400
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CW (TR_W - 2 * TR_MARGIN)

extern const app_desc_t app_comic;
void app_tr_settings_request_source(int tab);
void app_comic_request_rescan(void);

// 长度都随显示大小放大；封面图不放大。本页没有标签栏，翻页条贴着屏幕底。/ Lengths scale with the display size;
// cover images do not. This page has no tab bar; the pager sits near the screen bottom.
#define ITEMS_MAX 120
#define KEYS_MAX 40
#define REFRESH_RECT (EpdRect){TR_W - TR_MARGIN - TR_S(200), TR_S(70), TR_S(200), TR_S(80)}
#define SEARCH_BAR (EpdRect){TR_MARGIN, TR_S(168), CW, TR_S(72)}
#define SEG_RECT (EpdRect){TR_MARGIN, TR_S(258), CW, TR_S(60)}
#define CHIPS_Y TR_S(336)
#define CHIP_H TR_S(56)
#define CHIP_STEP TR_S(68)
#define CHIP_ROWS 2
#define PAGER_Y (TR_H - TR_S(76))
#define GRID_COLS 3
#define CELL_W 150
#define CELL_COVER_H 200
#define CELL_TEXT_H TR_S(80)
#define CELL_GAP ((CW - GRID_COLS * CELL_W) / (GRID_COLS - 1))
#define COVER_RECT (EpdRect){TR_MARGIN, TR_S(176), 150, 200}
#define DETAIL_PILL_H TR_S(76)
#define SETUP_TEXT "还没连接漫画服务器。先在电脑或 NAS 上部署 webtoon（见项目 webtoon/ 目录），" \
                   "再到“设置 › 内容来源 › 漫画”填服务器地址和设备令牌。"

typedef enum { V_LIST, V_SEARCH, V_DETAIL } view_t;
typedef enum { SEG_RANK, SEG_GENRE, SEG_DAY, SEG_DONE, SEG_COUNT } seg_t;
static const char* const SEG_KEYS[SEG_COUNT] = {"rank", "genre", "day", "done"};
static const char* const SEG_NAMES[SEG_COUNT] = {"排行", "类型", "星期", "完结"};

typedef struct {
    char id[16];
    char title[96];
    char sub[64];         // 类型或作者 / Genre or author
    bool canvas;
} item_t;

typedef struct {
    char key[24];
    char name[32];
} wt_key_t;

typedef struct {
    int no;
    char title[48];
} ep_t;

/* ---- 状态 / State ---- */

static view_t s_view;
static bool s_has_cfg;
static char s_msg[112];
static int64_t s_poll_ms;

static seg_t s_seg;
static char s_key[24];                // 当前二级选项 / Current second-level choice
static bool s_searching;              // 列表显示的是搜索结果 / The list shows search results
static char s_query[64];
static item_t* s_items;
static int s_item_count = -1;         // -1：没有缓存 / -1: no cache
static wt_key_t* s_keys;
static int s_key_count;
static int s_chip_page;
static int s_page;
static char s_list_key[24];           // 列表文件对应的分段和选项 / The segment and choice the list file holds
static char s_list_seg[8];

// 详情 / Detail
static char s_id[16];
static char s_title[96], s_author[64], s_genre[48];
static char* s_summary;
static ep_t* s_eps;
static int s_ep_count, s_latest;
static bool s_done;
static bool s_detail_loaded;
static bool s_subscribed;
static char s_folder[288];
static tomato_wt_mark_t s_mark;
static bool s_confirm_unsub;

EXT_RAM_BSS_ATTR static tr_kbd_t s_kbd;

// 后台任务 / Background job
static bool s_job_active;
static uint32_t s_job_seq;
static tomato_wt_req_t s_job;
static tomato_status_t s_status;
static bool s_step_loaded;
static tomato_wt_req_t s_pending;
static bool s_has_pending;

/* ---- 内存 / Memory ---- */

#define FREE(p) do { heap_caps_free(p); (p) = NULL; } while (0)

static void free_all(void) {
    FREE(s_items);
    FREE(s_keys);
    FREE(s_summary);
    FREE(s_eps);
    s_item_count = -1;
    s_key_count = s_ep_count = 0;
    book_cover_drop_pending();
}

static bool alloc_all(void) {
    if (!s_items) s_items = heap_caps_calloc(ITEMS_MAX, sizeof(item_t), PSRAM_CAPS);
    if (!s_keys) s_keys = heap_caps_calloc(KEYS_MAX, sizeof(wt_key_t), PSRAM_CAPS);
    if (!s_summary) s_summary = heap_caps_calloc(1, 2048, PSRAM_CAPS);
    if (!s_eps) s_eps = heap_caps_calloc(2000, sizeof(ep_t), PSRAM_CAPS);
    const bool ok = s_items && s_keys && s_summary && s_eps;
    if (!ok) { ESP_LOGE(TAG, "out of memory"); free_all(); }
    return ok;
}

/* ---- 文件 / Files ---- */

static cJSON* read_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < 1024 * 1024 ? heap_caps_malloc((size_t)n + 1, PSRAM_CAPS) : NULL;
    const bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
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

static int num(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static bool cover_file(const char* id, char* out, size_t cap) {
    struct stat st;
    tomato_wt_cover_path(id, out, cap);
    return stat(out, &st) == 0 && st.st_size > 0;
}

// 读列表文件（发现页或搜索）。/ Load a list file (store or search).
static void load_list(void) {
    s_item_count = -1;
    s_key_count = 0;
    s_list_seg[0] = s_list_key[0] = 0;
    cJSON* root = read_json(s_searching ? TOMATO_WT_SEARCH_FILE : TOMATO_WT_STORE_FILE);
    if (!root) return;
    str(s_list_seg, sizeof(s_list_seg), root, "seg");
    str(s_list_key, sizeof(s_list_key), root, "key");
    // 发现页文件里存的不是当前分段：当作没有缓存。/ The store file holds another segment: treat it as no cache.
    if (!s_searching && (strcmp(s_list_seg, SEG_KEYS[s_seg]) || (s_key[0] && strcmp(s_list_key, s_key)))) {
        cJSON_Delete(root);
        return;
    }
    if (!s_searching) snprintf(s_key, sizeof(s_key), "%s", s_list_key);
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "keys")) {
        if (s_key_count >= KEYS_MAX) break;
        wt_key_t* k = &s_keys[s_key_count];
        str(k->key, sizeof(k->key), it, "key");
        str(k->name, sizeof(k->name), it, "name");
        if (k->key[0]) s_key_count++;
    }
    s_item_count = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (s_item_count >= ITEMS_MAX) break;
        item_t* x = &s_items[s_item_count];
        memset(x, 0, sizeof(*x));
        str(x->id, sizeof(x->id), it, "id");
        if (!x->id[0]) continue;
        str(x->title, sizeof(x->title), it, "title");
        str(x->sub, sizeof(x->sub), it, "genre");
        if (!x->sub[0]) str(x->sub, sizeof(x->sub), it, "author");
        x->canvas = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "canvas"));
        s_item_count++;
    }
    cJSON_Delete(root);
}

static void load_detail(void) {
    s_detail_loaded = false;
    s_ep_count = s_latest = 0;
    char path[64];
    tomato_wt_title_path(s_id, path, sizeof(path));
    cJSON* root = read_json(path);
    if (root) {
        str(s_title, sizeof(s_title), root, "title");
        str(s_author, sizeof(s_author), root, "author");
        str(s_genre, sizeof(s_genre), root, "genre");
        str(s_summary, 2048, root, "summary");
        s_latest = num(root, "latest");
        s_done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done"));
        const cJSON* e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "episodes")) {
            if (s_ep_count >= 2000) break;
            s_eps[s_ep_count].no = num(e, "no");
            str(s_eps[s_ep_count].title, sizeof(s_eps[0].title), e, "title");
            s_ep_count++;
        }
        s_detail_loaded = true;
        cJSON_Delete(root);
    }
    s_subscribed = tomato_wt_find(s_id, s_folder, sizeof(s_folder)) && tomato_wt_mark_read(s_folder, &s_mark);
}

/* ---- 后台任务 / Background jobs ---- */

static bool start_req(const tomato_wt_req_t* r) {
    const esp_err_t err = tomato_sync_start_wt(r);
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

static bool ours_running(void) { return s_job_active && tomato_sync_busy() && tomato_sync_seq() == s_job_seq; }

// 前台优先：浏览类任务立刻取消；下载（包括更新漫画）和同步不打断，排在后面。/ Foreground first: browsing jobs are
// cancelled at once; downloads (comic updates included) and sync are left alone and this request waits.
static void start_job(const tomato_wt_req_t* req) {
    if (tomato_sync_busy()) {
        const bool yields = tomato_sync_preemptible();
        if (yields) tomato_sync_cancel();
        s_pending = *req;
        s_has_pending = true;
        if (yields) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "后台正在下载或同步，完成后自动开始");
        return;
    }
    s_has_pending = false;
    start_req(req);
}

static bool job_running(tomato_wt_job_t job) {
    return (s_has_pending && s_pending.job == job) || (s_job.job == job && ours_running());
}

static const char* loading_text(const char* what) {
    return s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : what;
}

static void request_list(void) {
    tomato_wt_req_t r = {.job = s_searching ? TOMATO_WT_SEARCH : TOMATO_WT_STORE};
    snprintf(r.seg, sizeof(r.seg), "%s", SEG_KEYS[s_seg]);
    snprintf(r.key, sizeof(r.key), "%s", s_key);
    snprintf(r.q, sizeof(r.q), "%s", s_query);
    start_job(&r);
}

/* ---- 布局 / Layout ---- */

static int chip_rows_used(void);

static int grid_rows(int* cover_h) {
    const int top = CHIPS_Y + chip_rows_used() * CHIP_STEP + TR_S(44);
    const int room = PAGER_Y - TR_S(12) - top;
    int rows = room / (CELL_COVER_H + CELL_TEXT_H);
    int h = CELL_COVER_H;
    if (rows < 1) { rows = 1; h = room - CELL_TEXT_H; if (h < 120) h = 120; }
    if (cover_h) *cover_h = h;
    return rows;
}

static int grid_top(void) { return CHIPS_Y + chip_rows_used() * CHIP_STEP + TR_S(44); }
static int grid_per(void) { return grid_rows(NULL) * GRID_COLS; }
static int list_pages(void) { return s_item_count > 0 ? (s_item_count + grid_per() - 1) / grid_per() : 1; }

static EpdRect cell_rect(int k) {
    int h;
    grid_rows(&h);
    return (EpdRect){TR_MARGIN + (k % GRID_COLS) * (CELL_W + CELL_GAP), grid_top() + (k / GRID_COLS) * (h + CELL_TEXT_H),
                     h * 3 / 4, h};
}

// 二级选项胶囊：最多两行，放不下的翻页（最后一格是“更多 ›”）。/ Second-level chips: at most two rows; the rest page
// (the last cell is "更多 ›").
#define CHIP_SLOTS 24
static int layout_chips(EpdRect* rects, int* idx, int* more, int* pages) {
    int n = 0, page = 0, x = TR_MARGIN, row = 0;
    *more = -1;
    const int more_w = ttf_text_width_px(TR_S(26), "更多 ›") + TR_S(36);
    int first_of_page = 0;
    for (int i = 0; i < s_key_count; i++) {
        const int w = ttf_text_width_px(TR_S(26), s_keys[i].name) + TR_S(36);
        const bool last_row = row == CHIP_ROWS - 1;
        const int limit = TR_W - TR_MARGIN - (last_row ? more_w + TR_S(10) : 0);
        if (x + w > limit) {
            if (!last_row) { row++; x = TR_MARGIN; i--; continue; }
            if (page == s_chip_page) *more = 1;
            page++;
            row = 0;
            x = TR_MARGIN;
            first_of_page = i;
            i--;
            continue;
        }
        if (page == s_chip_page && n < CHIP_SLOTS) {
            rects[n] = (EpdRect){x, CHIPS_Y + row * CHIP_STEP, w, CHIP_H};
            idx[n++] = i;
        }
        x += w + TR_S(10);
    }
    (void)first_of_page;
    *pages = page + 1;
    return n;
}

// 二级选项实际占几行（搜索时一行“回到发现”）。/ Rows the second-level chips take (one "back to store" row while
// searching).
static int chip_rows_used(void) {
    if (s_searching) return 1;
    if (!s_key_count) return 0;
    EpdRect rects[CHIP_SLOTS];
    int idx[CHIP_SLOTS], more, pages;
    const int n = layout_chips(rects, idx, &more, &pages);
    if (pages > 1) return CHIP_ROWS;
    int rows = 0;
    for (int k = 0; k < n; k++) {
        const int r = (rects[k].y - CHIPS_Y) / CHIP_STEP + 1;
        if (r > rows) rows = r;
    }
    return rows ? rows : 1;
}

static EpdRect more_rect(void) {
    const int w = ttf_text_width_px(TR_S(26), "更多 ›") + TR_S(36);
    return (EpdRect){TR_W - TR_MARGIN - w, CHIPS_Y + (CHIP_ROWS - 1) * CHIP_STEP, w, CHIP_H};
}

/* ---- 绘制 / Drawing ---- */

static void draw_back(uint8_t* fb, const char* title) {
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_S(96), TR_S(96), TR_S(40), title, EPD_DRAW_ALIGN_LEFT, true);
}

static void draw_refresh(uint8_t* fb, bool running) {
    if (running) {
        tr_text(fb, TR_W - TR_MARGIN, TR_S(98), TR_S(30), "加载中…", EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - ttf_text_width_px(TR_S(30), "刷新") - TR_S(26), TR_S(116), false);
        tr_text(fb, TR_W - TR_MARGIN, TR_S(98), TR_S(30), "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void draw_list(uint8_t* fb) {
    draw_back(fb, "发现漫画");
    if (!s_has_cfg) {
        tr_wrap(fb, TR_MARGIN, TR_S(200), CW, TR_S(30), TR_S(46), SETUP_TEXT, 8);
        tr_pill(fb, (EpdRect){TR_MARGIN, TR_S(470), TR_S(240), TR_S(72)}, "去设置", true, TR_S(30));
        return;
    }
    const bool loading = job_running(TOMATO_WT_STORE) || job_running(TOMATO_WT_SEARCH);
    draw_refresh(fb, loading && !s_step_loaded);
    // 搜索栏（Material 3 灰底胶囊）。/ Search bar (Material 3 gray capsule).
    const EpdRect bar = SEARCH_BAR;
    ui_fill_round_rect(fb, bar, bar.height / 2, TR_GRAY_SURFACE);
    tr_icon_on(fb, TR_ICON_SEARCH, bar.x + TR_S(46), bar.y + bar.height / 2, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    char line[160];
    if (s_searching) snprintf(line, sizeof(line), "%s", s_query);
    else snprintf(line, sizeof(line), "搜索作品名或作者");
    tr_fit(line, TR_S(28), bar.width - TR_S(120));
    tr_text_on(fb, bar.x + TR_S(82), bar.y + (bar.height - TR_S(28)) / 2 - TR_S(2), TR_S(28), line, EPD_DRAW_ALIGN_LEFT,
               false, s_searching ? UI_GRAY_BLACK : TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    tr_segment(fb, SEG_RECT, SEG_NAMES, SEG_COUNT, s_searching ? -1 : (int)s_seg);
    if (s_searching) {
        tr_pill(fb, (EpdRect){TR_MARGIN, CHIPS_Y, TR_S(200), CHIP_H}, "‹ 回到发现", false, TR_S(26));
    } else if (s_key_count) {
        EpdRect rects[CHIP_SLOTS];
        int idx[CHIP_SLOTS], more, pages;
        const int n = layout_chips(rects, idx, &more, &pages);
        for (int k = 0; k < n; k++) tr_pill(fb, rects[k], s_keys[idx[k]].name, !strcmp(s_keys[idx[k]].key, s_key), TR_S(26));
        if (pages > 1) tr_pill(fb, more_rect(), "更多 ›", false, TR_S(26));
    }
    const int status_y = grid_top() - TR_S(38);
    if (loading && !s_step_loaded) snprintf(line, sizeof(line), "%s", loading_text(s_searching ? "正在搜索…" : "正在加载…"));
    else if (s_msg[0]) snprintf(line, sizeof(line), "%s", s_msg);
    else if (s_item_count >= 0) snprintf(line, sizeof(line), s_searching ? "找到 %d 部" : "%d 部", s_item_count);
    else line[0] = 0;
    if (line[0]) tr_message(fb, TR_MARGIN, status_y, status_y - TR_S(10), CW, TR_S(24), line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_item_count <= 0) {
        if (!loading)
            tr_wrap(fb, TR_MARGIN, grid_top() + TR_S(20), CW, TR_S(30), TR_S(46), s_item_count == 0 ? "没有作品。" :
                    "还没有缓存。点右上角“刷新”从服务器取；之后不联网也能看上次取到的。", 4);
        return;
    }
    const int per = grid_per();
    for (int k = 0; k < per; k++) {
        const int i = s_page * per + k;
        if (i >= s_item_count) break;
        const item_t* x = &s_items[i];
        const EpdRect r = cell_rect(k);
        char cover[64];
        tr_book_cover(fb, r, cover_file(x->id, cover, sizeof(cover)) ? cover : NULL, x->title, "", "", false);
        snprintf(line, sizeof(line), "%s", x->title);
        tr_fit(line, TR_S(24), CELL_W);
        tr_text(fb, r.x, r.y + r.height + TR_S(8), TR_S(24), line, EPD_DRAW_ALIGN_LEFT, true);
        snprintf(line, sizeof(line), "%s%s", x->canvas ? "Canvas · " : "", x->sub);
        tr_fit(line, TR_S(22), CELL_W);
        tr_text_dim(fb, r.x, r.y + r.height + TR_S(42), TR_S(22), line, EPD_DRAW_ALIGN_LEFT);
    }
    tr_pager(fb, PAGER_Y, s_page, list_pages());
}

static void draw_search(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), "搜索漫画", EPD_DRAW_ALIGN_LEFT, true);
    const EpdRect f = {TR_MARGIN, TR_S(176), CW, TR_S(84)};
    ui_draw_round_rect(fb, f, TR_S(14), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(f, 1), TR_S(14) - 1, TR_GRAY_OUTLINE);
    const int px = TR_S(32);
    const char* tail = tr_tail_fit(s_kbd.text, px, f.width - TR_S(48));
    tr_text(fb, f.x + TR_S(20), f.y + TR_S(22), px, tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + TR_S(22) + ttf_text_width_px(px, tail), f.y + TR_S(18), TR_S(3), TR_S(46)},
                  UI_GRAY_BLACK, fb);
    if (TR_S(286) + TR_S(36) < TR_KBD_TOP)
        tr_text_dim(fb, TR_MARGIN, TR_S(286), TR_S(24), "作品名或作者，打简体也行（服务器会转成繁体去搜）", EPD_DRAW_ALIGN_LEFT);
    tr_kbd_draw(fb, &s_kbd);
}

static EpdRect detail_btn(int i, int n) {
    const int gap = TR_S(14), w = (CW - (n - 1) * gap) / n;
    return (EpdRect){TR_MARGIN + i * (w + gap), TR_H - TR_S(100) - DETAIL_PILL_H, w, DETAIL_PILL_H};
}

static void draw_detail(uint8_t* fb) {
    draw_back(fb, "作品详情");
    const bool loading = job_running(TOMATO_WT_TITLE);
    if (!s_detail_loaded) {
        tr_wrap(fb, TR_MARGIN, TR_S(200), CW, TR_S(30), TR_S(46),
                loading ? loading_text("正在加载作品详情…") : s_msg[0] ? s_msg : "没取到详情，点右上角“刷新”再试。", 4);
        if (!loading) draw_refresh(fb, false);
        return;
    }
    draw_refresh(fb, loading);
    const EpdRect cr = COVER_RECT;
    char cover[64];
    tr_book_cover(fb, cr, cover_file(s_id, cover, sizeof(cover)) ? cover : NULL, s_title, s_author, "WEBTOON", false);
    const int x = cr.x + cr.width + TR_S(28), w = TR_W - TR_MARGIN - x;
    int y = cr.y + TR_S(4);
    const int lines = tr_wrap(fb, x, y, w, TR_S(36), TR_S(48), s_title, 2);
    y += lines * TR_S(48) + TR_S(10);
    char line[160];
    snprintf(line, sizeof(line), "%s", s_author);
    tr_fit(line, TR_S(26), w);
    tr_text_dim(fb, x, y, TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
    snprintf(line, sizeof(line), "%s · 免费 %d 话%s", s_genre[0] ? s_genre : "WEBTOON", s_ep_count, s_done ? " · 完结" : "");
    tr_fit(line, TR_S(26), w);
    tr_text_dim(fb, x, y + TR_S(42), TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
    if (s_subscribed) {
        snprintf(line, sizeof(line), "已订阅 · 下载到第 %d 话", s_mark.last);
        tr_fit(line, TR_S(26), w);
        tr_text(fb, x, y + TR_S(84), TR_S(26), line, EPD_DRAW_ALIGN_LEFT, true);
    }
    int rule = cr.y + cr.height + TR_S(24);
    if (y + TR_S(130) > rule) rule = y + TR_S(130);
    tr_hline(fb, rule, TR_MARGIN, CW, true);
    const EpdRect b0 = detail_btn(0, 3);
    const int note_y = b0.y - TR_S(48);
    const int sum_lines = (note_y - TR_S(16) - (rule + TR_S(22))) / TR_S(44);
    if (sum_lines > 0)
        tr_wrap(fb, TR_MARGIN, rule + TR_S(22), CW, TR_S(28), TR_S(44), s_summary[0] ? s_summary : "暂无简介",
                sum_lines > 12 ? 12 : sum_lines);
    // 订阅按钮或已订阅的操作；上面一行写状态。/ Subscribe buttons or subscribed actions, with a status line above.
    const bool updating = tomato_wt_updating();
    if (updating) {
        tomato_status_t st;
        tomato_sync_get_status(&st);
        if (st.total > 0) snprintf(line, sizeof(line), "正在下载漫画 %d / %d 话…", st.current, st.total);
        else snprintf(line, sizeof(line), "%s", loading_text("正在检查更新…"));
    } else if (s_msg[0]) {
        snprintf(line, sizeof(line), "%s", s_msg);
    } else {
        snprintf(line, sizeof(line), "%s", s_subscribed ? "新话在我的漫画点“检查更新”下载" :
                 "订阅后在后台下载，下载好的在我的漫画里看");
    }
    tr_message(fb, TR_MARGIN, note_y, note_y - TR_S(14), CW, TR_S(24), line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_subscribed) {
        tr_pill(fb, detail_btn(0, 3), "检查更新", true, TR_S(28));
        tr_pill(fb, detail_btn(1, 3), "去我的漫画", false, TR_S(28));
        tr_pill(fb, detail_btn(2, 3), s_confirm_unsub ? "再点取消" : "取消订阅", s_confirm_unsub, TR_S(28));
    } else {
        tr_pill(fb, detail_btn(0, 3), "从第 1 话", true, TR_S(28));
        tr_pill(fb, detail_btn(1, 3), "只追新的", false, TR_S(28));
        tr_pill(fb, detail_btn(2, 3), "最近 5 话", false, TR_S(28));
    }
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    if (!s_items) {
        draw_back(fb, "发现漫画");
        tr_text(fb, TR_MARGIN, TR_S(240), TR_S(30), "内存不足，打不开发现页。", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    switch (s_view) {
    case V_SEARCH: draw_search(fb); break;
    case V_DETAIL: draw_detail(fb); break;
    default: draw_list(fb); break;
    }
}

/* ---- 动作 / Actions ---- */

static app_redraw_t show_seg(seg_t seg) {
    s_seg = seg;
    s_searching = false;
    s_key[0] = 0;
    s_page = s_chip_page = 0;
    s_msg[0] = 0;
    book_cover_drop_pending();
    load_list();
    if (s_item_count < 0) request_list();
    return APP_REDRAW_PAGE;
}

static app_redraw_t pick_key(int i) {
    snprintf(s_key, sizeof(s_key), "%s", s_keys[i].key);
    s_page = 0;
    s_msg[0] = 0;
    book_cover_drop_pending();
    request_list();
    // 新的列表来之前先清空网格，免得显示上一个类型的书。/ Clear the grid until the new list arrives so the previous
    // genre's titles do not show.
    s_item_count = -1;
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_detail(const char* id) {
    snprintf(s_id, sizeof(s_id), "%s", id);
    s_title[0] = s_author[0] = s_genre[0] = 0;
    if (s_summary) s_summary[0] = 0;
    s_confirm_unsub = false;
    s_msg[0] = 0;
    book_cover_drop_pending();
    load_detail();
    s_view = V_DETAIL;
    tomato_wt_req_t r = {.job = TOMATO_WT_TITLE};
    snprintf(r.id, sizeof(r.id), "%s", id);
    start_job(&r);
    return APP_REDRAW_PAGE;
}

static app_redraw_t back_to_list(void) {
    s_view = V_LIST;
    s_msg[0] = 0;
    book_cover_drop_pending();
    load_list();
    return APP_REDRAW_PAGE;
}

static void start_update(const char* id) {
    tomato_wt_req_t r = {.job = TOMATO_WT_UPDATE};
    snprintf(r.id, sizeof(r.id), "%s", id);
    start_job(&r);
}

static app_redraw_t subscribe(int choice) {
    if (!s_detail_loaded) return APP_REDRAW_NONE;
    // 起点：从第 1 话 → 0；只追新的 → 最新一话；最近 5 话 → 最新减 5。/ Starting point: from 1 -> 0; new only ->
    // the latest; latest 5 -> latest minus 5.
    const int last = choice == 0 ? 0 : choice == 1 ? s_latest : (s_latest > 5 ? s_latest - 5 : 0);
    const esp_err_t err = tomato_wt_subscribe(s_id, s_title, s_author, last, s_folder, sizeof(s_folder));
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "订阅没成功，检查 TF 卡");
        return APP_REDRAW_PAGE;
    }
    load_detail();
    app_comic_request_rescan();
    if (choice == 1) snprintf(s_msg, sizeof(s_msg), "已订阅，以后有新话按“检查更新”下载");
    else start_update(s_id);
    return APP_REDRAW_PAGE;
}

static app_redraw_t unsubscribe(void) {
    if (!s_confirm_unsub) { s_confirm_unsub = true; return APP_REDRAW_PAGE; }
    s_confirm_unsub = false;
    char marker[320];
    snprintf(marker, sizeof(marker), "%s/" TOMATO_WT_MARK, s_folder);
    remove(marker);
    // 一话都还没下的空文件夹顺手删掉。/ Remove the folder too when no episode was downloaded yet.
    rmdir(s_folder);
    load_detail();
    app_comic_request_rescan();
    snprintf(s_msg, sizeof(s_msg), "已取消订阅；下载好的话还在我的漫画里");
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_search(void) {
    tr_kbd_open(&s_kbd, s_searching ? s_query : "", true, "搜索");
    s_view = V_SEARCH;
    return APP_REDRAW_FULL;
}

static app_redraw_t turn(int dir) {
    if (s_view != V_LIST) return APP_REDRAW_NONE;
    const int next = s_page + dir;
    if (next < 0 || next >= list_pages()) return APP_REDRAW_NONE;
    s_page = next;
    book_cover_drop_pending();
    return APP_REDRAW_PAGE;
}

/* ---- 触摸 / Touch ---- */

static app_redraw_t to_comic(app_ctx_t* ctx) {
    ctx->request_app = &app_comic;
    return APP_REDRAW_NONE;
}

static app_redraw_t tap_list(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(320)) return to_comic(ctx);
    if (!s_has_cfg) {
        if (ui_rect_hit((EpdRect){TR_MARGIN, TR_S(470), TR_S(240), TR_S(72)}, x, y)) {
            app_tr_settings_request_source(TR_TAB_COMIC);
            ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
        }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(REFRESH_RECT, x, y)) { s_msg[0] = 0; request_list(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(SEARCH_BAR, x, y)) return open_search();
    const int seg = tr_segment_hit(SEG_RECT, SEG_COUNT, x, y);
    if (seg >= 0) return (seg != (int)s_seg || s_searching) ? show_seg((seg_t)seg) : APP_REDRAW_NONE;
    if (s_searching && ui_rect_hit((EpdRect){TR_MARGIN, CHIPS_Y, TR_S(200), CHIP_H}, x, y)) return show_seg(s_seg);
    if (!s_searching && s_key_count) {
        EpdRect rects[CHIP_SLOTS];
        int idx[CHIP_SLOTS], more, pages;
        const int n = layout_chips(rects, idx, &more, &pages);
        if (pages > 1 && ui_rect_hit(more_rect(), x, y)) { s_chip_page = (s_chip_page + 1) % pages; return APP_REDRAW_PAGE; }
        for (int k = 0; k < n; k++)
            if (ui_rect_hit(rects[k], x, y)) return strcmp(s_keys[idx[k]].key, s_key) ? pick_key(idx[k]) : APP_REDRAW_NONE;
    }
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    const int per = grid_per();
    for (int k = 0; k < per; k++) {
        const int i = s_page * per + k;
        if (i >= s_item_count) break;
        const EpdRect r = cell_rect(k);
        if (x >= r.x && x < r.x + CELL_W && y >= r.y && y < r.y + r.height + CELL_TEXT_H) return open_detail(s_items[i].id);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t tap_search(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(120)) { memset(&s_kbd, 0, sizeof(s_kbd)); s_view = V_LIST; return APP_REDRAW_FULL; }
    const tr_kbd_result_t res = tr_kbd_touch(&s_kbd, x, y);
    if (res == TR_KBD_CHANGED) { render(ctx, ctx->fb); return APP_REDRAW_AREA; }
    if (res != TR_KBD_SUBMIT || !s_kbd.text[0]) return APP_REDRAW_NONE;
    snprintf(s_query, sizeof(s_query), "%.60s", s_kbd.text);
    // 截在多字节字符中间就去掉那半个字。/ Drop a half character when the cut lands inside one.
    size_t n = strlen(s_query), lead = n;
    while (lead && ((unsigned char)s_query[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead) {
        const unsigned char c = (unsigned char)s_query[lead - 1];
        const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n - (lead - 1) < need) s_query[lead - 1] = 0;
    }
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_searching = true;
    s_page = 0;
    s_item_count = -1;
    s_view = V_LIST;
    request_list();
    return APP_REDRAW_FULL;
}

static app_redraw_t tap_detail(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(320)) return back_to_list();
    if (ui_rect_hit(REFRESH_RECT, x, y) && !job_running(TOMATO_WT_TITLE)) return open_detail(s_id);
    if (!s_detail_loaded) return APP_REDRAW_NONE;
    for (int i = 0; i < 3; i++) {
        if (!ui_rect_hit(detail_btn(i, 3), x, y)) continue;
        if (!s_subscribed) return subscribe(i);
        if (i == 0) {
            if (tomato_wt_updating()) return APP_REDRAW_NONE;
            s_confirm_unsub = false;
            start_update(s_id);
            return APP_REDRAW_PAGE;
        }
        if (i == 1) return to_comic(ctx);
        return unsubscribe();
    }
    if (s_confirm_unsub) { s_confirm_unsub = false; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (!s_items) return ev->type == UI_GESTURE_TAP ? to_comic(ctx) : APP_REDRAW_NONE;
    if (s_view == V_SEARCH) return ev->type == UI_GESTURE_TAP ? tap_search(ctx, ev->x0, ev->y0) : APP_REDRAW_NONE;
    if (ev->type == UI_GESTURE_TAP) return s_view == V_DETAIL ? tap_detail(ctx, ev->x0, ev->y0) : tap_list(ctx, ev->x0, ev->y0);
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U) return turn(1);
    if (ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D) return turn(-1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    if (s_view == V_SEARCH) return tap_search(ctx, 0, 0);
    if (s_view == V_DETAIL) return back_to_list();
    return to_comic(ctx);
}

/* ---- 后台进度 / Background progress ---- */

static app_redraw_t job_done(bool ok) {
    switch (s_job.job) {
    case TOMATO_WT_STORE:
    case TOMATO_WT_SEARCH:
        if (s_view == V_LIST) { load_list(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    case TOMATO_WT_TITLE:
        if (s_view == V_DETAIL && !strcmp(s_job.id, s_id)) { load_detail(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    case TOMATO_WT_UPDATE:
        app_comic_request_rescan();
        if (ok) snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
        if (s_view == V_DETAIL) { load_detail(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    default: return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (!s_items || s_view == V_SEARCH) return APP_REDRAW_NONE;
    if (!(ctx->touch && ctx->touch->touched) && book_cover_tick()) return APP_REDRAW_PAGE;
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tr_time_poll();
    if (s_has_pending && !tomato_sync_busy() && !s_job_active) {
        s_has_pending = false;
        const tomato_wt_req_t r = s_pending;
        start_req(&r);
        return APP_REDRAW_PAGE;
    }
    if (s_has_pending && !s_job_active && tomato_sync_preemptible()) tomato_sync_cancel();
    if (!s_job_active) return APP_REDRAW_NONE;
    if (tomato_sync_seq() != s_job_seq) { s_job_active = false; return APP_REDRAW_NONE; }
    const bool busy = tomato_sync_busy();
    tomato_status_t prev = s_status;
    tomato_sync_get_status(&s_status);
    // 列表写好就先显示，封面接着在后台下。/ Show the list once written; covers keep downloading behind it.
    if (busy && !s_step_loaded && s_status.current >= 1 && s_status.phase != TOMATO_FAILED &&
        (s_job.job == TOMATO_WT_STORE || s_job.job == TOMATO_WT_SEARCH)) {
        s_step_loaded = true;
        if (s_view == V_LIST) { load_list(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    // 下载进度变了：详情页更新那一行。/ Download progress moved: refresh the detail page's status line.
    if (busy && s_job.job == TOMATO_WT_UPDATE && s_view == V_DETAIL && prev.current != s_status.current)
        return APP_REDRAW_PAGE;
    if (busy) return APP_REDRAW_NONE;
    s_job_active = false;
    const bool ok = s_status.phase != TOMATO_FAILED;
    if (!ok) {
        if (s_status.error == ESP_ERR_INVALID_STATE) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    }
    if (s_has_pending) {
        job_done(ok);
        s_has_pending = false;
        const tomato_wt_req_t next = s_pending;
        start_req(&next);
        return APP_REDRAW_PAGE;
    }
    const app_redraw_t r = job_done(ok);
    return !ok && r == APP_REDRAW_NONE ? APP_REDRAW_PAGE : r;
}

/* ---- 生命周期 / Lifecycle ---- */

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    book_cover_drop_pending();
    tomato_wt_config_t cfg;
    s_has_cfg = tomato_wt_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    if (!alloc_all()) return;
    s_msg[0] = 0;
    if (s_job_active && !ours_running()) s_job_active = false;
    if (s_view == V_SEARCH) s_view = V_LIST;
    if (s_view == V_DETAIL && s_id[0]) {
        load_detail();
        return;
    }
    s_view = V_LIST;
    load_list();
    if (s_has_cfg && s_item_count < 0 && !s_searching) request_list();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    s_has_pending = false;
    memset(&s_kbd, 0, sizeof(s_kbd));
    free_all();
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    if (!tomato_wt_updating()) tomato_sync_cancel();
    s_job_active = false;
    s_has_pending = false;
    free_all();
    alloc_all();
    s_view = V_LIST;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){0, TR_S(160), TR_W, TR_H - TR_S(160)};
}

const app_desc_t app_tr_wt = {
    .title = "发现漫画",
    .detail = "WEBTOON 排行、类型、搜索与订阅（经自建 webtoon 服务器）",
    .enter_full = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
