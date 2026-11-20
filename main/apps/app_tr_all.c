/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * “全部”页：所有没隐藏的功能排成大图标网格，点一个进去。底部标签栏最后一格“全部”、KEY3、阅读和漫画工具条里的
 * “菜单”、老页面右下角的把手都打开这一页（代替原来的 KEY3 列表菜单）。打开前所在的功能在网格里标成选中，
 * 在最后一页按 KEY3 回到那里。
 *
 * The All page: every feature that is not hidden, as a large-icon grid; tap one to open it. The bar's last cell,
 * KEY3, the "Menu" button in the reader and comic toolbars and the corner handle on legacy pages all open this page
 * (replacing the old KEY3 list menu). The feature it was opened from is marked selected, and KEY3 on the last page
 * returns there.
 *
 * 冻结：只列功能、只做跳转，不改设置；哪些放在底部由“设置 › 底部标签”决定。
 * Frozen: it lists features and switches pages only; it never changes settings. Settings > Tab bar decides the bar.
 */

#include <stdio.h>

#include "app.h"
#include "tr_nav.h"
#include "tr_ui.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

// 长度都随显示大小放大；一页几行按标签栏上面剩下的高度算。/ Lengths scale with the display size; rows per page
// follow the height left above the tab bar.
#define GRID_TOP TR_S(212)
#define COLS 3
#define CELL_GAP TR_S(16)
#define CELL_W ((TR_W - 2 * TR_MARGIN - (COLS - 1) * CELL_GAP) / COLS)
#define CELL_H TR_S(170)
#define PAGER_Y (TR_TABS_Y - TR_S(84))
#define ROWS ((PAGER_Y - TR_S(20) - GRID_TOP + CELL_GAP) / (CELL_H + CELL_GAP))
#define PER_PAGE (ROWS * COLS)

extern const app_desc_t app_tr_all;

static const app_desc_t* s_origin;   // 打开前所在的页面 / The page it was opened from
static int s_page;

void tr_all_open_from(const app_desc_t* from) {
    if (from != &app_tr_all) s_origin = from;
}

// 没隐藏的功能，按功能号顺序。/ Features that are not hidden, in feature-id order.
static int listed(tr_tab_t out[TR_TAB_MAX]) {
    int n = 0;
    for (int t = 0; t < TR_TAB_MAX; t++)
        if (tr_tab_shown((tr_tab_t)t)) out[n++] = (tr_tab_t)t;
    return n;
}

static int pages(void) {
    tr_tab_t tabs[TR_TAB_MAX];
    const int n = listed(tabs);
    return n ? (n + PER_PAGE - 1) / PER_PAGE : 1;
}

static EpdRect cell_rect(int k) {
    return (EpdRect){TR_MARGIN + (k % COLS) * (CELL_W + CELL_GAP), GRID_TOP + (k / COLS) * (CELL_H + CELL_GAP), CELL_W,
                     CELL_H};
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    tr_title(fb, "全部");
    tr_text_dim(fb, TR_MARGIN, TR_S(156), TR_S(26), "底部放哪几个：设置 › 底部标签", EPD_DRAW_ALIGN_LEFT);
    tr_tab_t tabs[TR_TAB_MAX];
    const int n = listed(tabs);
    const int from = tr_nav_app_tab(s_origin);
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = s_page * PER_PAGE + k;
        if (i >= n) break;
        // Material 3 卡片格：浅灰底，打开前所在的功能用深一点的灰（这一页只用整页刷新）。
        // Material 3 tiles: light gray, the origin feature a darker gray (this page refreshes whole pages only).
        const EpdRect r = cell_rect(k);
        const bool on = (int)tabs[i] == from;
        const uint8_t bg = on ? TR_GRAY_SELECTED : TR_GRAY_SURFACE;
        ui_fill_round_rect(fb, r, TR_S(24), bg);
        tr_icon_on(fb, tr_tab_icon(tabs[i]), r.x + r.width / 2, r.y + TR_S(62), UI_GRAY_BLACK, bg);
        tr_text_on(fb, r.x + r.width / 2, r.y + TR_S(108), TR_S(30), tr_tab_label(tabs[i]), EPD_DRAW_ALIGN_CENTER, on,
                   UI_GRAY_BLACK, bg);
    }
    tr_pager(fb, PAGER_Y, s_page, pages());
    tr_tab_bar(fb, (tr_tab_t)TR_TAB_ALL);
}

static app_redraw_t turn(int dir) {
    const int next = s_page + dir;
    if (next < 0 || next >= pages()) return APP_REDRAW_NONE;
    s_page = next;
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, (tr_tab_t)TR_TAB_ALL)) return APP_REDRAW_NONE;
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    tr_tab_t tabs[TR_TAB_MAX];
    const int n = listed(tabs);
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = s_page * PER_PAGE + k;
        if (i >= n) break;
        if (ui_rect_hit(cell_rect(k), x, y)) {
            ctx->request_app = tr_nav_tab_app(tabs[i]);
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    const int dir = ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U ? 1
                  : ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D ? -1 : 0;
    return dir ? turn(dir) : APP_REDRAW_NONE;
}

// KEY1 上一页；KEY3 下一页，最后一页再按就回到打开前的页面。/ KEY1: previous page; KEY3: next page, and on the
// last page it returns to where the page was opened from.
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    // 这一页接管三键，中键短按要自己回书库（和别的页一样）。/ This page owns the keys, so a short middle press goes
    // home here itself, as on other pages.
    if (key == UI_KEY_2) { ctx->request_app = tr_nav_tab_app(TR_TAB_LIBRARY); return APP_REDRAW_NONE; }
    if (key == UI_KEY_1) return turn(-1);
    if (key != UI_KEY_3) return APP_REDRAW_NONE;
    if (s_page + 1 < pages()) return turn(1);
    ctx->request_app = s_origin ? s_origin : tr_nav_tab_app(TR_TAB_LIBRARY);
    return APP_REDRAW_NONE;
}

// 进页时停在打开前所在功能的那一页。/ On entry, show the page holding the origin feature.
static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    tr_tab_t tabs[TR_TAB_MAX];
    const int n = listed(tabs), from = tr_nav_app_tab(s_origin);
    s_page = 0;
    for (int i = 0; i < n; i++)
        if ((int)tabs[i] == from) s_page = i / PER_PAGE;
}

const app_desc_t app_tr_all = {
    .title = "全部",
    .detail = "所有功能",
    .enter_full = true,
    .owns_keys = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_gesture = on_gesture,
    .on_key = on_key,
};
