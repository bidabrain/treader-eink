/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Treader 界面原语实现。坐标都是竖屏逻辑像素（684×1216）。
 * Treader UI primitive implementation. Coordinates are portrait logical pixels (684x1216).
 */

#include "tr_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "book_cover.h"
#include "read_pico_pmu.h"
#include "ttf_font.h"
#include "ui_kit.h"

#define TR_WGHT_BOLD 700
#define TAB_ICON_Y (TR_TABS_Y + 38)
#define TAB_LABEL_Y (TR_TABS_Y + 70)

/* ---- 文字 / Text ---- */

static void text_with_weight(uint8_t* fb, int x, int y, int px, const char* text,
                             enum EpdFontFlags align, bool bold, bool inverted) {
    int old = ttf_get_weight();
    if (bold) ttf_set_weight(TR_WGHT_BOLD);
    ui_text(fb, x, y, px, text, align, inverted);
    if (bold) ttf_set_weight(old);
}

void tr_text(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold) {
    if (text && text[0]) text_with_weight(fb, x, y_top, px, text, align, bold, false);
}

void tr_text_inv(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold) {
    if (text && text[0]) text_with_weight(fb, x, y_top, px, text, align, bold, true);
}

static size_t u8_len(const char* s) {
    unsigned char c = (unsigned char)*s;
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    for (size_t i = 1; i < n; i++) if (!s[i]) return i;
    return n;
}

// 一个字符的宽度。字形宽度逐字相加，没有字距调整，所以逐字累加和整段测量结果一样，但只要 O(n)。
// Width of one character. Advances add up per glyph with no kerning, so a running sum equals measuring the
// whole prefix, in O(n) instead of O(n^2).
static int char_width(int px, const char* s, size_t n) {
    char ch[5];
    memcpy(ch, s, n);
    ch[n] = 0;
    return ttf_text_width_px(px, ch);
}

// 截断到 max_w 并加省略号。省略号只写进原字符串已占的字节里，绝不越过原来的结尾。
// Fit into max_w with an ellipsis. The ellipsis only overwrites bytes the string already occupies, never past its end.
static void fit_impl(char* text, int px, int max_w, bool force) {
    if (!force && ttf_text_width_px(px, text) <= max_w) return;
    static const char dots[] = "…";
    const size_t len = strlen(text);
    const int dots_w = ttf_text_width_px(px, dots);
    size_t end = 0, keep = 0;
    int width = 0;
    while (text[end]) {
        size_t n = u8_len(text + end);
        width += char_width(px, text + end, n);
        if (width + dots_w > max_w) break;
        end += n;
        keep = end;
    }
    // 省略号 3 字节：保证写完不超出原字符串。/ The ellipsis is 3 bytes; make sure it stays within the original string.
    while (keep && keep + sizeof(dots) > len + 1) {
        keep--;
        while (keep && ((unsigned char)text[keep] & 0xC0) == 0x80) keep--;
    }
    if (keep + sizeof(dots) <= len + 1) memcpy(text + keep, dots, sizeof(dots));
    else text[keep] = 0;
}

void tr_fit(char* text, int px, int max_w) { fit_impl(text, px, max_w, false); }

// 能放下多少字节。/ How many bytes fit within w.
static size_t fit_bytes(const char* p, int px, int w, bool stop_at_newline) {
    size_t len = 0;
    int width = 0;
    while (p[len] && !(stop_at_newline && p[len] == '\n')) {
        size_t n = u8_len(p + len);
        width += char_width(px, p + len, n);
        if (width > w) break;
        len += n;
    }
    return len;
}

