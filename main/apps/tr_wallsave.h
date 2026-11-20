/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 看大图时的“存为壁纸”工具条和存好后的提示框（小红书、微博的看图页共用）。只管画、点和计时；复制文件交给
 * tr_lock_wallpaper_save()。
 *
 * English: the "save as wallpaper" toolbar and the saved notice for full-screen image views (shared by the
 * Xiaohongshu and Weibo pages). It only draws, handles taps and times the notice; tr_lock_wallpaper_save() copies the
 * file.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tr_ui.h"

/// 一个看图页的工具条状态。/ Toolbar state of one image view.
typedef struct {
    bool shown;               ///< 工具条开着 / Toolbar open
    char title[32];           ///< 提示框标题，空表示没有提示 / Notice title; empty means no notice
    char line[160];           ///< 提示框说明 / Notice detail
    int64_t until_ms;         ///< 提示框到这个时间消失 / Notice disappears at this time
} tr_wallsave_t;

/// 工具条顶边；看图页在它下面画自己的底栏（TR_S(96) 高）。随显示大小变，正常档 936。/ Toolbar top edge; the image
/// view draws its own band (TR_S(96) tall) below it. Moves with the display size; 936 at the normal size.
#define TR_WALLSAVE_TOP (TR_H - TR_S(96) - TR_S(184))

/// 画工具条（开着时）和提示框（没过期时）；在看图页最后调用。/ Draw the toolbar (when open) and the notice (when
/// active); call last in the image view.
void tr_wallsave_draw(uint8_t* fb, const tr_wallsave_t* w);
/// 工具条开着时点在它上面：处理按钮并返回 true（页面要重画）。src 是这张图的文件，source、author、n 拼文件名。
/// A tap on the open toolbar: handle the buttons and return true (the page redraws). src is the image file; source,
/// author and n form the file name.
bool tr_wallsave_tap(tr_wallsave_t* w, int x, int y, const char* src, const char* source, const char* author, int n,
                     int64_t now_ms);
/// 提示框到时间了返回 true（页面要重画）。/ True when the notice has just expired (the page redraws).
bool tr_wallsave_tick(tr_wallsave_t* w, int64_t now_ms);
/// 打开工具条，同时撤掉还在显示的提示框（免得框下面的按钮看不见还能点）。/ Open the toolbar and drop any notice
/// still showing (so buttons hidden under it cannot be tapped).
void tr_wallsave_open(tr_wallsave_t* w);
/// 收起工具条和提示（翻页、离开时）。/ Close the toolbar and notice (on page turns and exit).
void tr_wallsave_close(tr_wallsave_t* w);
