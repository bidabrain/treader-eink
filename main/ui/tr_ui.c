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

uint8_t tr_gray_secondary = 0x33;
static int s_bold = 750;   // 粗体字重，随文字深浅 / Bold weight, following Text darkness

void tr_text_depth_apply(uint8_t level) {
    // 内置字体字重轴最高 800，更深档的粗体到顶。/ The built-in font's weight axis tops out at 800, so the darkest bold
    // sits at the top.
    static const int BODY[TR_TEXT_DEPTHS] = {500, 600, 700};
    static const int BOLD[TR_TEXT_DEPTHS] = {750, 800, 800};
    static const uint8_t GRAY[TR_TEXT_DEPTHS] = {0x33, 0x11, 0x00};
    if (level >= TR_TEXT_DEPTHS) level = 0;
    s_bold = BOLD[level];
    tr_gray_secondary = GRAY[level];
    ttf_set_base_weight(BODY[level]);
}

const tr_tab_t TR_TAB_ORDER[TR_TAB_MAX] = {
    TR_TAB_LIBRARY, TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU, TR_TAB_WEIBO, TR_TAB_X, TR_TAB_AUTH,
    TR_TAB_SETTINGS,
};
#define TAB_ICON_Y (TR_TABS_Y + TR_S(38))
#define TAB_LABEL_Y (TR_TABS_Y + TR_S(70))

/* ---- 文字 / Text ---- */

static void text_with_weight(uint8_t* fb, int x, int y, int px, const char* text,
                             enum EpdFontFlags align, bool bold, bool inverted) {
    int old = ttf_get_weight();
    if (bold) ttf_set_weight(s_bold);
    ui_text(fb, x, y, px, text, align, inverted);
    if (bold) ttf_set_weight(old);
}

void tr_text(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold) {
    if (text && text[0]) text_with_weight(fb, x, y_top, px, text, align, bold, false);
}

void tr_text_inv(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold) {
    if (text && text[0]) text_with_weight(fb, x, y_top, px, text, align, bold, true);
}

void tr_text_dim(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align) {
    tr_text_on(fb, x, y_top, px, text, align, false, TR_GRAY_SECONDARY, UI_GRAY_WHITE);
}

void tr_text_on(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold,
                uint8_t fg, uint8_t bg) {
    if (!text || !text[0]) return;
    int old = ttf_get_weight();
    if (bold) ttf_set_weight(s_bold);
    ttf_draw_text_px(fb, x, y_top + ttf_ascender_px(px), px, text, align, fg >> 4, bg >> 4);
    if (bold) ttf_set_weight(old);
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
        tr_text(fb, TR_MARGIN, TR_STATUS_Y, TR_S(28), clock, EPD_DRAW_ALIGN_LEFT, false);
    }
    if (!read_pico_pmu_ready()) return;
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    if (!pmu) return;
    int pct = (pmu->soc_permille + 5) / 10;
    if (pct > 100) pct = 100;
    // 电池外框、正极帽和电量填充。/ Battery outline, terminal and fill.
    const int bw = TR_S(40), bh = TR_S(22), bx = TR_W - TR_MARGIN - TR_S(44), by = TR_STATUS_Y + TR_S(6);
    epd_draw_rect((EpdRect){bx, by, bw, bh}, UI_GRAY_BLACK, fb);
    epd_draw_rect((EpdRect){bx + 1, by + 1, bw - 2, bh - 2}, UI_GRAY_BLACK, fb);
    epd_fill_rect((EpdRect){bx + bw, by + TR_S(7), TR_S(4), bh - 2 * TR_S(7)}, UI_GRAY_BLACK, fb);
    epd_fill_rect((EpdRect){bx + 4, by + 4, (bw - 8) * pct / 100, bh - 8}, UI_GRAY_BLACK, fb);
    char label[8];
    snprintf(label, sizeof(label), "%d%%", pct);
    tr_text(fb, bx - TR_S(10), TR_STATUS_Y, TR_S(28), label, EPD_DRAW_ALIGN_RIGHT, false);
}

void tr_title(uint8_t* fb, const char* title) {
    tr_text(fb, TR_MARGIN, TR_TITLE_Y, TR_S(52), title, EPD_DRAW_ALIGN_LEFT, true);
}

/* ---- 图标 / Icons ---- */

// 线宽 3 像素，特大档 4 像素。/ Strokes are 3 px wide, 4 px at the largest size.
static int stroke_hi(void) { return ui_scale_pct >= 130 ? 2 : 1; }

