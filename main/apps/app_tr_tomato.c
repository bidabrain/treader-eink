/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 番茄搜索：书城最上面的搜索栏进来，搜索键盘、搜索结果、书籍详情；返回回书城。番茄书架（检查更新、下载更新）
 * 并进了书库（app_book.c），这里不再有书架。
 * 联网操作全部交给 components/tomato 的后台任务，这里只发起、轮询和显示。
 *
 * Tomato search: opened from the bookstore's top search bar, with the search keyboard, results and book detail;
 * backing out returns to the store. The Tomato shelf (check and download updates) moved into the library
 * (app_book.c), so this page has no shelf any more.
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
// 长度都随显示大小放大；靠下的位置跟着标签栏走，每页几行按剩下的高度算。
// Lengths scale with the display size; positions near the bottom follow the tab bar and rows per page follow the
// height that is left.
#define RESULT_ROW_H TR_S(140)
#define RESULT_TOP TR_S(254)
#define PAGER_Y (TR_TABS_Y - TR_S(54))
#define RESULTS_PER_PAGE results_per_page()
#define DETAIL_BTN (EpdRect){TR_MARGIN, TR_TABS_Y - TR_S(134), TR_W - 2 * TR_MARGIN, TR_S(88)}
#define FIELD_X TR_S(96)
#define FIELD_RECT (EpdRect){FIELD_X, TR_S(84), TR_W - FIELD_X - TR_MARGIN, TR_S(80)}
#define RESULT_TEXT_X (TR_MARGIN + TR_S(96))   // 结果行封面右边的文字 / Result text right of the mini cover
#define RECENT_QUERIES 3

// 100% 时 5 行。/ Five rows at 100%.
static int results_per_page(void) { return (PAGER_Y - RESULT_TOP + TR_S(20)) / RESULT_ROW_H; }

extern const app_desc_t app_tr_store;
extern const app_desc_t app_tr_tomato;

typedef enum { V_SEARCH = 0, V_RESULTS, V_DETAIL } view_t;
// 书城请求下次进页打开搜索。/ The store asked to open search on entry.
static bool s_search_request;
static void open_search(const char* initial);
typedef enum { OP_NONE = 0, OP_SEARCH, OP_DOWNLOAD } op_t;

static view_t s_view;
static tomato_book_t* s_shelf;          // PSRAM
static int s_count;

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
// 详情页点“下载到书库”没能开始时的原因（另一本在下、后台在忙）。/ Why "下载到书库" on a detail page could not start
// (another book downloading, the worker busy).
static char s_detail_note[64];
// 正在下载（或刚下载完）的那本书；进度、“取消”和失败原因只显示在这本的详情上。
// The book downloading (or just downloaded); progress, "cancel" and the failure show on its detail only.
static char s_dl_id[sizeof(((tomato_hit_t*)0)->book_id)];
static int s_du_count;
static EpdRect s_area;

/* ---- 数据 / Data ---- */

static void book_path(const tomato_book_t* b, char* out, size_t cap) {
    snprintf(out, cap, TOMATO_BOOKS_DIR "/%s", b->file);
}

// 只用来判断搜到的书在不在书库里。/ Only used to tell whether a hit is already in the library.
static void load_shelf(void) {
    s_count = 0;
    if (s_shelf) tomato_shelf_load(s_shelf, TOMATO_SHELF_MAX, &s_count);
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

// 本页任务的序号；序号对不上就是别的页的任务。/ Our job's sequence; a mismatch means another page's job.
static uint32_t s_op_seq;
// 后台在停时记下的搜索词，停下后马上搜。/ A search queued while the worker stops, run as soon as it is free.
static char s_pending_query[96];

static bool start_op(op_t op, esp_err_t err) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start op %d: %s", op, esp_err_to_name(err));
        return false;
    }
    s_op = op;
    s_op_seq = tomato_sync_seq();
    s_last = OP_NONE;
    s_seen_revision = 0;
    return true;
}

