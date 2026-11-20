/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Treader 界面原语：状态栏、底部标签栏、文字封面、进度条、胶囊按钮、线条图标、自动换行。
 * 风格是 Material 3 灰阶，版式见 docs/design/ui.html；灰色只用下面的 TR_GRAY_* 几级。图片封面交给 book_cover 画。
 * 会被 DU 局部刷新的控件（进度条、按下态、描边按钮）只用 DU 下也能分清的深色，浅灰只做整页刷新的底色。
 *
 * Treader UI primitives: status bar, tab bar, text covers, progress, pills, line icons, wrapped text. The style is
 * Material 3 in grays, laid out per docs/design/ui.html, using only the TR_GRAY_* levels below; image covers come from
 * book_cover. Controls that DU area updates touch (progress, pressed states, outlined pills) use only dark tones that
 * survive DU; light grays are backgrounds for page refreshes.
 *
 * 冻结：纯绘制，不读文件、不碰 I2C；页面在 on_enter/on_tick 里准备数据。
 * Frozen: drawing only, no file or I2C access; pages prepare data in on_enter/on_tick.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epdiy.h"
#include "ui_scale.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TR_W 684
#define TR_H 1216
#define TR_MARGIN 40   // 页边距不随显示大小放大 / Side margins do not scale
/// 按显示大小换算长度，见 ui_scale.h。/ Scale a length by the display size; see ui_scale.h.
#define TR_S(v) UI_S(v)
#define TR_STATUS_Y TR_S(22)
#define TR_TITLE_Y TR_S(88)
#define TR_TABS_Y (TR_H - TR_S(122))
#define TR_GRAY_LINE 0xC0
/// Material 3 灰阶：底色、选中、描边与次要文字（8 位，高 4 位是墨水级）。/ Material 3 grays: surface, selected,
/// outline and secondary text (8-bit; the high nibble is the ink level).
#define TR_GRAY_SURFACE 0xE0     ///< #EEE 卡片、标签栏、圆按钮底 / Cards, tab bar, round buttons
#define TR_GRAY_SELECTED 0xC0    ///< #CCC 选中的标签底、分段选中 / Selected tab indicator and segment
#define TR_GRAY_OUTLINE 0x50     ///< #555 描边（DU 下按黑处理）/ Outlines (black under DU)
#define TR_GRAY_SECONDARY 0x50   ///< #555 次要文字 / Secondary text

/// 功能的身份（不是位置），也是“隐藏功能”位掩码里的位号。/ Feature identity, not position, and the bit number in the
/// hidden-feature mask.
typedef enum {
    TR_TAB_LIBRARY = 0, TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU, TR_TAB_WEIBO, TR_TAB_X, TR_TAB_SETTINGS, TR_TAB_MAX
} tr_tab_t;

/// 底部最后一格“全部”（tr_tab_hit 的返回值、tr_tab_bar 的选中值）。/ The last bar cell, "All" (a tr_tab_hit result and a
/// tr_tab_bar selection).
#define TR_TAB_ALL TR_TAB_MAX

/// 书库和设置不能隐藏；书库还固定在底部第一格。/ The library and settings cannot be hidden; the library also stays in
/// the first bar cell.
#define TR_TABS_FIXED ((1u << TR_TAB_LIBRARY) | (1u << TR_TAB_SETTINGS))

typedef enum {
    TR_ICON_HOME, TR_ICON_BOOKS, TR_ICON_TOMATO, TR_ICON_SETTINGS,
    TR_ICON_SEARCH, TR_ICON_SYNC, TR_ICON_IMPORT,
    TR_ICON_TOC, TR_ICON_MENU, TR_ICON_REFRESH, TR_ICON_BALL, TR_ICON_COMIC,
    TR_ICON_XHS, TR_ICON_FONT, TR_ICON_TABS, TR_ICON_WIFI, TR_ICON_SOURCE, TR_ICON_POWER, TR_ICON_USB, TR_ICON_WEIBO, TR_ICON_X,
} tr_icon_t;

