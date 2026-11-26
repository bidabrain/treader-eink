/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：页面渲染测试台的公共部分：epdiy 绘图替身（和设备一样的横屏 4bpp 缓冲）、PNG 输出、检查和主程序。
 * English: Shared part of the page render bench: epdiy drawing shim (the device's landscape 4bpp buffer), PNG output,
 * checks and main.
 *
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#define _GNU_SOURCE
#include <execinfo.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "page_render.h"
#include "read_pico_pmu.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"

// 基线模式：用改动前的代码只画 100%，给 compare.sh 逐像素比对。/ Baseline mode: draw only 100% with the pre-change code
// for compare.sh to diff pixel by pixel.
#ifdef HR_BASELINE
#define ui_set_scale(p) ((void)(p))
#define ui_scale_pct 100
#endif
#ifndef TR_S
#define TR_S(v) (v)
#endif

#define W TR_W
#define H TR_H
#define PW H
#define PH W

static uint8_t s_fb[W * H / 2];
uint8_t* hr_fb = s_fb;

static long s_offscreen;      // 画到屏幕外的墨水像素 / Ink pixels drawn off screen
static bool s_bar_check = true, s_allow_fit;
static int s_bar_hits, s_bar_top;
static char s_fits[2048];

/* ---- epdiy 替身 / epdiy shim ---- */

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    if (x < 0 || y < 0 || x >= W || y >= H) {
        if (color < 0xC0) s_offscreen++;
        return;
    }
    const int px = PW - y - 1, py = x;
    uint8_t* b = &fb[py * PW / 2 + px / 2];
    if (px % 2) *b = (*b & 0x0F) | (color & 0xF0);
    else *b = (*b & 0xF0) | (color >> 4);
}
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
void epd_draw_hline(int x, int y, int len, uint8_t color, uint8_t* fb) { epd_draw_line(x, y, x + len - 1, y, color, fb); }
void epd_draw_vline(int x, int y, int len, uint8_t color, uint8_t* fb) { epd_draw_line(x, y, x, y + len - 1, color, fb); }
void epd_draw_rect(EpdRect r, uint8_t color, uint8_t* fb) {
    epd_draw_line(r.x, r.y, r.x + r.width - 1, r.y, color, fb);
    epd_draw_line(r.x, r.y + r.height - 1, r.x + r.width - 1, r.y + r.height - 1, color, fb);
    epd_draw_line(r.x, r.y, r.x, r.y + r.height - 1, color, fb);
    epd_draw_line(r.x + r.width - 1, r.y, r.x + r.width - 1, r.y + r.height - 1, color, fb);
}
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
// 演示用的图片：斜向灰度渐变加一道亮带，seed 决定方向和明暗，像照片一样有层次。
// Demo picture: a diagonal gray gradient with a light band; seed picks the direction and tone, so it reads like a photo.
void hr_gradient(uint8_t* fb, EpdRect r, int seed) {
    const int dir = seed % 4, tone = 40 + (seed * 37) % 80;
    for (int y = 0; y < r.height; y++)
        for (int x = 0; x < r.width; x++) {
            const int u = dir == 0 ? x + y : dir == 1 ? r.width - x + y : dir == 2 ? x + r.height - y : r.width - x + r.height - y;
            const int span = r.width + r.height;
            int v = tone + u * (230 - tone) / (span ? span : 1);
            const int band = (x * 3 + y * 2 + seed * 29) % 220;
            if (band < 26) v += 30 - band;
            if (v > 245) v = 245;
            epd_draw_pixel(r.x + x, r.y + y, (uint8_t)v, fb);
        }
}

