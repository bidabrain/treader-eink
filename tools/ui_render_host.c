/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：界面预览渲染。在电脑上用固件真实的 tr_ui.c、ui_kit.c 和字体代码画几屏公共控件（状态栏、标题、
 * 胶囊、分段、进度、翻页、标签栏和图标），按 16 级灰度写成 PNG，改界面时先在电脑上看效果。
 * 用 tools/run_ui_render.sh 跑；字体用 sdcard/fonts 里的默认字体。
 *
 * English: UI preview renderer. Draws a few screens of the shared controls (status bar, title, pills, segments,
 * progress, pager, tab bar and icons) on a computer with the firmware's own tr_ui.c, ui_kit.c and font code, and
 * writes them as 16-gray PNGs so UI changes can be checked before flashing. Run it with tools/run_ui_render.sh; it
 * uses the default font from sdcard/fonts.
 *
 * 冻结：只做预览，不是回归测试，不改固件代码。/ Frozen: previews only, not a regression test; never changes firmware code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "book_cover.h"
#include "read_pico_pmu.h"
#include "settings.h"

#define W TR_W
#define H TR_H

/* ---- epdiy 替身 / epdiy shim ---- */

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb[y * W + x] = color & 0xF0;
}
void epd_fill_rect(EpdRect r, uint8_t color, uint8_t* fb) {
    for (int y = r.y; y < r.y + r.height; y++)
        for (int x = r.x; x < r.x + r.width; x++) epd_draw_pixel(x, y, color, fb);
}
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color, uint8_t* fb) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        epd_draw_pixel(x0, y0, color, fb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void epd_draw_rect(EpdRect r, uint8_t color, uint8_t* fb) {
    epd_draw_line(r.x, r.y, r.x + r.width - 1, r.y, color, fb);
    epd_draw_line(r.x, r.y + r.height - 1, r.x + r.width - 1, r.y + r.height - 1, color, fb);
    epd_draw_line(r.x, r.y, r.x, r.y + r.height - 1, color, fb);
    epd_draw_line(r.x + r.width - 1, r.y, r.x + r.width - 1, r.y + r.height - 1, color, fb);
}
// 和 epdiy（取自 Adafruit GFX）一样的中点画圆。/ Midpoint circles as in epdiy (from Adafruit GFX).
void epd_draw_circle(int x0, int y0, int r, uint8_t color, uint8_t* fb) {
    int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
    epd_draw_pixel(x0, y0 + r, color, fb); epd_draw_pixel(x0, y0 - r, color, fb);
    epd_draw_pixel(x0 + r, y0, color, fb); epd_draw_pixel(x0 - r, y0, color, fb);
    while (x < y) {
        if (f >= 0) { y--; ddy += 2; f += ddy; }
        x++; ddx += 2; f += ddx;
        epd_draw_pixel(x0 + x, y0 + y, color, fb); epd_draw_pixel(x0 - x, y0 + y, color, fb);
        epd_draw_pixel(x0 + x, y0 - y, color, fb); epd_draw_pixel(x0 - x, y0 - y, color, fb);
        epd_draw_pixel(x0 + y, y0 + x, color, fb); epd_draw_pixel(x0 - y, y0 + x, color, fb);
        epd_draw_pixel(x0 + y, y0 - x, color, fb); epd_draw_pixel(x0 - y, y0 - x, color, fb);
    }
}
void epd_fill_circle_helper(int x0, int y0, int r, int corners, int delta, uint8_t color, uint8_t* fb) {
    int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r, px = x, py = y;
    delta++;
    while (x < y) {
        if (f >= 0) { y--; ddy += 2; f += ddy; }
        x++; ddx += 2; f += ddx;
        if (x < y + 1) {
            if (corners & 1) epd_draw_line(x0 + x, y0 - y, x0 + x, y0 - y + 2 * y + delta - 1, color, fb);
            if (corners & 2) epd_draw_line(x0 - x, y0 - y, x0 - x, y0 - y + 2 * y + delta - 1, color, fb);
        }
        if (y != py) {
            if (corners & 1) epd_draw_line(x0 + py, y0 - px, x0 + py, y0 - px + 2 * px + delta - 1, color, fb);
            if (corners & 2) epd_draw_line(x0 - py, y0 - px, x0 - py, y0 - px + 2 * px + delta - 1, color, fb);
            py = y;
        }
        px = x;
    }
}
void epd_fill_circle(int x, int y, int r, uint8_t color, uint8_t* fb) {
    epd_draw_line(x, y - r, x, y + r, color, fb);
    epd_fill_circle_helper(x, y, r, 3, 0, color, fb);
}
void epd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color, uint8_t* fb) {
    int minx = x0 < x1 ? x0 : x1, maxx = x0 > x1 ? x0 : x1, miny = y0 < y1 ? y0 : y1, maxy = y0 > y1 ? y0 : y1;
    if (x2 < minx) minx = x2;
    if (x2 > maxx) maxx = x2;
    if (y2 < miny) miny = y2;
    if (y2 > maxy) maxy = y2;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            long a = (long)(x1 - x0) * (y - y0) - (long)(y1 - y0) * (x - x0);
            long b = (long)(x2 - x1) * (y - y1) - (long)(y2 - y1) * (x - x1);
            long c = (long)(x0 - x2) * (y - y2) - (long)(y0 - y2) * (x - x2);
            if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0)) epd_draw_pixel(x, y, color, fb);
        }
}
void epd_draw_rotated_image(EpdRect r, const uint8_t* image, uint8_t* fb) { (void)r; (void)image; (void)fb; }
int epd_rotated_display_width(void) { return W; }
int epd_rotated_display_height(void) { return H; }
int epd_width(void) { return H; }
int epd_height(void) { return W; }
enum EpdRotation epd_get_rotation(void) { return EPD_ROT_PORTRAIT; }

