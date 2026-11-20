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
    void (*screens[])(void) = {screen_library, screen_tomato, screen_icons};
    const char* names[] = {"library", "tomato", "icons"};
    for (int i = 0; i < 3; i++) {
        screens[i]();
        snprintf(path, sizeof(path), "%s/%s.png", out, names[i]);
        write_png(path, s_fb);
        printf("wrote %s\n", path);
    }
    return 0;
}
