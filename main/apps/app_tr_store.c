/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 书城：番茄标签页的第二个分页。频道和分类 → 分类榜单 → 书籍详情 → 下载到书库。
 * 数据是 FanqieRankTracker 的榜单 JSON，联网交给 components/tomato 的后台任务，这里只读 TF 卡缓存。
 *
 * Bookstore: the Tomato tab's second segment. Channel and category -> rank list -> book detail -> download.
 * Data is FanqieRankTracker rank JSON; the components/tomato task does the networking and this page reads
 * the TF card cache only.
 *
 * 冻结：只在用户点“刷新”、打开没缓存的分类、进详情取封面、点下载时联网；不自动刷新榜单。
 * Frozen: network only on "Refresh", opening an uncached category, fetching a detail cover, or downloading;
 * never refresh ranks automatically.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "book_cover.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "tomato.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_store"
#define POLL_MS 400
#define CHIP_H 62
#define CHIP_GAP 12
#define CHIP_STEP 76
#define CHANNEL_Y 322
#define CATS_Y 430
#define RANK_TOP 176
#define RANK_ROW_H 118
#define RANK_ROWS 7
#define PAGER_Y 1040
#define COVER_RECT (EpdRect){TR_MARGIN, 180, 220, 296}
#define DETAIL_BTN (EpdRect){TR_MARGIN, 1000, TR_W - 2 * TR_MARGIN, 84}

extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_store;

typedef enum { V_HOME, V_RANK, V_DETAIL } view_t;
typedef enum { OP_NONE, OP_RANK, OP_COVER, OP_DOWNLOAD } op_t;

static view_t s_view;
static tomato_rank_cat_t* s_cats;      // PSRAM
static int s_cat_count = -1;           // -1：没有缓存 / -1: no cache
static char s_date[16];
static bool s_male = true;
static char s_cat_name[32];            // 选中的分类 / Selected category
static int s_cur = -1;                 // s_cats 里的下标 / Index into s_cats
static tomato_rank_book_t* s_books;    // PSRAM
static int s_book_count = -1;
static int s_page;
static int s_detail;
static tomato_book_t* s_shelf;         // PSRAM，判断“已在书架” / PSRAM, for "on the shelf"
static int s_shelf_count;

static op_t s_op, s_last;
static tomato_status_t s_status;
static uint32_t s_seen_revision;
static int64_t s_poll_ms;

/* ---- 数据 / Data ---- */

static void load_prefs(void) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t male = 1;
    if (nvs_get_u8(h, "store_ch", &male) == ESP_OK) s_male = male != 0;
    size_t len = sizeof(s_cat_name);
    if (nvs_get_str(h, "store_cat", s_cat_name, &len) != ESP_OK) s_cat_name[0] = 0;
    nvs_close(h);
}

static void save_prefs(void) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "store_ch", s_male ? 1 : 0);
    nvs_set_str(h, "store_cat", s_cat_name);
    nvs_commit(h);
    nvs_close(h);
}

static void load_index(void) {
    s_cat_count = s_cats ? tomato_rank_load_index(s_cats, TOMATO_RANK_CAT_MAX, s_date, sizeof(s_date)) : -1;
    s_cur = -1;
    for (int i = 0; i < s_cat_count; i++)
        if (s_cats[i].male == s_male && !strcmp(s_cats[i].name, s_cat_name)) s_cur = i;
}

static void load_books(void) {
    s_book_count = s_cur >= 0 && s_books ? tomato_rank_load_books(&s_cats[s_cur], s_books, TOMATO_RANK_BOOK_MAX) : -1;
}

static void load_shelf(void) {
    s_shelf_count = 0;
    if (s_shelf) tomato_shelf_load(s_shelf, TOMATO_SHELF_MAX, &s_shelf_count);
}

static int shelf_index_of(const char* book_id) {
    for (int i = 0; i < s_shelf_count; i++) if (!strcmp(s_shelf[i].book_id, book_id)) return i;
    return -1;
}

static bool start_op(op_t op, esp_err_t err) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start op %d: %s", op, esp_err_to_name(err));
        return false;
    }
    s_op = op;
    s_last = OP_NONE;
    s_seen_revision = 0;
    tomato_sync_get_status(&s_status);
    return true;
}

