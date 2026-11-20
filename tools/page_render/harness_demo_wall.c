/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏壁纸页的演示截图（设计文档用）：壁纸列表和全屏预览。
 * English: Demo shots of the wallpaper page for the design doc: the list and the full-screen preview.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_wall.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "demo_wall";

// 预览图：整屏渐变当照片。/ Preview: a full-screen gradient as the photo.
void book_screen_draw(uint8_t* fb, const uint8_t* packed) {
    (void)packed;
    hr_gradient(fb, (EpdRect){0, 0, TR_W, TR_H}, 7);
}

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    static const char* const NAMES[] = {"城市夜景.jpg", "晨雾.jpg", "森林小径.jpg", "海边日落.jpg", "雨后街道.png", "雪山.png"};
    static const uint32_t SIZES[] = {1468006, 1258291, 943718, 1782579, 1992294, 629146};
    s_count = 6;
    for (int i = 0; i < s_count; i++) {
        snprintf(s_names[i], NAME_LEN, "%s", NAMES[i]);
        s_sizes[i] = SIZES[i];
    }
    s_no_card = false;
    s_view = V_LIST;
    s_page = 0;
    s_msg[0] = 0;
    snprintf(s_current, sizeof(s_current), WALL_DIR "/晨雾.jpg");
    hr_allow_fit();
    shot("ui58");
    static uint8_t img[4];
    s_preview = img;
    s_view = V_PREVIEW;
    s_confirm_del = false;
    shot("ui59");
    s_preview = NULL;
    s_view = V_LIST;
}