static void thick_line(uint8_t* fb, int x0, int y0, int x1, int y1, uint8_t c) {
    const int hi = stroke_hi();
    for (int dx = -1; dx <= hi; dx++)
        for (int dy = -1; dy <= hi; dy++)
            if (!dx || !dy) epd_draw_line(x0 + dx, y0 + dy, x1 + dx, y1 + dy, c, fb);
}

static void thick_circle(uint8_t* fb, int cx, int cy, int r, uint8_t c) {
    for (int d = -1; d <= stroke_hi(); d++) epd_draw_circle(cx, cy, r + d, c, fb);
}

// 3 像素粗的圆，只画 y < y_max 的点。/ A 3 px circle, plotting only points with y < y_max.
static void thick_arc_above(uint8_t* fb, int cx, int cy, int r, int y_max, uint8_t c) {
    for (int rr = r - 1; rr <= r + stroke_hi(); rr++) {
        int f = 1 - rr, ddx = 1, ddy = -2 * rr, px = 0, py = rr;
        for (;;) {
            const int pts[8][2] = {{px, py}, {-px, py}, {px, -py}, {-px, -py}, {py, px}, {-py, px}, {py, -px}, {-py, -px}};
            for (int k = 0; k < 8; k++)
                if (cy + pts[k][1] < y_max) epd_draw_pixel(cx + pts[k][0], cy + pts[k][1], c, fb);
            if (px >= py) break;
            if (f >= 0) { py--; ddy += 2; f += ddy; }
            px++; ddx += 2; f += ddx;
        }
    }
}

// 设计稿图标用 24 格的线条路径画；这里坐标是 24 格的十分之一，换算到 40 像素格。
// The design's icons are line paths on a 24-unit grid; coordinates here are tenths of that grid, mapped onto 40 px.
// 图标随显示大小放大：P() 换算 40 格上的长度，G() 换算 24 格的十分之一。
// Icons scale with the display size: P() maps lengths on the 40 px grid, G() tenths of the 24-unit grid.
#define P(v) TR_S(v)
#define G(v) TR_S((v) * 40 / 240)
static void seg24(uint8_t* fb, int x, int y, int ax, int ay, int bx, int by, uint8_t c) {
    thick_line(fb, x + G(ax), y + G(ay), x + G(bx), y + G(by), c);
}

void tr_icon(uint8_t* fb, tr_icon_t icon, int cx, int cy, bool white) {
    tr_icon_on(fb, icon, cx, cy, white ? UI_GRAY_WHITE : UI_GRAY_BLACK, white ? UI_GRAY_BLACK : UI_GRAY_WHITE);
}