static bool cover_file(const tomato_rank_book_t* b, char* out, size_t cap) {
    struct stat st;
    tomato_rank_cover_path(b->book_id, out, cap);
    return stat(out, &st) == 0 && st.st_size > 0;
}

static int rank_pages(void) {
    return s_book_count > 0 ? (s_book_count + RANK_ROWS - 1) / RANK_ROWS : 1;
}

/* ---- 分类胶囊排版 / Category chip layout ---- */

// 按顺序排胶囊，返回第 k 个本频道分类的位置。/ Lay chips out in order; returns the rect of the k-th channel category.
static int layout_chips(EpdRect* rects, int* idx, int max) {
    int n = 0, x = TR_MARGIN, y = CATS_Y;
    for (int i = 0; i < s_cat_count && n < max; i++) {
        if (s_cats[i].male != s_male) continue;
        const int w = ttf_text_width_px(30, s_cats[i].name) + 44;
        if (x + w > TR_W - TR_MARGIN + 10) { x = TR_MARGIN; y += CHIP_STEP; }
        rects[n] = (EpdRect){x, y, w, CHIP_H};
        idx[n++] = i;
        x += w + CHIP_GAP;
    }
    return n;
}

/* ---- 绘制 / Drawing ---- */

static void draw_segment(uint8_t* fb) {
    static const char* const LABELS[2] = {"书架", "书城"};
    tr_segment(fb, TR_SEGMENT_RECT, LABELS, 2, 1);
}

static const char* busy_text(void) {
    if (s_status.phase == TOMATO_CONNECTING) return "正在连接 Wi-Fi…";
    return "正在获取榜单…";
}

