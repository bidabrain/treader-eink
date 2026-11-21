/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tr_nav.h"

extern const app_desc_t app_book;
extern const app_desc_t app_comic;
extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_settings;
extern const app_desc_t app_tr_hupu;
extern const app_desc_t app_tr_xhs;
extern const app_desc_t app_tr_weibo;
extern const app_desc_t app_tr_x;
extern const app_desc_t app_tr_store;
extern const app_desc_t app_tr_all;
extern const app_desc_t app_tr_auth;

// 番茄标签上次停留的页面（书架或书城）。/ Last page under the Tomato tab (shelf or store).
static const app_desc_t* s_tomato_page = &app_tr_tomato;

void tr_nav_set_tomato_page(const app_desc_t* page) {
    if (page) s_tomato_page = page;
}

const app_desc_t* tr_nav_tab_app(int tab) {
    switch (tab) {
    case TR_TAB_LIBRARY: return &app_book;
    case TR_TAB_COMIC: return &app_comic;
    case TR_TAB_TOMATO: return s_tomato_page;
    case TR_TAB_XHS: return &app_tr_xhs;
    case TR_TAB_HUPU: return &app_tr_hupu;
    case TR_TAB_WEIBO: return &app_tr_weibo;
    case TR_TAB_X: return &app_tr_x;
    case TR_TAB_SETTINGS: return &app_tr_settings;
    case TR_TAB_AUTH: return &app_tr_auth;
    case TR_TAB_ALL: return &app_tr_all;
    default: return NULL;
    }
}

int tr_nav_app_tab(const app_desc_t* app) {
    if (!app) return -1;
    if (app == &app_tr_store) return TR_TAB_TOMATO;
    for (int t = 0; t < TR_TAB_MAX; t++)
        if (tr_nav_tab_app(t) == app || (t == TR_TAB_TOMATO && app == &app_tr_tomato)) return t;
    return -1;
}

bool tr_nav_touch(app_ctx_t* ctx, uint16_t x, uint16_t y, tr_tab_t self) {
    int tab = tr_tab_hit(x, y);
    if (tab < 0) return false;
    if (tab == (int)self) return true;
    // 点“全部”：记下从哪个功能来。/ Tapping "All": remember which feature it came from.
    if (tab == TR_TAB_ALL) tr_all_open_from(tr_nav_tab_app(self));
    ctx->request_app = tr_nav_tab_app(tab);
    return true;
}

void tr_nav_open_book(app_ctx_t* ctx, const char* path) {
    app_book_request_open(path);
    ctx->request_app = &app_book;
}
