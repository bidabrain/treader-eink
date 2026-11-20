/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：主机测试里没有 JPEGDEC：快速通道一律不接，book_image 走原来的 tjpgd 路径。
 * English: No JPEGDEC in host tests: the fast path always declines, so book_image takes the original tjpgd path.
 * 冻结：仅用于主机测试。/ Frozen: host tests only.
 */
#include "book_jpegdec.h"

bool book_jpegdec_eligible(const uint8_t* data, size_t len, unsigned* width, unsigned* height) {
    (void)data; (void)len; (void)width; (void)height;
    return false;
}

bool book_jpegdec_decode(const uint8_t* data, size_t len, size_t budget, uint8_t** pixels, uint16_t* width,
                         uint16_t* height) {
    (void)data; (void)len; (void)budget; (void)pixels; (void)width; (void)height;
    return false;
}
