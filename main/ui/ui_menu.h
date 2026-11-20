/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 菜单把手和屏幕下方那条触摸按键区。原来的全屏列表菜单（2026-11-21 起）换成了“全部”页（apps/app_tr_all.c）。
 *
 * The menu handle and the touch-key strip below the display. The old full-screen list menu was replaced by the All
 * page (apps/app_tr_all.c) on 2026-11-21.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 触摸面板比显示区长，屏幕下方那条感应区里有三个按键。它们不在画面里，
// 没有东西可画，只能按坐标识别：实测中心 x=80/240/400，y≈1500。
// 显示区最大 y 是 1216，所以用 1300 当分界就不会和底部标签栏抢点击。
// The touch panel is taller than the display; three keys sit in the strip
// below the image. They are not drawn; hit-test is by coordinate. Measured
// centers are x=80/240/400, y≈1500. Display y max is 1216, so 1300 as the
// split does not steal taps from the bottom bar.
#define UI_KEY_COUNT 3
#define UI_KEY_AREA_TOP 1300
#define UI_KEY_PITCH 160
#define UI_KEY_1 0
#define UI_KEY_2 1
#define UI_KEY_3 2

/// 没有标签栏的老页面右下角的菜单把手，点了打开“全部”页。menu_open 只决定箭头朝向。
/// Menu handle in the bottom-right corner of legacy pages without a tab bar; a tap opens the All page. menu_open only
/// sets the arrow direction.
void ui_draw_menu_handle(uint8_t* framebuffer, bool menu_open);
bool ui_menu_handle_hit_test(uint16_t x, uint16_t y);

/// 屏幕下方触摸按键区的命中测试，返回 UI_KEY_1/2/3，不在按键区时返回 -1。
/// Hit-test for the keys below the display. Returns UI_KEY_1/2/3, or -1 outside the key strip.
int ui_key_hit_test(uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif
