/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全部 demo 页的清单。菜单顺序。加页只改这张表。
 *
 * Demo page table. Menu order lives here; adding a page only changes this list.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_settings;
extern const app_desc_t app_selftest;

// 菜单只列三个主页面，和底部标签栏一致；传书、字体、睡眠等从“设置”进入。首页已并入书库（2026-11-17）。
// The menu lists the three main pages, matching the tab bar; transfer, fonts, sleep etc. live under Settings.
// Home was merged into the library (2026-11-17).
static const app_desc_t* const s_apps[] = {
    &app_book,
    &app_tr_tomato,
    &app_tr_settings,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
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
