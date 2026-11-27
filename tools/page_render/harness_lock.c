/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏书卡的渲染测试。/ English: Render bench for the lock-screen book card.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "tr_lock.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#ifndef HR_BASELINE   // 改动前没有今日塔罗 / No daily tarot before this change
#include "tr_tarot.c"
#endif
#include "page_render.h"

const char* hr_page = "lock";

// 壁纸画成灰块（默认浅灰，塔罗场景也试深色和上浅下深）。/ The wallpaper is a gray block (light by default; the
// tarot scenes also try dark and light-over-dark).
static uint8_t hr_wall_top = 0xD0, hr_wall_bottom = 0xD0;
void book_screen_draw(uint8_t* fb, const uint8_t* img) {
    (void)img;
    epd_fill_rect((EpdRect){0, 0, TR_W, TR_H / 2}, hr_wall_top, fb);
    epd_fill_rect((EpdRect){0, TR_H / 2, TR_W, TR_H - TR_H / 2}, hr_wall_bottom, fb);
}
bool book_screen_draw_file(uint8_t* fb, const char* image, uint8_t* lum) {
    book_screen_draw(fb, (const uint8_t*)image);
#ifndef HR_BASELINE
    for (int r = 0; r < BOOK_SCREEN_LUM_H; r++)
        for (int c = 0; c < BOOK_SCREEN_LUM_W; c++)
            lum[r * BOOK_SCREEN_LUM_W + c] = r * BOOK_SCREEN_LUM_CELL < TR_H / 2 ? hr_wall_top : hr_wall_bottom;
#endif
    return true;
}
#ifdef HR_BASELINE   // 改动前 s_wall 是读进内存的缓冲 / Before this change s_wall was a loaded buffer
#define HR_SET_WALL() (s_wall = malloc(4))
#else
#define HR_SET_WALL() snprintf(s_wall, sizeof(s_wall), "/sdcard/wallpapers/a.jpg")
#endif

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
    HR_SET_WALL();
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_allow_fit();
    hr_shot("wallpaper");
    s_card.ok = false;
    HR_SET_WALL();
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("wallpaper_no_book");
#ifndef HR_BASELINE
    // 今日塔罗：浅色壁纸正位、深色壁纸逆位、最长的牌名和两行讯息、上浅下深的壁纸。/ Daily tarot: upright on a light
    // wallpaper, reversed on a dark one, the longest name with two-line text, and light-over-dark.
    static const struct { int index; bool reversed; uint8_t top, bottom; const char* name; } T[] = {
        {17, false, 0xD0, 0xD0, "tarot_light"},
        {17, true, 0x30, 0x30, "tarot_dark_reversed"},
        {2, false, 0xF0, 0x90, "tarot_long"},
        {22 + 14 * 3 + 11, true, 0xF0, 0x20, "tarot_split"},
        {22 + 14 + 3, false, 0x80, 0x80, "tarot_mid"},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        hr_card("诡秘之主", "爱潜水的乌贼", true, 42, 82);
        HR_SET_WALL();
        hr_wall_top = T[i].top;
        hr_wall_bottom = T[i].bottom;
        s_tarot_ok = true;
        s_tarot = (tr_tarot_pick_t){T[i].index, T[i].reversed, &CARDS[T[i].index]};
        tr_lock_draw(hr_fb);
        hr_no_bar_check();
        hr_allow_fit();
        hr_shot(T[i].name);
    }
    s_tarot_ok = false;
    hr_wall_top = hr_wall_bottom = 0xD0;
#endif
}
