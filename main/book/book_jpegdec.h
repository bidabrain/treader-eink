/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：基线 JPEG 快速通道的接口，只给 book_image.c 用。
 *
 * English: Interface of the baseline JPEG fast path, used only by book_image.c.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 文件头是否是快速通道接受的普通基线 JPEG；是则给出原图宽高。
/// Whether the header is a plain baseline JPEG the fast path accepts; if so, report the source size.
bool book_jpegdec_eligible(const uint8_t* data, size_t len, unsigned* width, unsigned* height);

/// 原图 iw×ih 解出来的大小，按 book_image_decode / book_image_decode_fit 这一次的规则；budget 是像素上限。
/// The output size for an iw x ih source under the rules of the current book_image_decode / book_image_decode_fit
/// call; budget caps the pixels.
void book_image_out_size(unsigned iw, unsigned ih, size_t budget, unsigned* w, unsigned* h);

/// 用 JPEGDEC 解成 book_image_out_size 给的大小的灰度图，规则同 book_image_decode；不合格或失败返回 false，调用方改走原路径。
/// Decode with JPEGDEC to gray at the size book_image_out_size gives, under book_image_decode's rules; returns false when ineligible or on
/// failure so the caller falls back to the original path.
bool book_jpegdec_decode(const uint8_t* data, size_t len, size_t budget,
                         uint8_t** pixels, uint16_t* width, uint16_t* height);