int tr_message(uint8_t* fb, int x, int y_one, int y_two, int w, int px, const char* text,
               enum EpdFontFlags align, bool bold) {
    if (!text || !text[0]) return 0;
    if (ttf_text_width_px(px, text) <= w) {
        tr_text(fb, x, y_one, px, text, align, bold);
        return 1;
    }
    const int small = px - 4, lh = small + 4;
    char line[192];
    const char* p = text;
    int lines = 0;
    while (*p && lines < 2) {
        size_t take = fit_bytes(p, small, w, true);
        if (!take && *p != '\n') take = u8_len(p);
        if (take >= sizeof(line) - 4) take = sizeof(line) - 4;
        while (take && ((unsigned char)p[take] & 0xC0) == 0x80) take--;
        memcpy(line, p, take);
        line[take] = 0;
        p += take;
        if (*p == '\n') p++;
        if (lines == 1 && *p) {
            // 两行还放不下：第二行末尾加省略号。/ Still too long for two lines: end the second with an ellipsis.
            size_t n = strlen(line);
            if (n + 4 <= sizeof(line)) memcpy(line + n, "  ", 3);
            fit_impl(line, small, w, true);
        }
        tr_text(fb, x, y_two + lines * lh, small, line, align, bold);
        lines++;
    }
    return lines;
}

size_t tr_line_bytes(const char* text, int px, int w) {
    size_t take = fit_bytes(text, px, w, true);
    // 一个字都放不下时至少放一个，免得排版卡死。/ Take at least one character so layout always advances.
    if (!take && *text && *text != '\n') take = u8_len(text);
    return take;
}

const char* tr_tail_fit(const char* text, int px, int w) {
    // 从末尾往前累加，找到能放下的最长尾巴。/ Walk back from the end to find the longest tail that fits.
    size_t len = strlen(text);
    size_t start = len;
    int width = 0;
    while (start) {
        size_t prev = start - 1;
        while (prev && ((unsigned char)text[prev] & 0xC0) == 0x80) prev--;
        width += char_width(px, text + prev, start - prev);
        if (width > w) break;
        start = prev;
    }
    return text + start;
}

int tr_wrap(uint8_t* fb, int x, int y_top, int w, int px, int line_h, const char* text, int max_lines) {
    char line[256];
    int lines = 0;
    const char* p = text;
    while (*p && lines < max_lines) {
        size_t take = fit_bytes(p, px, w, true);
        if (!take && *p != '\n') take = u8_len(p);
        if (take >= sizeof(line) - 4) take = sizeof(line) - 4;
        while (take && ((unsigned char)p[take] & 0xC0) == 0x80) take--;
        memcpy(line, p, take);
        line[take] = 0;
        p += take;
        if (*p == '\n') p++;
        if (lines == max_lines - 1 && *p) {
            // 最后一行还没放完：截断并强制加省略号。/ Last line overflows: truncate and force an ellipsis.
            size_t n = strlen(line);
            if (n + 4 <= sizeof(line)) { memcpy(line + n, "  ", 3); }
            fit_impl(line, px, w, true);
        }
        tr_text(fb, x, y_top + lines * line_h, px, line, EPD_DRAW_ALIGN_LEFT, false);
        lines++;
    }
    return lines;
}

/* ---- 线与栏 / Lines and bars ---- */

void tr_hline(uint8_t* fb, int y, int x, int w, bool soft) {
    epd_fill_rect((EpdRect){x, y, w, 2}, soft ? TR_GRAY_LINE : UI_GRAY_BLACK, fb);
}

void tr_status_bar(uint8_t* fb) {
    time_t now = time(NULL);
    if (now > 1735689600) {
        struct tm tm;
        localtime_r(&now, &tm);
        char clock[8];
        snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
        tr_text(fb, TR_MARGIN, TR_STATUS_Y, 28, clock, EPD_DRAW_ALIGN_LEFT, false);
    }
    if (!read_pico_pmu_ready()) return;
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    if (!pmu) return;
    int pct = (pmu->soc_permille + 5) / 10;
    if (pct > 100) pct = 100;
    // 电池外框、正极帽和电量填充。/ Battery outline, terminal and fill.
    const int bx = TR_W - TR_MARGIN - 44, by = TR_STATUS_Y + 6;
    epd_draw_rect((EpdRect){bx, by, 40, 22}, UI_GRAY_BLACK, fb);
    epd_draw_rect((EpdRect){bx + 1, by + 1, 38, 20}, UI_GRAY_BLACK, fb);
    epd_fill_rect((EpdRect){bx + 40, by + 7, 4, 8}, UI_GRAY_BLACK, fb);
    epd_fill_rect((EpdRect){bx + 4, by + 4, 32 * pct / 100, 14}, UI_GRAY_BLACK, fb);
    char label[8];
    snprintf(label, sizeof(label), "%d%%", pct);
    tr_text(fb, bx - 10, TR_STATUS_Y, 28, label, EPD_DRAW_ALIGN_RIGHT, false);
}

