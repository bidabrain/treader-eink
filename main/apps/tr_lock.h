/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 画锁屏书卡；没有在读的书返回 false，调用方改画官方锁屏图。
/// Draw the lock-screen book card; returns false when no book is in progress so the caller draws the stock image.
/// 屏幕上电前读出书卡要用的数据（文件、NVS、PMU）。/ Read the card's data (files, NVS, PMU) before the panel powers on.
void tr_lock_prepare(void);
bool tr_lock_draw(uint8_t* fb);

/// 读锁屏壁纸路径（NVS）；没设置返回 false。/ Read the lock wallpaper path from NVS; false when unset.
bool tr_lock_wallpaper_get(char* out, size_t cap);
/// 设置锁屏壁纸；NULL 或空串表示改回书卡。/ Set the lock wallpaper; NULL or empty restores the book card.
void tr_lock_wallpaper_set(const char* path);

/// 存为壁纸的结果。/ Outcome of saving a wallpaper.
typedef enum {
    TR_WALL_SAVED,      ///< 存好了 / Saved
    TR_WALL_EXISTS,     ///< 同一张已经存过 / The same image was saved before
    TR_WALL_FAILED,     ///< 读不到原图或写不进卡 / Source unreadable or the card refused the write
} tr_wall_save_t;

/// 把一张 JPEG/PNG 复制到 TF 卡 wallpapers/，文件名“来源_作者_第几张.扩展名”，重名加序号；同名同大小的算已经存过。
/// out 返回存下（或已有）的完整路径。/ Copy a JPEG/PNG into wallpapers/ on the TF card as "source_author_n.ext", adding
/// a number on name clashes; a same-named file of the same size counts as already saved. out gets the full path saved
/// (or found).
tr_wall_save_t tr_lock_wallpaper_save(const char* src, const char* source, const char* author, int n, char* out,
                                      size_t cap);
