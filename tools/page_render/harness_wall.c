/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：锁屏壁纸页和看图“存为壁纸”工具条的渲染测试。/ English: Render bench for the wallpaper page and the image-view
 * "save as wallpaper" toolbar.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_wall.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "tr_wallsave.h"

const char* hr_page = "wall";

void book_screen_draw(uint8_t* fb, const uint8_t* packed) {
    (void)packed;
    epd_fill_rect((EpdRect){0, 0, TR_W, TR_H}, 0x90, fb);
}

void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    epd_fill_rect(area, 0x00, fb);
    epd_fill_rect((EpdRect){area.x + 12, area.y + 12, area.width - 24, area.height - 24}, 0xFF, fb);
}

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    static const char* const NAMES[] = {
        "a-very-long-wallpaper-file-name-from-the-camera-roll.jpg", "beach.png", "city-night.jpg", "forest.jpg",
        "mountain.png", "rain.jpg", "snow.jpg", "sunset-over-the-lake-2026.jpg",
    };
    s_count = 8;
    for (int i = 0; i < s_count; i++) {
        snprintf(s_names[i], NAME_LEN, "%s", NAMES[i]);
        s_sizes[i] = 1234567;
    }
    s_no_card = false;
    s_view = V_LIST;
    s_page = 0;
    snprintf(s_current, sizeof(s_current), WALL_DIR "/beach.png");
    snprintf(s_msg, sizeof(s_msg), "这张图打不开：太大（超过 2 MB）或格式不支持");
    hr_allow_fit();
    shot("list");
    s_msg[0] = 0;
    s_page = 1;
    hr_allow_fit();
    shot("list_page2");
    s_current[0] = 0;
    s_page = 0;
    hr_allow_fit();
    shot("list_no_current");
    s_count = 0;
    shot("empty");
    s_no_card = true;
    shot("no_card");
    s_no_card = false;
    // 手机上传：打开中、已有 Wi-Fi、热点、接收中。/ Upload from phone: starting, saved Wi-Fi, hotspot, receiving.
    s_view = V_UPLOAD;
    memset(&s_up, 0, sizeof(s_up));
    shot("upload_starting");
    s_up.network_ready = true;
    s_up.mode = READ_PICO_TRANSFER_MODE_STA;
    snprintf(s_up.wifi_ssid, sizeof(s_up.wifi_ssid), "HomeNetwork-5G");
    snprintf(s_up.url, sizeof(s_up.url), "http://192.168.1.123");
    shot("upload_sta");
    s_up.mode = READ_PICO_TRANSFER_MODE_AP;
    snprintf(s_up.ssid, sizeof(s_up.ssid), "ReadPico-1A2B");
    s_up.done_count = 3;
    shot("upload_ap");
    s_up.state = READ_PICO_TRANSFER_UPLOADING;
    snprintf(s_up.cur_name, sizeof(s_up.cur_name), "a-very-long-wallpaper-file-name-from-the-camera-roll.jpg");
    hr_allow_fit();
    shot("upload_receiving");
    s_view = V_LIST;
    static uint8_t img[4];
    s_preview = img;
    s_view = V_PREVIEW;
    s_confirm_del = false;
    shot("preview");
    s_confirm_del = true;
    shot("preview_confirm");
    s_preview = NULL;
    s_view = V_LIST;
    // 看图页的“存为壁纸”工具条和存好后的提示框。/ The image view's save toolbar and the saved notice.
    tr_wallsave_t w = {0};
    tr_wallsave_open(&w);
    hr_no_bar_check();
    book_screen_draw(hr_fb, NULL);
    tr_wallsave_draw(hr_fb, &w);
    hr_shot("wallsave_bar");
    w.shown = false;
    snprintf(w.title, sizeof(w.title), "已经存过，已设为锁屏");
    snprintf(w.line, sizeof(w.line), "weibo-someone-with-a-long-name-3.jpg\n到 设置 › 显示与阅读 › 锁屏壁纸 里选用");
    hr_no_bar_check();
    book_screen_draw(hr_fb, NULL);
    tr_wallsave_draw(hr_fb, &w);
    hr_shot("wallsave_notice");
}