void tr_icon_on(uint8_t* fb, tr_icon_t icon, int cx, int cy, uint8_t c, uint8_t bg) {
    const int x = cx - P(20), y = cy - P(20);   // 40×40 网格 / 40x40 grid
    switch (icon) {
    case TR_ICON_HOME:
        thick_line(fb, x + P(6), y + P(19), x + P(20), y + P(7), c);
        thick_line(fb, x + P(20), y + P(7), x + P(34), y + P(19), c);
        thick_line(fb, x + P(10), y + P(16), x + P(10), y + P(33), c);
        thick_line(fb, x + P(30), y + P(16), x + P(30), y + P(33), c);
        thick_line(fb, x + P(10), y + P(33), x + P(30), y + P(33), c);
        thick_line(fb, x + P(17), y + P(33), x + P(17), y + P(24), c);
        thick_line(fb, x + P(23), y + P(33), x + P(23), y + P(24), c);
        thick_line(fb, x + P(17), y + P(24), x + P(23), y + P(24), c);
        break;
    case TR_ICON_BOOKS:
        // 书架：三本竖书和一本斜靠的。/ Shelf: three upright books and one leaning.
        seg24(fb, x, y, 50, 40, 50, 200, c);
        seg24(fb, x, y, 90, 40, 90, 200, c);
        seg24(fb, x, y, 130, 40, 130, 200, c);
        seg24(fb, x, y, 165, 45, 200, 195, c);
        break;
    case TR_ICON_TOMATO:
        // 番茄：圆果子、两片叶子和果柄。/ Tomato: round fruit, two leaves and a stem.
        thick_circle(fb, cx, y + P(22), P(12), c);
        seg24(fb, x, y, 85, 65, 120, 87, c);
        seg24(fb, x, y, 120, 87, 155, 65, c);
        seg24(fb, x, y, 120, 87, 120, 35, c);
        break;
    case TR_ICON_BALL:
        // 篮球：圆、横竖两线、两条弧。/ Basketball: circle, cross lines and two curves.
        thick_circle(fb, cx, cy, P(15), c);
        seg24(fb, x, y, 30, 120, 210, 120, c);
        seg24(fb, x, y, 120, 30, 120, 210, c);
        seg24(fb, x, y, 60, 55, 84, 90, c);
        seg24(fb, x, y, 84, 90, 84, 150, c);
        seg24(fb, x, y, 84, 150, 60, 185, c);
        seg24(fb, x, y, 180, 55, 156, 90, c);
        seg24(fb, x, y, 156, 90, 156, 150, c);
        seg24(fb, x, y, 156, 150, 180, 185, c);
        break;
    case TR_ICON_SETTINGS:
        // 两根滑杆，各一个圆钮。/ Two sliders, each with a round knob.
        seg24(fb, x, y, 40, 70, 125, 70, c);
        seg24(fb, x, y, 175, 70, 200, 70, c);
        seg24(fb, x, y, 40, 170, 65, 170, c);
        seg24(fb, x, y, 115, 170, 200, 170, c);
        epd_fill_circle(x + G(150), y + G(70), P(5), bg, fb);
        thick_circle(fb, x + G(150), y + G(70), P(4), c);
        epd_fill_circle(x + G(90), y + G(170), P(5), bg, fb);
        thick_circle(fb, x + G(90), y + G(170), P(4), c);
        break;
    case TR_ICON_SEARCH:
        thick_circle(fb, x + G(110), y + G(110), P(11), c);
        seg24(fb, x, y, 157, 157, 200, 200, c);
        break;
    case TR_ICON_SYNC:
        thick_circle(fb, cx, cy, P(12), c);
        epd_fill_rect((EpdRect){cx + P(4), cy - P(14), P(12), P(10)}, bg, fb);
        epd_fill_rect((EpdRect){cx - P(16), cy + P(4), P(12), P(10)}, bg, fb);
        epd_fill_triangle(cx + P(6), cy - P(14), cx + P(16), cy - P(8), cx + P(8), cy - P(2), c, fb);
        epd_fill_triangle(cx - P(6), cy + P(14), cx - P(16), cy + P(8), cx - P(8), cy + P(2), c, fb);
        break;
    case TR_ICON_TOC:
        for (int i = 0; i < 3; i++) {
            epd_fill_circle(x + P(8), y + P(10) + P(i * 10), P(2), c, fb);
            thick_line(fb, x + P(15), y + P(10) + P(i * 10), x + P(34), y + P(10) + P(i * 10), c);
        }
        break;
    case TR_ICON_MENU:
        for (int i = 0; i < 4; i++) {
            int gx = x + P(7) + P((i % 2) * 15), gy = y + P(7) + P((i / 2) * 15);
            epd_draw_rect((EpdRect){gx, gy, P(11), P(11)}, c, fb);
            epd_draw_rect((EpdRect){gx + 1, gy + 1, P(11) - 2, P(11) - 2}, c, fb);
        }
        break;
    case TR_ICON_REFRESH:
        epd_draw_rect((EpdRect){x + P(7), y + P(7), P(26), P(26)}, c, fb);
        epd_draw_rect((EpdRect){x + P(7) + 1, y + P(7) + 1, P(26) - 2, P(26) - 2}, c, fb);
        thick_line(fb, x + P(14), y + P(20), x + P(26), y + P(20), c);
        thick_line(fb, x + P(20), y + P(14), x + P(20), y + P(26), c);
        break;
    case TR_ICON_COMIC:
        // 一页分格漫画：圆角外框、横分格和竖分格。/ A manga page: rounded frame with a horizontal and a vertical gutter.
        ui_draw_round_rect(fb, (EpdRect){x + P(5), y + P(5), P(30), P(30)}, P(4), c);
        ui_draw_round_rect(fb, (EpdRect){x + P(5) + 1, y + P(5) + 1, P(30) - 2, P(30) - 2}, P(4) - 1, c);
        ui_draw_round_rect(fb, (EpdRect){x + P(5) + 2, y + P(5) + 2, P(30) - 4, P(30) - 4}, P(4) - 2, c);
        seg24(fb, x, y, 30, 120, 130, 120, c);
        seg24(fb, x, y, 130, 30, 130, 210, c);
        break;
    case TR_ICON_XHS:
        // 小红书：圆角卡片上三行字。/ Xiaohongshu: a rounded card with three text lines.
        ui_draw_round_rect(fb, (EpdRect){x + P(7), y + P(5), P(26), P(30)}, P(5), c);
        ui_draw_round_rect(fb, (EpdRect){x + P(7) + 1, y + P(5) + 1, P(26) - 2, P(30) - 2}, P(5) - 1, c);
        ui_draw_round_rect(fb, (EpdRect){x + P(7) + 2, y + P(5) + 2, P(26) - 4, P(30) - 4}, P(5) - 2, c);
        seg24(fb, x, y, 80, 80, 160, 80, c);
        seg24(fb, x, y, 80, 120, 160, 120, c);
        seg24(fb, x, y, 80, 160, 130, 160, c);
        break;
    case TR_ICON_FONT:
        // “Aa”：大 A 和小 a 的笔画。/ "Aa": strokes of a large and a small A.
        thick_line(fb, x + P(5), y + P(32), x + P(13), y + P(10), c);
        thick_line(fb, x + P(13), y + P(10), x + P(21), y + P(32), c);
        thick_line(fb, x + P(8), y + P(25), x + P(18), y + P(25), c);
        thick_line(fb, x + P(24), y + P(32), x + P(29), y + P(19), c);
        thick_line(fb, x + P(29), y + P(19), x + P(34), y + P(32), c);
        thick_line(fb, x + P(26), y + P(28), x + P(32), y + P(28), c);
        break;
    case TR_ICON_TABS:
        // 底部一排三格，上面两行字。/ A three-cell bar at the bottom with two text lines above.
        ui_draw_round_rect(fb, (EpdRect){x + P(5), y + P(22), P(30), P(12)}, P(3), c);
        ui_draw_round_rect(fb, (EpdRect){x + P(5) + 1, y + P(22) + 1, P(30) - 2, P(12) - 2}, P(3) - 1, c);
        thick_line(fb, x + P(15), y + P(22), x + P(15), y + P(34), c);
        thick_line(fb, x + P(25), y + P(22), x + P(25), y + P(34), c);
        thick_line(fb, x + P(9), y + P(8), x + P(31), y + P(8), c);
        thick_line(fb, x + P(9), y + P(14), x + P(23), y + P(14), c);
        break;
    case TR_ICON_WIFI:
        // 三道弧：只画圆在 y+24 以上的部分（以前画整圆再涂掉下半边，涂得不够，底下露出两段弧）。
        // Three arcs: only the parts of the circles above y+24 (painting the lower half over used to leave two
        // stray arcs showing below).
        thick_arc_above(fb, cx, y + P(33), P(26), y + P(24), c);
        thick_arc_above(fb, cx, y + P(33), P(18), y + P(24), c);
        thick_arc_above(fb, cx, y + P(33), P(10), y + P(24), c);
        epd_fill_rect((EpdRect){x - P(8), y - P(2), P(6), P(30)}, bg, fb);
        epd_fill_rect((EpdRect){x + P(42), y - P(2), P(6), P(30)}, bg, fb);
        epd_fill_circle(cx, y + P(31), P(3), c, fb);
        break;
    case TR_ICON_SOURCE:
        // 两层服务器。/ A two-unit server.
        for (int k = 0; k < 2; k++) {
            ui_draw_round_rect(fb, (EpdRect){x + P(7), y + P(6) + P(k * 17), P(26), P(11)}, P(3), c);
            ui_draw_round_rect(fb, (EpdRect){x + P(7) + 1, y + P(6) + P(k * 17) + 1, P(26) - 2, P(11) - 2}, P(3) - 1,
                               c);
            thick_line(fb, x + P(12), y + P(11) + P(k * 17), x + P(14), y + P(11) + P(k * 17), c);
        }
        break;
    case TR_ICON_USB:
        // USB 标志：竖杆、顶上箭头、左右分叉（左圆右方）、底下圆点。/ USB mark: shaft, arrow, forks, base dot.
        thick_line(fb, cx, y + P(4), cx, y + P(30), c);
        thick_line(fb, cx - P(5), y + P(9), cx, y + P(4), c);
        thick_line(fb, cx + P(5), y + P(9), cx, y + P(4), c);
        thick_line(fb, cx, y + P(22), x + P(12), y + P(17), c);
        thick_line(fb, x + P(12), y + P(17), x + P(12), y + P(13), c);
        thick_line(fb, cx, y + P(26), x + P(28), y + P(21), c);
        thick_line(fb, x + P(28), y + P(21), x + P(28), y + P(17), c);
        epd_fill_circle(x + P(12), y + P(11), P(3), c, fb);
        epd_fill_rect((EpdRect){x + P(25), y + P(13), P(6), P(5)}, c, fb);
        epd_fill_circle(cx, y + P(33), P(4), c, fb);
        break;
    case TR_ICON_WEIBO:
        // 微博：带尾巴的对话框里一个话题“#”。/ Weibo: a speech bubble with a tail holding a topic "#".
        seg24(fb, x, y, 40, 50, 200, 50, c);
        seg24(fb, x, y, 200, 50, 200, 160, c);
        seg24(fb, x, y, 200, 160, 100, 160, c);
        seg24(fb, x, y, 100, 160, 40, 200, c);
        seg24(fb, x, y, 40, 200, 40, 50, c);
        seg24(fb, x, y, 100, 75, 90, 135, c);
        seg24(fb, x, y, 140, 75, 130, 135, c);
        seg24(fb, x, y, 80, 95, 155, 95, c);
        seg24(fb, x, y, 75, 115, 150, 115, c);
        break;
    case TR_ICON_X:
        // X：一只简化的小鸟（圆身子、圆头、尖嘴、上扬的翅膀、短尾巴）。/ X: a simple bird (round body and head, a
        // pointed beak, a raised wing and a short tail).
        epd_fill_circle(x + P(18), y + P(24), P(10), c, fb);
        epd_fill_circle(x + P(28), y + P(15), P(6), c, fb);
        epd_fill_triangle(x + P(33), y + P(12), x + P(39), y + P(13), x + P(33), y + P(17), c, fb);
        epd_fill_triangle(x + P(9), y + P(21), x + P(23), y + P(18), x + P(4), y + P(9), c, fb);
        epd_fill_triangle(x + P(10), y + P(27), x + P(2), y + P(31), x + P(13), y + P(33), c, fb);
        break;
    case TR_ICON_POWER:
        // 电源键：开口的圆和一竖。/ Power symbol: an open circle and a bar.
        thick_circle(fb, cx, y + P(21), P(12), c);
        epd_fill_rect((EpdRect){cx - P(6), y + P(4), P(12), P(12)}, bg, fb);
        thick_line(fb, cx, y + P(5), cx, y + P(19), c);
        break;
    case TR_ICON_KEY:
        // 钥匙：左边圆环，右边钥匙杆和两个齿。/ Key: a ring on the left, the shaft and two teeth on the right.
        thick_circle(fb, x + P(12), cy, P(7), c);
        thick_line(fb, x + P(19), cy, x + P(36), cy, c);
        thick_line(fb, x + P(30), cy, x + P(30), cy + P(7), c);
        thick_line(fb, x + P(35), cy, x + P(35), cy + P(6), c);
        break;
    case TR_ICON_LOCK:
        // 锁：上面半圆锁环，下面锁身和锁孔。/ Padlock: a half-ring shackle over the body with a keyhole.
        thick_circle(fb, cx, y + P(15), P(8), c);
        epd_fill_rect((EpdRect){cx - P(10), y + P(15), P(20), P(6)}, bg, fb);
        thick_line(fb, cx - P(8), y + P(15), cx - P(8), y + P(19), c);
        thick_line(fb, cx + P(8), y + P(15), cx + P(8), y + P(19), c);
        ui_draw_round_rect(fb, (EpdRect){x + P(8), y + P(19), P(24), P(17)}, P(3), c);
        ui_draw_round_rect(fb, (EpdRect){x + P(8) + 1, y + P(19) + 1, P(24) - 2, P(17) - 2}, P(3) - 1, c);
        epd_fill_circle(cx, y + P(26), P(2), c, fb);
        thick_line(fb, cx, y + P(26), cx, y + P(31), c);
        break;
    case TR_ICON_PROXY:
        // 代理：设备圆点，中间一个方框，对面圆点，三者连成线。/ Proxy: a device dot, a box in between and a far dot,
        // joined by lines.
        epd_fill_circle(x + P(6), cy, P(3), c, fb);
        epd_fill_circle(x + P(34), cy, P(3), c, fb);
        thick_line(fb, x + P(9), cy, x + P(14), cy, c);
        thick_line(fb, x + P(26), cy, x + P(31), cy, c);
        ui_draw_round_rect(fb, (EpdRect){x + P(14), cy - P(8), P(12), P(16)}, P(3), c);
        ui_draw_round_rect(fb, (EpdRect){x + P(14) + 1, cy - P(8) + 1, P(12) - 2, P(16) - 2}, P(3) - 1, c);
        break;
    case TR_ICON_IMPORT:
        thick_line(fb, x + P(20), y + P(6), x + P(20), y + P(26), c);
        thick_line(fb, x + P(12), y + P(18), x + P(20), y + P(26), c);
        thick_line(fb, x + P(28), y + P(18), x + P(20), y + P(26), c);
        thick_line(fb, x + P(7), y + P(28), x + P(7), y + P(33), c);
        thick_line(fb, x + P(33), y + P(28), x + P(33), y + P(33), c);
        thick_line(fb, x + P(7), y + P(33), x + P(33), y + P(33), c);
        break;
    }
}
#undef G
#undef P