/* ---- 其他替身 / Other shims ---- */

const char* app_settings_font_path(void) { return ""; }
static pmu_snapshot_t s_pmu = {.soc_permille = 820};
bool read_pico_pmu_ready(void) { return true; }
const pmu_snapshot_t* read_pico_pmu_get(void) { return &s_pmu; }
bool book_cover_draw(uint8_t* fb, EpdRect r, const char* path) { (void)fb; (void)r; (void)path; return false; }

/* ---- PNG ---- */

static void put32(FILE* f, uint32_t v) {
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    fwrite(b, 1, 4, f);
}
static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
    put32(f, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc32(0, (const Bytef*)type, 4);
    if (len) crc = crc32(crc, data, len);
    put32(f, crc);
}
// 墨水级 0..15 线性换成 0..255 灰度。/ Map ink levels 0..15 linearly onto 0..255.
static void write_png(const char* path, const uint8_t* fb) {
    const size_t row = W + 1, raw_len = row * H;
    uint8_t* raw = malloc(raw_len);
    for (int y = 0; y < H; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < W; x++) raw[y * row + 1 + x] = (uint8_t)((fb[y * W + x] >> 4) * 17);
    }
    uLongf zlen = compressBound(raw_len);
    uint8_t* z = malloc(zlen);
    compress2(z, &zlen, raw, raw_len, 9);
    FILE* f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {0};
    ihdr[0] = W >> 24; ihdr[1] = W >> 16; ihdr[2] = W >> 8; ihdr[3] = W & 0xFF;
    ihdr[4] = H >> 24; ihdr[5] = H >> 16; ihdr[6] = H >> 8; ihdr[7] = H & 0xFF;
    ihdr[8] = 8; ihdr[9] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

/* ---- 预览屏 / Preview screens ---- */

static uint8_t s_fb[W * H];

static void clear(void) { memset(s_fb, UI_GRAY_WHITE, sizeof(s_fb)); }