void tr_title(uint8_t* fb, const char* title) {
    tr_text(fb, TR_MARGIN, TR_TITLE_Y, 52, title, EPD_DRAW_ALIGN_LEFT, true);
}

/* ---- 图标 / Icons ---- */

static void thick_line(uint8_t* fb, int x0, int y0, int x1, int y1, uint8_t c) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (!dx || !dy) epd_draw_line(x0 + dx, y0 + dy, x1 + dx, y1 + dy, c, fb);
}

static void thick_circle(uint8_t* fb, int cx, int cy, int r, uint8_t c) {
    for (int d = -1; d <= 1; d++) epd_draw_circle(cx, cy, r + d, c, fb);
}

void tr_icon(uint8_t* fb, tr_icon_t icon, int cx, int cy, bool white) {
    const uint8_t c = white ? UI_GRAY_WHITE : UI_GRAY_BLACK;
    const uint8_t bg = white ? UI_GRAY_BLACK : UI_GRAY_WHITE;
    const int x = cx - 20, y = cy - 20;   // 40×40 网格 / 40x40 grid
    switch (icon) {
    case TR_ICON_HOME:
        thick_line(fb, x + 6, y + 19, x + 20, y + 7, c);
        thick_line(fb, x + 20, y + 7, x + 34, y + 19, c);
        thick_line(fb, x + 10, y + 16, x + 10, y + 33, c);
        thick_line(fb, x + 30, y + 16, x + 30, y + 33, c);
        thick_line(fb, x + 10, y + 33, x + 30, y + 33, c);
        thick_line(fb, x + 17, y + 33, x + 17, y + 24, c);
        thick_line(fb, x + 23, y + 33, x + 23, y + 24, c);
        thick_line(fb, x + 17, y + 24, x + 23, y + 24, c);
        break;
    case TR_ICON_BOOKS:
        thick_line(fb, x + 7, y + 8, x + 7, y + 33, c);
        thick_line(fb, x + 13, y + 8, x + 13, y + 33, c);
        thick_line(fb, x + 19, y + 9, x + 26, y + 33, c);
        thick_line(fb, x + 27, y + 8, x + 33, y + 33, c);
        break;
    case TR_ICON_TOMATO:
        thick_circle(fb, cx, y + 22, 12, c);
        thick_line(fb, x + 14, y + 10, x + 20, y + 14, c);
        thick_line(fb, x + 26, y + 10, x + 20, y + 14, c);
        thick_line(fb, x + 20, y + 14, x + 20, y + 7, c);
        break;
    case TR_ICON_BALL:
        // 篮球：圆、横竖两线、两条弧。/ Basketball: circle, cross lines and two curves.
        thick_circle(fb, cx, cy, 13, c);
        thick_line(fb, x + 7, cy, x + 33, cy, c);
        thick_line(fb, cx, y + 7, cx, y + 33, c);
        thick_line(fb, x + 11, y + 10, x + 15, y + 20, c);
        thick_line(fb, x + 15, y + 20, x + 11, y + 30, c);
        thick_line(fb, x + 29, y + 10, x + 25, y + 20, c);
        thick_line(fb, x + 25, y + 20, x + 29, y + 30, c);
        break;
    case TR_ICON_SETTINGS:
        thick_line(fb, x + 7, y + 12, x + 33, y + 12, c);
        thick_line(fb, x + 7, y + 28, x + 33, y + 28, c);
        epd_fill_circle(x + 15, y + 12, 5, bg, fb);
        thick_circle(fb, x + 15, y + 12, 4, c);
        epd_fill_circle(x + 26, y + 28, 5, bg, fb);
        thick_circle(fb, x + 26, y + 28, 4, c);
        break;
    case TR_ICON_SEARCH:
        thick_circle(fb, x + 17, y + 17, 10, c);
        thick_line(fb, x + 25, y + 25, x + 34, y + 34, c);
        break;
    case TR_ICON_SYNC:
        thick_circle(fb, cx, cy, 12, c);
        epd_fill_rect((EpdRect){cx + 4, cy - 14, 12, 10}, bg, fb);
        epd_fill_rect((EpdRect){cx - 16, cy + 4, 12, 10}, bg, fb);
        epd_fill_triangle(cx + 6, cy - 14, cx + 16, cy - 8, cx + 8, cy - 2, c, fb);
        epd_fill_triangle(cx - 6, cy + 14, cx - 16, cy + 8, cx - 8, cy + 2, c, fb);
        break;
    case TR_ICON_TOC:
        for (int i = 0; i < 3; i++) {
            epd_fill_circle(x + 8, y + 10 + i * 10, 2, c, fb);
            thick_line(fb, x + 15, y + 10 + i * 10, x + 34, y + 10 + i * 10, c);
        }
        break;
    case TR_ICON_MENU:
        for (int i = 0; i < 4; i++) {
            int gx = x + 7 + (i % 2) * 15, gy = y + 7 + (i / 2) * 15;
            epd_draw_rect((EpdRect){gx, gy, 11, 11}, c, fb);
            epd_draw_rect((EpdRect){gx + 1, gy + 1, 9, 9}, c, fb);
        }
        break;
    case TR_ICON_REFRESH:
        epd_draw_rect((EpdRect){x + 7, y + 7, 26, 26}, c, fb);
        epd_draw_rect((EpdRect){x + 8, y + 8, 24, 24}, c, fb);
        thick_line(fb, x + 14, y + 20, x + 26, y + 20, c);
        thick_line(fb, x + 20, y + 14, x + 20, y + 26, c);
        break;
    case TR_ICON_IMPORT:
        thick_line(fb, x + 20, y + 6, x + 20, y + 26, c);
        thick_line(fb, x + 12, y + 18, x + 20, y + 26, c);
        thick_line(fb, x + 28, y + 18, x + 20, y + 26, c);
        thick_line(fb, x + 7, y + 28, x + 7, y + 33, c);
        thick_line(fb, x + 33, y + 28, x + 33, y + 33, c);
        thick_line(fb, x + 7, y + 33, x + 33, y + 33, c);
        break;
    }
}

