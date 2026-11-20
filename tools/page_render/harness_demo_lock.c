/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏的演示图（README / 网站预览和设计文档用）：书卡和壁纸两种样式。
 * English: Demo shots of the lock screen (README / site previews and the design doc): book card and wallpaper styles.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include <math.h>

#include "tr_lock.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_lock";

// 演示壁纸：一张灰阶的山湖风景（天空渐变、太阳、两层山、湖面倒影）。
// Demo wallpaper: a grayscale mountain lake (sky gradient, sun, two ridges, a lake with reflections).
void book_screen_draw(uint8_t* fb, const uint8_t* img) {
    (void)img;
    // 4×4 有序抖动，免得 16 级灰的渐变出现色带。/ 4x4 ordered dither so 16-level gradients do not band.
    static const int BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int lake = TR_H * 60 / 100;
    for (int y = 0; y < TR_H; y++)
        for (int x = 0; x < TR_W; x++) {
            const int sy = y < lake ? y : 2 * lake - y;   // 湖面照出上方的景 / The lake mirrors what is above it
            const double far = TR_H * 0.43 + 40 * sin(x * 0.011 + 0.6) + 18 * sin(x * 0.031 + 1.7);
            const double near = TR_H * 0.51 + 46 * sin(x * 0.007 + 2.4) + 14 * sin(x * 0.043);
            const double dx = x - TR_W * 0.68, dy = sy - TR_H * 0.27;
            const double r2 = dx * dx + dy * dy;
            double v;
            if (sy >= near) v = 58 + (sy - near) * 0.25;
            else if (sy >= far) v = 112 + (sy - far) * 0.35;
            else if (r2 < 56.0 * 56.0) v = 250;
            else {
                v = 236 - (lake - sy) * 0.13;                  // 天空上暗下亮 / Sky darker above, lighter below
                const double glow = 160.0 * 160.0 - r2;        // 太阳周围一圈光晕 / A halo around the sun
                if (glow > 0) v += glow / (160.0 * 160.0) * 30;
            }
            if (y >= lake) {
                // 水面压暗一点，加几道横向波纹。/ Darken the water a little and add some horizontal ripples.
                v = v * 0.80;
                if ((y - lake) % 19 < 2 && (int)(x + 40 * sin(y * 0.3)) % 140 < 90) v += 22;
            }
            int q = (int)v + (BAYER[y & 3][x & 3] - 8);
            if (q < 0) q = 0;
            if (q > 245) q = 245;
            epd_draw_pixel(x, y, (uint8_t)q, fb);
        }
}

static void hr_card(void) {
    memset(&s_card, 0, sizeof(s_card));
    s_card.ok = true;
    s_card.has_progress = true;
    s_card.pct = 41;
    s_card.battery = 82;
    snprintf(s_card.title, sizeof(s_card.title), "宿命之环");
    snprintf(s_card.author, sizeof(s_card.author), "爱潜水的乌贼");
    snprintf(s_card.format, sizeof(s_card.format), "番茄");
    snprintf(s_card.path, sizeof(s_card.path), "/sdcard/books/宿命之环.txt");
}

void hr_scenes(void) {
    hr_card();
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("card");

    // 设计文档：书卡（没设壁纸）和壁纸两种锁屏。/ Design doc: the book card (no wallpaper) and wallpaper lock screens.
    hr_card();
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("ui4");
    hr_card();
    s_wall = malloc(1);   // 有缓存就走壁纸样式，画完由页面释放 / A cache selects the wallpaper style; the page frees it
    tr_lock_draw(hr_fb);
    hr_no_bar_check();
    hr_shot("ui5");
}