/* ---- 标签栏 / Tab bar ---- */

EpdRect tr_tab_area(void) { return (EpdRect){0, TR_TABS_Y, TR_W, TR_H - TR_TABS_Y}; }

static uint16_t s_hidden = 1u << TR_TAB_X;
static uint8_t s_pins[TR_TAB_MAX] = {TR_TAB_COMIC, TR_TAB_XHS, TR_TAB_HUPU, TR_TAB_WEIBO};
static int s_pin_n = 4;

static const char* const TAB_LABELS[TR_TAB_MAX] = {"书库", "漫画", "番茄", "小红书", "虎扑", "微博", "X", "设置",
                                                     "验证器"};
static const tr_icon_t TAB_ICONS[TR_TAB_MAX] = {TR_ICON_BOOKS, TR_ICON_COMIC, TR_ICON_TOMATO, TR_ICON_XHS,
                                                TR_ICON_BALL, TR_ICON_WEIBO, TR_ICON_X, TR_ICON_SETTINGS,
                                                TR_ICON_KEY};

void tr_tabs_configure(uint16_t hidden, const uint8_t* pins, int n) {
    s_hidden = hidden & (uint16_t)~TR_TABS_FIXED;
    s_pin_n = 0;
    // 只收合法、没隐藏、不重复、不是书库的。/ Keep valid, visible, unique, non-library entries only.
    for (int i = 0; pins && i < n && s_pin_n < TR_TAB_MAX; i++) {
        const int t = pins[i];
        if (t <= TR_TAB_LIBRARY || t >= TR_TAB_MAX || (s_hidden & (1u << t)) || (TR_TABS_RETIRED & (1u << t))) continue;
        bool dup = false;
        for (int k = 0; k < s_pin_n; k++) dup |= s_pins[k] == t;
        if (!dup) s_pins[s_pin_n++] = (uint8_t)t;
    }
}

