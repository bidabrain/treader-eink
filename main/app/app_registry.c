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
extern const app_desc_t app_transfer;
extern const app_desc_t app_font_pick;
extern const app_desc_t app_sleep;
extern const app_desc_t app_home;
extern const app_desc_t app_selftest;

// 只留读书要用的页；开发演示页（刷新、触摸、加速度计、PMU、扩展口等）已移出固件。
// Reading pages only; developer demo pages (refresh, touch, IMU, PMU, IO expander, ...) are dropped from the firmware.
static const app_desc_t* const s_apps[] = {
    &app_book,
    &app_transfer,
    &app_font_pick,
    &app_sleep,
    &app_home,
    &app_selftest,
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

// 开机和回退的默认页是书架。/ Boot and fallback page is the shelf.
const app_desc_t* app_home_page(void) {
    return &app_book;
}

const app_desc_t* app_selftest_page(void) {
    return &app_selftest;
}