// 刚结束的是本页的这个任务、之后也没有别的任务开始：这时全局状态才是它的结果。
// The op that just ended is ours and no other job started since: only then is the global status its result.
static bool last_is(op_t op) { return op != OP_NONE && s_last == op && tomato_sync_seq() == s_op_seq; }

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
    // 先按字符边界截到存得下的长度，查重也用截过的。/ Cut to the stored size on a character boundary first; the
    // duplicate check uses the cut form too.
    char cut[sizeof(s_recent[0])];
    size_t n = strlen(q);
    if (n >= sizeof(cut)) {
        n = sizeof(cut) - 1;
        while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
    }
    memcpy(cut, q, n);
    cut[n] = 0;
    int at = RECENT_QUERIES - 1;
    for (int i = 0; i < RECENT_QUERIES; i++) if (!strcmp(s_recent[i], cut)) { at = i; break; }
    // 用过的词移到最前。/ Move a reused query to the front.
    memmove(s_recent[1], s_recent[0], sizeof(s_recent[0]) * (size_t)at);
    memcpy(s_recent[0], cut, sizeof(cut));
    save_recent();
}

/* ---- 搜索 / Search ---- */

static void draw_field(uint8_t* fb, const char* text, bool caret) {
    tr_text(fb, TR_MARGIN, TR_S(94), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect f = FIELD_RECT;
    tr_pill(fb, f, "", false, TR_S(30));
    tr_icon(fb, TR_ICON_SEARCH, f.x + TR_S(44), f.y + TR_S(40), false);
    char shown[TR_KBD_TEXT_MAX];
    // 太长时显示末尾，按字符边界。/ Show the tail when too long, on a character boundary.
    snprintf(shown, sizeof(shown), "%s", tr_tail_fit(text, TR_S(34), f.width - TR_S(120)));
    tr_text(fb, f.x + TR_S(78), f.y + TR_S(20), TR_S(34), shown, EPD_DRAW_ALIGN_LEFT, false);
    if (caret) {
        int cx = f.x + TR_S(80) + ttf_text_width_px(TR_S(34), shown);
        epd_fill_rect((EpdRect){cx, f.y + TR_S(18), 3, TR_S(44)}, UI_GRAY_BLACK, fb);
    }
}

// 最近搜索胶囊：绘制和命中共用。键盘放大后往上长，胶囊仍在它上面。/ Recent-search chips, shared by drawing and hit
// testing. The keyboard grows upward with the display size; the chips still sit above it.
#define RECENT_Y TR_S(254)
#define RECENT_H TR_S(54)
// 排出放得下的胶囊，返回个数；越过右边距的那个和之后的都不画、也点不到。
// Lay out the chips that fit and return how many; the first one past the right margin and all after it are neither
// drawn nor tappable.
static int recent_layout(EpdRect rects[RECENT_QUERIES]) {
    int n = 0, x = TR_MARGIN;
    for (int i = 0; i < RECENT_QUERIES && s_recent[i][0]; i++) {
        const int w = ttf_text_width_px(TR_S(28), s_recent[i]) + TR_S(48);
        if (x + w > TR_W - TR_MARGIN) break;
        rects[n++] = (EpdRect){x, RECENT_Y, w, RECENT_H};
        x += w + TR_S(12);
    }
    return n;
}

static void draw_search(uint8_t* fb) {
    draw_field(fb, s_kbd.text, true);
    if (s_recent[0][0]) {
        tr_text_dim(fb, TR_MARGIN, TR_S(208), TR_S(28), "最近搜索", EPD_DRAW_ALIGN_LEFT);
        EpdRect rects[RECENT_QUERIES];
        const int n = recent_layout(rects);
        for (int i = 0; i < n; i++) tr_pill(fb, rects[i], s_recent[i], false, TR_S(28));
    }
    tr_kbd_draw(fb, &s_kbd);
}

static int recent_hit(uint16_t x, uint16_t y) {
    if (y < RECENT_Y || y > RECENT_Y + RECENT_H) return -1;
    EpdRect rects[RECENT_QUERIES];
    const int n = recent_layout(rects);
    for (int i = 0; i < n; i++)
        if (x >= rects[i].x && x < rects[i].x + rects[i].width) return i;
    return -1;
}

// 只有真正开始搜索了才清掉旧结果。/ Clear old results only once a search actually starts.
static bool submit_search(const char* q) {
    // 前台优先：浏览任务让路；在下载或检查更新就不搜。/ Foreground first: browsing jobs yield; not while downloading.
    if (!q[0]) return false;
    const tomato_preempt_t room = tomato_sync_preempt();
    if (room == TOMATO_PREEMPT_STOPPING) {
        snprintf(s_pending_query, sizeof(s_pending_query), "%s", q);
        return false;
    }
    if (room != TOMATO_PREEMPT_FREE) return false;
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
    char line[160];
    bool searching = tomato_sync_busy() && s_op == OP_SEARCH;
    if (searching) snprintf(line, sizeof(line), "%s", s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : "正在搜索…");
    else if (s_status.phase == TOMATO_FAILED && last_is(OP_SEARCH)) snprintf(line, sizeof(line), "%s", s_status.message);
    else if (!s_hit_count) snprintf(line, sizeof(line), "没有找到");
    else snprintf(line, sizeof(line), "%d 个结果", s_hit_count);
    tr_message(fb, TR_MARGIN, TR_S(204), TR_S(196), TR_W - 2 * TR_MARGIN, TR_S(28), line, EPD_DRAW_ALIGN_LEFT, false);
    // 搜索中点右边“取消”。/ Tap "Cancel" on the right while searching.
    if (searching) tr_text(fb, TR_W - TR_MARGIN, TR_S(196), TR_S(30), "取消", EPD_DRAW_ALIGN_RIGHT, true);
    const int per = RESULTS_PER_PAGE, tx = RESULT_TEXT_X;
    for (int row = 0; row < per; row++) {
        int i = s_result_page * per + row;
        if (i >= s_hit_count) break;
        const tomato_hit_t* h = &s_hits[i];
        int y = RESULT_TOP + row * RESULT_ROW_H;
        tr_mini_cover(fb, (EpdRect){TR_MARGIN, y + TR_S(2), TR_S(70), TR_S(96)}, h->title, false);
        bool owned = shelf_index_of(h->book_id) >= 0;
        int mark_w = owned ? ttf_text_width_px(TR_S(24), "已在书库") + TR_S(24) + TR_S(16) : 0;
        snprintf(line, sizeof(line), "%s", h->title);
        tr_fit(line, TR_S(34), TR_W - TR_MARGIN - tx - mark_w);
        tr_text(fb, tx, y, TR_S(34), line, EPD_DRAW_ALIGN_LEFT, true);
        if (owned) tr_badge(fb, TR_W - TR_MARGIN, y + TR_S(6), "已在书库", false, TR_S(24));
        snprintf(line, sizeof(line), "%s%s%s", h->author, h->category[0] ? " · " : "", h->category);
        tr_fit(line, TR_S(26), TR_W - TR_MARGIN - tx);
        tr_text_dim(fb, tx, y + TR_S(48), TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
        format_meta(h, line, sizeof(line));
        tr_text_dim(fb, tx, y + TR_S(84), TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
        tr_hline(fb, y + RESULT_ROW_H - TR_S(20), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(fb, PAGER_Y, s_result_page, (s_hit_count + RESULTS_PER_PAGE - 1) / RESULTS_PER_PAGE);
}

/* ---- 详情 / Detail ---- */

// 当前详情页这本书就是下载的那本。/ The detail page shows the book being downloaded.
static bool detail_is_dl(void) {
    return s_dl_id[0] && s_detail >= 0 && s_detail < s_hit_count && !strcmp(s_hits[s_detail].book_id, s_dl_id);
}

// 下载条：知道大小按已收占比（最多 99%），不知道就停在 80%。/ Download bar: received over expected when the size is
// known (at most 99%), otherwise held at 80%.
static int dl_pct(const tomato_status_t* st) {
    if (!st->expect) return 80;
    return st->bytes >= st->expect ? 99 : (int)((uint64_t)st->bytes * 100 / st->expect);
}

static void draw_detail(uint8_t* fb) {
    const tomato_hit_t* h = &s_hits[s_detail];
    tr_text(fb, TR_MARGIN, TR_S(90), TR_S(30), "‹ 搜索结果", EPD_DRAW_ALIGN_LEFT, false);
    char line[128];
    int top;   // 信息卡片顶边 / Top of the stat card
    if (ui_scale_pct == 100) {
        tr_cover(fb, (EpdRect){232, 160, 220, 296}, h->title, h->author, "番茄", false);
        snprintf(line, sizeof(line), "%s", h->title);
        tr_fit(line, 48, TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_W / 2, 488, 48, line, EPD_DRAW_ALIGN_CENTER, true);
        tr_text_dim(fb, TR_W / 2, 554, 30, h->author, EPD_DRAW_ALIGN_CENTER);
        top = 626;
    } else {
        // 放大后居中摆放放不下：封面缩小放左边，书名作者在右边，省下的高度留给简介。
        // Scaled up, the centered stack no longer fits: a smaller cover on the left, title and author on its right,
        // leaving height for the synopsis.
        const EpdRect cover = {TR_MARGIN, TR_S(160), 180, 242};
        tr_cover(fb, cover, h->title, h->author, "番茄", false);
        const int x = cover.x + cover.width + TR_S(24), w = TR_W - TR_MARGIN - x;
        const int lines = tr_wrap(fb, x, cover.y + TR_S(8), w, TR_S(44), TR_S(56), h->title, 2);
        snprintf(line, sizeof(line), "%s", h->author);
        tr_fit(line, TR_S(30), w);
        tr_text_dim(fb, x, cover.y + TR_S(8) + lines * TR_S(56) + TR_S(12), TR_S(30), line, EPD_DRAW_ALIGN_LEFT);
        top = cover.y + cover.height + TR_S(30);
    }

    // 四格信息放在浅灰卡片里（下载进度的 DU 区域在它下面，不碰灰底）。/ Four stat cells on a light gray card (the
    // download progress DU area is below it and never touches the gray).
    const int cell = (TR_W - 2 * TR_MARGIN) / 4;
    ui_fill_round_rect(fb, (EpdRect){TR_MARGIN - 8, top, TR_W - 2 * TR_MARGIN + 16, TR_S(116)}, TR_S(24), TR_GRAY_SURFACE);
    char words[16], chapters[16];
    snprintf(chapters, sizeof(chapters), "%d", h->chapters);
    if (h->words >= 10000) snprintf(words, sizeof(words), "%lu 万", (unsigned long)(h->words / 10000));
    else snprintf(words, sizeof(words), "%lu", (unsigned long)h->words);
    const char* labels[4] = {"分类", "状态", "章节", "字数"};
    const char* values[4] = {h->category[0] ? h->category : "—", h->finished ? "完结" : "连载", chapters, words};
    for (int i = 0; i < 4; i++) {
        int cx = TR_MARGIN + i * cell + cell / 2;
        if (i) epd_fill_rect((EpdRect){TR_MARGIN + i * cell, top + TR_S(20), 2, TR_S(76)}, TR_GRAY_LINE, fb);
        tr_text_on(fb, cx, top + TR_S(16), TR_S(24), labels[i], EPD_DRAW_ALIGN_CENTER, false, TR_GRAY_SECONDARY,
                   TR_GRAY_SURFACE);
        // 格子宽不变：放大后长分类名缩小字号放进格子。/ Cells keep their width: a long category steps its size down.
        int px = TR_S(32);
        while (px > TR_S(24) && ttf_text_width_px(px, values[i]) > cell - 8) px -= 2;
        tr_text_on(fb, cx, top + TR_S(52) + (TR_S(32) - px) / 2, px, values[i], EPD_DRAW_ALIGN_CENTER, true,
                   UI_GRAY_BLACK, TR_GRAY_SURFACE);
    }
    // 下载失败时简介少显示两行，给两行的失败原因让位；放大后按按钮上面剩下的高度再少几行。
    // On a failed download the synopsis gives two lines to the cause; scaled up it also drops lines to fit above the
    // button.
    const bool failed = (last_is(OP_DOWNLOAD) && detail_is_dl() && s_status.phase == TOMATO_FAILED &&
                         !(tomato_sync_busy() && s_op == OP_DOWNLOAD)) || s_detail_note[0];
    EpdRect btn = DETAIL_BTN;
    const int abs_y = top + TR_S(146), abs_lh = TR_S(44);
    const int room = ((failed ? btn.y - TR_S(92) - 4 : btn.y - TR_S(12)) - abs_y) / abs_lh;
    const int abs_lines = room < (failed ? 2 : 4) ? room : (failed ? 2 : 4);
    if (h->abstract[0] && abs_lines > 0)
        tr_wrap(fb, TR_MARGIN, abs_y, TR_W - 2 * TR_MARGIN, TR_S(28), abs_lh, h->abstract, abs_lines);

    bool downloading = tomato_sync_busy() && s_op == OP_DOWNLOAD && detail_is_dl();
    if (downloading) {
        const char* what = s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi"
                         : s_status.phase == TOMATO_QUEUED ? "服务器正在生成 TXT" : "正在下载";
        if (s_status.phase == TOMATO_DOWNLOADING && s_status.expect)
            snprintf(line, sizeof(line), "%s · %.1f / %.1f MB", what, s_status.bytes / 1048576.0, s_status.expect / 1048576.0);
        else if (s_status.phase == TOMATO_DOWNLOADING) snprintf(line, sizeof(line), "%s · %.1f MB", what, s_status.bytes / 1048576.0);
        else snprintf(line, sizeof(line), "%s…", what);
        tr_text(fb, TR_MARGIN, btn.y - TR_S(4), TR_S(28), line, EPD_DRAW_ALIGN_LEFT, false);
        tr_progress(fb, TR_MARGIN, btn.y + TR_S(54), btn.width - TR_S(110),
                    s_status.phase == TOMATO_DOWNLOADING ? dl_pct(&s_status) : s_status.phase == TOMATO_QUEUED ? 40 : 10);
        tr_text(fb, TR_W - TR_MARGIN, btn.y + TR_S(38), TR_S(30), "取消", EPD_DRAW_ALIGN_RIGHT, false);
        return;
    }
    if (failed) tr_message(fb, TR_MARGIN, btn.y - TR_S(50), btn.y - TR_S(92), TR_W - 2 * TR_MARGIN, TR_S(28),
                           s_detail_note[0] ? s_detail_note : s_status.message, EPD_DRAW_ALIGN_LEFT, false);
    bool owned = shelf_index_of(h->book_id) >= 0;
    tr_pill(fb, btn, owned ? "开始阅读" : "下载到书库", true, TR_S(32));
}

/* ---- 页面框架 / Page plumbing ---- */

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    switch (s_view) {
    case V_SEARCH: draw_search(fb); break;
    case V_RESULTS: draw_results(fb); break;
    case V_DETAIL: draw_detail(fb); break;
    }
    if (s_view != V_SEARCH) tr_tab_bar(fb, TR_TAB_LIBRARY);
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

// 本页任务结束：取结果、重读书架。on_tick 和 on_enter（在别的页时结束的）共用。
// Our op finished: collect results and reload the shelf. Shared by on_tick and on_enter (it ended while away).
static void finish_op(void) {
    s_last = s_op;
    s_op = OP_NONE;
    if (s_last == OP_SEARCH) s_hit_count = tomato_sync_hits(s_hits, TOMATO_SEARCH_MAX);
    load_shelf();
    s_du_count = 0;
    s_seen_revision = s_status.revision;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_shelf) s_shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(tomato_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tomato_sync_set_file_hook(file_hook);
    tomato_sync_set_open_check(app_book_path_is_open);
    static bool recent_loaded;
    if (!recent_loaded) { load_recent(); recent_loaded = true; }
    tomato_sync_get_status(&s_status);
    s_detail_note[0] = 0;
    // 本页任务在别的页时已经结束：照 on_tick 收下结果。/ Our op ended while we were away: collect it as on_tick does.
    if (s_op != OP_NONE && !tomato_sync_busy() && tomato_sync_seq() == s_op_seq) finish_op();
    else load_shelf();
    // 之后有别的任务开始过：上次结果的状态已被盖掉，不再显示。/ Another job started since: the last result's status
    // was overwritten, so stop showing it.
    if (s_last != OP_NONE && tomato_sync_seq() != s_op_seq) s_last = OP_NONE;
    s_seen_revision = s_status.revision;
    // 后台还在跑、或有结果、详情可看就保持原来的视图，否则打开搜索。/ Keep the results or detail view when there is
    // work running or something to show; otherwise open search.
    const bool ours = tomato_sync_busy() && tomato_sync_seq() == s_op_seq;
    if (!ours) s_op = OP_NONE;
    s_pending_query[0] = 0;
    // 显示大小变了每页结果数也变：结果页码夹回范围。/ Results per page follow the display size: clamp the page.
    const int result_pages = (s_hit_count + RESULTS_PER_PAGE - 1) / RESULTS_PER_PAGE;
    if (s_result_page >= result_pages) s_result_page = result_pages > 0 ? result_pages - 1 : 0;
    s_du_count = 0;
    if (s_search_request || s_view == V_SEARCH) {
        s_search_request = false;
        open_search("");
    }
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    // 词库留在 PSRAM（约 1 MB），下次打开键盘不用再读 1 秒。/ Keep the ~1 MB lexicon in PSRAM so the next keyboard opens instantly.
    // 搜索页下次进来重新打开键盘。/ The search view reopens its keyboard on the next entry.
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    tomato_sync_cancel();
    pinyin_ime_unload();
    s_view = V_SEARCH;
    s_count = 0;
}

static void open_search(const char* initial) {
    tr_kbd_open(&s_kbd, initial, true, "搜索");
    s_view = V_SEARCH;
}

// 搜索从书城进来（书城最上面的搜索栏）：下次进页直接开搜索。/ Search opened from the bookstore's top bar: open it on
// the next entry.
void tr_tomato_open_search(void) { s_search_request = true; }

// 从搜索或结果退出：回书城。/ Back out of search or results: return to the store.
static app_redraw_t leave_search(app_ctx_t* ctx) {
    s_view = V_SEARCH;
    ctx->request_app = &app_tr_store;
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_search(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(180) && x < FIELD_X) {
        if (s_hit_count) { s_view = V_RESULTS; return APP_REDRAW_PAGE; }
        return leave_search(ctx);
    }
    int r = recent_hit(x, y);
    if (r >= 0) {
        char q[sizeof(s_recent[0])];
        memcpy(q, s_recent[r], sizeof(q));
        return submit_search(q) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    tr_kbd_result_t res = tr_kbd_touch(&s_kbd, x, y);
    if (res == TR_KBD_SUBMIT) {
        if (!s_kbd.text[0]) return area_redraw(ctx, (EpdRect){0, TR_S(70), TR_W, TR_H - TR_S(70)});
        char q[TR_KBD_TEXT_MAX];
        snprintf(q, sizeof(q), "%s", s_kbd.text);
        return submit_search(q) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (res == TR_KBD_CHANGED) return area_redraw(ctx, (EpdRect){0, TR_S(70), TR_W, TR_H - TR_S(70)});
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
    // 输入框左边的“‹”是返回，和搜索页同一块区域。/ The "‹" left of the field goes back, same zone as the search view.
    if (y < TR_S(180) && x < FIELD_X) return leave_search(ctx);
    if (y >= TR_S(84) && y < TR_S(164)) { open_search(s_query); return APP_REDRAW_PAGE; }
    if (tomato_sync_busy() && s_op == OP_SEARCH && y >= TR_S(180) && y < TR_S(250) &&
        x > TR_W - TR_MARGIN - TR_S(140)) {
        tomato_sync_cancel();
        return APP_REDRAW_PAGE;
    }
    int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn_results(pager);
    if (y >= RESULT_TOP && y < RESULT_TOP + RESULTS_PER_PAGE * RESULT_ROW_H) {
        int i = s_result_page * RESULTS_PER_PAGE + (y - RESULT_TOP) / RESULT_ROW_H;
        if (i < s_hit_count) { s_detail = i; s_view = V_DETAIL; s_detail_note[0] = 0; return APP_REDRAW_PAGE; }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_detail(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(150) && x < TR_S(300)) { s_view = V_RESULTS; return APP_REDRAW_PAGE; }
    EpdRect btn = DETAIL_BTN;
    bool downloading = tomato_sync_busy() && s_op == OP_DOWNLOAD && detail_is_dl();
    if (downloading) {
        if (y >= btn.y && y < btn.y + btn.height && x > TR_W - TR_MARGIN - TR_S(140)) tomato_sync_cancel();
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
    // 后台忙就说原因，不悄悄没反应：另一本在下就等它下完；别的页的浏览任务先停；下载或同步在跑就稍后再试。
    // When the worker is busy, say why instead of doing nothing: another book downloading waits for it; a browsing
    // job from another page is stopped first; a running download or sync means try again later.
    if (tomato_sync_busy()) {
        const tomato_preempt_t room = s_op == OP_DOWNLOAD ? TOMATO_PREEMPT_BLOCKED : tomato_sync_preempt();
        if (room != TOMATO_PREEMPT_FREE) {
            snprintf(s_detail_note, sizeof(s_detail_note), "%s",
                     s_op == OP_DOWNLOAD ? "正在下载另一本，下完再点"
                     : room == TOMATO_PREEMPT_STOPPING ? "正在停下后台加载，请再点一次" : "后台正在下载或同步，完成后再试");
            return APP_REDRAW_PAGE;
        }
    }
    s_detail_note[0] = 0;
    if (start_op(OP_DOWNLOAD, tomato_sync_start_download(h))) {
        snprintf(s_dl_id, sizeof(s_dl_id), "%s", h->book_id);
        tomato_sync_get_status(&s_status);
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_view != V_SEARCH && tr_nav_touch(ctx, x, y, TR_TAB_LIBRARY)) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_SEARCH: return touch_search(ctx, x, y);
    case V_RESULTS: return touch_results(ctx, x, y);
    case V_DETAIL: return touch_detail(ctx, x, y);
    }
    return APP_REDRAW_NONE;
}

// 键盘照旧按下即响应；其余页松手才算轻点。搜索结果左滑 / 上滑下一页，右滑 / 下滑上一页，不滚动。
// The keyboard still acts on press; other views act on a tap. Results turn a whole page per swipe.
static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_view == V_SEARCH) return ev->type == UI_GESTURE_PRESS ? tap(ctx, ev->x0, ev->y0) : APP_REDRAW_NONE;
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    const int turn = ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U ? 1
                   : ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D ? -1 : 0;
    if (!turn) return APP_REDRAW_NONE;
    if (s_view == V_RESULTS) return turn_results(turn);
    return APP_REDRAW_NONE;
}

// KEY1 返回上一层。/ KEY1 goes back one level.
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_SEARCH:
        if (s_hit_count) { s_view = V_RESULTS; return APP_REDRAW_PAGE; }
        return leave_search(ctx);
    case V_RESULTS: return leave_search(ctx);
    case V_DETAIL: s_view = V_RESULTS; return APP_REDRAW_PAGE;
    default: return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    tr_time_poll();
    s_poll_ms = ctx->now_ms;
    if (s_pending_query[0] && !tomato_sync_busy()) {
        char q[sizeof(s_pending_query)];
        snprintf(q, sizeof(q), "%s", s_pending_query);
        s_pending_query[0] = 0;
        if (s_view == V_SEARCH && submit_search(q)) return APP_REDRAW_PAGE;
    }
    if (s_op == OP_NONE) return APP_REDRAW_NONE;
    // 序号变了：本页的任务早结束了，后台在跑别人的。/ The sequence moved on: our job ended, another runs.
    if (tomato_sync_seq() != s_op_seq) {
        s_op = OP_NONE;
        load_shelf();
        return s_view == V_SEARCH ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
    }
    bool busy = tomato_sync_busy();
    tomato_sync_get_status(&s_status);
    if (!busy) {
        // 操作结束：取结果、重读书架、整页刷新。/ Operation finished: collect results, reload the shelf, full page.
        finish_op();
        return s_view == V_SEARCH ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
    }
    bool changed = s_status.revision != s_seen_revision;
    bool stale = ctx->now_ms - s_busy_draw_ms >= BUSY_REDRAW_MS && s_status.phase == TOMATO_DOWNLOADING;
    if (!changed && !stale) return APP_REDRAW_NONE;
    s_seen_revision = s_status.revision;
    s_busy_draw_ms = ctx->now_ms;
    // 只在当前视图显示这个任务的进度时刷。/ Refresh only when the current view shows this op's progress.
    if (s_view == V_DETAIL && s_op == OP_DOWNLOAD && detail_is_dl())
        return area_redraw(ctx, (EpdRect){0, DETAIL_BTN.y - TR_S(60), TR_W, TR_S(160)});
    if (s_view == V_RESULTS && s_op == OP_SEARCH) return area_redraw(ctx, (EpdRect){0, TR_S(190), TR_W, TR_S(60)});
    return APP_REDRAW_NONE;
}

const app_desc_t app_tr_tomato = {
    .title = "番茄搜索",
    .detail = "搜索、下载到书库",
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
