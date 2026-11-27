/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：微信读书接口的弱替身，所有页面共用：书库、书城、设置画图时会问微信读书的状态，电脑上没有这个组件。
 * 微信读书页自己的渲染测试用强定义覆盖它们。
 * English: Weak WeRead stand-ins shared by every page: the library, store and settings ask WeRead for its state while
 * drawing, and the host has no such component. The WeRead page's own bench overrides them with strong definitions.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
// 比对改动前的源码（compare.sh）时没有这个组件，整份跳过。/ Skipped entirely when comparing against sources that
// predate the component (compare.sh).
#if __has_include("read_pico_weread.h")
#include <stdbool.h>
#include <string.h>

#include "app.h"
#include "read_pico_weread.h"

#define WEAK __attribute__((weak))

WEAK bool weread_configure(void) { return true; }
WEAK bool weread_disclaimer_accepted(void) { return true; }
WEAK bool weread_accept_disclaimer(void) { return true; }
WEAK bool weread_logged_in(void) { return false; }
WEAK esp_err_t weread_start_sync(void) { return ESP_OK; }
WEAK esp_err_t weread_start_download(const char* book_id, bool images) { (void)book_id, (void)images; return ESP_OK; }
WEAK bool weread_busy(void) { return false; }
WEAK void weread_snapshot(weread_snapshot_t* out) { memset(out, 0, sizeof(*out)); }
WEAK int weread_shelf_count(void) { return 0; }
WEAK bool weread_shelf_get(int index, weread_book_t* out) { (void)index, (void)out; return false; }
WEAK bool weread_is_book(const char* path) { (void)path; return false; }
WEAK esp_err_t weread_logout(void) { return ESP_OK; }
WEAK void weread_drop_partial(void) {}
WEAK void weread_forget_book(const char* path) { (void)path; }

// 书城来源切换在 app_tr_weread.c；别的页面的测试没有编它。/ The store source switch lives in app_tr_weread.c, which
// other pages' benches do not build.
extern const app_desc_t app_tr_store;
WEAK void tr_store_mark(bool weread) { (void)weread; }
WEAK const app_desc_t* tr_store_entry(bool tomato_on) { (void)tomato_on; return &app_tr_store; }
WEAK void tr_store_draw_source(uint8_t* fb, bool tomato_on, bool weread) { (void)fb, (void)tomato_on, (void)weread; }
WEAK bool tr_store_source_hit(app_ctx_t* ctx, bool tomato_on, bool weread, uint16_t x, uint16_t y) {
    (void)ctx, (void)tomato_on, (void)weread, (void)x, (void)y;
    return false;
}
#endif
