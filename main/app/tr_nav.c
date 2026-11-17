/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tr_nav.h"

extern const app_desc_t app_tr_home;
extern const app_desc_t app_book;
extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_settings;

const app_desc_t* tr_nav_tab_app(int tab) {
    switch (tab) {
    case TR_TAB_HOME: return &app_tr_home;
    case TR_TAB_LIBRARY: return &app_book;
    case TR_TAB_TOMATO: return &app_tr_tomato;
    case TR_TAB_SETTINGS: return &app_tr_settings;
    default: return NULL;
    }
}

bool tr_nav_touch(app_ctx_t* ctx, uint16_t x, uint16_t y, tr_tab_t self) {
    int tab = tr_tab_hit(x, y);
    if (tab < 0) return false;
    if (tab != (int)self) ctx->request_app = tr_nav_tab_app(tab);
    return true;
}

void tr_nav_open_book(app_ctx_t* ctx, const char* path) {
    app_book_request_open(path);
    ctx->request_app = &app_book;
}