void epd_draw_rotated_image(EpdRect r, const uint8_t* image, uint8_t* fb) {
#ifdef HR_DEMO
    hr_gradient(fb, r, (int)(((uintptr_t)image >> 4) & 0xFF));
#else
    // 图片画成浅灰块，看得出位置。/ Images become a light gray block so their place shows.
    (void)image;
    epd_fill_rect(r, 0xD0, fb);
#endif
}
int epd_rotated_display_width(void) { return W; }
int epd_rotated_display_height(void) { return H; }
int epd_width(void) { return H; }
int epd_height(void) { return W; }
enum EpdRotation epd_get_rotation(void) { return EPD_ROT_PORTRAIT; }

/* ---- 其他替身 / Other shims ---- */

static pmu_snapshot_t s_pmu = {.soc_permille = 820};
bool read_pico_pmu_ready(void) { return true; }
const pmu_snapshot_t* read_pico_pmu_get(void) { return &s_pmu; }
// 状态栏的 Wi-Fi 名默认为空，原有截图不变；页面可以改 hr_wifi_name。/ The status-bar Wi-Fi name is empty by default so
// existing shots stay the same; a page may set hr_wifi_name.
char hr_wifi_name[33];
const char* tr_wifi_current_name(void) { return hr_wifi_name; }
__attribute__((weak)) bool tr_wifi_card_ok(void) { return true; }
__attribute__((weak)) bool tomato_time_valid(void) { return true; }
bool book_cover_draw(uint8_t* fb, EpdRect r, const char* path) {
#ifdef HR_DEMO
    // 演示：有图片封面的书画渐变，路径决定花样；路径里带 "text:" 的当作没有图片封面。/ Demo: books with an image cover get a
    // gradient picked by the path; a path containing "text:" counts as having no image cover.
    if (!path || strstr(path, "text:")) return false;
    int seed = 0;
    for (const char* c = path; *c; c++) seed = seed * 31 + (unsigned char)*c;
    hr_gradient(fb, r, seed & 0x7FFF);
    epd_draw_rect(r, 0x60, fb);
#else
    (void)path;
    epd_fill_rect(r, 0xD0, fb);
#endif
    return true;
}

// 时间固定在 2026-11-21 15:43（本地时区按 UTC），状态栏每次画得一样，100% 的图能逐像素比对。
// Time is pinned to 2026-11-21 15:43 (local zone UTC) so the status bar is identical and 100% shots compare exactly.
time_t __wrap_time(time_t* t) {
    const time_t v = 1795275780;
    if (t) *t = v;
    return v;
}

/* ---- 检查 / Checks ---- */

// 画标签栏之前，标签栏区域里已经有墨水：内容压在标签栏下面了。/ Ink already inside the bar area before the bar is
// drawn means content sits under the bar.
void __real_tr_tab_bar(uint8_t* fb, tr_tab_t active);
void __wrap_tr_tab_bar(uint8_t* fb, tr_tab_t active) {
    const EpdRect a = tr_tab_area();
    s_bar_top = a.y;
    for (int y = a.y; y < a.y + a.height; y++)
        for (int x = 0; x < W; x++)
            if (px_get(fb, x, y) < 0xC0) s_bar_hits++;
    __real_tr_tab_bar(fb, active);
}

// 记下被截断的文字。/ Record truncated text.
void __real_tr_fit(char* text, int px, int max_w);
void __wrap_tr_fit(char* text, int px, int max_w) {
    char before[256];
    snprintf(before, sizeof(before), "%s", text);
    __real_tr_fit(text, px, max_w);
    if (strcmp(before, text) && strlen(s_fits) + strlen(before) + 8 < sizeof(s_fits)) {
        strcat(s_fits, "  [");
        strcat(s_fits, before);
        strcat(s_fits, "]");
    }
}