// 正常 6 格、大 5 格、特大 4 格，每格宽度够放下放大的图标和名字。/ Six cells normal, five large, four largest, so
// each cell fits the scaled icon and label.
int tr_tab_slots(void) { return ui_scale_pct >= 130 ? 4 : ui_scale_pct >= 115 ? 5 : 6; }

bool tr_tab_shown(tr_tab_t tab) { return tab < TR_TAB_MAX && !((s_hidden | TR_TABS_RETIRED) & (1u << tab)); }
const char* tr_tab_label(tr_tab_t tab) { return tab < TR_TAB_MAX ? TAB_LABELS[tab] : tab == TR_TAB_ALL ? "全部" : ""; }
tr_icon_t tr_tab_icon(tr_tab_t tab) { return tab < TR_TAB_MAX ? TAB_ICONS[tab] : TR_ICON_MENU; }

int tr_tab_count(void) {
    int n = 0;
    for (int t = 0; t < TR_TAB_MAX; t++) n += tr_tab_shown((tr_tab_t)t);
    return n;
}

int tr_tab_bar_tabs(tr_tab_t out[TR_TAB_MAX]) {
    int n = 0;
    out[n++] = TR_TAB_LIBRARY;
    // 留一格给“全部”；放不下的常用功能这一档只在“全部”里。/ Keep one cell for "All"; pins that do not fit live on the
    // All page at this size.
    for (int i = 0; i < s_pin_n && n < tr_tab_slots() - 1; i++) out[n++] = (tr_tab_t)s_pins[i];
    return n;
}

