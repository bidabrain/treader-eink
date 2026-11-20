/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全部 demo 页的清单。菜单、主循环、开机页都从这里取，不再有 UI_PAGE_* 数字常量。
 *
 * Table of every demo page. Menu, loop, and boot page all read from here;
 * there are no UI_PAGE_* numeric constants.
 */

#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 开机默认页（书库）与设备功能自检页，供 app_main 与主循环引用。/ Boot default page (library) and device self-test page, for app_main and the loop.
const app_desc_t* app_home_page(void);
const app_desc_t* app_selftest_page(void);

#ifdef __cplusplus
}
#endif
