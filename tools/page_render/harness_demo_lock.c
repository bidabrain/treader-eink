/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏书卡的演示图（README / 网站预览用）。/ English: Demo shot of the lock-screen book card (README / site).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "tr_lock.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_lock";

// 演示里不用壁纸。/ No wallpaper in the demo.
void book_screen_draw(uint8_t* fb, const uint8_t* img) {
    (void)img;
    hr_gradient(fb, (EpdRect){0, 0, TR_W, TR_H}, 7);
}

void hr_scenes(void) {
    memset(&s_card, 0, sizeof(s_card));
    s_card.ok = true;
    s_card.has_progress = true;
    s_card.pct = 41;
    s_card.battery = 82;
    snprintf(s_card.title, sizeof(s_card.title), "宿命之环");
    snprintf(s_card.author, sizeof(s_card.author), "爱潜水的乌贼");
    snprintf(s_card.format, sizeof(s_card.format), "番茄");
    snprintf(s_card.path, sizeof(s_card.path), "/sdcard/books/宿命之环.txt");
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("card");
}
