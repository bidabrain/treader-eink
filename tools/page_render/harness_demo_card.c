/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：TF 卡页的演示截图（设计文档用）：状态和容量、格式化确认。
 * English: Demo shots of the TF card page for the design doc: status and capacity, format confirmation.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_card.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "demo_card";

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    // 64 GB 卡，用了三分之一。/ A 64 GB card, a third used.
    memset(&s_info, 0, sizeof(s_info));
    s_info_err = ESP_OK;
    s_info.present = s_info.mounted = true;
    s_info.capacity_bytes = 62537072640ull;
    s_info.free_bytes = 44238163558ull;
    s_msg[0] = 0;
    s_view = V_INFO;
    shot("ui56");
    // 已点过一次“清空并格式化”。/ "Erase" already tapped once.
    s_view = V_ASK;
    s_armed = true;
    shot("ui57");
    s_armed = false;
    s_view = V_INFO;
}
