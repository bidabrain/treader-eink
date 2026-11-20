/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 开机页和自检页。功能清单在 tr_ui.c 和 tr_nav.c（见下面的说明）。
 *
 * The boot and self-test pages. The feature list lives in tr_ui.c and tr_nav.c (see the note below).
 */

#include "app_registry.h"
#include "tr_ui.h"

extern const app_desc_t app_book;
extern const app_desc_t app_selftest;

// 功能清单（2026-11-21 起）在 tr_ui.c 的 TAB_LABELS / TAB_ICONS 和 tr_nav.c 的 tr_nav_tab_app 里；“全部”页和底部
// 标签栏都从那里取。这里只留开机页和自检页。
// The feature list (since 2026-11-21) lives in TAB_LABELS / TAB_ICONS in tr_ui.c and tr_nav_tab_app in tr_nav.c;
// the All page and the bar read it there. Only the boot and self-test pages remain here.

// 开机、回退和中键（home）的目标是书库。/ Boot, fallback and the home key all lead to the library.
const app_desc_t* app_home_page(void) {
    return &app_book;
}

const app_desc_t* app_selftest_page(void) {
    return &app_selftest;
}