/// 文字，按行上沿定位；bold 用可变字体 700 字重。/ Text by line top; bold uses VF weight 700.
void tr_text(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold);
/// 画在灰底上的文字：fg、bg 是 8 位灰，抗锯齿边缘向 bg 过渡，不出白边。/ Text on a gray fill: fg and bg are 8-bit
/// grays and anti-aliased edges blend toward bg, so no white fringe.
void tr_text_on(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold,
                uint8_t fg, uint8_t bg);
/// 次要文字：白底上的深灰字。/ Secondary text: dark gray on white.
void tr_text_dim(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align);
/// 白字（画在黑底上）。/ White text for black fills.
void tr_text_inv(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold);
/// 截断到 max_w 像素并加省略号，按 UTF-8 边界。/ Fit into max_w with an ellipsis on UTF-8 boundaries.
void tr_fit(char* text, int px, int max_w);
/// 提示文字：一行放得下就在 y_one 用 px 画一行；放不下就从 y_two 起用小 4 号的字画两行（还不够就省略号）。
/// 返回画了几行。调用方给两行留好位置，原因（括号里的部分）就不会被截掉。
/// Message text: one line at y_one in px when it fits; otherwise two lines 4 px smaller from y_two (ellipsis if still
/// too long). Returns the lines drawn. Callers reserve room for two lines so the cause in brackets is not cut off.
int tr_message(uint8_t* fb, int x, int y_one, int y_two, int w, int px, const char* text,
               enum EpdFontFlags align, bool bold);
/// 从 text 开始一行能放下多少字节（遇换行停，至少一个字符）。/ Bytes of text that fit on one line of width w
/// (stops at a newline, at least one character).
size_t tr_line_bytes(const char* text, int px, int w);
/// 能完整放进 w 的最长尾巴（输入框显示末尾用），按字符边界。/ Longest tail that fits in w, on a character boundary.
const char* tr_tail_fit(const char* text, int px, int w);
/// 自动换行，最多 max_lines 行，超出加省略号；返回实际行数。/ Wrap into at most max_lines; returns lines drawn.
int tr_wrap(uint8_t* fb, int x, int y_top, int w, int px, int line_h, const char* text, int max_lines);

/// 细线；soft 用浅灰。/ Hairline; soft uses light gray.
void tr_hline(uint8_t* fb, int y, int x, int w, bool soft);
/// 顶部状态栏：时间（未对时则不显示）与电量。/ Status bar: time (hidden until set) and battery.
void tr_status_bar(uint8_t* fb);
/// 页标题（大号粗体）。/ Page title (large bold).
void tr_title(uint8_t* fb, const char* title);

/// 告诉本文件哪些功能隐藏、哪些放在底部（书库之后，按顺序）。开机和改设置时由调用方告知，本文件不读设置。
/// Tell this file which features are hidden and which are pinned after the library, in order. Callers set it at boot
/// and on change; this file never reads settings.
void tr_tabs_configure(uint8_t hidden, const uint8_t* pins, int n);
/// 底部一共几格（含书库和“全部”）：正常 6、大 5、特大 4。/ Bar cells in total, the library and "All" included:
/// 6 normal, 5 large, 4 largest.
int tr_tab_slots(void);
/// 底部实际显示的功能（书库加放得下的常用，不含“全部”），按位置顺序；返回个数。/ Features actually in the bar
/// (the library plus the pins that fit, "All" excluded), in position order; returns the count.
int tr_tab_bar_tabs(tr_tab_t out[TR_TAB_MAX]);
/// 这个功能没被隐藏（“全部”页里有它）。/ Whether this feature is not hidden (the All page lists it).
bool tr_tab_shown(tr_tab_t tab);
/// 功能名和图标（设置页和“全部”页也用）；TR_TAB_ALL 是“全部”。/ Feature label and icon (settings and the All page
/// use them too); TR_TAB_ALL is "All".
const char* tr_tab_label(tr_tab_t tab);
tr_icon_t tr_tab_icon(tr_tab_t tab);
/// 没隐藏的功能有几个。/ Number of features that are not hidden.
int tr_tab_count(void);
/// 底部标签栏：书库、常用功能、最后一格“全部”。active 不在底部时选中“全部”。/ Bottom bar: the library, pinned
/// features, and "All" last. When active is not in the bar, "All" is selected.
void tr_tab_bar(uint8_t* fb, tr_tab_t active);
/// 命中的格子：功能号或 TR_TAB_ALL，未命中返回 -1。/ Hit cell: a feature id or TR_TAB_ALL, or -1.
int tr_tab_hit(uint16_t x, uint16_t y);
/// 标签栏区域。/ Tab bar area.
EpdRect tr_tab_area(void);

