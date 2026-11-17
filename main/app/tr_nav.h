/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 标签页之间的跳转，以及从别的页面直接打开一本书。
 * Switching between tab pages, and opening a book directly from another page.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "tr_ui.h"

/// 标签对应的页面。/ Page for a tab.
const app_desc_t* tr_nav_tab_app(int tab);
/// 记住番茄标签上次停在书架还是书城。/ Remember whether the Tomato tab was last on the shelf or the store.
void tr_nav_set_tomato_page(const app_desc_t* page);
/// 处理标签栏点击；点中别的标签就请求切页。返回是否点在标签栏上。
/// Handle a tab-bar tap; request a switch for another tab. Returns whether the tap hit the bar.
bool tr_nav_touch(app_ctx_t* ctx, uint16_t x, uint16_t y, tr_tab_t self);
/// 打开一本书：书库页扫描完后直接进入阅读。/ Open a book: the library opens it as soon as its scan finishes.
void tr_nav_open_book(app_ctx_t* ctx, const char* path);

/// 由书库页实现。/ Implemented by the library page.
void app_book_request_open(const char* path);
/// 这个路径是不是阅读器正开着的书（可跨任务调用）。/ Whether this path is the book open in the reader (any task).
bool app_book_path_is_open(const char* path);
