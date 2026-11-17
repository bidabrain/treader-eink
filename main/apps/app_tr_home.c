/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 首页：日期、继续阅读、番茄更新提示、最近翻过的三本。开机默认页。
 * Home: date, continue reading, Tomato update hint and three recent books. The boot page.
 *
 * 冻结：数据只在进页时准备一次；之后只按分钟局部刷新状态栏，不整页重画。
 * Frozen: data is prepared once on entry; afterwards only the status bar refreshes per minute.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "book_progress.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "tomato.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ui_kit.h"

#define RECENT_MAX 4
#define COVER_Y 280
#define TOMATO_ROW_Y 566
#define SMALL_Y 712
#define SMALL_W 120
#define SMALL_H 164

typedef struct {
    char path[BOOK_STORE_PATH_MAX];
    char title[96];
    char author[48];
    char format[8];
    uint8_t pct;
} home_book_t;

EXT_RAM_BSS_ATTR static home_book_t s_books[RECENT_MAX];
static int s_book_count;
static int s_pending;          // 番茄待更新本数，-1 表示没有番茄书 / Tomato books with updates, -1 if none
static int s_drawn_minute = -1;

static const int SMALL_X[3] = {40, 282, 524};

/* ---- 数据 / Data ---- */

static void load_data(void) {
    book_progress_recent_t recent[8];
    int n = book_progress_recent(recent, 8);
    // 番茄书架约 18 KB，临时放 PSRAM。/ The Tomato shelf is ~18 KB; borrow PSRAM briefly.
    tomato_book_t* shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(tomato_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int shelf_count = 0;
    bool have_shelf = shelf && tomato_shelf_load(shelf, TOMATO_SHELF_MAX, &shelf_count) == ESP_OK;
    s_pending = have_shelf && shelf_count ? 0 : -1;
    for (int i = 0; i < shelf_count; i++) s_pending += shelf[i].remote > shelf[i].chapters;

    s_book_count = 0;
    for (int i = 0; i < n && s_book_count < RECENT_MAX; i++) {
        struct stat st;
        if (stat(recent[i].path, &st) != 0) continue;   // 文件已删 / File gone
        home_book_t* b = &s_books[s_book_count++];
        memset(b, 0, sizeof(*b));
        snprintf(b->path, sizeof(b->path), "%s", recent[i].path);
        tr_title_from_path(b->path, b->title, sizeof(b->title));
        b->pct = recent[i].pct;
        const char* dot = strrchr(b->path, '.');
        snprintf(b->format, sizeof(b->format), "%s", dot && !strcasecmp(dot, ".epub") ? "EPUB" : "TXT");
        const char* base = strrchr(b->path, '/');
        base = base ? base + 1 : b->path;
        for (int j = 0; j < shelf_count; j++) {
            if (strcmp(shelf[j].file, base)) continue;
            snprintf(b->title, sizeof(b->title), "%s", shelf[j].title);
            snprintf(b->author, sizeof(b->author), "%s", shelf[j].author);
            snprintf(b->format, sizeof(b->format), "番茄");
            break;
        }
    }
    heap_caps_free(shelf);
}

/* ---- 绘制 / Drawing ---- */

static void draw_header(uint8_t* fb) {
    static const char* const WEEK[7] = {"日", "一", "二", "三", "四", "五", "六"};
    time_t now = time(NULL);
    if (tomato_time_valid()) {
        struct tm tm;
        localtime_r(&now, &tm);
        char date[48];
        snprintf(date, sizeof(date), "%d 月 %d 日 · 星期%s", tm.tm_mon + 1, tm.tm_mday, WEEK[tm.tm_wday]);
        tr_text(fb, TR_MARGIN, 92, 46, date, EPD_DRAW_ALIGN_LEFT, true);
    } else {
        tr_text(fb, TR_MARGIN, 92, 46, "Treader", EPD_DRAW_ALIGN_LEFT, true);
    }
    tr_text(fb, TR_MARGIN, 152, 30, "留一点时间，读几页。", EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_current(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 232, 28, "继续阅读", EPD_DRAW_ALIGN_LEFT, false);
    if (!s_book_count) {
        tr_wrap(fb, TR_MARGIN, COVER_Y + 20, TR_W - 2 * TR_MARGIN, 30, 46,
                "还没有读过的书。把 TXT 或 EPUB 放进 TF 卡的 books 目录，或者到“番茄”里搜索下载。", 4);
        return;
    }
    const home_book_t* b = &s_books[0];
    tr_cover(fb, (EpdRect){TR_MARGIN, COVER_Y, 176, 236}, b->title, b->author, b->format, b->pct >= 100);
    const int x = 248, w = TR_W - TR_MARGIN - x;
    char line[96];
    snprintf(line, sizeof(line), "%s", b->title);
    tr_fit(line, 36, w);
    tr_text(fb, x, COVER_Y + 8, 36, line, EPD_DRAW_ALIGN_LEFT, true);
    snprintf(line, sizeof(line), "%s", b->author[0] ? b->author : b->format);
    tr_fit(line, 28, w);
    tr_text(fb, x, COVER_Y + 60, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    tr_progress(fb, x, COVER_Y + 166, w, b->pct);
    snprintf(line, sizeof(line), b->pct >= 100 ? "读完" : "已读 %u%%", b->pct);
    tr_text(fb, x, COVER_Y + 190, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, COVER_Y + 190, 30, "接着读 →", EPD_DRAW_ALIGN_RIGHT, false);
}

static void draw_tomato_row(uint8_t* fb) {
    char line[64];
    if (s_pending > 0) snprintf(line, sizeof(line), "番茄小说 · %d 本有新章节", s_pending);
    else snprintf(line, sizeof(line), "番茄小说 · 搜索和下载");
    tr_text(fb, TR_MARGIN, TOMATO_ROW_Y, 30, line, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, TOMATO_ROW_Y, 30, s_pending > 0 ? "去更新 ›" : "去看看 ›", EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, TOMATO_ROW_Y + 54, TR_MARGIN, TR_W - 2 * TR_MARGIN, false);
}

static void draw_recent(uint8_t* fb) {
    if (s_book_count < 2) return;
    tr_text(fb, TR_MARGIN, 662, 28, "最近翻过", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, 662, 28, "全部书籍 →", EPD_DRAW_ALIGN_RIGHT, false);
    for (int i = 1; i < s_book_count && i <= 3; i++) {
        const home_book_t* b = &s_books[i];
        int x = SMALL_X[i - 1];
        tr_cover(fb, (EpdRect){x, SMALL_Y, SMALL_W, SMALL_H}, b->title, b->author, b->format, b->pct >= 100);
        char line[96];
        snprintf(line, sizeof(line), "%s", b->title);
        tr_fit(line, 28, 150);
        tr_text(fb, x, SMALL_Y + 180, 28, line, EPD_DRAW_ALIGN_LEFT, true);
        snprintf(line, sizeof(line), b->pct >= 100 ? "读完" : "已读 %u%%", b->pct);
        tr_text(fb, x, SMALL_Y + 220, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    }
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    draw_header(fb);
    draw_current(fb);
    draw_tomato_row(fb);
    draw_recent(fb);
    tr_text(fb, TR_W / 2, 1020, 34, "慢一点，读几页。", EPD_DRAW_ALIGN_CENTER, false);
    tr_tab_bar(fb, TR_TAB_HOME);
}

/* ---- 事件 / Events ---- */

static int current_minute(void) {
    time_t now = time(NULL);
    return (int)(now / 60);
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    tr_time_restore();
    load_data();
    s_drawn_minute = current_minute();
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    uint16_t x = touch->x, y = touch->y;
    if (tr_nav_touch(ctx, x, y, TR_TAB_HOME)) return APP_REDRAW_NONE;
    if (y >= COVER_Y && y < COVER_Y + 240 && s_book_count) {
        tr_nav_open_book(ctx, s_books[0].path);
        return APP_REDRAW_NONE;
    }
    if (y >= TOMATO_ROW_Y - 20 && y < TOMATO_ROW_Y + 60) {
        ctx->request_app = tr_nav_tab_app(TR_TAB_TOMATO);
        return APP_REDRAW_NONE;
    }
    if (y >= 640 && y < 700 && x > TR_W / 2) {
        ctx->request_app = tr_nav_tab_app(TR_TAB_LIBRARY);
        return APP_REDRAW_NONE;
    }
    if (y >= SMALL_Y && y < SMALL_Y + 260) {
        for (int i = 0; i < 3; i++) {
            if (x >= SMALL_X[i] && x < SMALL_X[i] + SMALL_W + 30 && i + 1 < s_book_count) {
                tr_nav_open_book(ctx, s_books[i + 1].path);
                return APP_REDRAW_NONE;
            }
        }
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){0, 0, TR_W, 72};
}

// 时间变了只重画状态栏一条。/ When the minute changes, redraw only the status strip.
static app_redraw_t on_tick(app_ctx_t* ctx) {
    tr_time_poll();
    int minute = current_minute();
    if (minute == s_drawn_minute || !tomato_time_valid()) return APP_REDRAW_NONE;
    s_drawn_minute = minute;
    epd_fill_rect(area_hint(ctx), UI_GRAY_WHITE, ctx->fb);
    tr_status_bar(ctx->fb);
    return APP_REDRAW_AREA;
}

const app_desc_t app_tr_home = {
    .title = "首页",
    .detail = "继续阅读与最近的书",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