static void draw_home(uint8_t* fb) {
    tr_title(fb, "番茄小说");
    draw_segment(fb);
    char line[96];
    const bool fetching = tomato_sync_busy() && s_op == OP_RANK;
    if (fetching) snprintf(line, sizeof(line), "%s", busy_text());
    else if (s_last == OP_RANK && s_status.phase == TOMATO_FAILED) snprintf(line, sizeof(line), "%s", s_status.message);
    else if (s_cat_count >= 0) snprintf(line, sizeof(line), "榜单 %s", s_date[0] ? s_date : "已缓存");
    else snprintf(line, sizeof(line), "还没有榜单");
    tr_fit(line, 28, TR_W - 2 * TR_MARGIN - 160);
    tr_text(fb, TR_MARGIN, 258, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    if (!tomato_sync_busy()) {
        tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - 96, 274, false);
        tr_text(fb, TR_W - TR_MARGIN, 254, 30, "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    }
    tr_pill(fb, (EpdRect){TR_MARGIN, CHANNEL_Y, 120, CHIP_H}, "男频", s_male, 30);
    tr_pill(fb, (EpdRect){TR_MARGIN + 134, CHANNEL_Y, 120, CHIP_H}, "女频", !s_male, 30);
    tr_hline(fb, 404, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    if (s_cat_count < 0) {
        tr_wrap(fb, TR_MARGIN, CATS_Y + 10, TR_W - 2 * TR_MARGIN, 30, 46,
                "点右上角“刷新”联网获取榜单。之后打开会直接显示上次取到的内容，不用联网。", 4);
        return;
    }
    EpdRect rects[TOMATO_RANK_CAT_MAX];
    int idx[TOMATO_RANK_CAT_MAX];
    int n = layout_chips(rects, idx, TOMATO_RANK_CAT_MAX);
    for (int k = 0; k < n; k++) {
        if (rects[k].y + CHIP_H > TR_TABS_Y - 8) break;
        tr_pill(fb, rects[k], s_cats[idx[k]].name, idx[k] == s_cur, 30);
    }
}

static void draw_rank(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 92, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    char line[160];
    if (s_cur >= 0) {
        tr_text(fb, 96, 96, 40, s_cats[s_cur].name, EPD_DRAW_ALIGN_LEFT, true);
        snprintf(line, sizeof(line), "%s · %s", s_cats[s_cur].male ? "男频" : "女频", s_date);
        tr_text(fb, TR_W - TR_MARGIN, 104, 28, line, EPD_DRAW_ALIGN_RIGHT, false);
    }
    tr_hline(fb, 170, TR_MARGIN, TR_W - 2 * TR_MARGIN, false);
    const bool fetching = tomato_sync_busy() && s_op == OP_RANK;
    if (fetching || s_book_count <= 0) {
        const char* msg = fetching ? busy_text()
                        : s_last == OP_RANK && s_status.phase == TOMATO_FAILED ? s_status.message
                        : s_book_count < 0 ? "还没取到这个分类。后台有别的同步在跑时取不了，返回后再点一次分类。"
                        : "这个分类没有书";
        tr_wrap(fb, TR_MARGIN, RANK_TOP + 30, TR_W - 2 * TR_MARGIN, 30, 46, msg, 3);
        return;
    }
    for (int row = 0; row < RANK_ROWS; row++) {
        const int i = s_page * RANK_ROWS + row;
        if (i >= s_book_count) break;
        const tomato_rank_book_t* b = &s_books[i];
        const int y = RANK_TOP + row * RANK_ROW_H;
        snprintf(line, sizeof(line), "%d", i + 1);
        tr_text(fb, TR_MARGIN, y + 22, 44, line, EPD_DRAW_ALIGN_LEFT, true);
        snprintf(line, sizeof(line), "%s", b->title);
        tr_fit(line, 34, TR_W - TR_MARGIN - 110);
        tr_text(fb, 110, y + 16, 34, line, EPD_DRAW_ALIGN_LEFT, true);
        const bool owned = shelf_index_of(b->book_id) >= 0;
        const int mark_w = owned ? ttf_text_width_px(24, "已在书架") + 24 : 0;
        snprintf(line, sizeof(line), "%s · 在读 %s", b->author, b->reads[0] ? b->reads : "—");
        tr_fit(line, 26, TR_W - TR_MARGIN - 110 - mark_w);
        tr_text(fb, 110, y + 64, 26, line, EPD_DRAW_ALIGN_LEFT, false);
        if (owned) tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - mark_w + 8, y + 60, mark_w - 8, 38}, "已在书架", false, 24);
        tr_hline(fb, y + RANK_ROW_H - 4, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(fb, PAGER_Y, s_page, rank_pages());
}

static void draw_detail(uint8_t* fb) {
    const tomato_rank_book_t* b = &s_books[s_detail];
    char line[160];
    tr_text(fb, TR_MARGIN, 92, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%s · 第 %d 名", s_cur >= 0 ? s_cats[s_cur].name : "", s_detail + 1);
    tr_text(fb, 96, 100, 30, line, EPD_DRAW_ALIGN_LEFT, false);
    char cover[96];
    tr_book_cover(fb, COVER_RECT, cover_file(b, cover, sizeof(cover)) ? cover : NULL, b->title, b->author, "番茄", false);
    const int x = 292, w = TR_W - TR_MARGIN - x;
    int lines = tr_wrap(fb, x, 188, w, 40, 52, b->title, 3);
    int y = 188 + lines * 52 + 18;
    snprintf(line, sizeof(line), "%s", b->author);
    tr_fit(line, 30, w);
    tr_text(fb, x, y, 30, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "在读 %s", b->reads[0] ? b->reads : "—");
    tr_text(fb, x, y + 50, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_cur >= 0) {
        snprintf(line, sizeof(line), "%s · %s", s_cats[s_cur].male ? "男频" : "女频", s_cats[s_cur].name);
        tr_text(fb, x, y + 94, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    tr_hline(fb, 510, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    tr_wrap(fb, TR_MARGIN, 536, TR_W - 2 * TR_MARGIN, 30, 48, b->intro[0] ? b->intro : "暂无简介", 8);

    EpdRect btn = DETAIL_BTN;
    const bool downloading = tomato_sync_busy() && s_op == OP_DOWNLOAD;
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
    if (s_last == OP_DOWNLOAD && s_status.phase == TOMATO_FAILED)
        tr_text(fb, TR_MARGIN, btn.y - 50, 28, s_status.message, EPD_DRAW_ALIGN_LEFT, false);
    const bool owned = shelf_index_of(b->book_id) >= 0;
    tr_pill(fb, btn, owned ? "开始阅读" : "下载到书库", true, 32);
    tr_text(fb, TR_W / 2, btn.y + btn.height + 26, 26,
            owned ? "已在番茄书架和书库里" : "经番茄服务器生成 TXT，下载后在书库和番茄书架里都能看到",
            EPD_DRAW_ALIGN_CENTER, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    switch (s_view) {
    case V_HOME: draw_home(fb); break;
    case V_RANK: draw_rank(fb); break;
    case V_DETAIL: draw_detail(fb); break;
    }
    if (s_view != V_DETAIL) tr_tab_bar(fb, TR_TAB_TOMATO);
}

/* ---- 操作 / Actions ---- */

static app_redraw_t open_category(int i) {
    s_cur = i;
    snprintf(s_cat_name, sizeof(s_cat_name), "%s", s_cats[i].name);
    save_prefs();
    s_page = 0;
    s_last = OP_NONE;
    s_view = V_RANK;
    if (tomato_rank_cat_cached(&s_cats[i])) load_books();
    else {
        s_book_count = -1;
        if (!tomato_sync_busy()) start_op(OP_RANK, tomato_sync_start_rank(&s_cats[i]));
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_detail(int i) {
    s_detail = i;
    s_last = OP_NONE;
    s_view = V_DETAIL;
    book_cover_drop_pending();
    // 只为这一本取封面；没设置 Wi-Fi 或失败就用文字封面。/ Fetch this book's cover only; a failure keeps the text cover.
    char cover[96];
    if (!cover_file(&s_books[i], cover, sizeof(cover)) && s_books[i].cover[0] && !tomato_sync_busy())
        start_op(OP_COVER, tomato_sync_start_rank_cover(&s_books[i]));
    return APP_REDRAW_PAGE;
}

static app_redraw_t download_or_read(app_ctx_t* ctx) {
    const tomato_rank_book_t* b = &s_books[s_detail];
    int owned = shelf_index_of(b->book_id);
    if (owned >= 0) {
        char path[BOOK_STORE_PATH_MAX];
        snprintf(path, sizeof(path), TOMATO_BOOKS_DIR "/%s", s_shelf[owned].file);
        struct stat st;
        if (s_shelf[owned].file[0] && stat(path, &st) == 0) tr_nav_open_book(ctx, path);
        return APP_REDRAW_NONE;
    }
    if (tomato_sync_busy()) return APP_REDRAW_NONE;
    // 榜单书转成搜索结果的格式，走同一条下载路。/ Turn the rank book into a search hit and use the same download path.
    tomato_hit_t hit = {0};
    snprintf(hit.book_id, sizeof(hit.book_id), "%s", b->book_id);
    snprintf(hit.title, sizeof(hit.title), "%s", b->title);
    snprintf(hit.author, sizeof(hit.author), "%s", b->author);
    snprintf(hit.thumb, sizeof(hit.thumb), "%s", b->cover);
    start_op(OP_DOWNLOAD, tomato_sync_start_download(&hit));
    return APP_REDRAW_PAGE;
}

/* ---- 事件 / Events ---- */

static app_redraw_t touch_home(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_segment_hit(TR_SEGMENT_RECT, 2, x, y) == 0) { ctx->request_app = &app_tr_tomato; return APP_REDRAW_NONE; }
    if (y >= 240 && y < 310 && x > TR_W - TR_MARGIN - 160 && !tomato_sync_busy()) {
        start_op(OP_RANK, tomato_sync_start_rank(NULL));
        return APP_REDRAW_PAGE;
    }
    if (y >= CHANNEL_Y && y < CHANNEL_Y + CHIP_H && x < TR_MARGIN + 260) {
        bool male = x < TR_MARGIN + 127;
        if (male == s_male) return APP_REDRAW_NONE;
        s_male = male;
        save_prefs();
        load_index();
        return APP_REDRAW_PAGE;
    }
    if (s_cat_count <= 0) return APP_REDRAW_NONE;
    EpdRect rects[TOMATO_RANK_CAT_MAX];
    int idx[TOMATO_RANK_CAT_MAX];
    int n = layout_chips(rects, idx, TOMATO_RANK_CAT_MAX);
    for (int k = 0; k < n; k++)
        if (ui_rect_hit(rects[k], x, y)) return open_category(idx[k]);
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_rank(uint16_t x, uint16_t y) {
    if (y < 160 && x < 140) { s_view = V_HOME; return APP_REDRAW_PAGE; }
    int turn = tr_pager_hit(PAGER_Y, x, y);
    if (turn) {
        int next = s_page + turn;
        if (next < 0 || next >= rank_pages()) return APP_REDRAW_NONE;
        s_page = next;
        return APP_REDRAW_PAGE;
    }
    if (s_book_count > 0 && y >= RANK_TOP && (y - RANK_TOP) / RANK_ROW_H < RANK_ROWS) {
        int i = s_page * RANK_ROWS + (y - RANK_TOP) / RANK_ROW_H;
        if (i < s_book_count) return open_detail(i);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_detail(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 160 && x < 300) { s_view = V_RANK; return APP_REDRAW_PAGE; }
    EpdRect btn = DETAIL_BTN;
    if (tomato_sync_busy() && s_op == OP_DOWNLOAD) {
        if (y >= btn.y && y < btn.y + btn.height && x > TR_W - TR_MARGIN - 140) tomato_sync_cancel();
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(btn, x, y)) return download_or_read(ctx);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    uint16_t x = touch->x, y = touch->y;
    if (s_view != V_DETAIL && tr_nav_touch(ctx, x, y, TR_TAB_TOMATO)) {
        // 从书城点“番茄”标签回到书架。/ Tapping the Tomato tab from the store returns to the shelf.
        if (!ctx->request_app) ctx->request_app = &app_tr_tomato;
        return APP_REDRAW_NONE;
    }
    switch (s_view) {
    case V_HOME: return touch_home(ctx, x, y);
    case V_RANK: return touch_rank(x, y);
    case V_DETAIL: return touch_detail(ctx, x, y);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_DETAIL: s_view = V_RANK; return APP_REDRAW_PAGE;
    case V_RANK: s_view = V_HOME; return APP_REDRAW_PAGE;
    default: ctx->request_app = &app_tr_tomato; return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view == V_DETAIL && !(ctx->touch && ctx->touch->touched) && book_cover_tick()) return APP_REDRAW_PAGE;
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tr_time_poll();
    if (s_op == OP_NONE) return APP_REDRAW_NONE;
    tomato_sync_get_status(&s_status);
    if (!tomato_sync_busy()) {
        // 操作结束：重读缓存和书架，整页刷新一次。/ Done: reload caches and the shelf, refresh the page once.
        s_last = s_op;
        s_op = OP_NONE;
        if (s_last == OP_RANK) {
            char keep[32];
            snprintf(keep, sizeof(keep), "%s", s_cat_name);
            load_index();
            if (s_view != V_HOME) load_books();
            snprintf(s_cat_name, sizeof(s_cat_name), "%s", keep);
        }
        if (s_last == OP_DOWNLOAD) load_shelf();
        // 封面取失败不提示，文字封面就够了。/ A failed cover stays silent; the text cover is enough.
        if (s_last == OP_COVER && s_status.phase == TOMATO_FAILED) return APP_REDRAW_NONE;
        return APP_REDRAW_PAGE;
    }
    if (s_status.revision == s_seen_revision || s_view != V_DETAIL || s_op != OP_DOWNLOAD) return APP_REDRAW_NONE;
    s_seen_revision = s_status.revision;
    return APP_REDRAW_PAGE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_cats) s_cats = heap_caps_calloc(TOMATO_RANK_CAT_MAX, sizeof(*s_cats), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_books) s_books = heap_caps_calloc(TOMATO_RANK_BOOK_MAX, sizeof(*s_books), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_shelf) s_shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*s_shelf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static bool prefs_loaded;
    if (!prefs_loaded) { load_prefs(); prefs_loaded = true; }
    tr_nav_set_tomato_page(&app_tr_store);
    book_cover_drop_pending();
    load_shelf();
    load_index();
    tomato_sync_get_status(&s_status);
    // 后台还在跑就保留原视图，否则回到书城首页。/ Keep the view while work runs; otherwise start at the store home.
    if (!tomato_sync_busy() || s_op == OP_NONE) { s_view = V_HOME; s_op = OP_NONE; }
    if (s_view != V_HOME) load_books();
    if (s_view == V_DETAIL && s_detail >= s_book_count) s_view = V_HOME;
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    book_cover_drop_pending();
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    tomato_sync_cancel();
    s_view = V_HOME;
    s_cat_count = -1;
    s_book_count = -1;
    s_shelf_count = 0;
}

const app_desc_t app_tr_store = {
    .title = "书城",
    .detail = "番茄榜单",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
};
