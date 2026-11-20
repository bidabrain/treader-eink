/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏书卡的渲染测试。/ English: Render bench for the lock-screen book card.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "tr_lock.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "lock";

// 壁纸画成浅灰块。/ The wallpaper is a light gray block.
void book_screen_draw(uint8_t* fb, const uint8_t* img) {
    (void)img;
    epd_fill_rect((EpdRect){0, 0, TR_W, TR_H}, 0xD0, fb);
}

static void hr_card(const char* title, const char* author, bool progress, uint8_t pct, int battery) {
    memset(&s_card, 0, sizeof(s_card));
    s_card.ok = true;
    s_card.has_progress = progress;
    s_card.pct = pct;
    s_card.battery = battery;
    snprintf(s_card.title, sizeof(s_card.title), "%s", title);
    snprintf(s_card.author, sizeof(s_card.author), "%s", author);
    snprintf(s_card.format, sizeof(s_card.format), "TXT");
    snprintf(s_card.path, sizeof(s_card.path), "/sdcard/books/x.txt");
}

void hr_scenes(void) {
    hr_card("诡秘之主", "爱潜水的乌贼", true, 42, 82);
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("card");
    hr_card("一个特别特别长的书名用来测试锁屏上书名截断的效果", "一位名字很长的作者", true, 100, -1);
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_allow_fit();
    hr_shot("card_long");
    hr_card("三体", "", false, 0, 5);
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("card_no_progress");
    // 壁纸加底部白条。/ Wallpaper with the bottom strip.
    hr_card("诡秘之主：一个特别长的书名看底条截断", "爱潜水的乌贼", true, 42, 82);
    s_wall = malloc(4);
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_allow_fit();
    hr_shot("wallpaper");
    s_card.ok = false;
    s_wall = malloc(4);
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("wallpaper_no_book");
}
