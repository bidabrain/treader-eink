/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：TF 卡页的渲染测试。/ English: Render bench for the TF card page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_card.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "card";

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    memset(&s_info, 0, sizeof(s_info));
    s_info_err = ESP_OK;
    s_info.present = s_info.mounted = true;
    s_info.capacity_bytes = 31914983424ull;
    s_info.free_bytes = 12884901888ull;
    s_view = V_INFO;
    s_msg[0] = 0;
    shot("info");
    snprintf(s_msg, sizeof(s_msg), "格式化完成，但有文件夹没建好，请重新挂载后再试。");
    shot("info_message");
    s_msg[0] = 0;
    s_info.mounted = false;
    s_info.needs_format = true;
    shot("needs_format");
    memset(&s_info, 0, sizeof(s_info));
    shot("no_card");
    s_info.present = s_info.mounted = true;
    s_view = V_ASK;
    s_armed = false;
    shot("ask");
    s_armed = true;
    shot("ask_armed");
    s_view = V_WORKING;
    shot("working");
}
