/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 虎扑页的图片：把下载好的 JPG 缩放成指定尺寸的灰度图，缓存到 TF 卡和内存，绘制时抖动到 16 级灰度。
 *
 * English: images for the Hupu page. Scale a downloaded JPEG to a set size in gray, cache it on the TF card and
 * in memory, and dither it to 16 grays when drawing.
 *
 * 冻结：只在页面的 on_tick 里解码（一次一张），绘制只读缓存，不解码。
 * Frozen: decode only from the page's on_tick, one image per call; drawing reads caches and never decodes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 取 jpg 缩放到 w×h 的灰度像素；内存或 TF 卡上有就返回，没有就排队生成并返回 NULL。
/// Get jpg scaled to w x h in gray; returns it from memory or the TF cache, else queues a build and returns NULL.
const uint8_t* hupu_img_get(const char* jpg, int w, int h);

/// 这张图解码失败过（不会再试）。/ Whether this image failed to decode (it will not be retried).
bool hupu_img_failed(const char* jpg, int w, int h);

/// 页面 on_tick 调用：生成一张排队的图。队列刚清空且有新图时返回 true，页面整页重画一次。
/// Call from on_tick: build one queued image. Returns true when the queue just emptied with a new image.
bool hupu_img_tick(void);

/// 还有排队的图。/ Whether builds are queued.
bool hupu_img_pending(void);

/// 清空排队（翻页、换视图时调用）。/ Drop queued builds (on page turns and view changes).
void hupu_img_drop_pending(void);

/// 释放内存里的全部图（离开虎扑页时调用）。/ Free all in-memory images (when leaving the Hupu page).
void hupu_img_release(void);

/// 只放掉内存里的图（TF 卡上都有缓存，用时读回），排队和失败记录不动；给 mem_reclaim 用。
/// Drop only the images in memory (all cached on the card and read back when needed), keeping the queue and failure
/// records; for mem_reclaim.
void hupu_img_trim(void);

/// 把 w×h 灰度像素抖动画到 (x, y)，border 时加双线框。/ Dither w x h gray pixels at (x, y), with a double frame if border.
void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border);