/* ---- 标签栏 / Tab bar ---- */

EpdRect tr_tab_area(void) { return (EpdRect){0, TR_TABS_Y, TR_W, TR_H - TR_TABS_Y}; }

void tr_tab_bar(uint8_t* fb, tr_tab_t active) {
    static const char* const labels[TR_TAB_COUNT] = {"书库", "番茄", "虎扑", "设置"};
    static const tr_icon_t icons[TR_TAB_COUNT] = {TR_ICON_BOOKS, TR_ICON_TOMATO, TR_ICON_BALL, TR_ICON_SETTINGS};
    epd_fill_rect(tr_tab_area(), UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_TABS_Y, 0, TR_W, false);
    const int cell = TR_W / TR_TAB_COUNT;
    for (int i = 0; i < TR_TAB_COUNT; i++) {
        int cx = i * cell + cell / 2;
        bool on = i == (int)active;
        if (on) ui_fill_round_rect(fb, (EpdRect){cx - 46, TAB_ICON_Y - 26, 92, 52}, 26, UI_GRAY_BLACK);
        tr_icon(fb, icons[i], cx, TAB_ICON_Y, on);
        tr_text(fb, cx, TAB_LABEL_Y, 28, labels[i], EPD_DRAW_ALIGN_CENTER, on);
    }
}

int tr_tab_hit(uint16_t x, uint16_t y) {
    if (y < TR_TABS_Y || y >= TR_H || x >= TR_W) return -1;
    return x / (TR_W / TR_TAB_COUNT);
}

/* ---- 封面 / Covers ---- */

