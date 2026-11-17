/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 番茄标签页：番茄书架（检查更新、下载全部更新）、搜索键盘、搜索结果、书籍详情。顶部“书架 / 书城”切到书城页。
 * 联网操作全部交给 components/tomato 的后台任务，这里只发起、轮询和显示。
 *
 * Tomato tab: Tomato shelf (check, download all), search keyboard, results and book detail. The top
 * "shelf / store" segment switches to the bookstore page.
 * All networking runs on the components/tomato task; this page starts, polls and displays.
 *
 * 冻结：检查和下载都只在点击时运行，不自动检查。
 * Frozen: checking and downloading run only on a tap; never automatically.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "book_progress.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "pinyin_ime.h"
#include "tomato.h"
#include "tr_kbd.h"
#include "ttf_font.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_tomato"
#define POLL_MS 400
#define BUSY_REDRAW_MS 1500
#define DU_SETTLE_MAX 12
#define ROW_H 92
#define LIST_BOTTOM 1030
#define RESULT_ROW_H 140
#define RESULT_TOP 254
#define RESULTS_PER_PAGE 5
#define PAGER_Y 1040
#define DETAIL_BTN (EpdRect){TR_MARGIN, 960, TR_W - 2 * TR_MARGIN, 88}
#define FIELD_RECT (EpdRect){96, 84, TR_W - 96 - TR_MARGIN, 80}
#define RECENT_QUERIES 3
// 分段按钮下面的内容整体下移。/ Everything below the segment moves down by this much.
#define SEG_SHIFT 78

extern const app_desc_t app_tr_store;
extern const app_desc_t app_tr_tomato;

typedef enum { V_LIST = 0, V_SEARCH, V_RESULTS, V_DETAIL } view_t;
typedef enum { OP_NONE = 0, OP_CHECK, OP_UPDATE, OP_SEARCH, OP_DOWNLOAD } op_t;

static view_t s_view;
static tomato_book_t* s_shelf;          // PSRAM
static int s_count;
static int8_t s_pct[TOMATO_SHELF_MAX];   // -1 未读 / -1 unread
static uint8_t s_order[TOMATO_SHELF_MAX];
static int s_pending;
static bool s_has_config;
static int s_page;

// 约 6 KB，放 PSRAM。/ About 6 KB, kept in PSRAM.
EXT_RAM_BSS_ATTR static tomato_hit_t s_hits[TOMATO_SEARCH_MAX];
static int s_hit_count;
static int s_result_page;
static int s_detail;
static char s_query[TR_KBD_TEXT_MAX];
static char s_recent[RECENT_QUERIES][48];
EXT_RAM_BSS_ATTR static tr_kbd_t s_kbd;

static op_t s_op;         // 正在跑的操作 / Running operation
static op_t s_last;       // 刚结束的操作，用于显示结果或失败 / Just-finished op, for result or error text
static tomato_status_t s_status;
static uint32_t s_seen_revision;
static int64_t s_poll_ms, s_busy_draw_ms;
static int s_du_count;
static EpdRect s_area;

/* ---- 数据 / Data ---- */

static void book_path(const tomato_book_t* b, char* out, size_t cap) {
    snprintf(out, cap, TOMATO_BOOKS_DIR "/%s", b->file);
}

static void load_shelf(void) {
    tomato_config_t cfg;
    s_has_config = tomato_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    s_count = 0;
    if (s_shelf) tomato_shelf_load(s_shelf, TOMATO_SHELF_MAX, &s_count);
    s_pending = 0;
    int n = 0;
    // 有新章节的排前面。/ Books with new chapters first.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_count; i++) {
            bool pending = s_shelf[i].remote > s_shelf[i].chapters;
            if (pending != (pass == 0)) continue;
            s_order[n++] = (uint8_t)i;
            s_pending += pending;
        }
    }
    for (int i = 0; i < s_count; i++) {
        char path[BOOK_STORE_PATH_MAX];
        book_path(&s_shelf[i], path, sizeof(path));
        struct stat st;
        book_progress_t p;
        s_pct[i] = stat(path, &st) == 0 && book_progress_load(path, (uint32_t)st.st_size, &p) ? (int8_t)p.pct : -1;
    }
}

