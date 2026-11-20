/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：“全部”页的演示截图（README / 网站预览用）：七个功能，X 隐藏，从微博进来。
 * English: Demo shot of the All page for the README / site previews: seven features, X hidden, opened from Weibo.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_all.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "demo_all";

// tr_nav 的替身：功能和页面一一对应。/ tr_nav stand-ins: features map one to one onto pages.
static const app_desc_t s_feature[TR_TAB_MAX];
const app_desc_t* tr_nav_tab_app(int tab) { return tab >= 0 && tab < TR_TAB_MAX ? &s_feature[tab] : &app_tr_all; }
int tr_nav_app_tab(const app_desc_t* app) {
    for (int t = 0; t < TR_TAB_MAX; t++)
        if (app == &s_feature[t]) return t;
    return -1;
}

void hr_scenes(void) {
    // 默认排法：漫画 番茄 小红书 虎扑 放底部，X 隐藏。/ Default arrangement: comics, Tomato, Xiaohongshu, Hupu pinned; X hidden.
    static const uint8_t pins[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU};
    app_ctx_t ctx = {.fb = hr_fb};
    tr_tabs_configure(1u << TR_TAB_X, pins, 4);
    s_origin = &s_feature[TR_TAB_WEIBO];
    on_enter(&ctx);
    render(&ctx, hr_fb);
    hr_shot("all");
}
