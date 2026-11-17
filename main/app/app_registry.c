/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全部 demo 页的清单。菜单顺序。加页只改这张表。
 *
 * Demo page table. Menu order lives here; adding a page only changes this list.
 */

#include "app_registry.h"

extern const app_desc_t app_tr_home;
extern const app_desc_t app_book;
extern const app_desc_t app_tr_tomato;
extern const app_desc_t app_tr_settings;
extern const app_desc_t app_selftest;

// 菜单（中键打开）只列四个主页面，和底部标签栏一致；传书、字体、睡眠等从“设置”进入。
// The menu (KEY3) lists the four main pages, matching the tab bar; transfer, fonts, sleep etc. live under Settings.
static const app_desc_t* const s_apps[] = {
    &app_tr_home,
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

// 开机和回退的默认页是首页。/ Boot and fallback page is Home.
const app_desc_t* app_home_page(void) {
    return &app_tr_home;
}

const app_desc_t* app_selftest_page(void) {
    return &app_selftest;
}