static int shelf_index_of(const char* book_id) {
    for (int i = 0; i < s_count; i++) if (!strcmp(s_shelf[i].book_id, book_id)) return i;
    return -1;
}

// 番茄更新只在末尾追加章节：把阅读进度记录迁到新文件大小。
// A Tomato update only appends chapters: move the progress record to the new file size.
static void file_hook(const char* path, uint32_t old_size, uint32_t new_size) {
    if (old_size && old_size != new_size) book_progress_resize(path, old_size, new_size);
}

static bool start_op(op_t op, esp_err_t err) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start op %d: %s", op, esp_err_to_name(err));
        return false;
    }
    s_op = op;
    s_last = OP_NONE;
    s_seen_revision = 0;
    return true;
}

// 最近搜索存 NVS，重启后还在。/ Recent searches live in NVS so they survive restarts.
static void load_recent(void) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_recent);
    if (nvs_get_blob(h, "searches", s_recent, &len) != ESP_OK || len != sizeof(s_recent)) memset(s_recent, 0, sizeof(s_recent));
    nvs_close(h);
    for (int i = 0; i < RECENT_QUERIES; i++) s_recent[i][sizeof(s_recent[i]) - 1] = 0;
}

static void save_recent(void) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "searches", s_recent, sizeof(s_recent)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void remember_query(const char* q) {
    int at = RECENT_QUERIES - 1;
    for (int i = 0; i < RECENT_QUERIES; i++) if (!strcmp(s_recent[i], q)) { at = i; break; }
    // 用过的词移到最前。/ Move a reused query to the front.
    memmove(s_recent[1], s_recent[0], sizeof(s_recent[0]) * (size_t)at);
    snprintf(s_recent[0], sizeof(s_recent[0]), "%.47s", q);
    save_recent();
}

/* ---- 列表 / List ---- */

static int list_top(void) {
    if (tomato_sync_busy() && (s_op == OP_CHECK || s_op == OP_UPDATE)) return 460 + SEG_SHIFT;
    return (s_pending ? 420 : 320) + SEG_SHIFT;
}

static int rows_per_page(void) { return (LIST_BOTTOM - list_top()) / ROW_H; }

static int list_pages(void) {
    int per = rows_per_page();
    return s_count ? (s_count + per - 1) / per : 1;
}