// 文字和图标的外框：两两相交就是排版压字了。/ Text and icon boxes: any two that intersect mean overlapping layout.
typedef struct { int x0, y0, x1, y1; char what[48]; } box_t;
static box_t s_boxes[1024];
static int s_box_n;
static void add_box(int x0, int y0, int x1, int y1, const char* what) {
    if (s_box_n >= (int)(sizeof(s_boxes) / sizeof(s_boxes[0])) || x1 <= x0) return;
    box_t* b = &s_boxes[s_box_n++];
    *b = (box_t){x0, y0, x1, y1, ""};
    snprintf(b->what, sizeof(b->what), "%s", what);
}
static void text_box(int x, int y, int px, const char* text, enum EpdFontFlags align) {
    if (!text || !text[0]) return;
    const int w = ttf_text_width_px(px, text), top = y - ttf_ascender_px(px);
    const int x0 = align & EPD_DRAW_ALIGN_CENTER ? x - w / 2 : align & EPD_DRAW_ALIGN_RIGHT ? x - w : x;
    add_box(x0, top, x0 + w, top + px, text);
}
void __real_ttf_draw_text_px(uint8_t* fb, int x, int y, int px, const char* text, enum EpdFontFlags align,
                             uint8_t fg, uint8_t bg);
void __wrap_ttf_draw_text_px(uint8_t* fb, int x, int y, int px, const char* text, enum EpdFontFlags align,
                             uint8_t fg, uint8_t bg) {
    text_box(x, y, px, text, align);
    __real_ttf_draw_text_px(fb, x, y, px, text, align, fg, bg);
}
void __real_ttf_draw_text_px_bw(uint8_t* fb, int x, int y, int px, const char* text, enum EpdFontFlags align,
                                uint8_t fg, uint8_t bg);
void __wrap_ttf_draw_text_px_bw(uint8_t* fb, int x, int y, int px, const char* text, enum EpdFontFlags align,
                                uint8_t fg, uint8_t bg) {
    text_box(x, y, px, text, align);
    __real_ttf_draw_text_px_bw(fb, x, y, px, text, align, fg, bg);
}
void __real_tr_icon(uint8_t* fb, tr_icon_t icon, int cx, int cy, bool white);
void __wrap_tr_icon(uint8_t* fb, tr_icon_t icon, int cx, int cy, bool white) {
    const int r = TR_S(18);
    add_box(cx - r, cy - r, cx + r, cy + r, "(icon)");
    __real_tr_icon(fb, icon, cx, cy, white);
}
void __real_tr_icon_on(uint8_t* fb, tr_icon_t icon, int cx, int cy, uint8_t ink, uint8_t bg);
void __wrap_tr_icon_on(uint8_t* fb, tr_icon_t icon, int cx, int cy, uint8_t ink, uint8_t bg) {
    const int r = TR_S(18);
    add_box(cx - r, cy - r, cx + r, cy + r, "(icon)");
    __real_tr_icon_on(fb, icon, cx, cy, ink, bg);
}
// 翻页的两个灰底圆钮也算进外框（以前只看文字和图标，压到卡片文字上查不出来）。/ The pager's two round buttons count
// as boxes too (only text and icons used to count, so buttons over card text went unnoticed).
void __real_tr_pager(uint8_t* fb, int y, int page, int pages);
void __wrap_tr_pager(uint8_t* fb, int y, int page, int pages) {
    if (pages > 1) {
        const int cy = y + TR_S(17), r = TR_S(34);
        for (int k = -1; k <= 1; k += 2) {
            const int cx = TR_W / 2 + k * TR_S(120);
            add_box(cx - r, cy - r, cx + r, cy + r, "(pager button)");
        }
    }
    __real_tr_pager(fb, y, page, pages);
}

