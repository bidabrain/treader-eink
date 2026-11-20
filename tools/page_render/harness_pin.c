/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏密码键盘的渲染测试。/ English: Render bench for the lock PIN keypad.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "tr_pin.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "pin";

void hr_scenes(void) {
    hr_no_bar_check();
    draw_all(hr_fb, "设置锁屏密码", "输入 4 位数字", 0, true);
    hr_shot("set");
    hr_no_bar_check();
    draw_all(hr_fb, "输入密码解锁", "密码不对，还能试 3 次", 3, false);
    hr_shot("gate");
#ifndef HR_BASELINE
    hr_no_bar_check();
    draw_locked(hr_fb, "输错太多次，300 秒后再试");
    hr_shot("locked");
#endif
}