// 书库样子的一屏：标题、胶囊、进度、翻页、标签栏。/ A library-like screen: title, pills, progress, pager, tab bar.
static void screen_library(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "书库");
    tr_icon(s_fb, TR_ICON_SEARCH, TR_W - TR_MARGIN - 24, TR_TITLE_Y + 30, false);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 168, 150, 60}, "番茄", true, 28);
    tr_pill(s_fb, (EpdRect){TR_MARGIN + 166, 168, 150, 60}, "按最近", false, 28);
    tr_pill(s_fb, (EpdRect){TR_MARGIN + 332, 168, 150, 60}, "管理", false, 28);
    tr_text(s_fb, TR_MARGIN, 250, 28, "12 本 · 按最近阅读", EPD_DRAW_ALIGN_LEFT, false);
    const char* names[4] = {"宿命之环", "十日终焉", "夜航船", "三体"};
    const int pct[4] = {41, 64, 12, 100};
    for (int i = 0; i < 4; i++) {
        const int y = 310 + i * 170;
        tr_mini_cover(s_fb, (EpdRect){TR_MARGIN, y, 90, 130}, names[i], i % 2 == 0);
        tr_text(s_fb, TR_MARGIN + 116, y + 6, 34, names[i], EPD_DRAW_ALIGN_LEFT, true);
        tr_progress(s_fb, TR_MARGIN + 116, y + 80, 360, pct[i]);
        char p[16];
        snprintf(p, sizeof(p), "%d%%", pct[i]);
        tr_text(s_fb, TR_W - TR_MARGIN, y + 66, 28, p, EPD_DRAW_ALIGN_RIGHT, false);
        tr_hline(s_fb, y + 150, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(s_fb, 1010, 0, 3);
    tr_tab_bar(s_fb, TR_TAB_LIBRARY);
}

// 番茄样子的一屏：两段和三段分段、实心按钮。/ A Tomato-like screen: two- and three-way segments, a filled button.
static void screen_tomato(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "番茄小说");
    tr_text(s_fb, TR_W - TR_MARGIN, 98, 30, "检查更新", EPD_DRAW_ALIGN_RIGHT, false);
    static const char* const two[2] = {"书架", "书城"};
    tr_segment(s_fb, TR_SEGMENT_RECT, two, 2, 0);
    static const char* const three[3] = {"推荐", "关注", "比分"};
    tr_segment(s_fb, (EpdRect){TR_MARGIN, 250, TR_W - 2 * TR_MARGIN, 64}, three, 3, 1);
    tr_segment(s_fb, (EpdRect){TR_MARGIN, 340, TR_W - 2 * TR_MARGIN, 64}, three, 3, 2);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 440, TR_W - 2 * TR_MARGIN, 72}, "下载全部更新（1 本）", true, 30);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 540, 280, 72}, "取消", false, 30);
    tr_progress(s_fb, TR_MARGIN, 660, TR_W - 2 * TR_MARGIN, 0);
    tr_progress(s_fb, TR_MARGIN, 700, TR_W - 2 * TR_MARGIN, 3);
    tr_progress(s_fb, TR_MARGIN, 740, TR_W - 2 * TR_MARGIN, 55);
    tr_pager(s_fb, 1010, 4, 12);
    tr_tab_bar(s_fb, TR_TAB_TOMATO);
}

// 所有图标，以及六个标签时的标签栏。/ Every icon, and the tab bar with six tabs.
static void screen_icons(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "图标");
    for (int i = 0; i <= TR_ICON_X; i++) {
        const int cx = TR_MARGIN + 40 + (i % 6) * 104, cy = 220 + (i / 6) * 110;
        tr_icon(s_fb, (tr_icon_t)i, cx, cy, false);
    }
    tr_tabs_set_hidden((1u << TR_TAB_HUPU) | (1u << TR_TAB_X));
    tr_tab_bar(s_fb, TR_TAB_SETTINGS);
    tr_tabs_set_hidden(1u << TR_TAB_X);
}

/* ---- 按页面坐标仿画的几屏（app_book.c、tr_lock.c）/ Screens copied from page coordinates (app_book.c, tr_lock.c) ---- */

