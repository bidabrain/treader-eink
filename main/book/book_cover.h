/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：书籍封面。EPUB 取书里的封面图，番茄书取下载时存的封面；解码一次缩成 300×400 灰度图，
 * 缓存在 TF 卡 /sdcard/.treader/covers，之后只读缓存。内存里最多留 12 张。
 *
 * 壁纸：wallpapers 文件夹里的图片也能当“封面”取缩略图；全屏壁纸另存 684×1216 的 16 级灰度缓存。
 *
 * English: Book covers. EPUBs use their embedded cover image; Tomato books use the image saved at download.
 * Each is decoded once into a 300x400 grayscale thumbnail cached under /sdcard/.treader/covers; later reads
 * hit the cache only. Up to 12 thumbnails stay in memory. Images in the wallpapers folder can serve as their own
 * thumbnail source; full-screen wallpapers get a separate 684x1216 16-level cache.
 *
 * 冻结：只在主任务调用。绘制只读缓存文件，不解码；封面解码只在 book_cover_tick() 里一次一张，
 * 全屏壁纸只在选壁纸页和锁屏准备（屏幕上电前）时解码。
 * Frozen: main task only. Drawing reads cache files but never decodes; covers decode one per book_cover_tick(),
 * wallpapers only in the picker and in lock preparation before the panel powers on.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"

#define BOOK_COVER_W 300
#define BOOK_COVER_H 400
#define BOOK_SCREEN_W 684
#define BOOK_SCREEN_H 1216
/// 全屏图缓冲大小：每字节两个像素，高 4 位在左。/ Full-screen buffer: two pixels per byte, high nibble first.
#define BOOK_SCREEN_BYTES ((size_t)BOOK_SCREEN_W * BOOK_SCREEN_H / 2)

/// 封面状态。/ Cover state.
typedef enum {
    BOOK_COVER_NONE = 0,   ///< 没有封面，画文字封面 / No cover; draw the text cover
    BOOK_COVER_READY,      ///< 已在内存 / In memory
    BOOK_COVER_PENDING,    ///< 还没生成缓存，已排队 / Cache not built yet; queued
} book_cover_state_t;

/// 只查内存和缓存文件，不解码；缺缓存时排队并返回 PENDING。
/// Check memory and the cache file without decoding; queue and return PENDING when the cache is missing.
book_cover_state_t book_cover_load(const char* book_path);

/// 有封面就缩放画进 r（带边框）并返回 true；没有返回 false，由调用方画文字封面。
/// Scale the cover into r with a border and return true; false means the caller draws a text cover.
bool book_cover_draw(uint8_t* fb, EpdRect r, const char* book_path);

/// 页面的 on_tick 调用：生成一张排队的封面。队列刚清空且有新封面时返回 true，页面应整页重画一次。
/// Call from a page's on_tick: build one queued cover. Returns true when the queue just emptied with a new
/// cover, so the page should redraw once.
bool book_cover_tick(void);

/// 清空排队（进页时调用，别替上一页干活）。/ Drop queued work (call on page entry).
void book_cover_drop_pending(void);

/// 把图片做成全屏缓存（铺满裁切、抖动到 16 级灰度）。已有新鲜缓存直接返回 true。慢，约 1～2 秒。
/// Build the full-screen cache for an image (fill, crop, dither to 16 grays). A fresh cache returns true at once.
bool book_screen_build(const char* image_path);
/// 读全屏缓存到 PSRAM（BOOK_SCREEN_BYTES，调用方 heap_caps_free）；没有新鲜缓存返回 NULL，不解码。
/// Load the full-screen cache into PSRAM (caller frees); NULL without a fresh cache. Never decodes.
uint8_t* book_screen_load(const char* image_path);
/// 把全屏缓冲画进 fb。/ Paint a full-screen buffer into fb.
void book_screen_draw(uint8_t* fb, const uint8_t* packed);
/// 是不是能当壁纸的图片（JPEG/PNG）。/ Whether a file name is a usable image (JPEG/PNG).
bool book_image_name(const char* name);

/// 删书后调用：删掉这本书的缩略图缓存和内存里的那张。/ After deleting a book: drop its thumbnail cache and memory copy.
void book_cover_forget(const char* book_path);
/// 只留 keep_image 的全屏壁纸缓存，其余删掉（keep 为 NULL 全删）。/ Keep only keep_image's wallpaper cache (NULL deletes all).
void book_screen_prune(const char* keep_image);

/// 把灰度图缩放到 dw×dh，铺满并居中裁切（比例相同时就是纯缩放）。/ Scale a gray image to dw x dh, filling and
/// center-cropping (a plain resize when the aspect ratios match).
void book_image_fill(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh);

/// 释放内存里的全部封面（打开书前调用，把 PSRAM 让给排版）。/ Free all thumbnails (before opening a book).
void book_cover_release(void);
