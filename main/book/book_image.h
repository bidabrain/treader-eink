/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：有界 JPEG/PNG 解码与灰度缩放，调用方持有输出。
 * English: Bounded JPEG/PNG decoding and grayscale resizing; caller owns output.
 * 冻结：不访问文件和显示；失败交还空输出，不影响正文阅读。
 * Frozen: No file or display access; failure leaves empty output and must not block text reading.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 解码到不超过 648×1000 的灰度图；输入至多 8 MiB。基线 JPEG 和非交错 PNG 边解边缩，多大都行（至多 16384 边长）；
/// 渐进 JPEG、交错或 16 位 PNG 要整张解，按剩余 PSRAM 算上限（约 1～1.6 百万像素）。
/// Decode to at most 648x1000 gray; input <= 8 MiB. Baseline JPEG and non-interlaced PNG scale while decoding at any
/// size (sides up to 16384); progressive JPEG and interlaced or 16-bit PNG decode whole, capped by free PSRAM (~1-1.6 MP).
bool book_image_decode(const uint8_t* data, size_t len, size_t budget,
                       uint8_t** pixels, uint16_t* width, uint16_t* height);

/// 直接解成要显示的大小，省掉先解 648×1000 再缩放的那块中间图。cover 为假时等比装进 box_w×box_h；为真时取刚好铺满
/// box_w×box_h 的最小尺寸（调用方再居中裁切），超过 max(框面积, 648×1000) 时退回装进框里。都不放大原图，原图更小时
/// 输出原图大小，调用方自己放大。规则和上限同 book_image_decode。
/// Decode straight to the display size, skipping the 648x1000 intermediate that is scaled afterwards. With cover false
/// the image fits inside box_w x box_h keeping its aspect; with cover true it is the smallest size filling box_w x box_h
/// (the caller crops the center), falling back to fitting inside the box above max(box area, 648x1000). Neither enlarges
/// the source: a smaller source comes back at its own size for the caller to enlarge. Otherwise as book_image_decode.
bool book_image_decode_fit(const uint8_t* data, size_t len, unsigned box_w, unsigned box_h, bool cover,
                           uint8_t** pixels, uint16_t* width, uint16_t* height);