void tr_tab_bar(uint8_t* fb, tr_tab_t active) {
    // Material 3 导航栏：浅灰底，选中的图标垫一块灰胶囊，名字加粗变黑，其余名字深灰。最后一格固定是“全部”。
    // Material 3 navigation bar: light gray surface; the active icon sits on a gray capsule with a bold black label,
    // the others have dark gray labels. The last cell is always "All".
    epd_fill_rect(tr_tab_area(), TR_GRAY_SURFACE, fb);
    tr_tab_t tabs[TR_TAB_MAX + 1];
    int n = tr_tab_bar_tabs(tabs);
    bool found = false;
    for (int i = 0; i < n; i++) found |= tabs[i] == active;
    tabs[n++] = TR_TAB_ALL;
    if (!found) active = TR_TAB_ALL;
    // 名字和选中胶囊随显示大小放大（格子跟着变少，所以放得下）。/ Labels and the active capsule scale with the display
    // size (the bar has fewer cells then, so they fit).
    const int px = TR_S(n >= 7 ? 24 : n >= 6 ? 26 : 28), pill = TR_S(n >= 7 ? 84 : n >= 6 ? 92 : 100);
    for (int i = 0; i < n; i++) {
        int cx = (2 * i + 1) * TR_W / (2 * n);
        bool on = tabs[i] == active;
        uint8_t under = TR_GRAY_SURFACE;
        if (on) {
            ui_fill_round_rect(fb, (EpdRect){cx - pill / 2, TAB_ICON_Y - TR_S(26), pill, TR_S(52)}, TR_S(26),
                               TR_GRAY_SELECTED);
            under = TR_GRAY_SELECTED;
        }
        tr_icon_on(fb, tr_tab_icon(tabs[i]), cx, TAB_ICON_Y, UI_GRAY_BLACK, under);
        tr_text_on(fb, cx, TAB_LABEL_Y, px, tr_tab_label(tabs[i]), EPD_DRAW_ALIGN_CENTER, on,
                   on ? UI_GRAY_BLACK : TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    }
}

int tr_tab_hit(uint16_t x, uint16_t y) {
    if (y < TR_TABS_Y || y >= TR_H || x >= TR_W) return -1;
    tr_tab_t tabs[TR_TAB_MAX + 1];
    int n = tr_tab_bar_tabs(tabs);
    tabs[n++] = TR_TAB_ALL;
    return tabs[x * n / TR_W];
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
    // 文字封面当图片看：字号只跟封面高度走，不另外随显示大小放大（封面本身放大了字就跟着大）。
    // A text cover acts like an image: sizes follow the cover height only, not the display size (a scaled cover gets
    // larger text by itself).
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
    // 小封面同样只跟自己的尺寸走。/ Small covers likewise follow their own size only.
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
    // 轨道是 2 像素深灰细线，DU 下按黑画也不难看；已读部分是圆头黑条。
    // The track is a 2 px dark gray line that still reads under DU; the read part is a round-capped black bar.
    const int h = TR_S(8);
    if (done < w) epd_fill_rect((EpdRect){x + done, y + h / 2 - 1, w - done, 2}, TR_GRAY_OUTLINE, fb);
    if (done >= h) ui_fill_round_rect(fb, (EpdRect){x, y, done, h}, h / 2, UI_GRAY_BLACK);
    else if (done > 0) epd_fill_rect((EpdRect){x, y, done, h}, UI_GRAY_BLACK, fb);
}

int tr_badge(uint8_t* fb, int right, int y, const char* label, bool filled, int px) {
    if (!label || !label[0]) return 0;
    const int w = ttf_text_width_px(px, label) + TR_S(24), h = px + TR_S(14);
    const EpdRect r = {right - w, y, w, h};
    if (filled) {
        ui_fill_round_rect(fb, r, h / 2, UI_GRAY_BLACK);
        tr_text_inv(fb, r.x + w / 2, y + TR_S(5), px, label, EPD_DRAW_ALIGN_CENTER, true);
    } else {
        // 描边不用灰底：番茄书架下载时按 DU 局部刷新，灰底会被刷成白。/ Outline instead of a gray fill: the Tomato
        // shelf refreshes by DU during downloads, which would turn a gray fill white.
        ui_draw_round_rect(fb, r, h / 2, TR_GRAY_OUTLINE);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), h / 2 - 1, TR_GRAY_OUTLINE);
        tr_text(fb, r.x + w / 2, y + TR_S(5), px, label, EPD_DRAW_ALIGN_CENTER, true);
    }
    return w;
}

