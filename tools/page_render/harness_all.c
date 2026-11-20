/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：“全部”页的渲染测试。/ English: Render bench for the All page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_all.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "all";

// tr_nav 的替身：只要功能和页面一一对应。/ tr_nav stand-ins: features map one to one onto pages.
static const app_desc_t s_feature[TR_TAB_MAX];
const app_desc_t* tr_nav_tab_app(int tab) { return tab >= 0 && tab < TR_TAB_MAX ? &s_feature[tab] : &app_tr_all; }
int tr_nav_app_tab(const app_desc_t* app) {
    for (int t = 0; t < TR_TAB_MAX; t++)
        if (app == &s_feature[t]) return t;
    return -1;
}

void hr_scenes(void) {
    static const uint8_t pins[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU};
    app_ctx_t ctx = {.fb = hr_fb};
    tr_tabs_configure(1u << TR_TAB_X, pins, 4);
    s_origin = &s_feature[TR_TAB_WEIBO];
    on_enter(&ctx);
    render(&ctx, hr_fb);
    hr_shot("default");
    tr_tabs_configure(0, pins, 4);
    s_origin = &s_feature[TR_TAB_X];
    on_enter(&ctx);
    render(&ctx, hr_fb);
    hr_shot("eight_last_page");
    s_page = 0;
    render(&ctx, hr_fb);
    hr_shot("eight_first_page");
}
