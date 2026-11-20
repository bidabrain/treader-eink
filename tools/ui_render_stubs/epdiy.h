/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：界面预览渲染用的 epdiy 替身：一字节一像素的竖屏缓冲，坐标不旋转。
 * English: epdiy shim for the UI preview renderer: one byte per pixel, portrait, no rotation.
 * 冻结：仅用于主机预览。/ Frozen: host previews only.
 */
#pragma once
#include <stdint.h>

typedef struct { int x, y, width, height; } EpdRect;
enum EpdFontFlags {
    EPD_DRAW_BACKGROUND = 0x1,
    EPD_DRAW_ALIGN_LEFT = 0x2,
    EPD_DRAW_ALIGN_RIGHT = 0x4,
    EPD_DRAW_ALIGN_CENTER = 0x8,
};
enum EpdRotation { EPD_ROT_LANDSCAPE, EPD_ROT_PORTRAIT, EPD_ROT_INVERTED_LANDSCAPE, EPD_ROT_INVERTED_PORTRAIT };

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb);
void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* fb);
void epd_draw_rect(EpdRect rect, uint8_t color, uint8_t* fb);
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color, uint8_t* fb);
void epd_draw_circle(int x, int y, int r, uint8_t color, uint8_t* fb);
void epd_fill_circle(int x, int y, int r, uint8_t color, uint8_t* fb);
void epd_fill_circle_helper(int x, int y, int r, int corners, int delta, uint8_t color, uint8_t* fb);
void epd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color, uint8_t* fb);
void epd_draw_rotated_image(EpdRect rect, const uint8_t* image, uint8_t* fb);
int epd_rotated_display_width(void);
int epd_rotated_display_height(void);
int epd_width(void);
int epd_height(void);
enum EpdRotation epd_get_rotation(void);