void tr_pill(uint8_t* fb, EpdRect r, const char* label, bool filled, int px) {
    int radius = r.height / 2;
    if (filled) {
        ui_fill_round_rect(fb, r, radius, UI_GRAY_BLACK);
        tr_text_inv(fb, r.x + r.width / 2, r.y + (r.height - px) / 2 - 2, px, label, EPD_DRAW_ALIGN_CENTER, true);
    } else {
        // Material 3 描边按钮：深灰细框（DU 下按黑处理），黑字。/ Material 3 outlined button: a thin dark gray frame
        // (black under DU) with black text.
        ui_draw_round_rect(fb, r, radius, TR_GRAY_OUTLINE);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), radius - 1, TR_GRAY_OUTLINE);
        tr_text(fb, r.x + r.width / 2, r.y + (r.height - px) / 2 - 2, px, label, EPD_DRAW_ALIGN_CENTER, false);
    }
}

// 选中段的勾。/ The check mark on the active segment.
static void check_mark(uint8_t* fb, int x, int cy, uint8_t c) {
    thick_line(fb, x, cy, x + TR_S(7), cy + TR_S(7), c);
    thick_line(fb, x + TR_S(7), cy + TR_S(7), x + TR_S(20), cy - TR_S(7), c);
}

void tr_segment(uint8_t* fb, EpdRect r, const char* const* labels, int n, int active) {
    const int radius = r.height / 2, w = r.width / n;
    const int px = TR_S(30);
    // 先铺选中段的灰底（两头的段跟着外框的圆角），再画外框和分隔线。
    // Fill the active segment first (end segments follow the outer rounding), then the frame and dividers.
    if (active >= 0 && active < n) {
        EpdRect cell = {r.x + active * w, r.y, active == n - 1 ? r.width - active * w : w, r.height};
        if (n == 1) {
            ui_fill_round_rect(fb, r, radius, TR_GRAY_SELECTED);
        } else if (active == 0) {
            ui_fill_round_rect(fb, (EpdRect){cell.x, cell.y, cell.width + radius, cell.height}, radius,
                               TR_GRAY_SELECTED);
            epd_fill_rect((EpdRect){cell.x + cell.width, cell.y, radius + 1, cell.height}, UI_GRAY_WHITE, fb);
        } else if (active == n - 1) {
            ui_fill_round_rect(fb, (EpdRect){cell.x - radius, cell.y, cell.width + radius, cell.height}, radius,
                               TR_GRAY_SELECTED);
            epd_fill_rect((EpdRect){cell.x - radius - 1, cell.y, radius + 1, cell.height}, UI_GRAY_WHITE, fb);
        } else {
            epd_fill_rect(cell, TR_GRAY_SELECTED, fb);
        }
    }
    ui_draw_round_rect(fb, r, radius, TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), radius - 1, TR_GRAY_OUTLINE);
    for (int i = 1; i < n; i++) epd_fill_rect((EpdRect){r.x + i * w - 1, r.y, 2, r.height}, TR_GRAY_OUTLINE, fb);
    for (int i = 0; i < n; i++) {
        EpdRect cell = {r.x + i * w, r.y, i == n - 1 ? r.width - i * w : w, r.height};
        const int mid = cell.x + cell.width / 2;
        // 放大后放不下（比如“评论 123.4万”）就逐级缩小这一段的字，不截字。/ When a label no longer fits after scaling
        // (e.g. "评论 123.4万"), step this segment's font down instead of cutting it.
        int lpx = px;
        while (lpx > 18 && ttf_text_width_px(lpx, labels[i]) + (i == active ? lpx : 0) > cell.width - TR_S(16)) lpx--;
        const int lty = r.y + (r.height - lpx) / 2 - 2;
        if (i == active) {
            // 勾在字左边，两者一起居中。/ The check sits left of the label; together they are centered.
            const int tw = ttf_text_width_px(lpx, labels[i]) + lpx;
            const int left = mid - tw / 2;
            check_mark(fb, left, r.y + r.height / 2, UI_GRAY_BLACK);
            tr_text_on(fb, left + lpx, lty, lpx, labels[i], EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SELECTED);
        } else {
            tr_text(fb, mid, lty, lpx, labels[i], EPD_DRAW_ALIGN_CENTER, false);
        }
    }
}

