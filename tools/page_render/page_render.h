/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：页面渲染测试台。每个 harness_*.c 直接 #include 一个真实页面文件，设好页面的静态状态后调它的 render()，
 * 在正常 / 大 / 特大三档显示大小下各画一张 PNG，并检查：画到屏幕外的像素、压在底部标签栏下面的内容、被截断的文字。
 *
 * English: Page render bench. Each harness_*.c #includes one real page file, sets the page's static state, calls its
 * render(), and writes a PNG at the normal / large / largest display sizes while checking for pixels drawn off
 * screen, content hidden under the tab bar, and truncated text.
 *
 * 冻结：只在电脑上跑，不改固件代码。/ Frozen: host-only; never changes firmware code.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"

/// 当前的帧缓冲（设备格式：横屏 4bpp）。/ The current framebuffer (device layout: landscape 4bpp).
extern uint8_t* hr_fb;

/// 页面提供：画这一页的几种状态，每种状态调一次 hr_shot。主程序按三档显示大小各调一次。
/// Page-provided: draw the page's states, calling hr_shot once per state. main runs it once per display size.
void hr_scenes(void);
/// 页面名（输出目录名）。/ Page name (output directory).
extern const char* hr_page;

/// 清屏（白），开始一张图。/ Clear to white and start a shot.
void hr_begin(void);
/// 结束一张图：写 PNG、打印检查结果。/ Finish a shot: write the PNG and print the checks.
void hr_shot(const char* scene);
/// 这一张图不检查标签栏遮挡（全屏页、没有标签栏的页）。/ Skip the tab-bar overlap check for this shot.
void hr_no_bar_check(void);
/// 状态栏右上角显示的 Wi-Fi 名，默认空。/ The Wi-Fi name shown at the top right of the status bar, empty by default.
extern char hr_wifi_name[33];
/// 标记文字截断是预期的（内容文字，例如书名、帖子）。/ Mark truncation as expected (content text such as titles).
void hr_allow_fit(void);
/// 演示用的“照片”：斜向灰度渐变，seed 换花样（演示模式下封面和图片都用它）。/ Demo "photo": a diagonal gray gradient,
/// seed varies it (covers and images use it in demo mode).
void hr_gradient(uint8_t* fb, EpdRect r, int seed);
