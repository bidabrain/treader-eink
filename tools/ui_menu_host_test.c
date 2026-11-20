/* SPDX-License-Identifier: Apache-2.0
 * 中文：菜单把手和屏下三键区的命中测试（列表菜单已换成“全部”页）。
 * English: Hit tests for the menu handle and the key strip below the display (the list menu became the All page).
 */
#include "ui_menu.h"
#include "../main/ui/ui_kit.h"
#include <assert.h>
#include <stdio.h>
int ui_scale_pct = 100;
static int drawn;
int epd_rotated_display_width(void) {return 684;}
void epd_fill_triangle(int a,int b,int c,int d,int e,int f,uint8_t g,uint8_t*h) {(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;drawn++;}
bool ui_rect_hit(EpdRect r,uint16_t x,uint16_t y) {return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
void ui_draw_selected_round_rect(uint8_t*f,EpdRect r,int rad) {(void)f;(void)r;(void)rad;drawn++;}
int main(void) {
    // 屏下三键：按 160 像素分段，显示区里不算。/ Keys below the display: 160 px segments; the display area never hits.
    assert(ui_key_hit_test(80,1500)==UI_KEY_1&&ui_key_hit_test(240,1500)==UI_KEY_2&&ui_key_hit_test(400,1500)==UI_KEY_3);
    assert(ui_key_hit_test(80,1200)==-1&&ui_key_hit_test(500,1500)==-1);
    // 把手在右下角底栏位置，随显示大小放大，画和点用同一块。/ The handle sits in the bottom-right bar cell, scales with
    // the display size, and draws and hits the same area.
    for (int pct = 100; pct <= 130; pct += 15) {
        ui_scale_pct = pct;
        const int top = UI_BAR_TOP, left = 684 - UI_BAR_MARGIN - UI_MENU_BTN_SIZE;
        assert(ui_menu_handle_hit_test(left + 2, top + 2) && ui_menu_handle_hit_test(684 - UI_BAR_MARGIN - 1, top + UI_BAR_H - 1));
        assert(!ui_menu_handle_hit_test(left - 1, top + 2) && !ui_menu_handle_hit_test(left + 2, top - 1));
        drawn = 0;
        ui_draw_menu_handle(NULL, false);
        assert(drawn == 2);
    }
    puts("ui_menu: key strip and handle hit areas passed at 100/115/130%");
    return 0;
}