// 书架：继续阅读卡片、待下载和新章节标记、进度、翻页、标签栏。坐标照 app_book.c 的 draw_shelf。
// Shelf: continue card, pending and new-chapter badges, progress, pager, tab bar; coordinates follow draw_shelf.
static void screen_shelf(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "书库");
    tr_icon(s_fb, TR_ICON_SEARCH, 540, 116, false);
    tr_icon(s_fb, TR_ICON_IMPORT, 612, 116, false);
    tr_text_dim(s_fb, UI_MARGIN, 156, 28, "12 本 · 按最近阅读", EPD_DRAW_ALIGN_LEFT);
    tr_text(s_fb, UI_LOCK_WIDTH - UI_MARGIN, 156, 28, "番茄 1 本有更新 ›", EPD_DRAW_ALIGN_RIGHT, true);
    tr_pill(s_fb, (EpdRect){40, 212, 150, 54}, "全部", false, 28);
    tr_pill(s_fb, (EpdRect){202, 212, 150, 54}, "按最近", false, 28);
    tr_pill(s_fb, (EpdRect){364, 212, 120, 54}, "管理", false, 28);
    const EpdRect c = {UI_MARGIN, 284, ui_content_width(), 136};
    ui_fill_round_rect(s_fb, c, 24, TR_GRAY_SURFACE);
    tr_mini_cover(s_fb, (EpdRect){c.x + 16, c.y + 20, 70, 96}, "宿命之环", true);
    const int tx = c.x + 108, right = c.x + c.width - 20, tw = right - tx;
    tr_text_on(s_fb, tx, c.y + 14, 24, "继续阅读", EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    tr_text_on(s_fb, right, c.y + 14, 24, "接着读 →", EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(s_fb, tx, c.y + 46, 32, "宿命之环", EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_progress(s_fb, tx, c.y + 108, tw - 80, 41);
    tr_text_on(s_fb, right, c.y + 94, 26, "41%", EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    struct { const char *t, *meta, *mark; bool pending; int pct; } rows[4] = {
        {"斗破苍穹", "番茄 · 天蚕土豆", "+3 章 待下载", true, 8}, {"十日终焉", "番茄 · 杀虫队队员", "+2 章", false, 64},
        {"夜航船", "TXT", "", false, 12}, {"三体", "EPUB", "", false, 100}};
    for (int i = 0; i < 4; i++) {
        const EpdRect r = {UI_MARGIN, 436 + i * 140, ui_content_width(), 128};
        tr_mini_cover(s_fb, (EpdRect){r.x, r.y + 8, 70, 96}, rows[i].t, i % 2 == 1);
        const int x = r.x + 96;
        tr_text(s_fb, x, r.y + 4, 34, rows[i].t, EPD_DRAW_ALIGN_LEFT, true);
        if (rows[i].mark[0]) tr_badge(s_fb, r.x + r.width - 10, r.y + 10, rows[i].mark, rows[i].pending, 24);
        tr_text_dim(s_fb, x, r.y + 52, 26, rows[i].meta, EPD_DRAW_ALIGN_LEFT);
        if (rows[i].pct < 100) {
            char p[16];
            snprintf(p, sizeof(p), "%d%%", rows[i].pct);
            tr_progress(s_fb, x, r.y + 98, r.x + r.width - x - 90, rows[i].pct);
            tr_text_dim(s_fb, r.x + r.width, r.y + 86, 26, p, EPD_DRAW_ALIGN_RIGHT);
        } else {
            tr_text_dim(s_fb, x, r.y + 88, 26, "读完", EPD_DRAW_ALIGN_LEFT);
        }
        tr_hline(s_fb, r.y + r.height + 4, UI_MARGIN, ui_content_width(), true);
    }
    tr_pager(s_fb, 1010, 0, 3);
    tr_tab_bar(s_fb, TR_TAB_LIBRARY);
}

// 阅读工具条：顶部返回和书名、底部面板。坐标照 app_book.c 的 draw_reader。
// Reading toolbar: back and title on top, bottom panel; coordinates follow draw_reader.
static void screen_toolbar(void) {
    clear();
    tr_wrap(s_fb, 40, 140, TR_W - 80, 40, 66,
            "这是一个阴天，乌云压得很低，仿佛随时都会坠落下来。克莱恩站在窗前，望着街道上来往的行人。他已经在这座城市生活了"
            "三个月，却依然觉得一切都很陌生。煤气灯在雾气中显得昏黄，马车的车轮碾过石板路，发出单调的声响。",
            9);
    epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, 110}, UI_GRAY_WHITE, s_fb);
    tr_hline(s_fb, 110, 0, UI_LOCK_WIDTH, true);
    tr_text(s_fb, UI_MARGIN, 40, 32, "‹ 书库", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(s_fb, UI_LOCK_WIDTH / 2 + 40, 38, 34, "宿命之环", EPD_DRAW_ALIGN_CENTER, true);
    const int py = 680;
    epd_fill_rect((EpdRect){0, py, UI_LOCK_WIDTH, UI_BAR_TOP - py}, UI_GRAY_WHITE, s_fb);
    ui_draw_round_rect(s_fb, (EpdRect){0, py, UI_LOCK_WIDTH, 120}, 32, TR_GRAY_LINE);
    ui_draw_round_rect(s_fb, (EpdRect){0, py + 1, UI_LOCK_WIDTH, 120}, 32, TR_GRAY_LINE);
    epd_fill_rect((EpdRect){2, py + 40, UI_LOCK_WIDTH - 4, 82}, UI_GRAY_WHITE, s_fb);
    ui_fill_round_rect(s_fb, (EpdRect){UI_LOCK_WIDTH / 2 - 36, py + 8, 72, 8}, 4, TR_GRAY_SELECTED);
    tr_text_dim(s_fb, UI_MARGIN, py + 24, 28, "第三章 黑夜", EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(s_fb, UI_LOCK_WIDTH - UI_MARGIN, py + 24, 28, "字号 48", EPD_DRAW_ALIGN_RIGHT);
    static const tr_icon_t icons[4] = {TR_ICON_TOC, TR_ICON_MENU, TR_ICON_REFRESH, TR_ICON_BOOKS};
    static const char* const labels[4] = {"目录", "菜单", "排版", "书库"};
    for (int k = 0; k < 4; k++) {
        const EpdRect r = {UI_MARGIN + k * 151, py + 80, 151, 120};
        ui_fill_round_rect(s_fb, ui_inset_rect(r, 10), 24, TR_GRAY_SURFACE);
        if (k == 2) tr_text_on(s_fb, r.x + r.width / 2, r.y + 16, 36, "Aa", EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK,
                               TR_GRAY_SURFACE);
        else tr_icon_on(s_fb, icons[k], r.x + r.width / 2, r.y + 36, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        tr_text_on(s_fb, r.x + r.width / 2, r.y + 70, 28, labels[k], EPD_DRAW_ALIGN_CENTER, false, UI_GRAY_BLACK,
                   TR_GRAY_SURFACE);
    }
    tr_pill(s_fb, (EpdRect){UI_MARGIN, py + 226, 200, 84}, "A−", false, 34);
    tr_pill(s_fb, (EpdRect){UI_LOCK_WIDTH - UI_MARGIN - 200, py + 226, 200, 84}, "A+", false, 34);
    tr_text(s_fb, UI_LOCK_WIDTH / 2, py + 246, 40, "48", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(s_fb, TR_MARGIN, py + 326 + 14, 28, "自动翻页", EPD_DRAW_ALIGN_LEFT, false);
    tr_pill(s_fb, (EpdRect){290, py + 326, 110, 56}, "−", false, 34);
    tr_pill(s_fb, (EpdRect){UI_LOCK_WIDTH - UI_MARGIN - 100, py + 326, 110, 56}, "+", false, 34);
    tr_text(s_fb, 470, py + 326 + 10, 32, "关", EPD_DRAW_ALIGN_CENTER, true);
    const EpdRect track = {UI_MARGIN, UI_BAR_TOP, ui_content_width(), UI_BAR_H};
    epd_fill_rect((EpdRect){track.x, track.y + 16, track.width, 12}, UI_GRAY_LIGHT, s_fb);
    epd_fill_rect((EpdRect){track.x, track.y + 16, track.width * 41 / 100, 12}, UI_GRAY_BLACK, s_fb);
    ui_text(s_fb, track.x, track.y + 36, UI_PX_CAPTION, "宿命之环 · 本章 3/12 · 全书 41%", EPD_DRAW_ALIGN_LEFT, false);
}

// 锁屏书卡。坐标照 tr_lock.c 的 tr_lock_draw。/ Lock-screen card; coordinates follow tr_lock_draw.
static void screen_lock(void) {
    clear();
    tr_text_dim(s_fb, TR_MARGIN, 120, 30, "正在读", EPD_DRAW_ALIGN_LEFT);
    const EpdRect card = {TR_MARGIN - 16, 164, TR_W - 2 * TR_MARGIN + 32, 680};
    ui_fill_round_rect(s_fb, card, 28, TR_GRAY_SURFACE);
    tr_cover(s_fb, (EpdRect){TR_MARGIN, 180, 300, 400}, "宿命之环", "爱潜水的乌贼", "番茄", false);
    tr_text_on(s_fb, TR_MARGIN, 630, 54, "宿命之环", EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(s_fb, TR_MARGIN, 704, 32, "爱潜水的乌贼 · 已读 41%", EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY,
               TR_GRAY_SURFACE);
    tr_progress(s_fb, TR_MARGIN, 772, TR_W - 2 * TR_MARGIN, 41);
    tr_text(s_fb, TR_W / 2, 960, 34, "慢一点，读几页。", EPD_DRAW_ALIGN_CENTER, false);
    tr_text_dim(s_fb, TR_W / 2, 1120, 26, "按电源键或拿起设备继续 · 电量 82%", EPD_DRAW_ALIGN_CENTER);
}

// 图书详情弹窗压在书架上。坐标照 app_book.c 的 draw_manage。/ The detail dialog over the shelf; follows draw_manage.
static void screen_dialog(void) {
    screen_shelf();
    const EpdRect panel = {UI_MARGIN - 16, 360, ui_content_width() + 32, 470};
    ui_fill_round_rect(s_fb, panel, 28, UI_GRAY_WHITE);
    ui_draw_round_rect(s_fb, panel, 28, TR_GRAY_OUTLINE);
    ui_draw_round_rect(s_fb, ui_inset_rect(panel, 1), 27, TR_GRAY_OUTLINE);
    tr_text(s_fb, UI_MARGIN, panel.y + 18, 38, "图书详情", EPD_DRAW_ALIGN_LEFT, true);
    tr_text(s_fb, UI_MARGIN, panel.y + 74, 32, "斗破苍穹.txt", EPD_DRAW_ALIGN_LEFT, false);
    tr_text_dim(s_fb, UI_MARGIN, panel.y + 130, UI_PX_CAPTION, "TF 卡 · txt · 6.21 MB", EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(s_fb, UI_MARGIN, panel.y + 174, UI_PX_CAPTION, "阅读进度 8%", EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(s_fb, UI_MARGIN, panel.y + 218, UI_PX_CAPTION, "/sdcard/books", EPD_DRAW_ALIGN_LEFT);
    for (int i = 0; i < 3; i++) {
        const EpdRect b = ui_row_rect(i, 3, panel.y + panel.height - 94, 76);
        static const char* const labels[3] = {"关闭", "清进度", "删除文件"};
        tr_pill(s_fb, b, labels[i], i == 2, 30);
    }
}

/* ---- 第三组：漫画、传书（照 app_comic.c、app_usb.c、app_transfer.c）/ Group 3: comics and transfer ---- */

// 漫画书架。坐标照 app_comic.c 的 draw_shelf。/ Comic shelf; coordinates follow draw_shelf in app_comic.c.
static void screen_comics(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "漫画");
    tr_icon(s_fb, TR_ICON_IMPORT, 612, 116, false);
    tr_text_dim(s_fb, TR_MARGIN, 156, 28, "8 部 · 按最近", EPD_DRAW_ALIGN_LEFT);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 212, 150, 54}, "按最近", false, 28);
    const EpdRect card = {TR_MARGIN, 284, TR_W - 2 * TR_MARGIN, 136};
    ui_fill_round_rect(s_fb, card, 24, TR_GRAY_SURFACE);
    tr_mini_cover(s_fb, (EpdRect){56, 304, 70, 96}, "海贼王", true);
    tr_text_on(s_fb, 148, 298, 24, "继续看", EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    tr_text_on(s_fb, TR_W - 60, 298, 24, "接着看 →", EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(s_fb, 148, 330, 32, "海贼王 · 第 32 话", EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_progress(s_fb, 148, 392, TR_W - 170 - 148, 60);
    tr_text_on(s_fb, TR_W - 60, 378, 26, "12 / 20", EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    static const int cx[3] = {40, 267, 494}, cy[2] = {454, 748};
    struct { const char *name, *state; bool series, done; } cells[6] = {
        {"海贼王", "108 话 · 看完 31", true, false}, {"鬼灭之刃", "205 话 · 在看", true, false},
        {"灌篮高手", "31 话 · 看完", true, true}, {"一拳超人", "单本 · 45%", false, false},
        {"进击的巨人", "139 话 · 未看", true, false}, {"间谍过家家", "单本 · 未看", false, false}};
    for (int k = 0; k < 6; k++) {
        const int x = cx[k % 3], y = cy[k / 3];
        if (cells[k].series)
            for (int d = 2; d >= 1; d--) {
                EpdRect r = {x + 5 * d, y - 5 * d, 150, 200};
                epd_fill_rect(r, d == 2 ? TR_GRAY_SELECTED : TR_GRAY_SURFACE, s_fb);
                epd_draw_rect(r, TR_GRAY_OUTLINE, s_fb);
            }
        tr_cover(s_fb, (EpdRect){x, y, 150, 200}, cells[k].name, "", "CBZ", k % 2 == 0);
        tr_text(s_fb, x, y + 208, 24, cells[k].name, EPD_DRAW_ALIGN_LEFT, true);
        if (cells[k].done) tr_text(s_fb, x, y + 246, 22, cells[k].state, EPD_DRAW_ALIGN_LEFT, true);
        else tr_text_dim(s_fb, x, y + 246, 22, cells[k].state, EPD_DRAW_ALIGN_LEFT);
    }
    tr_pager(s_fb, 1040, 0, 2);
    tr_tab_bar(s_fb, TR_TAB_COMIC);
}

// 漫画阅读工具条。坐标照 draw_reader。/ Comic reader toolbar; coordinates follow draw_reader.
static void screen_comic_bar(void) {
    clear();
    for (int y = 140; y < 690; y += 6) epd_fill_rect((EpdRect){60, y, TR_W - 120, 3}, 0x90, s_fb);
    epd_fill_rect((EpdRect){0, 0, TR_W, 120}, UI_GRAY_WHITE, s_fb);
    tr_hline(s_fb, 118, 0, TR_W, true);
    tr_text(s_fb, TR_MARGIN, 40, 32, "‹ 漫画", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(s_fb, TR_W / 2, 38, 34, "海贼王 · 第 32 话", EPD_DRAW_ALIGN_CENTER, true);
    const int bar = 700, tool = 840, col = (TR_W - 2 * TR_MARGIN) / 3;
    epd_fill_rect((EpdRect){0, bar, TR_W, TR_H - bar}, UI_GRAY_WHITE, s_fb);
    ui_draw_round_rect(s_fb, (EpdRect){0, bar, TR_W, 120}, 32, TR_GRAY_LINE);
    ui_draw_round_rect(s_fb, (EpdRect){0, bar + 1, TR_W, 120}, 32, TR_GRAY_LINE);
    epd_fill_rect((EpdRect){2, bar + 40, TR_W - 4, 82}, UI_GRAY_WHITE, s_fb);
    ui_fill_round_rect(s_fb, (EpdRect){TR_W / 2 - 36, bar + 8, 72, 8}, 4, TR_GRAY_SELECTED);
    tr_text_dim(s_fb, TR_MARGIN, bar + 32, 28, "第 12 / 20 页", EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(s_fb, TR_W - TR_MARGIN, bar + 32, 28, "第 32 / 108 话", EPD_DRAW_ALIGN_RIGHT);
    tr_progress(s_fb, TR_MARGIN, 786, TR_W - 2 * TR_MARGIN, 60);
    static const char* const labels[3] = {"话列表", "右→左", "漫画"};
    for (int i = 0; i < 3; i++) {
        const int cx = TR_MARGIN + col * i + col / 2;
        ui_fill_round_rect(s_fb, (EpdRect){TR_MARGIN + col * i + 10, tool - 12, col - 20, 108}, 24, TR_GRAY_SURFACE);
        if (i == 1) tr_text_on(s_fb, cx, tool + 4, 36, "⇠", EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        else tr_icon_on(s_fb, i == 0 ? TR_ICON_TOC : TR_ICON_COMIC, cx, tool + 24, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        tr_text_on(s_fb, cx, tool + 56, 28, labels[i], EPD_DRAW_ALIGN_CENTER, false, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    }
    tr_text(s_fb, TR_MARGIN, 962 + 14, 28, "自动翻页", EPD_DRAW_ALIGN_LEFT, false);
    tr_pill(s_fb, (EpdRect){290, 962, 110, 56}, "−", false, 34);
    tr_pill(s_fb, (EpdRect){TR_W - TR_MARGIN - 100, 962, 110, 56}, "+", false, 34);
    tr_text(s_fb, 470, 972, 32, "关", EPD_DRAW_ALIGN_CENTER, true);
    tr_hline(s_fb, 962 + 56 + 18, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    const int w = (TR_W - 2 * TR_MARGIN - 24) / 2;
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 1076, w, 76}, "‹ 上一话", false, 30);
    tr_pill(s_fb, (EpdRect){TR_W - TR_MARGIN - w, 1076, w, 76}, "下一话 ›", false, 30);
}

// USB 传书说明页。坐标照 app_usb.c 的 draw_intro。/ USB transfer intro; coordinates follow draw_intro.
static void screen_usb(void) {
    clear();
    tr_status_bar(s_fb);
    tr_text(s_fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(s_fb, 96, 100, 36, "USB 传书", EPD_DRAW_ALIGN_LEFT, true);
    tr_icon(s_fb, TR_ICON_USB, TR_W / 2, 250, false);
    static const char* const steps[4] = {"用数据线连上电脑", "点“开始”，电脑上出现 U 盘", "拷完在电脑上“弹出”这个 U 盘",
                                         "回到设备点“结束传输”"};
    for (int i = 0; i < 4; i++) {
        const int y = 350 + i * 46;
        epd_fill_circle(TR_MARGIN + 21, y + 21, 21, UI_GRAY_BLACK, s_fb);
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        tr_text_inv(s_fb, TR_MARGIN + 21, y + 6, 24, n, EPD_DRAW_ALIGN_CENTER, true);
        tr_text(s_fb, TR_MARGIN + 58, y + 2, 28, steps[i], EPD_DRAW_ALIGN_LEFT, false);
    }
    const EpdRect w = {TR_MARGIN, 580, TR_W - 2 * TR_MARGIN, 210};
    ui_fill_round_rect(s_fb, w, 24, TR_GRAY_SURFACE);
    tr_text_on(s_fb, w.x + 26, w.y + 20, 28, "开始后：", EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(s_fb, w.x + 26, w.y + 62, 28, "· 设备其他功能全部暂停，只显示传输页", EPD_DRAW_ALIGN_LEFT, false,
               UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(s_fb, w.x + 26, w.y + 104, 28, "· 换成内置字体，结束后自动换回", EPD_DRAW_ALIGN_LEFT, false, UI_GRAY_BLACK,
               TR_GRAY_SURFACE);
    tr_text_on(s_fb, w.x + 26, w.y + 146, 28, "· Wi-Fi 和后台加载都会停掉", EPD_DRAW_ALIGN_LEFT, false, UI_GRAY_BLACK,
               TR_GRAY_SURFACE);
    tr_wrap(s_fb, TR_MARGIN, 836, TR_W - 2 * TR_MARGIN, 24, 36,
            "TF 卡可用 27.4 GB。书放 books/，漫画放 comics/（一部一个文件夹），字体放 fonts/，壁纸放 wallpapers/。", 3);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 1000, TR_W - 2 * TR_MARGIN, 96}, "开始", true, 34);
}

// Wi-Fi 传书首页的页头和按钮（ui_kit）。坐标照 app_transfer.c 的 render。/ Wi-Fi transfer header and buttons
// (ui_kit); coordinates follow render in app_transfer.c.
static void screen_wifi(void) {
    clear();
    ui_draw_header(s_fb, "传书", "浏览器上传图书或 TTF 字体");
    ui_draw_button(s_fb, ui_row_rect(0, 2, UI_CONTENT_TOP, UI_BTN_H), "设备热点", true);
    ui_draw_button(s_fb, ui_row_rect(1, 2, UI_CONTENT_TOP, UI_BTN_H), "已有 WiFi", false);
    ui_text(s_fb, UI_MARGIN, 274, UI_PX_CAPTION, "TreaderPico-1A2B", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(s_fb, UI_MARGIN, 326, UI_PX_CAPTION, "口令：12345678", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(s_fb, UI_MARGIN, 370, UI_PX_CAPTION, "http://192.168.4.1", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(s_fb, UI_MARGIN, 412, 24, "扫码连接 WiFi", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(s_fb, (EpdRect){UI_MARGIN, 454, 350, 60}, "切换为网页码", false);
    epd_fill_rect((EpdRect){404, 274, 240, 240}, UI_GRAY_BLACK, s_fb);
    epd_fill_rect((EpdRect){424, 294, 200, 200}, UI_GRAY_WHITE, s_fb);
    ui_text(s_fb, UI_MARGIN, 528, UI_PX_CAPTION, "TF 卡 · 可上传 27.4 GB（预留 1515 MB）", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(s_fb, 578, UI_MARGIN, ui_content_width(), UI_GRAY_BLACK);
    ui_text(s_fb, UI_MARGIN, 610, UI_PX_BODY, "等待上传", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(s_fb, UI_MARGIN, 674, UI_PX_CAPTION, "已连接 1 台 · 已完成 2 个文件", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(s_fb, ui_bar_rect(0, 1), "停止并返回", false);
}

int main(int argc, char** argv) {
    const char* font = argc > 1 ? argv[1] : "sdcard/fonts/ChillDuanSansVF.ttf";
    const char* out = argc > 2 ? argv[2] : "build/ui-preview";
    ttf_font_init();
    if (ttf_font_open(font) != ESP_OK) {
        fprintf(stderr, "cannot open font %s\n", font);
        return 1;
    }
    tr_tabs_set_hidden(1u << TR_TAB_X);
    char path[256];
    void (*screens[])(void) = {screen_library, screen_tomato, screen_icons, screen_shelf, screen_toolbar, screen_lock,
                               screen_dialog, screen_comics, screen_comic_bar, screen_usb, screen_wifi};
    const char* names[] = {"library", "tomato", "icons", "shelf", "toolbar", "lock", "dialog", "comics", "comic_bar",
                           "usb", "wifi"};
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        screens[i]();
        snprintf(path, sizeof(path), "%s/%s.png", out, names[i]);
        write_png(path, s_fb);
        printf("wrote %s\n", path);
    }
    return 0;
}
