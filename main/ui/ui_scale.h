/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 显示大小：界面长度（字号、行高、图标、间距）按百分比放大，像安卓的“显示大小”。100% 时 UI_S(v) 原样返回 v，
 * 正常档的画面和以前逐像素相同。左右页边距和细线不放大，留给内容的宽度不缩水。
 *
 * Display size: UI lengths (font sizes, row heights, icons, gaps) scale by a percentage, like Android's "Display
 * size". At 100% UI_S(v) returns v unchanged, so the normal size matches the old screens pixel for pixel. Side margins
 * and hairlines do not scale, so content keeps its width.
 *
 * 冻结：只有三档 100 / 115 / 130；主循环按当前页设置（fixed_scale 的页始终 100）。阅读正文字号、漫画和图片按原样；
 * 阅读页的页脚和工具条会放大，所以正文版心略矮、每页少一两行。
 * Frozen: only 100 / 115 / 130; the loop sets it per page (fixed_scale pages always 100). The reading font size,
 * comics and images stay as they are; the reader's footer and toolbars scale, so the text area is slightly shorter
 * and a page may hold a line or two less.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// 当前百分比，只由 ui_set_scale 改。/ Current percentage; only ui_set_scale changes it.
extern int ui_scale_pct;

/// 设置显示大小（100、115、130，其他值按 100）。/ Set the display size (100, 115, 130; anything else means 100).
void ui_set_scale(int pct);

/// 按显示大小换算一个长度，四舍五入。/ Scale a length by the display size, rounded.
static inline int ui_s(int v) {
    if (ui_scale_pct == 100) return v;
    return (v * ui_scale_pct + (v < 0 ? -50 : 50)) / 100;
}
#define UI_S(v) ui_s(v)

#ifdef __cplusplus
}
#endif
