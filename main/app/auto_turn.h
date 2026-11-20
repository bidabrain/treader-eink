/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读和漫画共用的定时自动翻页：档位（关、3、6……30 秒）、工具条里的调节行，以及“正在自动翻页”的全局标记，
 * 主循环看到它就不按空闲自动锁屏。计时和翻页由各自的页面做。
 *
 * Timed auto page turn shared by the reader and comics: levels (off, 3, 6 ... 30 s), the toolbar adjust row, and a
 * global "auto turning" flag that keeps the main loop from idle-locking. Pages own the timing and the turn itself.
 *
 * 冻结：每档相差 3 秒；碰屏幕或按键会重新计时，工具条开着时暂停，到最后一页停下。
 * Frozen: levels are 3 s apart; a touch or key restarts the countdown, the open toolbar pauses it, the last page stops it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"
#include "ui_scale.h"

#define AUTO_TURN_STEP_S 3
#define AUTO_TURN_LEVELS 10   ///< 1～10 档 = 3～30 秒，0 为关 / Levels 1-10 = 3-30 s, 0 is off
#define AUTO_TURN_ROW_H UI_S(60)   ///< 行高随显示大小放大 / Row height follows the display size
/// −、+ 按钮宽度。/ Width of the minus and plus buttons.
#define AUTO_TURN_BTN_W UI_S(90)

/// 档位对应的毫秒数，0 档返回 0。/ Milliseconds for a level; 0 for off.
int32_t auto_turn_ms(int level);
/// 画“自动翻页  −  12 秒  +”这一行，y 是行顶。/ Draw the "auto turn  -  12 s  +" row; y is the row top.
void auto_turn_draw_row(uint8_t* fb, int y, int level);
/// 点在这一行的 − 或 + 上：返回 -1 / 1，否则 0。/ Hit on the row's minus or plus: -1 / 1, else 0.
int auto_turn_row_hit(int y, uint16_t tx, uint16_t ty);
/// 调一档，夹在 0～AUTO_TURN_LEVELS。/ Step a level, clamped to 0..AUTO_TURN_LEVELS.
int auto_turn_step(int level, int dir);

/// 页面告诉主循环现在是否在自动翻页。/ Pages tell the loop whether auto turning is running.
void auto_turn_set_active(bool active);
bool auto_turn_active(void);
