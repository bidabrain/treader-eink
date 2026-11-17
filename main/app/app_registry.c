/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全部 demo 页的清单。菜单顺序。加页只改这张表。
 *
 * Demo page table. Menu order lives here; adding a page only changes this list.
 */

#include "app_registry.h"
#include "settings.h"

extern const app_desc_t app_book;
extern const app_desc_t app_comic;
extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_settings;
extern const app_desc_t app_tr_hupu;
extern const app_desc_t app_selftest;

// 菜单只列主页面（书库、漫画、番茄、虎扑、设置），和底部标签栏一致；传书、字体、睡眠等从“设置”进入。首页已并入书库
// （2026-11-17）。设置里打开“隐藏虎扑”后菜单跳过虎扑（2026-11-18）。
// The menu lists the main pages (library, comics, Tomato, Hupu, settings), matching the tab bar; transfer, fonts, sleep
// etc. live under Settings. Home was merged into the library (2026-11-17). "Hide Hupu" skips Hupu (2026-11-18).
static const app_desc_t* const s_apps[] = {
    &app_book,
    &app_comic,
    &app_tr_tomato,
    &app_tr_hupu,
    &app_tr_settings,
};

#define APP_TOTAL ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

static bool listed(const app_desc_t* app) {
    return app != &app_tr_hupu || !app_settings_hide_hupu();
}

int app_count(void) {
    int n = 0;
    for (int i = 0; i < APP_TOTAL; i++) n += listed(s_apps[i]);
    return n;
}

const app_desc_t* app_at(int index) {
    for (int i = 0; i < APP_TOTAL; i++) {
        if (!listed(s_apps[i])) continue;
        if (index-- == 0) return s_apps[i];
    }
    return NULL;
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0, k = 0; i < APP_TOTAL; i++) {
        if (!listed(s_apps[i])) continue;
        if (s_apps[i] == app) return k;
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