// 书名分成最多 3 行，每行不超过 w 像素；放不下时第 3 行末尾加省略号。
// Split a title into at most 3 lines of width w; if it does not fit, end line 3 with an ellipsis.
static int split_title(const char* title, int px, int w, char lines[3][64]) {
    int n = 0;
    const char* p = title;
    while (*p && n < 3) {
        size_t len = fit_bytes(p, px, w, false);
        if (!len) len = u8_len(p);
        if (len > 60) len = 60;
        while (len && ((unsigned char)p[len] & 0xC0) == 0x80) len--;
        memcpy(lines[n], p, len);
        lines[n][len] = 0;
        p += len;
        n++;
    }
    if (*p && n == 3) {
        size_t m = strlen(lines[2]);
        if (m + 4 <= 64) memcpy(lines[2] + m, "  ", 3);   // 留出空间再截断 / Make room, then truncate
        fit_impl(lines[2], px, w - 4, true);
    }
    return n;
}

void tr_cover(uint8_t* fb, EpdRect r, const char* title, const char* author, const char* format, bool dark) {
    const uint8_t ink = dark ? UI_GRAY_WHITE : UI_GRAY_BLACK;
    epd_fill_rect(r, dark ? UI_GRAY_BLACK : UI_GRAY_WHITE, fb);
    epd_draw_rect(r, UI_GRAY_BLACK, fb);
    epd_draw_rect((EpdRect){r.x + 1, r.y + 1, r.width - 2, r.height - 2}, UI_GRAY_BLACK, fb);
    EpdRect in = {r.x + 8, r.y + 8, r.width - 16, r.height - 16};
    epd_draw_rect(in, ink, fb);
    epd_draw_rect((EpdRect){in.x + 1, in.y + 1, in.width - 2, in.height - 2}, ink, fb);
    // 字号随封面高度缩放。/ Font sizes scale with the cover height.
    const int title_px = r.height >= 280 ? 44 : r.height >= 200 ? 32 : 24;
    const int small_px = r.height >= 280 ? 26 : r.height >= 200 ? 22 : 16;
    const int cx = r.x + r.width / 2;
    char au[64];
    snprintf(au, sizeof(au), "%s", author ? author : "");
    tr_fit(au, small_px, in.width - 12);
    void (*draw)(uint8_t*, int, int, int, const char*, enum EpdFontFlags, bool) = dark ? tr_text_inv : tr_text;
    draw(fb, cx, in.y + small_px / 2 + 6, small_px, au, EPD_DRAW_ALIGN_CENTER, false);
    char lines[3][64];
    int n = split_title(title ? title : "", title_px, in.width - 16, lines);
    int line_h = title_px + title_px / 4;
    int top = in.y + (in.height - n * line_h) / 2;
    for (int i = 0; i < n; i++) draw(fb, cx, top + i * line_h, title_px, lines[i], EPD_DRAW_ALIGN_CENTER, true);
    draw(fb, cx, in.y + in.height - small_px - 10, small_px, format, EPD_DRAW_ALIGN_CENTER, false);
}

void tr_mini_cover(uint8_t* fb, EpdRect r, const char* title, bool dark) {
    epd_fill_rect(r, dark ? UI_GRAY_BLACK : UI_GRAY_WHITE, fb);
    epd_draw_rect(r, UI_GRAY_BLACK, fb);
    epd_draw_rect((EpdRect){r.x + 1, r.y + 1, r.width - 2, r.height - 2}, UI_GRAY_BLACK, fb);
    const int px = 20, step = 22;
    int max_chars = (r.height - 12) / step;
    int y = r.y + (r.height - max_chars * step) / 2;
    const char* p = title ? title : "";
    char ch[5];
    for (int i = 0; i < max_chars && *p; i++) {
        size_t n = u8_len(p);
        memcpy(ch, p, n);
        ch[n] = 0;
        p += n;
        if (dark) tr_text_inv(fb, r.x + r.width / 2, y + i * step, px, ch, EPD_DRAW_ALIGN_CENTER, true);
        else tr_text(fb, r.x + r.width / 2, y + i * step, px, ch, EPD_DRAW_ALIGN_CENTER, true);
    }
}

/* ---- 控件 / Controls ---- */

void tr_progress(uint8_t* fb, int x, int y, int w, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int done = w * pct / 100;
    epd_fill_rect((EpdRect){x, y, done, 8}, UI_GRAY_BLACK, fb);
    for (int dx = done + 4; dx < w; dx += 7) epd_fill_rect((EpdRect){x + dx, y + 3, 2, 2}, UI_GRAY_BLACK, fb);
}