// 相交至少 3 像素才算（字形外框本来就有些余量）。/ Count only intersections of 3 px or more (glyph boxes have slack).
static void overlaps(char* out, size_t cap) {
    int n = 0;
    for (int i = 0; i < s_box_n; i++)
        for (int k = i + 1; k < s_box_n; k++) {
            const box_t *a = &s_boxes[i], *b = &s_boxes[k];
            const int w = (a->x1 < b->x1 ? a->x1 : b->x1) - (a->x0 > b->x0 ? a->x0 : b->x0);
            const int h = (a->y1 < b->y1 ? a->y1 : b->y1) - (a->y0 > b->y0 ? a->y0 : b->y0);
            if (w < 3 || h < 3) continue;
            if (s_bar_top && a->y0 >= s_bar_top && b->y0 >= s_bar_top) continue;   // 标签栏自己 / The bar itself
            if (n++ < 6) {
                const size_t len = strlen(out);
                snprintf(out + len, cap - len, " [%s]x[%s]", a->what, b->what);
            }
        }
    if (n > 6) {
        const size_t len = strlen(out);
        snprintf(out + len, cap - len, " (+%d)", n - 6);
    }
}

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

/* ---- 镜头 / Shots ---- */

static const char* s_out;
static int s_problems;

void hr_begin(void) {
    memset(s_fb, 0xFF, sizeof(s_fb));
    s_offscreen = 0;
    s_bar_hits = 0;
    s_bar_check = true;
    s_allow_fit = false;
    s_fits[0] = 0;
    s_box_n = 0;
    s_bar_top = 0;
}
void hr_no_bar_check(void) { s_bar_check = false; }
void hr_allow_fit(void) { s_allow_fit = true; }

void hr_shot(const char* scene) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%d.png", s_out, scene, ui_scale_pct);
    write_png(path, s_fb);
    char issues[2600] = "";
    if (s_offscreen) snprintf(issues + strlen(issues), sizeof(issues) - strlen(issues), " OFFSCREEN=%ld", s_offscreen);
    if (s_bar_check && s_bar_hits)
        snprintf(issues + strlen(issues), sizeof(issues) - strlen(issues), " UNDER_BAR=%d(y>=%d)", s_bar_hits, s_bar_top);
    if (s_fits[0])
        snprintf(issues + strlen(issues), sizeof(issues) - strlen(issues), " %s%s", s_allow_fit ? "fit(ok):" : "FIT:",
                 s_fits);
    char lap[1024] = "";
    overlaps(lap, sizeof(lap));
    if (lap[0]) snprintf(issues + strlen(issues), sizeof(issues) - strlen(issues), " OVERLAP:%s", lap);
    const bool bad = s_offscreen || (s_bar_check && s_bar_hits) || (s_fits[0] && !s_allow_fit) || lap[0];
    s_problems += bad;
    printf("%-5s %s %3d%%%s\n", bad ? "CHECK" : "ok", scene, ui_scale_pct, issues);
    hr_begin();
}

static sigjmp_buf s_jump;
static void on_crash(int sig) {
    void* frames[16];
    const int n = backtrace(frames, 16);
    fprintf(stdout, "CRASH signal %d (an unstubbed function was called?); backtrace:\n", sig);
    fflush(stdout);
    backtrace_symbols_fd(frames, n, 1);
    siglongjmp(s_jump, 1);
}

int main(int argc, char** argv) {
    const char* font = argc > 1 ? argv[1] : "sdcard/fonts/ChillDuanSansVF.ttf";
    const char* out = argc > 2 ? argv[2] : "build/page-render";
    static char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", out, hr_page);
    mkdir(out, 0755);
    mkdir(dir, 0755);
    s_out = dir;
    ttf_font_init();
    if (ttf_font_open(font) != ESP_OK) {
        fprintf(stderr, "cannot open font %s\n", font);
        return 1;
    }
    setenv("TZ", "UTC", 1);
    tzset();
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    static const int scales[] = {100, 115, 130};
#if defined(HR_BASELINE) || defined(HR_DEMO)
    const int scale_n = 1;
#else
    const int scale_n = 3;
#endif
    for (int i = 0; i < scale_n; i++) {
        ui_set_scale(scales[i]);
        hr_begin();
        if (sigsetjmp(s_jump, 1) == 0) hr_scenes();
        else s_problems++;
    }
    printf("%s: %d shot(s) need a look\n", hr_page, s_problems);
    return 0;
}
