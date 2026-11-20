/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 菜单把手和屏幕下方按键区的绘制与命中。全屏列表菜单已换成“全部”页（apps/app_tr_all.c）。
 *
 * Draw and hit-test for the menu handle and the key strip below the display. The full-screen list menu became the
 * All page (apps/app_tr_all.c).
 */

#include "ui_menu.h"

#include "ui_kit.h"

static EpdRect menu_btn_rect(void) {
    return (EpdRect){
        .x = epd_rotated_display_width() - UI_BAR_MARGIN - UI_MENU_BTN_SIZE,
        .y = UI_BAR_TOP,
        .width = UI_MENU_BTN_SIZE,
        .height = UI_MENU_BTN_SIZE,
    };
}

static void draw_menu_arrow(
    uint8_t* framebuffer, int cx, int cy, bool up, uint8_t color
) {
    if (up) {
        epd_fill_triangle(cx, cy - UI_S(11), cx - UI_S(14), cy + UI_S(10), cx + UI_S(14), cy + UI_S(10), color,
                          framebuffer);
    } else {
        epd_fill_triangle(cx, cy + UI_S(11), cx - UI_S(14), cy - UI_S(10), cx + UI_S(14), cy - UI_S(10), color,
                          framebuffer);
    }
}

void ui_draw_menu_handle(uint8_t* framebuffer, bool menu_open) {
    EpdRect btn = menu_btn_rect();
    ui_draw_selected_round_rect(framebuffer, btn, UI_BTN_RADIUS);
    draw_menu_arrow(
        framebuffer, btn.x + btn.width / 2, btn.y + btn.height / 2,
        !menu_open, UI_GRAY_BLACK
    );
}

bool ui_menu_handle_hit_test(uint16_t x, uint16_t y) {
    return ui_rect_hit(menu_btn_rect(), x, y);
}

// 按 UI_KEY_PITCH 均分，三个按键的中心（80/240/400）正好落在各段中点，
// 手指偏一点也还在同一段里。
// Split by UI_KEY_PITCH so the three key centers (80/240/400) sit at mid-segment;
// a slight finger miss still lands in the same segment.
int ui_key_hit_test(uint16_t x, uint16_t y) {
    if (y < UI_KEY_AREA_TOP) return -1;
    int index = (int)x / UI_KEY_PITCH;
    if (index >= UI_KEY_COUNT) return -1;
    return index;
}
