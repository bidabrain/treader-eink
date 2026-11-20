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
#include "ui_menu.h"
#include "book_cover.h"
#include "read_pico_pmu.h"
#include "settings.h"

#define W TR_W
#define H TR_H

/* ---- epdiy 替身 / epdiy shim ---- */

// 和设备一样：物理缓冲是横屏 1216×684、一字节两个像素，竖屏坐标按 epdiy 的 _rotate 换过去。
// As on the device: the physical buffer is landscape 1216x684 with two pixels per byte; portrait coordinates go
// through epdiy's _rotate.
#define PW H
#define PH W
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const int px = PW - y - 1, py = x;
    uint8_t* b = &fb[py * PW / 2 + px / 2];
    if (px % 2) *b = (*b & 0x0F) | (color & 0xF0);
    else *b = (*b & 0xF0) | (color >> 4);
}
// 读回一个竖屏像素，返回 8 位灰（高 4 位有效）。/ Read one portrait pixel back as 8-bit gray (high nibble).
static uint8_t px_get(const uint8_t* fb, int x, int y) {
    const int px = PW - y - 1, py = x;
    const uint8_t b = fb[py * PW / 2 + px / 2];
    return px % 2 ? (b & 0xF0) : (uint8_t)(b << 4);
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
        for (int x = 0; x < W; x++) raw[y * row + 1 + x] = (uint8_t)((px_get(fb, x, y) >> 4) * 17);
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

static uint8_t s_fb[W * H / 2];

static void clear(void) { memset(s_fb, 0xFF, sizeof(s_fb)); }

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

// 所有图标，以及默认的标签栏。/ Every icon, and the default tab bar.
static void screen_icons(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "图标");
    for (int i = 0; i <= TR_ICON_X; i++) {
        const int cx = TR_MARGIN + 40 + (i % 6) * 104, cy = 220 + (i / 6) * 110;
        tr_icon(s_fb, (tr_icon_t)i, cx, cy, false);
    }
    tr_tab_bar(s_fb, TR_TAB_SETTINGS);
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

/* ---- 第四组：番茄（照 app_tr_tomato.c、app_tr_store.c）/ Group 4: Tomato ---- */

// 番茄书架。坐标照 app_tr_tomato.c 的 draw_list（有待下载时）。/ Tomato shelf with pending updates; follows draw_list.
static void screen_tomato_shelf(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "番茄小说");
    tr_icon(s_fb, TR_ICON_SYNC, TR_W - TR_MARGIN - 150, 116, false);
    tr_text(s_fb, TR_W - TR_MARGIN, 98, 30, "检查更新", EPD_DRAW_ALIGN_RIGHT, false);
    static const char* const seg[2] = {"书架", "书城"};
    tr_segment(s_fb, TR_SEGMENT_RECT, seg, 2, 0);
    const int sh = 78;
    tr_text(s_fb, TR_MARGIN, 156 + sh + 6, 28, "5 本 · 1 本有新章节", EPD_DRAW_ALIGN_LEFT, false);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 316 + sh - 104, TR_W - 2 * TR_MARGIN, 80}, "下载全部更新（1 本）", true, 30);
    struct { const char *t, *meta, *mark; int kind; } rows[6] = {
        {"斗破苍穹", "天蚕土豆 · 1712 章", "+3 章 待下载", 2}, {"十日终焉", "杀虫队队员 · 980 章", "+2 章", 1},
        {"我在精神病院学斩神", "三九音域 · 1420 章", "已读 64%", 0}, {"夜的命名术", "会说话的肘子 · 1100 章", "读完", 0},
        {"深海余烬", "远瞳 · 1030 章", "未读", 0}, {"灵境行者", "卖报小郎君 · 900 章", "已读 12%", 0}};
    for (int i = 0; i < 6; i++) {
        const int y = 420 + sh - 104 + i * 92;
        tr_text(s_fb, TR_MARGIN, y + 8, 34, rows[i].t, EPD_DRAW_ALIGN_LEFT, rows[i].kind > 0);
        tr_text_dim(s_fb, TR_MARGIN, y + 52, 26, rows[i].meta, EPD_DRAW_ALIGN_LEFT);
        if (rows[i].kind) tr_badge(s_fb, TR_W - TR_MARGIN, y + 22, rows[i].mark, rows[i].kind == 2, 24);
        else tr_text_dim(s_fb, TR_W - TR_MARGIN, y + 24, 28, rows[i].mark, EPD_DRAW_ALIGN_RIGHT);
        tr_hline(s_fb, y + 92 - 4, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_tab_bar(s_fb, TR_TAB_TOMATO);
}

// 番茄搜索结果详情。坐标照 draw_detail。/ Tomato search detail; follows draw_detail.
static void screen_tomato_detail(void) {
    clear();
    tr_status_bar(s_fb);
    tr_text(s_fb, TR_MARGIN, 90, 30, "‹ 搜索结果", EPD_DRAW_ALIGN_LEFT, false);
    tr_cover(s_fb, (EpdRect){232, 160, 220, 296}, "深海余烬", "远瞳", "番茄", false);
    tr_text(s_fb, TR_W / 2, 488, 48, "深海余烬", EPD_DRAW_ALIGN_CENTER, true);
    tr_text_dim(s_fb, TR_W / 2, 554, 30, "远瞳", EPD_DRAW_ALIGN_CENTER);
    const int top = 626, cell = (TR_W - 2 * TR_MARGIN) / 4;
    ui_fill_round_rect(s_fb, (EpdRect){TR_MARGIN - 8, top, TR_W - 2 * TR_MARGIN + 16, 116}, 24, TR_GRAY_SURFACE);
    const char* labels[4] = {"分类", "状态", "章节", "字数"};
    const char* values[4] = {"科幻", "连载", "1030", "302 万"};
    for (int i = 0; i < 4; i++) {
        const int cx = TR_MARGIN + i * cell + cell / 2;
        if (i) epd_fill_rect((EpdRect){TR_MARGIN + i * cell, top + 20, 2, 76}, TR_GRAY_LINE, s_fb);
        tr_text_on(s_fb, cx, top + 16, 24, labels[i], EPD_DRAW_ALIGN_CENTER, false, TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
        tr_text_on(s_fb, cx, top + 52, 32, values[i], EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    }
    tr_wrap(s_fb, TR_MARGIN, 772, TR_W - 2 * TR_MARGIN, 28, 44,
            "深海时代，人类文明在无尽的海洋中挣扎求生。一艘名为失乡号的幽灵船载着一个来自异乡的灵魂，驶入了这片被诅咒的海域……", 4);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 960, TR_W - 2 * TR_MARGIN, 88}, "下载到书库", true, 32);
    tr_tab_bar(s_fb, TR_TAB_TOMATO);
}

// 书城首页：最上面是搜索栏。坐标照 app_tr_store.c 的 draw_home。/ Bookstore home with the search bar on top; follows
// draw_home.
static void screen_store_home(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "番茄小说");
    static const char* const seg[2] = {"书架", "书城"};
    tr_segment(s_fb, TR_SEGMENT_RECT, seg, 2, 1);
    const EpdRect bar = {TR_MARGIN, 244, TR_W - 2 * TR_MARGIN, 80};
    ui_fill_round_rect(s_fb, bar, bar.height / 2, TR_GRAY_SURFACE);
    tr_icon_on(s_fb, TR_ICON_SEARCH, bar.x + 50, bar.y + 40, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(s_fb, bar.x + 86, bar.y + 22, 30, "搜索书名或作者", EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY,
               TR_GRAY_SURFACE);
    const int dy = 104;
    tr_text(s_fb, TR_MARGIN, 258 + dy, 28, "榜单 2026-11-20", EPD_DRAW_ALIGN_LEFT, false);
    tr_icon(s_fb, TR_ICON_SYNC, TR_W - TR_MARGIN - 96, 274 + dy, false);
    tr_text(s_fb, TR_W - TR_MARGIN, 254 + dy, 30, "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    tr_pill(s_fb, (EpdRect){TR_MARGIN, 322 + dy, 120, 62}, "男频", true, 30);
    tr_pill(s_fb, (EpdRect){TR_MARGIN + 134, 322 + dy, 120, 62}, "女频", false, 30);
    tr_hline(s_fb, 404 + dy, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    static const char* const cats[] = {"都市日常", "玄幻", "仙侠", "历史", "科幻末世", "悬疑灵异", "都市高武", "游戏体育",
                                       "西方奇幻", "抗战谍战", "传统玄幻", "都市修真", "系统流", "穿越", "重生", "神豪"};
    int x = TR_MARGIN, y = 430 + dy;
    for (int i = 0; i < (int)(sizeof(cats) / sizeof(cats[0])); i++) {
        const int w = ttf_text_width_px(30, cats[i]) + 44;
        if (x + w > TR_W - TR_MARGIN + 10) { x = TR_MARGIN; y += 76; }
        tr_pill(s_fb, (EpdRect){x, y, w, 62}, cats[i], i == 0, 30);
        x += w + 12;
    }
    tr_tab_bar(s_fb, TR_TAB_TOMATO);
}

// 书城榜单网格。坐标照 app_tr_store.c 的 draw_rank。/ Bookstore rank grid; follows draw_rank.
static void screen_store_rank(void) {
    clear();
    tr_status_bar(s_fb);
    tr_text(s_fb, TR_MARGIN, 92, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(s_fb, 96, 96, 40, "都市日常", EPD_DRAW_ALIGN_LEFT, true);
    tr_text_dim(s_fb, TR_W - TR_MARGIN, 104, 28, "男频 · 2026-11-20", EPD_DRAW_ALIGN_RIGHT);
    tr_hline(s_fb, 170, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    const int gap = (TR_W - 2 * TR_MARGIN - 3 * 150) / 2;
    static const char* const t[9] = {"十日终焉", "深海余烬", "夜的命名术", "斗破苍穹", "宿命之环", "三体", "灵境行者",
                                     "我在精神病院学斩神", "道诡异仙"};
    for (int k = 0; k < 9; k++) {
        const EpdRect r = {TR_MARGIN + (k % 3) * (150 + gap), 186 + (k / 3) * 284, 150, 200};
        tr_cover(s_fb, r, t[k], "", "", k % 2 == 0);
        char line[96];
        snprintf(line, sizeof(line), "%d %s", k + 1, t[k]);
        tr_fit(line, 24, 150);
        tr_text(s_fb, r.x, r.y + r.height + 8, 24, line, EPD_DRAW_ALIGN_LEFT, true);
        if (k == 1) tr_text(s_fb, r.x, r.y + r.height + 46, 22, "已在书架", EPD_DRAW_ALIGN_LEFT, true);
        else tr_text_dim(s_fb, r.x, r.y + r.height + 46, 22, "在读 25.4万", EPD_DRAW_ALIGN_LEFT);
    }
    tr_tab_bar(s_fb, TR_TAB_TOMATO);
}

/* ---- 第五组：虎扑（照 app_tr_hupu.c）/ Group 5: Hupu ---- */

static void hupu_header(int seg) {
    tr_status_bar(s_fb);
    tr_title(s_fb, "虎扑");
    tr_icon(s_fb, TR_ICON_SYNC, TR_W - TR_MARGIN - 86, 116, false);
    tr_text(s_fb, TR_W - TR_MARGIN, 98, 30, "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    static const char* const s3[3] = {"推荐", "关注", "比分"};
    tr_segment(s_fb, TR_SEGMENT_RECT, s3, 3, seg);
}

// 虎扑推荐。坐标照 draw_feed / draw_rows。/ Hupu feed; follows draw_feed and draw_rows.
static void screen_hupu_feed(void) {
    clear();
    hupu_header(0);
    tr_text_dim(s_fb, TR_MARGIN, 240, 26, "首页推荐 · 30 条", EPD_DRAW_ALIGN_LEFT);
    static const char* const t[5] = {"詹姆斯赛后：我们需要更好地保护篮板，今天对手的二次进攻太多了",
                                     "[流言板] 曼城官方：哈兰德完成续约，合同至 2034 年",
                                     "虎扑评分：本赛季到目前为止最让你惊喜的新秀是谁？",
                                     "步行街主干道：第一次自己装修，踩过的坑都在这里了",
                                     "国足最新一期集训名单公布，三名归化球员入选"};
    static const char* const m[5] = {"湿乎乎的话题 · 312 回复", "英超 · 1024 回复", "NBA · 586 回复", "步行街 · 233 回复",
                                     "中国足球 · 871 回复"};
    for (int i = 0; i < 5; i++) {
        const int y = 284 + i * 148, thumb = i % 2 == 0;
        const int tw = thumb ? TR_W - TR_MARGIN - 150 - TR_MARGIN - 20 : TR_W - 2 * TR_MARGIN;
        tr_wrap(s_fb, TR_MARGIN, y + 10, tw, 30, 40, t[i], 2);
        tr_text_dim(s_fb, TR_MARGIN, y + 100, 24, m[i], EPD_DRAW_ALIGN_LEFT);
        if (thumb) ui_fill_round_rect(s_fb, (EpdRect){TR_W - TR_MARGIN - 150, y + 18, 150, 100}, 12, TR_GRAY_SURFACE);
        tr_hline(s_fb, y + 148 - 6, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(s_fb, 1040, 0, 6);
    tr_tab_bar(s_fb, TR_TAB_HUPU);
}

// 虎扑比分。坐标照 draw_scores。/ Hupu scores; follows draw_scores.
static void screen_hupu_scores(void) {
    clear();
    hupu_header(2);
    static const char* const lg[4] = {"NBA", "CBA", "英超", "中超"};
    int x = TR_MARGIN;
    for (int i = 0; i < 4; i++) {
        const int w = ttf_text_width_px(28, lg[i]) + 48;
        tr_pill(s_fb, (EpdRect){x, 244, w, 54}, lg[i], i == 0, 28);
        x += w + 12;
    }
    tr_text(s_fb, TR_W / 2, 320, 30, "11月20日 星期四", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(s_fb, TR_MARGIN + 20, 320, 30, "‹", EPD_DRAW_ALIGN_CENTER, true);
    tr_text(s_fb, TR_W - TR_MARGIN - 20, 320, 30, "›", EPD_DRAW_ALIGN_CENTER, true);
    tr_hline(s_fb, 378, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    struct { const char *a, *b, *score, *st; } m[5] = {
        {"湖人", "勇士", "112 : 108", "已结束 · 3.2万人评分 ›"}, {"凯尔特人", "热火", "98 : 101", "已结束 · 1.1万人评分 ›"},
        {"掘金", "太阳", "56 : 49", "第二节 05:12"}, {"雄鹿", "尼克斯", "VS", "19:30 开赛"}, {"快船", "独行侠", "VS", "21:00 开赛"}};
    for (int i = 0; i < 5; i++) {
        const int y = 396 + i * 124;
        ui_fill_round_rect(s_fb, (EpdRect){TR_MARGIN - 8, y, TR_W - 2 * TR_MARGIN + 16, 112}, 20, TR_GRAY_SURFACE);
        tr_text_on(s_fb, TR_MARGIN + 95, y + 16, 32, m[i].a, EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        tr_text_on(s_fb, TR_W - TR_MARGIN - 95, y + 16, 32, m[i].b, EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK,
                   TR_GRAY_SURFACE);
        tr_text_on(s_fb, TR_W / 2, y + 8, 40, m[i].score, EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        tr_text_on(s_fb, TR_W / 2, y + 70, 24, m[i].st, EPD_DRAW_ALIGN_CENTER, false, TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    }
    tr_tab_bar(s_fb, TR_TAB_HUPU);
}

// 虎扑回复页。坐标照 draw_post / draw_reply。/ Hupu replies; follows draw_post and draw_reply.
static void screen_hupu_post(void) {
    clear();
    tr_status_bar(s_fb);
    tr_text(s_fb, TR_MARGIN, 92, 30, "‹ 湿乎乎的话题", EPD_DRAW_ALIGN_LEFT, false);
    static const char* const tabs[3] = {"正文", "亮了 12", "回复 312"};
    tr_segment(s_fb, (EpdRect){TR_MARGIN, 150, TR_W - 2 * TR_MARGIN, 64}, tabs, 3, 1);
    struct { const char *user, *quote, *text; int lights; } r[3] = {
        {"篮板狂魔", NULL, "说得对，二次进攻给太多了，内线必须加强卡位，替补阵容也得提高对抗强度。", 356},
        {"紫金王朝", "篮板狂魔：说得对，二次进攻给太多了", "关键还是轮换，主力打太久了，第四节明显腿软。", 128},
        {"路人甲", NULL, "赢了就行，常规赛别要求太高。", 42}};
    int y = 250;
    for (int i = 0; i < 3; i++) {
        tr_text(s_fb, TR_MARGIN, y, 26, r[i].user, EPD_DRAW_ALIGN_LEFT, true);
        char l[16];
        snprintf(l, sizeof(l), "亮 %d", r[i].lights);
        tr_badge(s_fb, TR_W - TR_MARGIN, y - 4, l, false, 22);
        int yy = y + 40;
        if (r[i].quote) {
            epd_fill_rect((EpdRect){TR_MARGIN, yy, 6, 30}, 0x90, s_fb);
            char q[96];
            snprintf(q, sizeof(q), "引用 %s", r[i].quote);
            tr_text_dim(s_fb, TR_MARGIN + 22, yy, 24, q, EPD_DRAW_ALIGN_LEFT);
            yy += 40;
        }
        yy += 46 * tr_wrap(s_fb, TR_MARGIN, yy, TR_W - 2 * TR_MARGIN, 32, 46, r[i].text, 4);
        y = yy + 30;
        tr_hline(s_fb, y - 16, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(s_fb, 1040, 0, 8);
    tr_tab_bar(s_fb, TR_TAB_HUPU);
}

/* ---- 第六组：微博、X、小红书（照 app_tr_weibo.c、app_tr_xhs.c）/ Group 6: Weibo, X, Xiaohongshu ---- */

static void sns_header(const char* title, const char* a, const char* b, const char* refresh) {
    tr_status_bar(s_fb);
    tr_title(s_fb, title);
    tr_icon(s_fb, TR_ICON_SYNC, TR_W - TR_MARGIN - ttf_text_width_px(30, refresh) - 26, 116, false);
    tr_text(s_fb, TR_W - TR_MARGIN, 98, 30, refresh, EPD_DRAW_ALIGN_RIGHT, false);
    const char* seg[2] = {a, b};
    tr_segment(s_fb, TR_SEGMENT_RECT, seg, 2, 0);
}

static void sns_avatar(int x, int y, int d, const char* first, bool dot) {
    const int cx = x + d / 2, cy = y + d / 2, px = d * 28 / 60;
    epd_fill_circle(cx, cy, d / 2, TR_GRAY_SELECTED, s_fb);
    tr_text_on(s_fb, cx, cy - px / 2 - 3, px, first, EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK, TR_GRAY_SELECTED);
    if (dot) {
        epd_fill_circle(x + d - 6, y + 6, 10, UI_GRAY_WHITE, s_fb);
        epd_fill_circle(x + d - 6, y + 6, 7, UI_GRAY_BLACK, s_fb);
    }
}

static void sns_counts(int y, const char* a, const char* b, const char* c) {
    const char* v[3] = {a, b, c};
    const int cw = TR_W - 2 * TR_MARGIN;
    for (int k = 0; k < 3; k++) {
        const int w = ttf_text_width_px(24, v[k]) + 40, x0 = TR_MARGIN + k * cw / 3 + (cw / 3 - w) / 2;
        epd_fill_circle(x0 + 14, y + 22, 9, TR_GRAY_SECONDARY, s_fb);
        tr_text_dim(s_fb, x0 + 40, y + 8, 24, v[k], EPD_DRAW_ALIGN_LEFT);
    }
}

// 微博关注列表：一条一张卡片。坐标照 draw_card。/ Weibo following list, one card per post; follows draw_card.
static void screen_weibo(void) {
    clear();
    sns_header("微博", "关注", "热门", "刷新");
    tr_text_dim(s_fb, TR_MARGIN, 240, 26, "关注的人 · 100 条 · 刷新于 21:05", EPD_DRAW_ALIGN_LEFT);
    const int cw = TR_W - 2 * TR_MARGIN, sw = (cw - 22) / 3;
    int y = 288;
    sns_avatar(TR_MARGIN, y, 60, "科", true);
    tr_text(s_fb, TR_MARGIN + 78, y + 2, 30, "科技美学", EPD_DRAW_ALIGN_LEFT, true);
    tr_text_dim(s_fb, TR_MARGIN + 78, y + 40, 22, "10 分钟前 · 微博网页版", EPD_DRAW_ALIGN_LEFT);
    tr_wrap(s_fb, TR_MARGIN, y + 80, cw, 30, 42,
            "今天上手了新款电子书阅读器，4.7 寸墨水屏，前光可以调色温，整体做工比上一代好不少。续航还在测。", 3);
    for (int k = 0; k < 3; k++) ui_fill_round_rect(s_fb, (EpdRect){TR_MARGIN + k * (sw + 11), y + 222, sw, 164}, 12,
                                                   TR_GRAY_SURFACE);
    const int bw = ttf_text_width_px(24, "+3") + 20;
    ui_fill_round_rect(s_fb, (EpdRect){TR_MARGIN + 2 * (sw + 11) + sw - bw - 6, y + 222 + 164 - 40, bw, 34}, 17,
                       UI_GRAY_BLACK);
    tr_text_inv(s_fb, TR_MARGIN + 2 * (sw + 11) + sw - 6 - bw / 2, y + 222 + 164 - 36, 24, "+3", EPD_DRAW_ALIGN_CENTER, true);
    sns_counts(y + 396, "128", "56", "1024");
    y += 460;
    tr_hline(s_fb, y - 9, TR_MARGIN, cw, true);
    sns_avatar(TR_MARGIN, y, 60, "读", false);
    tr_text(s_fb, TR_MARGIN + 78, y + 2, 30, "读书日记", EPD_DRAW_ALIGN_LEFT, true);
    tr_text_dim(s_fb, TR_MARGIN + 78, y + 40, 22, "1 小时前 · iPhone", EPD_DRAW_ALIGN_LEFT);
    tr_wrap(s_fb, TR_MARGIN, y + 80, cw, 30, 42, "转发：这本书读到第三遍才看懂结尾。", 3);
    ui_fill_round_rect(s_fb, (EpdRect){TR_MARGIN, y + 134, cw, 58}, 14, 0xE0);
    tr_text_on(s_fb, TR_MARGIN + 20, y + 144, 26, "@出版社：《长安的荔枝》精装版今天上市。", EPD_DRAW_ALIGN_LEFT, false,
               UI_GRAY_BLACK, 0xE0);
    sns_counts(y + 202, "12", "8", "230");
    tr_pager(s_fb, 1040, 0, 34);
    tr_tab_bar(s_fb, TR_TAB_WEIBO);
}

// 小红书关注：两列封面卡片。坐标照 draw_cards。/ Xiaohongshu following: two-column cover cards; follows draw_cards.
static void screen_xhs(void) {
    clear();
    sns_header("小红书", "关注", "推荐", "刷新");
    tr_text_dim(s_fb, TR_MARGIN, 240, 26, "关注的博主 · 36 篇", EPD_DRAW_ALIGN_LEFT);
    const int cw = (TR_W - 2 * TR_MARGIN - 20) / 2;
    static const char* const t[4] = {"周末去了一趟安吉，竹林真的太出片了", "极简书桌改造｜一千块搞定",
                                     "秋天的第一锅关东煮，汤底这样熬", "Kindle 之外，我还在用的阅读器"};
    static const char* const a[4] = {"山野小记 · 赞 2.3万", "Lin 的家 · 赞 8412", "厨房日记 · 赞 1.1万", "读书的猫 · 赞 5206"};
    for (int k = 0; k < 4; k++) {
        const int cx = TR_MARGIN + (k % 2) * (cw + 20), cy = 284 + (k / 2) * 376;
        ui_fill_round_rect(s_fb, (EpdRect){cx, cy, cw, 262}, 12, TR_GRAY_SURFACE);
        if (k % 3 == 0) {
            ui_fill_round_rect(s_fb, (EpdRect){cx + 10, cy + 10, 48, 34}, 17, UI_GRAY_BLACK);
            tr_text_inv(s_fb, cx + 34, cy + 13, 22, "新", EPD_DRAW_ALIGN_CENTER, true);
        }
        tr_wrap(s_fb, cx, cy + 262 + 10, cw, 26, 34, t[k], 2);
        tr_text_dim(s_fb, cx, cy + 262 + 84, 22, a[k], EPD_DRAW_ALIGN_LEFT);
    }
    tr_pager(s_fb, 1040, 0, 9);
    tr_tab_bar(s_fb, TR_TAB_XHS);
}

// tr_pin.c 的布局常量（照抄）。/ Layout constants copied from tr_pin.c.
#define PIN_TITLE_Y 190
#define PIN_HINT_Y 262
#define PIN_BOX_W 80
#define PIN_BOX_H 96
#define PIN_BOX_GAP 24
#define PIN_BOX_Y 330
#define PIN_BOX_X ((TR_W - 4 * PIN_BOX_W - 3 * PIN_BOX_GAP) / 2)
#define PIN_KEY_W 180
#define PIN_KEY_H 150
#define PIN_KEY_GAP 24

/* ---- 第七组：设置、锁屏密码（照 app_tr_settings.c、tr_pin.c）/ Group 7: settings and lock PIN ---- */

static void set_cat(int y, tr_icon_t icon, const char* name, const char* sum, bool last) {
    const EpdRect box = {TR_MARGIN, y + 30, 76, 76};
    ui_fill_round_rect(s_fb, box, 38, TR_GRAY_SURFACE);
    tr_icon_on(s_fb, icon, box.x + 38, box.y + 38, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text(s_fb, TR_MARGIN + 104, y + 24, 34, name, EPD_DRAW_ALIGN_LEFT, true);
    char s[200];
    snprintf(s, sizeof(s), "%s", sum);
    tr_fit(s, 26, TR_W - 2 * TR_MARGIN - 144);
    tr_text_dim(s_fb, TR_MARGIN + 104, y + 78, 26, s, EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(s_fb, TR_W - TR_MARGIN, y + 46, 34, "›", EPD_DRAW_ALIGN_RIGHT);
    if (!last) tr_hline(s_fb, y + 136 - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

// 设置首页。坐标照 draw_home / draw_cat。/ Settings home; follows draw_home and draw_cat.
static void screen_settings(void) {
    clear();
    tr_status_bar(s_fb);
    tr_title(s_fb, "设置");
    static const tr_icon_t icons[6] = {TR_ICON_FONT, TR_ICON_TABS, TR_ICON_WIFI, TR_ICON_SOURCE, TR_ICON_SYNC,
                                       TR_ICON_POWER};
    static const char* const names[6] = {"显示与阅读", "底部标签", "传书", "内容来源", "同步", "电源与设备"};
    static const char* const sums[6] = {"寒蝉端黑体 · 锁屏壁纸 无", "显示 7 个：书库、漫画、番茄、小红书、虎扑、微博、设置",
                                        "Wi-Fi 传书：Home · USB 传书", "番茄内置 · 小红书已设置 · 微博已设置 · X未设置",
                                        "dav.example.com", "自动锁屏 2 分钟 · 有密码 · TF 卡 29.7 GB"};
    for (int i = 0; i < 6; i++) set_cat(170 + i * 136, icons[i], names[i], sums[i], i == 5);
    tr_text_dim(s_fb, TR_W / 2, 1030, 24, "Treader 0.8.0", EPD_DRAW_ALIGN_CENTER);
    tr_tab_bar(s_fb, TR_TAB_SETTINGS);
}

static void set_switch(int right, int y, bool on) {
    const EpdRect r = {right - 100, y, 100, 54};
    if (on) {
        ui_fill_round_rect(s_fb, r, 27, UI_GRAY_BLACK);
        epd_fill_circle(r.x + r.width - 27, y + 27, 19, UI_GRAY_WHITE, s_fb);
    } else {
        ui_draw_round_rect(s_fb, r, 27, TR_GRAY_OUTLINE);
        ui_draw_round_rect(s_fb, ui_inset_rect(r, 1), 26, TR_GRAY_OUTLINE);
        epd_fill_circle(r.x + 27, y + 27, 13, TR_GRAY_OUTLINE, s_fb);
    }
}

// 设置 › 底部标签：放在底部 / 只在“全部”里 / 已隐藏三段。照 app_tr_settings.c 的 tb_layout 和 draw_tabs。
// Settings > tab bar: pinned / All page only / hidden; follows tb_layout and draw_tabs in app_tr_settings.c.
#define TAB_TOP 210
#define TAB_H 70
#define TAB_SEC_H 40
#define TAB_SEC_GAP 22
#define TAB_BTN_H 52
static void tb_btn(int x, int y, int w, const char* label, int arrow, bool enabled) {
    const EpdRect r = {x, y, w, TAB_BTN_H};
    if (enabled) {
        ui_draw_round_rect(s_fb, r, r.height / 2, TR_GRAY_OUTLINE);
        ui_draw_round_rect(s_fb, ui_inset_rect(r, 1), r.height / 2 - 1, TR_GRAY_OUTLINE);
    } else {
        ui_fill_round_rect(s_fb, r, r.height / 2, TR_GRAY_SURFACE);
    }
    const uint8_t ink = enabled ? UI_GRAY_BLACK : TR_GRAY_OUTLINE, bg = enabled ? UI_GRAY_WHITE : TR_GRAY_SURFACE;
    if (arrow) {
        const int cx = r.x + r.width / 2, cy = r.y + r.height / 2, a = 10, dy = arrow < 0 ? -a / 2 : a / 2;
        for (int d = -1; d <= 1; d++) {
            epd_draw_line(cx - a + d, cy - dy, cx + d, cy + dy, ink, s_fb);
            epd_draw_line(cx + d, cy + dy, cx + a + d, cy - dy, ink, s_fb);
        }
        return;
    }
    tr_text_on(s_fb, r.x + r.width / 2, r.y + (r.height - 26) / 2 - 2, 26, label, EPD_DRAW_ALIGN_CENTER, false, ink, bg);
}
static int tb_w(const char* label) { return ttf_text_width_px(26, label) + 32; }
static void draw_tabs_page(const uint8_t* pins, int pin_n, uint8_t hidden, const char* message) {
    clear();
    tr_tabs_configure(hidden, pins, pin_n);
    tr_status_bar(s_fb);
    tr_text(s_fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(s_fb, 96, 100, 36, "底部标签", EPD_DRAW_ALIGN_LEFT, true);
    if (message) tr_message(s_fb, TR_W - TR_MARGIN, 104, 92, 360, 26, message, EPD_DRAW_ALIGN_RIGHT, false);
    char line[96];
    snprintf(line, sizeof(line), "底部放 %d 个，最后一格固定是“全部”", tr_tab_slots() - 1);
    tr_text_dim(s_fb, TR_MARGIN, 158, 26, line, EPD_DRAW_ALIGN_LEFT);
    const int cap = tr_tab_slots() - 2;
    int y = TAB_TOP;
    tr_text_dim(s_fb, TR_MARGIN, y, 26, "放在底部（按顺序）", EPD_DRAW_ALIGN_LEFT);
    y += TAB_SEC_H;
    for (int i = -1; i < pin_n; i++) {
        const tr_tab_t t = i < 0 ? TR_TAB_LIBRARY : (tr_tab_t)pins[i];
        snprintf(line, sizeof(line), "%d", i + 2);
        tr_text_dim(s_fb, TR_MARGIN + 6, y + (TAB_H - 28) / 2 - 2, 28, line, EPD_DRAW_ALIGN_LEFT);
        const int x = TR_MARGIN + 40;
        tr_icon(s_fb, tr_tab_icon(t), x + 20, y + TAB_H / 2, false);
        tr_text(s_fb, x + 60, y + (TAB_H - 32) / 2 - 2, 32, tr_tab_label(t), EPD_DRAW_ALIGN_LEFT, false);
        const int top = y + (TAB_H - TAB_BTN_H) / 2;
        if (i < 0) {
            tr_text_dim(s_fb, TR_W - TR_MARGIN, y + (TAB_H - 26) / 2, 26, "固定第一格（主页）", EPD_DRAW_ALIGN_RIGHT);
        } else {
            if (i >= cap)
                tr_text_dim(s_fb, x + 60 + ttf_text_width_px(32, tr_tab_label(t)) + 16, y + (TAB_H - 24) / 2, 24,
                            "这一档在“全部”里", EPD_DRAW_ALIGN_LEFT);
            int right = TR_W - TR_MARGIN;
            tb_btn(right - tb_w("移出"), top, tb_w("移出"), "移出", 0, true);
            right -= tb_w("移出") + 14;
            tb_btn(right - TAB_BTN_H, top, TAB_BTN_H, "", 1, i + 1 < pin_n);
            right -= TAB_BTN_H + 10;
            tb_btn(right - TAB_BTN_H, top, TAB_BTN_H, "", -1, i > 0);
        }
        if (i + 1 < pin_n) tr_hline(s_fb, y + TAB_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
        y += TAB_H;
    }
    y += TAB_SEC_GAP;
    tr_text_dim(s_fb, TR_MARGIN, y, 26, "只在“全部”里", EPD_DRAW_ALIGN_LEFT);
    y += TAB_SEC_H;
    int rest[TR_TAB_MAX], rn = 0;
    for (int t = 1; t < TR_TAB_MAX; t++) {
        bool pinned = false;
        for (int i = 0; i < pin_n; i++) pinned |= pins[i] == t;
        if (!pinned && !(hidden & (1u << t))) rest[rn++] = t;
    }
    for (int k = 0; k < rn; k++) {
        const tr_tab_t t = (tr_tab_t)rest[k];
        tr_icon(s_fb, tr_tab_icon(t), TR_MARGIN + 20, y + TAB_H / 2, false);
        tr_text(s_fb, TR_MARGIN + 60, y + (TAB_H - 32) / 2 - 2, 32, tr_tab_label(t), EPD_DRAW_ALIGN_LEFT, false);
        const int top = y + (TAB_H - TAB_BTN_H) / 2;
        int right = TR_W - TR_MARGIN;
        tb_btn(right - tb_w("放到底部"), top, tb_w("放到底部"), "放到底部", 0, pin_n < cap);
        right -= tb_w("放到底部") + 14;
        if (!(TR_TABS_FIXED & (1u << t))) tb_btn(right - tb_w("隐藏"), top, tb_w("隐藏"), "隐藏", 0, true);
        if (k + 1 < rn) tr_hline(s_fb, y + TAB_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
        y += TAB_H;
    }
    y += TAB_SEC_GAP;
    if (hidden) {
        tr_text_dim(s_fb, TR_MARGIN, y, 26, "已隐藏（“全部”里也不显示）", EPD_DRAW_ALIGN_LEFT);
        y += TAB_SEC_H;
        for (int t = 1; t < TR_TAB_MAX; t++) {
            if (!(hidden & (1u << t))) continue;
            tr_icon(s_fb, tr_tab_icon((tr_tab_t)t), TR_MARGIN + 20, y + TAB_H / 2, false);
            tr_text_dim(s_fb, TR_MARGIN + 60, y + (TAB_H - 32) / 2 - 2, 32, tr_tab_label((tr_tab_t)t), EPD_DRAW_ALIGN_LEFT);
            tb_btn(TR_W - TR_MARGIN - tb_w("显示"), y + (TAB_H - TAB_BTN_H) / 2, tb_w("显示"), "显示", 0, true);
            y += TAB_H;
        }
    }
    tr_tab_bar(s_fb, TR_TAB_SETTINGS);
}
static const uint8_t DEF_PINS[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU};
// 默认：底部满了。/ Default: the bar is full.
static void screen_set_tabs(void) { draw_tabs_page(DEF_PINS, 4, 1u << TR_TAB_X, NULL); }
// 移出一个、隐藏虎扑：“放到底部”能按。/ One unpinned and Hupu hidden: "Pin" is enabled.
static void screen_set_tabs_room(void) {
    static const uint8_t pins[] = {TR_TAB_TOMATO, TR_TAB_COMIC, TR_TAB_XHS};
    draw_tabs_page(pins, 3, (1u << TR_TAB_X) | (1u << TR_TAB_HUPU), NULL);
}
// 底部满了还按“放到底部”。/ Pinning with a full bar.
static void screen_set_tabs_full(void) { draw_tabs_page(DEF_PINS, 4, 1u << TR_TAB_X, "底部满了，先移出一个"); }

// “全部”页。照 app_tr_all.c 的 render。/ The All page; follows render in app_tr_all.c.
#define ALL_GRID_TOP 212
#define ALL_COLS 3
#define ALL_GAP 16
#define ALL_CELL_W ((TR_W - 2 * TR_MARGIN - (ALL_COLS - 1) * ALL_GAP) / ALL_COLS)
#define ALL_CELL_H 170
#define ALL_PAGER_Y 1010
#define ALL_ROWS ((ALL_PAGER_Y - 20 - ALL_GRID_TOP + ALL_GAP) / (ALL_CELL_H + ALL_GAP))
static void draw_all_page(uint8_t hidden, int from, int page) {
    clear();
    tr_tabs_configure(hidden, DEF_PINS, 4);
    tr_status_bar(s_fb);
    tr_title(s_fb, "全部");
    tr_text_dim(s_fb, TR_MARGIN, 156, 26, "底部放哪几个：设置 › 底部标签", EPD_DRAW_ALIGN_LEFT);
    tr_tab_t tabs[TR_TAB_MAX];
    int n = 0;
    for (int t = 0; t < TR_TAB_MAX; t++)
        if (tr_tab_shown((tr_tab_t)t)) tabs[n++] = (tr_tab_t)t;
    const int per = ALL_ROWS * ALL_COLS;
    for (int k = 0; k < per; k++) {
        const int i = page * per + k;
        if (i >= n) break;
        const EpdRect r = {TR_MARGIN + (k % ALL_COLS) * (ALL_CELL_W + ALL_GAP),
                           ALL_GRID_TOP + (k / ALL_COLS) * (ALL_CELL_H + ALL_GAP), ALL_CELL_W, ALL_CELL_H};
        const bool on = (int)tabs[i] == from;
        const uint8_t bg = on ? TR_GRAY_SELECTED : TR_GRAY_SURFACE;
        ui_fill_round_rect(s_fb, r, 24, bg);
        tr_icon_on(s_fb, tr_tab_icon(tabs[i]), r.x + r.width / 2, r.y + 62, UI_GRAY_BLACK, bg);
        tr_text_on(s_fb, r.x + r.width / 2, r.y + 108, 30, tr_tab_label(tabs[i]), EPD_DRAW_ALIGN_CENTER, on,
                   UI_GRAY_BLACK, bg);
    }
    tr_pager(s_fb, ALL_PAGER_Y, page, n ? (n + per - 1) / per : 1);
    tr_tab_bar(s_fb, (tr_tab_t)TR_TAB_ALL);
}
// 从微博打开（微博不在底部）。/ Opened from Weibo (not pinned).
static void screen_all(void) { draw_all_page(1u << TR_TAB_X, TR_TAB_WEIBO, 0); }
// X 也显示：八个功能。/ X shown too: all eight features.
static void screen_all_x(void) { draw_all_page(0, TR_TAB_TOMATO, 0); }

// 锁屏密码键盘。坐标照 tr_pin.c 的 draw_all。/ Lock PIN keypad; follows draw_all in tr_pin.c.
static void screen_pin(void) {
    clear();
    tr_text(s_fb, TR_W / 2, PIN_TITLE_Y, 40, "输入密码解锁", EPD_DRAW_ALIGN_CENTER, true);
    tr_text_dim(s_fb, TR_W / 2, PIN_HINT_Y, 26, "输入 4 位数字", EPD_DRAW_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        const EpdRect r = {PIN_BOX_X + i * (PIN_BOX_W + PIN_BOX_GAP), PIN_BOX_Y, PIN_BOX_W, PIN_BOX_H};
        ui_draw_round_rect(s_fb, r, 10, TR_GRAY_OUTLINE);
        ui_draw_round_rect(s_fb, ui_inset_rect(r, 1), 9, TR_GRAY_OUTLINE);
        if (i < 2) epd_fill_circle(r.x + PIN_BOX_W / 2, r.y + PIN_BOX_H / 2, 14, UI_GRAY_BLACK, s_fb);
    }
    static const int keys[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
    const int kx = (TR_W - 3 * PIN_KEY_W - 2 * PIN_KEY_GAP) / 2;
    for (int i = 0; i < 12; i++) {
        const int k = keys[i];
        const EpdRect r = {kx + (i % 3) * (PIN_KEY_W + PIN_KEY_GAP), 520 + (i / 3) * (PIN_KEY_H + PIN_KEY_GAP), PIN_KEY_W,
                           PIN_KEY_H};
        if (k <= 9) ui_fill_round_rect(s_fb, r, 24, TR_GRAY_SURFACE);
        else {
            ui_draw_round_rect(s_fb, r, 24, TR_GRAY_OUTLINE);
            ui_draw_round_rect(s_fb, ui_inset_rect(r, 1), 23, TR_GRAY_OUTLINE);
        }
        char label[8];
        if (k <= 9) snprintf(label, sizeof(label), "%d", k);
        else snprintf(label, sizeof(label), "%s", k == 10 ? "取消" : "删除");
        const int px = k <= 9 ? 56 : 34;
        tr_text_on(s_fb, r.x + PIN_KEY_W / 2, r.y + (PIN_KEY_H - px) / 2 - 4, px, label, EPD_DRAW_ALIGN_CENTER, k <= 9,
                   UI_GRAY_BLACK, k <= 9 ? TR_GRAY_SURFACE : UI_GRAY_WHITE);
    }
}

int main(int argc, char** argv) {
    const char* font = argc > 1 ? argv[1] : "sdcard/fonts/ChillDuanSansVF.ttf";
    const char* out = argc > 2 ? argv[2] : "build/ui-preview";
    ttf_font_init();
    if (ttf_font_open(font) != ESP_OK) {
        fprintf(stderr, "cannot open font %s\n", font);
        return 1;
    }
    tr_tabs_configure(1u << TR_TAB_X, DEF_PINS, 4);
    char path[256];
    void (*screens[])(void) = {screen_library, screen_tomato, screen_icons, screen_shelf, screen_toolbar, screen_lock,
                               screen_dialog, screen_comics, screen_comic_bar, screen_usb, screen_wifi,
                               screen_tomato_shelf, screen_tomato_detail, screen_store_rank, screen_store_home,
                               screen_hupu_feed, screen_hupu_scores, screen_hupu_post, screen_weibo, screen_xhs,
                               screen_settings, screen_set_tabs, screen_set_tabs_room, screen_set_tabs_full,
                               screen_all, screen_all_x, screen_pin};
    const char* names[] = {"library", "tomato", "icons", "shelf", "toolbar", "lock", "dialog", "comics", "comic_bar",
                           "usb", "wifi", "tomato_shelf", "tomato_detail", "store_rank", "store_home", "hupu_feed", "hupu_scores", "hupu_post", "weibo", "xhs", "settings", "set_tabs", "set_tabs_room", "set_tabs_full", "all", "all_x", "pin"};
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        screens[i]();
        snprintf(path, sizeof(path), "%s/%s.png", out, names[i]);
        write_png(path, s_fb);
        printf("wrote %s\n", path);
    }
    return 0;
}
