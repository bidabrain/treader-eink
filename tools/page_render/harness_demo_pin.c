/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏密码键盘的演示图（README / 网站预览用）。/ English: Demo shots of the lock PIN keypad (README / site).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "tr_pin.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_pin";

void hr_scenes(void) {
    // 设密码：已输两位。/ Setting a PIN with two digits entered.
    hr_no_bar_check();
    draw_all(hr_fb, "设置锁屏密码", "输入 4 位数字", 2, true);
    hr_shot("set");
    // 解锁：输错一次。/ Unlocking after one wrong try.
    hr_no_bar_check();
    draw_all(hr_fb, "输入密码解锁", "密码不对，还能试 4 次", 0, false);
    hr_shot("gate");
    // 输错太多次的倒计时。/ Lockout countdown after too many tries.
    hr_no_bar_check();
    draw_locked(hr_fb, "输错太多次，268 秒后再试");
    hr_shot("locked");
}
