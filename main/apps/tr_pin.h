/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 4 位数字锁屏密码：整屏数字键盘（设置里设密码、关密码也用它），以及开机和解锁时的密码关。
 *
 * English: the 4-digit lock PIN: a full-screen keypad (also used by settings to set or remove the PIN), and the PIN
 * gate at boot and on unlock.
 *
 * 冻结：密码开着时，开机和每次从锁屏回来都要先过密码关，过关之前不进任何页面；不操作 30 秒或按电源键回到锁屏睡眠。
 * 输错 5 次以后每次要等一会儿才能再输。忘了密码只能用电脑擦掉 NVS（见 docs/TREADER.md），设备上没有绕过的入口。
 * Frozen: with the PIN on, boot and every return from the lock screen pass the gate before any page shows; 30 s
 * without input or the power key go back to the locked sleep. After 5 wrong tries each new try waits a while. A
 * forgotten PIN can only be cleared by erasing NVS from a computer (see docs/TREADER.md); the device has no bypass.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cst836u.h"
#include "epd_highlevel.h"
#include "sc7a20h.h"

/// 键盘的结果。/ Keypad outcome.
typedef enum {
    TR_PIN_ENTERED,    ///< 输满 4 位 / Four digits entered
    TR_PIN_CANCELLED,  ///< 点了“取消” / "Cancel" tapped
    TR_PIN_TIMEOUT,    ///< 太久没操作 / No input for too long
    TR_PIN_POWER,      ///< 按了电源键 / Power key pressed
} tr_pin_result_t;

/// 整屏数字键盘，阻塞到出结果。title 是大标题，hint 是下面一行提示；cancel 为真时左下角有“取消”。
/// ignore_power_until_ms 之前的电源键短按不算（刚唤醒时的残留）。out 收 4 位数字和结尾 0。
/// A blocking full-screen keypad. title is the heading and hint the line under it; with cancel the bottom-left key
/// reads "Cancel". Power-key presses before ignore_power_until_ms do not count (leftovers from waking). out receives
/// the four digits and a terminating zero.
tr_pin_result_t tr_pin_pad(EpdiyHighlevelState* hl, cst836u_handle_t tp, const char* title, const char* hint,
                           bool cancel, int timeout_ms, int64_t ignore_power_until_ms, char out[5]);

/// 开机或解锁时的密码关：没开密码直接返回；过关才返回。不操作或按电源键就锁屏睡眠，醒来接着要密码。
/// The PIN gate at boot or unlock: returns at once when no PIN is set, otherwise only after the right PIN. No input
/// or the power key locks and sleeps, and waking asks again.
void tr_pin_gate(EpdiyHighlevelState* hl, cst836u_handle_t tp, sc7a20h_handle_t acc, int64_t* ignore_until_ms);