void tr_pill(uint8_t* fb, EpdRect r, const char* label, bool filled, int px) {
    int radius = r.height / 2;
    if (filled) {
        ui_fill_round_rect(fb, r, radius, UI_GRAY_BLACK);
        tr_text_inv(fb, r.x + r.width / 2, r.y + (r.height - px) / 2 - 2, px, label, EPD_DRAW_ALIGN_CENTER, true);
    } else {
        ui_draw_round_rect(fb, r, radius, UI_GRAY_BLACK);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), radius - 1, UI_GRAY_BLACK);
        tr_text(fb, r.x + r.width / 2, r.y + (r.height - px) / 2 - 2, px, label, EPD_DRAW_ALIGN_CENTER, false);
    }
}

void tr_segment(uint8_t* fb, EpdRect r, const char* const* labels, int n, int active) {
    const int radius = r.height / 2, w = r.width / n;
    ui_draw_round_rect(fb, r, radius, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), radius - 1, UI_GRAY_BLACK);
    for (int i = 0; i < n; i++) {
        EpdRect cell = {r.x + i * w, r.y, i == n - 1 ? r.width - i * w : w, r.height};
        const int ty = r.y + (r.height - 30) / 2 - 2;
        if (i == active) {
            ui_fill_round_rect(fb, cell, radius, UI_GRAY_BLACK);
            tr_text_inv(fb, cell.x + cell.width / 2, ty, 30, labels[i], EPD_DRAW_ALIGN_CENTER, true);
        } else {
            tr_text(fb, cell.x + cell.width / 2, ty, 30, labels[i], EPD_DRAW_ALIGN_CENTER, false);
        }
    }
}

int tr_segment_hit(EpdRect r, int n, uint16_t x, uint16_t y) {
    if (!ui_rect_hit(r, x, y) || n <= 0) return -1;
    int i = (x - r.x) / (r.width / n);
    return i < n ? i : n - 1;
}

void tr_pager(uint8_t* fb, int y, int page, int pages) {
    if (pages <= 1) return;
    char label[32];
    snprintf(label, sizeof(label), "%d / %d", page + 1, pages);
    tr_text(fb, TR_W / 2, y, 30, label, EPD_DRAW_ALIGN_CENTER, false);
    tr_text(fb, TR_W / 2 - 120, y, 30, "‹", EPD_DRAW_ALIGN_CENTER, false);
    tr_text(fb, TR_W / 2 + 120, y, 30, "›", EPD_DRAW_ALIGN_CENTER, false);
}

int tr_pager_hit(int y, uint16_t x, uint16_t y_touch) {
    if (y_touch < y - 30 || y_touch > y + 70) return 0;
    if (x < TR_W / 2 - 40) return -1;
    if (x > TR_W / 2 + 40) return 1;
    return 0;
}

void tr_title_from_path(const char* path, char* out, size_t cap) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, cap, "%s", base);
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

/* ---- 图片封面 / Image covers ---- */

// 读完的书在封面底部压一条黑带写“读完”。/ Finished books get a black band reading "读完" at the bottom.
static void done_band(uint8_t* fb, EpdRect r, int px) {
    const int band = px + 12;
    epd_fill_rect((EpdRect){r.x, r.y + r.height - band, r.width, band}, UI_GRAY_BLACK, fb);
    tr_text_inv(fb, r.x + r.width / 2, r.y + r.height - band + 4, px, "读完", EPD_DRAW_ALIGN_CENTER, true);
}

void tr_book_cover(uint8_t* fb, EpdRect r, const char* path, const char* title, const char* author,
                   const char* format, bool done) {
    if (!path || !book_cover_draw(fb, r, path)) { tr_cover(fb, r, title, author, format, done); return; }
    if (done) done_band(fb, r, r.height >= 280 ? 30 : r.height >= 200 ? 26 : 20);
}

void tr_book_mini_cover(uint8_t* fb, EpdRect r, const char* path, const char* title, bool done) {
    if (!path || !book_cover_draw(fb, r, path)) { tr_mini_cover(fb, r, title, done); return; }
    if (done) done_band(fb, r, 16);
}