int tr_segment_hit(EpdRect r, int n, uint16_t x, uint16_t y) {
    if (!ui_rect_hit(r, x, y) || n <= 0) return -1;
    int i = (x - r.x) / (r.width / n);
    return i < n ? i : n - 1;
}

static void pager(uint8_t* fb, int y, const char* label);

void tr_pager(uint8_t* fb, int y, int page, int pages) {
    if (pages <= 1) return;
    char label[32];
    snprintf(label, sizeof(label), "%d / %d", page + 1, pages);
    pager(fb, y, label);
}

void tr_pager_more(uint8_t* fb, int y, int page, int pages) {
    char label[32];
    snprintf(label, sizeof(label), "%d / %d+", page + 1, pages);
    pager(fb, y, label);
}

static void pager(uint8_t* fb, int y, const char* label) {
    tr_text(fb, TR_W / 2, y, TR_S(30), label, EPD_DRAW_ALIGN_CENTER, true);
    // 两侧灰底圆钮里画箭头；位置和以前的“‹ ›”一样，热区不变。
    // Arrows inside gray round buttons, where "‹ ›" used to be; the hit zones are unchanged.
    const int cy = y + TR_S(17);
    for (int k = -1; k <= 1; k += 2) {
        const int cx = TR_W / 2 + k * TR_S(120);
        epd_fill_circle(cx, cy, TR_S(34), TR_GRAY_SURFACE, fb);
        thick_line(fb, cx - TR_S(5) * k, cy - TR_S(11), cx + TR_S(6) * k, cy, UI_GRAY_BLACK);
        thick_line(fb, cx + TR_S(6) * k, cy, cx - TR_S(5) * k, cy + TR_S(11), UI_GRAY_BLACK);
    }
}

int tr_pager_hit(int y, uint16_t x, uint16_t y_touch) {
    if (y_touch < y - TR_S(30) || y_touch > y + TR_S(70)) return 0;
    if (x < TR_W / 2 - TR_S(40)) return -1;
    if (x > TR_W / 2 + TR_S(40)) return 1;
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
