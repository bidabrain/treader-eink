/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Treader 界面原语：状态栏、底部标签栏、文字封面、虚线进度条、胶囊按钮、线条图标、自动换行。
 * 版式见 docs/design/ui.html。只用黑、白和一级浅灰；图片封面交给 book_cover 画。
 *
 * Treader UI primitives: status bar, tab bar, text covers, dotted progress, pills, line icons,
 * wrapped text. Layout follows docs/design/ui.html. Black, white and one light gray only; image covers come from book_cover.
 *
 * 冻结：纯绘制，不读文件、不碰 I2C；页面在 on_enter/on_tick 里准备数据。
 * Frozen: drawing only, no file or I2C access; pages prepare data in on_enter/on_tick.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TR_W 684
#define TR_H 1216
#define TR_MARGIN 40
#define TR_STATUS_Y 22
#define TR_TITLE_Y 88
#define TR_TABS_Y 1094
#define TR_GRAY_LINE 0xC0

/// 标签的身份（不是位置），也是“隐藏标签”位掩码里的位号；显示的标签平分宽度。
/// Tab identity, not position, and the bit number in the hidden-tab mask; visible tabs share the width.
typedef enum {
    TR_TAB_LIBRARY = 0, TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU, TR_TAB_SETTINGS, TR_TAB_MAX
} tr_tab_t;

/// 书库和设置不能隐藏。/ The library and settings cannot be hidden.
#define TR_TABS_FIXED ((1u << TR_TAB_LIBRARY) | (1u << TR_TAB_SETTINGS))

typedef enum {
    TR_ICON_HOME, TR_ICON_BOOKS, TR_ICON_TOMATO, TR_ICON_SETTINGS,
    TR_ICON_SEARCH, TR_ICON_SYNC, TR_ICON_IMPORT,
    TR_ICON_TOC, TR_ICON_MENU, TR_ICON_REFRESH, TR_ICON_BALL, TR_ICON_COMIC,
    TR_ICON_XHS, TR_ICON_FONT, TR_ICON_TABS, TR_ICON_WIFI, TR_ICON_SOURCE, TR_ICON_POWER, TR_ICON_USB,
} tr_icon_t;

/// 文字，按行上沿定位；bold 用可变字体 700 字重。/ Text by line top; bold uses VF weight 700.
void tr_text(uint8_t* fb, int x, int y_top, int px, const char* text, enum EpdFontFlags align, bool bold);
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

/// 隐藏哪些标签（按 tr_tab_t 位号的掩码；开机和改设置时由调用方告知，本文件不读设置）。固定的两个忽略。
/// Which tabs are hidden (mask by tr_tab_t bit; callers set it at boot and on change; this file never reads
/// settings). The two fixed tabs are ignored.
void tr_tabs_set_hidden(uint8_t mask);
/// 这个标签现在显示吗。/ Whether this tab is shown.
bool tr_tab_shown(tr_tab_t tab);
/// 标签名和图标（设置页列表也用）。/ Tab label and icon (the settings list uses them too).
const char* tr_tab_label(tr_tab_t tab);
tr_icon_t tr_tab_icon(tr_tab_t tab);
/// 显示几个标签。/ Number of visible tabs.
int tr_tab_count(void);
/// 底部标签栏。/ Bottom tab bar.
void tr_tab_bar(uint8_t* fb, tr_tab_t active);
/// 命中的标签（tr_tab_t），未命中返回 -1。/ Hit tab as a tr_tab_t, or -1.
int tr_tab_hit(uint16_t x, uint16_t y);
/// 标签栏区域。/ Tab bar area.
EpdRect tr_tab_area(void);

void tr_icon(uint8_t* fb, tr_icon_t icon, int cx, int cy, bool white);

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

/// 进度：已读部分实心，剩余部分点线。/ Progress: solid read part, dotted remainder.
void tr_progress(uint8_t* fb, int x, int y, int w, int pct);
/// 胶囊按钮。filled 为黑底白字。/ Pill button; filled = black with white text.
void tr_pill(uint8_t* fb, EpdRect r, const char* label, bool filled, int px);
/// 番茄页“书架 / 书城”分段按钮的位置。/ Where the Tomato "shelf / store" segment sits.
#define TR_SEGMENT_RECT (EpdRect){TR_MARGIN, 160, TR_W - 2 * TR_MARGIN, 64}
/// 分段按钮，active 那段黑底白字。/ Segmented control; the active segment is filled.
void tr_segment(uint8_t* fb, EpdRect r, const char* const* labels, int n, int active);
/// 命中的段，未命中返回 -1。/ Hit segment, or -1.
int tr_segment_hit(EpdRect r, int n, uint16_t x, uint16_t y);
/// 分页指示“‹ 1 / 5 ›”。/ Pager "‹ 1 / 5 ›".
void tr_pager(uint8_t* fb, int y, int page, int pages);
/// 分页指示的左右热区。返回 -1 上一页、1 下一页、0 未命中。/ Pager hit: -1 prev, 1 next, 0 none.
int tr_pager_hit(int y, uint16_t x, uint16_t y_touch);

/// 去掉目录和扩展名得到书名。/ Book title from a path: no directory, no extension.
void tr_title_from_path(const char* path, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