void tr_icon(uint8_t* fb, tr_icon_t icon, int cx, int cy, bool white);
/// 画在灰底上的图标：ink 是线条色，bg 是镂空处要补的底色。/ Icon on a gray fill: ink for strokes, bg for cut-outs.
void tr_icon_on(uint8_t* fb, tr_icon_t icon, int cx, int cy, uint8_t ink, uint8_t bg);

/// 文字封面：双线框，作者、书名、格式。dark 为黑底白字。/ Text cover: double frame, author, title, format; dark = inverted.
void tr_cover(uint8_t* fb, EpdRect r, const char* title, const char* author, const char* format, bool dark);
/// 列表用小封面：竖排前几个字。/ Small list cover: first characters stacked vertically.
void tr_mini_cover(uint8_t* fb, EpdRect r, const char* title, bool dark);
/// 有图片封面就画图（读完的书加“读完”角标），否则画文字封面。只读缓存，不解码。
/// Draw the image cover when there is one (finished books get a "done" band), else the text cover. Cache only.
void tr_book_cover(uint8_t* fb, EpdRect r, const char* path, const char* title, const char* author,
                   const char* format, bool done);
/// 书架行里的小封面版本。/ Small shelf-row variant.
void tr_book_mini_cover(uint8_t* fb, EpdRect r, const char* path, const char* title, bool done);

/// 进度：已读部分是圆头实心条，剩余部分是一条细轨道（DU 下也看得见）。/ Progress: a round-capped solid bar for the read
/// part on a thin track that stays visible under DU.
void tr_progress(uint8_t* fb, int x, int y, int w, int pct);
/// 小标记胶囊，右边缘对齐 right、上沿 y；filled 为黑底白字，否则深灰描边黑字（DU 局部刷新也画得对）。返回宽度。
/// A small badge capsule, right edge at right and top at y; filled is black with white text, else a dark gray outline
/// with black text (correct under DU area updates too). Returns its width.
int tr_badge(uint8_t* fb, int right, int y, const char* label, bool filled, int px);
/// 胶囊按钮。filled 为黑底白字，否则深灰描边。/ Pill button; filled = black with white text, else a dark outline.
void tr_pill(uint8_t* fb, EpdRect r, const char* label, bool filled, int px);
/// 番茄页“书架 / 书城”分段按钮的位置。/ Where the Tomato "shelf / store" segment sits.
#define TR_SEGMENT_RECT (EpdRect){TR_MARGIN, TR_S(160), TR_W - 2 * TR_MARGIN, TR_S(64)}
/// 分段按钮（Material 3）：描边胶囊，选中段灰底加勾、粗体。/ Material 3 segmented button: outlined capsule; the active
/// segment gets a gray fill, a check mark and bold text.
void tr_segment(uint8_t* fb, EpdRect r, const char* const* labels, int n, int active);
/// 命中的段，未命中返回 -1。/ Hit segment, or -1.
int tr_segment_hit(EpdRect r, int n, uint16_t x, uint16_t y);
/// 分页指示：两侧灰底圆钮，中间“1 / 5”。/ Pager: gray round buttons either side of "1 / 5".
void tr_pager(uint8_t* fb, int y, int page, int pages);
/// 分页指示的左右热区。返回 -1 上一页、1 下一页、0 未命中。/ Pager hit: -1 prev, 1 next, 0 none.
int tr_pager_hit(int y, uint16_t x, uint16_t y_touch);

/// 去掉目录和扩展名得到书名。/ Book title from a path: no directory, no extension.
void tr_title_from_path(const char* path, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