static void draw_list(uint8_t* fb) {
    tr_title(fb, "番茄小说");
    bool busy = tomato_sync_busy();
    if (!busy) {
        tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - 150, 116, false);
        tr_text(fb, TR_W - TR_MARGIN, 98, 30, "检查更新", EPD_DRAW_ALIGN_RIGHT, false);
    }
    static const char* const SEG[2] = {"书架", "书城"};
    tr_segment(fb, TR_SEGMENT_RECT, SEG, 2, 0);
    char line[96];
    if (busy && (s_op == OP_CHECK || s_op == OP_UPDATE)) snprintf(line, sizeof(line), "正在%s，可以先去看书", s_op == OP_CHECK ? "检查" : "更新");
    else if (s_status.phase == TOMATO_DONE || s_status.phase == TOMATO_FAILED) snprintf(line, sizeof(line), "%d 本 · %s", s_count, s_status.message);
    else snprintf(line, sizeof(line), "%d 本 · %d 本有新章节", s_count, s_pending);
    tr_fit(line, 28, TR_W - 2 * TR_MARGIN);
    tr_text(fb, TR_MARGIN, 156 + SEG_SHIFT + 6, 28, line, EPD_DRAW_ALIGN_LEFT, false);

    EpdRect field = {TR_MARGIN, 212 + SEG_SHIFT, TR_W - 2 * TR_MARGIN, 80};
    tr_pill(fb, field, "", false, 30);
    tr_icon(fb, TR_ICON_SEARCH, field.x + 50, field.y + 40, false);
    tr_text(fb, field.x + 86, field.y + 22, 30, "搜索书名或作者", EPD_DRAW_ALIGN_LEFT, false);

    if (busy && (s_op == OP_CHECK || s_op == OP_UPDATE)) {
        if (s_status.total) snprintf(line, sizeof(line), "%d / %d　%s", s_status.current, s_status.total, s_status.message);
        else snprintf(line, sizeof(line), "%s", s_status.message);
        tr_fit(line, 40, TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_MARGIN, 316 + SEG_SHIFT, 40, line, EPD_DRAW_ALIGN_LEFT, true);
        if (s_status.phase == TOMATO_DOWNLOADING) {
            snprintf(line, sizeof(line), "正在下载 · %.1f MB", s_status.bytes / 1048576.0);
            tr_text(fb, TR_MARGIN, 370 + SEG_SHIFT, 28, line, EPD_DRAW_ALIGN_LEFT, false);
        } else if (s_status.phase == TOMATO_QUEUED) {
            tr_text(fb, TR_MARGIN, 370 + SEG_SHIFT, 28, "服务器正在生成 TXT", EPD_DRAW_ALIGN_LEFT, false);
        }
        int pct = s_status.total ? (s_status.current - 1) * 100 / s_status.total : 0;
        tr_progress(fb, TR_MARGIN, 412 + SEG_SHIFT, TR_W - 2 * TR_MARGIN - 110, pct);
        tr_text(fb, TR_W - TR_MARGIN, 396 + SEG_SHIFT, 30, "取消", EPD_DRAW_ALIGN_RIGHT, false);
        tr_hline(fb, 446 + SEG_SHIFT, TR_MARGIN, TR_W - 2 * TR_MARGIN, false);
    } else if (s_pending) {
        snprintf(line, sizeof(line), "下载全部更新（%d 本）", s_pending);
        tr_pill(fb, (EpdRect){TR_MARGIN, 316 + SEG_SHIFT, TR_W - 2 * TR_MARGIN, 80}, line, true, 30);
    }

    int top = list_top();
    if (!s_count) {
        tr_wrap(fb, TR_MARGIN, top + 20, TR_W - 2 * TR_MARGIN, 30, 46,
                s_has_config ? "还没有番茄小说。点上面的搜索框找书。"
                             : "还没设置番茄服务器。在 TF 卡 tomato/config.json 里填写服务器地址和密码，格式见“设置”。", 4);
        return;
    }
    int per = rows_per_page();
    for (int row = 0; row < per; row++) {
        int k = s_page * per + row;
        if (k >= s_count) break;
        const tomato_book_t* b = &s_shelf[s_order[k]];
        int y = top + row * ROW_H;
        bool pending = b->remote > b->chapters;
        char mark[24];
        if (pending) snprintf(mark, sizeof(mark), "+%d 章", b->remote - b->chapters);
        else if (s_pct[s_order[k]] < 0) snprintf(mark, sizeof(mark), "未读");
        else if (s_pct[s_order[k]] >= 100) snprintf(mark, sizeof(mark), "读完");
        else snprintf(mark, sizeof(mark), "已读 %d%%", s_pct[s_order[k]]);
        int mark_w = ttf_text_width_px(28, mark);
        snprintf(line, sizeof(line), "%s", b->title);
        tr_fit(line, 34, TR_W - 2 * TR_MARGIN - mark_w - 24);
        tr_text(fb, TR_MARGIN, y + 8, 34, line, EPD_DRAW_ALIGN_LEFT, pending);
        snprintf(line, sizeof(line), "%s · %d 章", b->author, b->chapters);
        tr_fit(line, 26, TR_W - 2 * TR_MARGIN - mark_w - 24);
        tr_text(fb, TR_MARGIN, y + 52, 26, line, EPD_DRAW_ALIGN_LEFT, false);
        tr_text(fb, TR_W - TR_MARGIN, y + 24, 28, mark, EPD_DRAW_ALIGN_RIGHT, pending);
        tr_hline(fb, y + ROW_H - 4, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(fb, PAGER_Y, s_page, list_pages());
}

/* ---- 搜索 / Search ---- */

static void draw_field(uint8_t* fb, const char* text, bool caret) {
    tr_text(fb, TR_MARGIN, 94, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect f = FIELD_RECT;
    tr_pill(fb, f, "", false, 30);
    tr_icon(fb, TR_ICON_SEARCH, f.x + 44, f.y + 40, false);
    char shown[TR_KBD_TEXT_MAX];
    // 太长时显示末尾，按字符边界。/ Show the tail when too long, on a character boundary.
    snprintf(shown, sizeof(shown), "%s", tr_tail_fit(text, 34, f.width - 120));
    tr_text(fb, f.x + 78, f.y + 20, 34, shown, EPD_DRAW_ALIGN_LEFT, false);
    if (caret) {
        int cx = f.x + 80 + ttf_text_width_px(34, shown);
        epd_fill_rect((EpdRect){cx, f.y + 18, 3, 44}, UI_GRAY_BLACK, fb);
    }
}

static void draw_search(uint8_t* fb) {
    draw_field(fb, s_kbd.text, true);
    if (s_recent[0][0]) {
        tr_text(fb, TR_MARGIN, 208, 28, "最近搜索", EPD_DRAW_ALIGN_LEFT, false);
        int x = TR_MARGIN;
        for (int i = 0; i < RECENT_QUERIES && s_recent[i][0]; i++) {
            int w = ttf_text_width_px(28, s_recent[i]) + 48;
            if (x + w > TR_W - TR_MARGIN) break;
            tr_pill(fb, (EpdRect){x, 254, w, 54}, s_recent[i], false, 28);
            x += w + 12;
        }
    }
    tr_kbd_draw(fb, &s_kbd);
}

static int recent_hit(uint16_t x, uint16_t y) {
    if (y < 254 || y > 308) return -1;
    int cx = TR_MARGIN;
    for (int i = 0; i < RECENT_QUERIES && s_recent[i][0]; i++) {
        int w = ttf_text_width_px(28, s_recent[i]) + 48;
        if (x >= cx && x < cx + w) return i;
        cx += w + 12;
    }
    return -1;
}

// 只有真正开始搜索了才清掉旧结果。/ Clear old results only once a search actually starts.
static bool submit_search(const char* q) {
    if (!q[0] || tomato_sync_busy()) return false;
    if (!start_op(OP_SEARCH, tomato_sync_start_search(q))) return false;
    snprintf(s_query, sizeof(s_query), "%s", q);
    remember_query(q);
    s_hit_count = 0;
    s_result_page = 0;
    s_view = V_RESULTS;
    return true;
}

/* ---- 结果 / Results ---- */

static void format_meta(const tomato_hit_t* h, char* out, size_t cap) {
    char words[24] = "";
    if (h->words >= 10000) snprintf(words, sizeof(words), " · %lu 万字", (unsigned long)(h->words / 10000));
    snprintf(out, cap, "%s · %d 章%s", h->finished ? "完结" : "连载", h->chapters, words);
}

static void draw_results(uint8_t* fb) {
    draw_field(fb, s_query, false);
    char line[96];
    bool searching = tomato_sync_busy() && s_op == OP_SEARCH;
    if (searching) snprintf(line, sizeof(line), "%s", s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : "正在搜索…");
    else if (s_status.phase == TOMATO_FAILED && s_last == OP_SEARCH) snprintf(line, sizeof(line), "%s", s_status.message);
    else if (!s_hit_count) snprintf(line, sizeof(line), "没有找到");
    else snprintf(line, sizeof(line), "%d 个结果", s_hit_count);
    tr_text(fb, TR_MARGIN, 204, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    for (int row = 0; row < RESULTS_PER_PAGE; row++) {
        int i = s_result_page * RESULTS_PER_PAGE + row;
        if (i >= s_hit_count) break;
        const tomato_hit_t* h = &s_hits[i];
        int y = RESULT_TOP + row * RESULT_ROW_H;
        tr_mini_cover(fb, (EpdRect){TR_MARGIN, y + 2, 70, 96}, h->title, false);
        bool owned = shelf_index_of(h->book_id) >= 0;
        int mark_w = owned ? ttf_text_width_px(26, "已在书库") + 16 : 0;
        snprintf(line, sizeof(line), "%s", h->title);
        tr_fit(line, 34, TR_W - TR_MARGIN - 136 - mark_w);
        tr_text(fb, 136, y, 34, line, EPD_DRAW_ALIGN_LEFT, true);
        if (owned) tr_text(fb, TR_W - TR_MARGIN, y + 6, 26, "已在书库", EPD_DRAW_ALIGN_RIGHT, false);
        snprintf(line, sizeof(line), "%s%s%s", h->author, h->category[0] ? " · " : "", h->category);
        tr_fit(line, 26, TR_W - TR_MARGIN - 136);
        tr_text(fb, 136, y + 48, 26, line, EPD_DRAW_ALIGN_LEFT, false);
        format_meta(h, line, sizeof(line));
        tr_text(fb, 136, y + 84, 26, line, EPD_DRAW_ALIGN_LEFT, false);
        tr_hline(fb, y + RESULT_ROW_H - 20, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(fb, PAGER_Y, s_result_page, (s_hit_count + RESULTS_PER_PAGE - 1) / RESULTS_PER_PAGE);
}

/* ---- 详情 / Detail ---- */

static void draw_detail(uint8_t* fb) {
    const tomato_hit_t* h = &s_hits[s_detail];
    tr_text(fb, TR_MARGIN, 90, 30, "‹ 搜索结果", EPD_DRAW_ALIGN_LEFT, false);
    tr_cover(fb, (EpdRect){232, 160, 220, 296}, h->title, h->author, "番茄", false);
    char line[128];
    snprintf(line, sizeof(line), "%s", h->title);
    tr_fit(line, 48, TR_W - 2 * TR_MARGIN);
    tr_text(fb, TR_W / 2, 488, 48, line, EPD_DRAW_ALIGN_CENTER, true);
    tr_text(fb, TR_W / 2, 554, 30, h->author, EPD_DRAW_ALIGN_CENTER, false);

    // 四格信息。/ Four stat cells.
    const int top = 626, cell = (TR_W - 2 * TR_MARGIN) / 4;
    tr_hline(fb, top, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    tr_hline(fb, top + 116, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    char words[16], chapters[16];
    snprintf(chapters, sizeof(chapters), "%d", h->chapters);
    if (h->words >= 10000) snprintf(words, sizeof(words), "%lu 万", (unsigned long)(h->words / 10000));
    else snprintf(words, sizeof(words), "%lu", (unsigned long)h->words);
    const char* labels[4] = {"分类", "状态", "章节", "字数"};
    const char* values[4] = {h->category[0] ? h->category : "—", h->finished ? "完结" : "连载", chapters, words};
    for (int i = 0; i < 4; i++) {
        int cx = TR_MARGIN + i * cell + cell / 2;
        if (i) epd_fill_rect((EpdRect){TR_MARGIN + i * cell, top + 20, 2, 76}, TR_GRAY_LINE, fb);
        tr_text(fb, cx, top + 16, 24, labels[i], EPD_DRAW_ALIGN_CENTER, false);
        tr_text(fb, cx, top + 52, 32, values[i], EPD_DRAW_ALIGN_CENTER, true);
    }
    if (h->abstract[0]) tr_wrap(fb, TR_MARGIN, 772, TR_W - 2 * TR_MARGIN, 28, 44, h->abstract, 4);

    EpdRect btn = DETAIL_BTN;
    bool downloading = tomato_sync_busy() && s_op == OP_DOWNLOAD;
    if (downloading) {
        const char* what = s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi"
                         : s_status.phase == TOMATO_QUEUED ? "服务器正在生成 TXT" : "正在下载";
        if (s_status.phase == TOMATO_DOWNLOADING) snprintf(line, sizeof(line), "%s · %.1f MB", what, s_status.bytes / 1048576.0);
        else snprintf(line, sizeof(line), "%s…", what);
        tr_text(fb, TR_MARGIN, btn.y - 4, 28, line, EPD_DRAW_ALIGN_LEFT, false);
        tr_progress(fb, TR_MARGIN, btn.y + 54, btn.width - 110,
                    s_status.phase == TOMATO_DOWNLOADING ? 80 : s_status.phase == TOMATO_QUEUED ? 40 : 10);
        tr_text(fb, TR_W - TR_MARGIN, btn.y + 38, 30, "取消", EPD_DRAW_ALIGN_RIGHT, false);
        return;
    }
    if (s_last == OP_DOWNLOAD && s_status.phase == TOMATO_FAILED) {
        tr_text(fb, TR_MARGIN, btn.y - 50, 28, s_status.message, EPD_DRAW_ALIGN_LEFT, false);
    }
    bool owned = shelf_index_of(h->book_id) >= 0;
    tr_pill(fb, btn, owned ? "开始阅读" : "下载到书库", true, 32);
}

/* ---- 页面框架 / Page plumbing ---- */

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    switch (s_view) {
    case V_LIST: draw_list(fb); break;
    case V_SEARCH: draw_search(fb); break;
    case V_RESULTS: draw_results(fb); break;
    case V_DETAIL: draw_detail(fb); break;
    }
    if (s_view != V_SEARCH) tr_tab_bar(fb, TR_TAB_TOMATO);
}

// 局部 DU 刷新；累计太多次就整页刷一次清残影。/ Partial DU; after too many, one page refresh clears ghosts.
static app_redraw_t area_redraw(app_ctx_t* ctx, EpdRect area) {
    if (++s_du_count >= DU_SETTLE_MAX) {
        s_du_count = 0;
        return APP_REDRAW_PAGE;
    }
    render(ctx, ctx->fb);
    s_area = area;
    return APP_REDRAW_AREA;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return s_area;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_shelf) s_shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(tomato_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tomato_sync_set_file_hook(file_hook);
    tomato_sync_set_open_check(app_book_path_is_open);
    tr_nav_set_tomato_page(&app_tr_tomato);
    static bool recent_loaded;
    if (!recent_loaded) { load_recent(); recent_loaded = true; }
    load_shelf();
    tomato_sync_get_status(&s_status);
    s_seen_revision = s_status.revision;
    // 后台还在跑的话保持原来的视图，否则回到书架。/ Keep the view if work is still running; otherwise show the shelf.
    if (!tomato_sync_busy() && s_view != V_DETAIL && s_view != V_RESULTS) s_view = V_LIST;
    if (s_view == V_SEARCH) s_view = V_LIST;
    s_page = 0;
    s_du_count = 0;
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    // 词库留在 PSRAM（约 1 MB），下次打开键盘不用再读 1 秒。/ Keep the ~1 MB lexicon in PSRAM so the next keyboard opens instantly.
    if (s_view == V_SEARCH) s_view = V_LIST;
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    tomato_sync_cancel();
    pinyin_ime_unload();
    s_view = V_LIST;
    s_count = 0;
}

static void open_search(const char* initial) {
    tr_kbd_open(&s_kbd, initial, true, "搜索");
    s_view = V_SEARCH;
}

static app_redraw_t turn_list(int turn) {
    int next = s_page + turn;
    if (next < 0 || next >= list_pages()) return APP_REDRAW_NONE;
    s_page = next;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_list(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    bool busy = tomato_sync_busy();
    if (!busy && y >= 80 && y < 150 && x > TR_W - TR_MARGIN - 200) {
        start_op(OP_CHECK, tomato_sync_start_check());
        tomato_sync_get_status(&s_status);
        return APP_REDRAW_PAGE;
    }
    if (tr_segment_hit(TR_SEGMENT_RECT, 2, x, y) == 1) { ctx->request_app = &app_tr_store; return APP_REDRAW_NONE; }
    if (y >= 212 + SEG_SHIFT && y < 292 + SEG_SHIFT) { open_search(""); return APP_REDRAW_PAGE; }
    if (busy && (s_op == OP_CHECK || s_op == OP_UPDATE) && y >= 380 + SEG_SHIFT && y < 446 + SEG_SHIFT && x > TR_W - TR_MARGIN - 140) {
        tomato_sync_cancel();
        return APP_REDRAW_NONE;
    }
    if (!busy && s_pending && y >= 316 + SEG_SHIFT && y < 396 + SEG_SHIFT) {
        start_op(OP_UPDATE, tomato_sync_start_update());
        tomato_sync_get_status(&s_status);
        return APP_REDRAW_PAGE;
    }
    int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn_list(pager);
    int top = list_top();
    // 只认画出来的行，行下面的空白不算。/ Only drawn rows count; the gap below them does not.
    if (y >= top && (y - top) / ROW_H < rows_per_page()) {
        int k = s_page * rows_per_page() + (y - top) / ROW_H;
        struct stat st;
        char path[BOOK_STORE_PATH_MAX];
        if (k < s_count) book_path(&s_shelf[s_order[k]], path, sizeof(path));
        // 还没下载到本机的书（WebDAV 合并来的）没有文件，不打开。/ Books merged from WebDAV but not downloaded have no file.
        if (k < s_count && s_shelf[s_order[k]].file[0] && stat(path, &st) == 0) tr_nav_open_book(ctx, path);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_search(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 180 && x < 96) { s_view = s_hit_count ? V_RESULTS : V_LIST; return APP_REDRAW_PAGE; }
    int r = recent_hit(x, y);
    if (r >= 0) {
        char q[sizeof(s_recent[0])];
        memcpy(q, s_recent[r], sizeof(q));
        return submit_search(q) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    tr_kbd_result_t res = tr_kbd_touch(&s_kbd, x, y);
    if (res == TR_KBD_SUBMIT) {
        if (!s_kbd.text[0] || tomato_sync_busy()) return area_redraw(ctx, (EpdRect){0, 70, TR_W, TR_H - 70});
        char q[TR_KBD_TEXT_MAX];
        snprintf(q, sizeof(q), "%s", s_kbd.text);
        return submit_search(q) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (res == TR_KBD_CHANGED) return area_redraw(ctx, (EpdRect){0, 70, TR_W, TR_H - 70});
    return APP_REDRAW_NONE;
}

// 翻一整页；到头不动。/ Turn one whole page; stop at the ends.
static app_redraw_t turn_results(int turn) {
    int pages = (s_hit_count + RESULTS_PER_PAGE - 1) / RESULTS_PER_PAGE;
    int next = s_result_page + turn;
    if (next < 0 || next >= pages) return APP_REDRAW_NONE;
    s_result_page = next;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_results(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)ctx;
    if (y >= 84 && y < 164) { open_search(s_query); return APP_REDRAW_PAGE; }
    int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn_results(pager);
    if (y >= RESULT_TOP && y < RESULT_TOP + RESULTS_PER_PAGE * RESULT_ROW_H) {
        int i = s_result_page * RESULTS_PER_PAGE + (y - RESULT_TOP) / RESULT_ROW_H;
        if (i < s_hit_count) { s_detail = i; s_view = V_DETAIL; return APP_REDRAW_PAGE; }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_detail(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 150 && x < 300) { s_view = V_RESULTS; return APP_REDRAW_PAGE; }
    EpdRect btn = DETAIL_BTN;
    bool downloading = tomato_sync_busy() && s_op == OP_DOWNLOAD;
    if (downloading) {
        if (y >= btn.y && y < btn.y + btn.height && x > TR_W - TR_MARGIN - 140) tomato_sync_cancel();
        return APP_REDRAW_NONE;
    }
    if (!ui_rect_hit(btn, x, y)) return APP_REDRAW_NONE;
    const tomato_hit_t* h = &s_hits[s_detail];
    int owned = shelf_index_of(h->book_id);
    if (owned >= 0) {
        char path[BOOK_STORE_PATH_MAX];
        book_path(&s_shelf[owned], path, sizeof(path));
        tr_nav_open_book(ctx, path);
        return APP_REDRAW_NONE;
    }
    if (start_op(OP_DOWNLOAD, tomato_sync_start_download(h))) tomato_sync_get_status(&s_status);
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_view != V_SEARCH && tr_nav_touch(ctx, x, y, TR_TAB_TOMATO)) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_LIST: return touch_list(ctx, x, y);
    case V_SEARCH: return touch_search(ctx, x, y);
    case V_RESULTS: return touch_results(ctx, x, y);
    case V_DETAIL: return touch_detail(ctx, x, y);
    }
    return APP_REDRAW_NONE;
}

// 键盘照旧按下即响应；其余页松手才算轻点。书架和搜索结果左滑 / 上滑下一页，右滑 / 下滑上一页，不滚动。
// The keyboard still acts on press; other views act on a tap. Shelf and results turn a whole page per swipe.
static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_view == V_SEARCH) return ev->type == UI_GESTURE_PRESS ? tap(ctx, ev->x0, ev->y0) : APP_REDRAW_NONE;
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    const int turn = ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U ? 1
                   : ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D ? -1 : 0;
    if (!turn) return APP_REDRAW_NONE;
    if (s_view == V_LIST) return turn_list(turn);
    if (s_view == V_RESULTS) return turn_results(turn);
    return APP_REDRAW_NONE;
}

// KEY1 返回上一层。/ KEY1 goes back one level.
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_SEARCH: s_view = s_hit_count ? V_RESULTS : V_LIST; return APP_REDRAW_PAGE;
    case V_RESULTS: s_view = V_LIST; return APP_REDRAW_PAGE;
    case V_DETAIL: s_view = V_RESULTS; return APP_REDRAW_PAGE;
    default: return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    tr_time_poll();
    s_poll_ms = ctx->now_ms;
    if (s_op == OP_NONE) return APP_REDRAW_NONE;
    bool busy = tomato_sync_busy();
    tomato_sync_get_status(&s_status);
    if (!busy) {
        // 操作结束：取结果、重读书架、整页刷新。/ Operation finished: collect results, reload the shelf, full page.
        s_last = s_op;
        s_op = OP_NONE;
        if (s_last == OP_SEARCH) s_hit_count = tomato_sync_hits(s_hits, TOMATO_SEARCH_MAX);
        load_shelf();
        if (s_page >= list_pages()) s_page = list_pages() - 1;
        s_du_count = 0;
        s_seen_revision = s_status.revision;
        return s_view == V_SEARCH ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
    }
    bool changed = s_status.revision != s_seen_revision;
    bool stale = ctx->now_ms - s_busy_draw_ms >= BUSY_REDRAW_MS && s_status.phase == TOMATO_DOWNLOADING;
    if (!changed && !stale) return APP_REDRAW_NONE;
    s_seen_revision = s_status.revision;
    s_busy_draw_ms = ctx->now_ms;
    if (s_view == V_LIST) return area_redraw(ctx, (EpdRect){0, 150, TR_W, LIST_BOTTOM - 150});
    if (s_view == V_DETAIL) return area_redraw(ctx, (EpdRect){0, 900, TR_W, 160});
    if (s_view == V_RESULTS) return area_redraw(ctx, (EpdRect){0, 190, TR_W, 60});
    return APP_REDRAW_NONE;
}

const app_desc_t app_tr_tomato = {
    .title = "番茄小说",
    .detail = "搜索、下载、检查更新",
    .enter_full = true,
    .tab_nav = true,
    .blocks_idle_sleep = false,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
