/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全部 demo 页的清单。菜单顺序。加页只改这张表。
 *
 * Demo page table. Menu order lives here; adding a page only changes this list.
 */

#include "app_registry.h"
#include "tr_ui.h"

extern const app_desc_t app_book;
extern const app_desc_t app_comic;
extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_settings;
extern const app_desc_t app_tr_hupu;
extern const app_desc_t app_tr_xhs;
extern const app_desc_t app_tr_weibo;
extern const app_desc_t app_tr_x;
extern const app_desc_t app_selftest;

// 菜单只列主页面（书库、漫画、番茄、小红书、虎扑、微博、X、设置），和底部标签栏一致；传书、字体、睡眠等从“设置”进入。
// 首页已并入书库（2026-11-17）。设置“底部标签”里隐藏的标签，菜单也跳过（2026-11-18，代替“隐藏虎扑”）。
// The menu lists the main pages (library, comics, Tomato, Xiaohongshu, Hupu, Weibo, X, settings), matching the tab bar;
// transfer, fonts, sleep etc. live under Settings. Home was merged into the library (2026-11-17). Tabs hidden under Settings
// "Tab bar" are skipped here too (2026-11-18, replacing "Hide Hupu").
static const struct {
    const app_desc_t* app;
    tr_tab_t tab;
} s_apps[] = {
    {&app_book, TR_TAB_LIBRARY},
    {&app_comic, TR_TAB_COMIC},
    {&app_tr_tomato, TR_TAB_TOMATO},
    {&app_tr_xhs, TR_TAB_XHS},
    {&app_tr_hupu, TR_TAB_HUPU},
    {&app_tr_weibo, TR_TAB_WEIBO},
    {&app_tr_x, TR_TAB_X},
    {&app_tr_settings, TR_TAB_SETTINGS},
};

#define APP_TOTAL ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

static bool listed(int i) {
    return tr_tab_shown(s_apps[i].tab);
}

int app_count(void) {
    int n = 0;
    for (int i = 0; i < APP_TOTAL; i++) n += listed(i);
    return n;
}

const app_desc_t* app_at(int index) {
    for (int i = 0; i < APP_TOTAL; i++) {
        if (!listed(i)) continue;
        if (index-- == 0) return s_apps[i].app;
    }
    return NULL;
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0, k = 0; i < APP_TOTAL; i++) {
        if (!listed(i)) continue;
        if (s_apps[i].app == app) return k;
        k++;
    }
    return -1;
}

// 开机、回退和中键（home）的目标是书库。/ Boot, fallback and the home key all lead to the library.
const app_desc_t* app_home_page(void) {
    return &app_book;
}

const app_desc_t* app_selftest_page(void) {
    return &app_selftest;
}
