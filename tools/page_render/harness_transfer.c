/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：传书页的渲染测试。/ English: Render bench for the Wi-Fi transfer page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_transfer.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "transfer";

// 二维码替身：按真实绘制的缩放规则画一个 33 模块的方块图案。/ QR stand-in: a 33-module pattern using the real
// drawing's scale rule.
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    epd_fill_rect(area, 0xff, fb);
    const int side = 33;
    int width = area.width < area.height ? area.width : area.height;
    int scale = width / (side + 8);
    if (scale < 1) return;
    int x0 = area.x + (area.width - side * scale) / 2;
    int y0 = area.y + (area.height - side * scale) / 2;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            if ((x * 7 + y * 13) % 3 == 0 || x < 7 && y < 7 || x >= side - 7 && y < 7 || x < 7 && y >= side - 7)
                epd_fill_rect((EpdRect){x0 + x * scale, y0 + y * scale, scale, scale}, 0, fb);
}

static void home_ap(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_view = TRANSFER_HOME;
    s_mode = READ_PICO_TRANSFER_MODE_AP;
    s_start_pending = false;
    s_media_lost = false;
    s_status.mode = s_mode;
    s_status.network_ready = true;
    s_status.state = READ_PICO_TRANSFER_UPLOADING;
    snprintf(s_status.ssid, sizeof(s_status.ssid), "ReadPico-1A2B");
    snprintf(s_status.url, sizeof(s_status.url), "http://192.168.4.1");
    s_status.sta_count = 2;
    s_status.done_count = 12;
    snprintf(s_status.cur_name, sizeof(s_status.cur_name), "一本名字特别特别长的小说（全本精校版）第一部到第三部合集.epub");
    s_status.cur_bytes = 3 * 1048576 + 400000;
    s_status.cur_total = 9 * 1048576;
    s_status.last_error = ESP_FAIL;
    s_qr_ready = true;
    s_qr_url = false;
    s_free = 12345ull * 1048576;
    s_reserve = 64ull * 1048576;
    s_root.is_flash = false;
    s_pressed = -1;
}

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    render(&ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    home_ap();
    hr_allow_fit();
    shot("home_ap_worst");
    home_ap();
    s_qr_url = true;
    s_status.state = READ_PICO_TRANSFER_READY;
    s_status.last_error = ESP_OK;
    s_status.cur_name[0] = 0;
    s_pressed = 3;
    shot("home_ap_url_pressed");
    home_ap();
    s_mode = READ_PICO_TRANSFER_MODE_STA;
    s_status.mode = s_mode;
    snprintf(s_status.wifi_ssid, sizeof(s_status.wifi_ssid), "MyHomeNetwork_5G_Extended");
    snprintf(s_status.url, sizeof(s_status.url), "http://192.168.100.200");
    s_status.state = READ_PICO_TRANSFER_ERROR;
    hr_allow_fit();
    shot("home_sta_error");
    home_ap();
    s_mode = READ_PICO_TRANSFER_MODE_STA;
    s_status.mode = s_mode;
    s_status.network_ready = false;
    s_status.url[0] = 0;
    s_start_pending = true;
    s_status.last_error = ESP_OK;
    s_status.cur_name[0] = 0;
    s_qr_ready = false;
    shot("home_sta_connecting");
    home_ap();
    s_media_lost = true;
    s_status.network_ready = false;
    s_status.state = READ_PICO_TRANSFER_STOPPED;
    s_status.cur_name[0] = 0;
    s_qr_ready = false;
    shot("home_media_lost");

    // 选网页：已保存网络、8 个网络、长名字。/ Network list: saved network, 8 networks, long names.
    s_view = TRANSFER_NETWORKS;
    s_forget_confirm = false;
    s_scan_pending = false;
    s_saved_configured = true;
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "MyHomeNetwork_5G_Extended_X");
    s_network_count = 8;
    s_network_page = 0;
    s_network_message[0] = 0;
    for (int i = 0; i < 8; i++) {
        snprintf(s_networks[i].ssid, sizeof(s_networks[i].ssid), i == 0 ? "A-Very-Long-Network-Name-For-Test" : "Net-%d", i);
        s_networks[i].rssi = -40 - i * 5;
        s_networks[i].supported = i != 2;
        s_networks[i].requires_password = i % 2 == 0;
    }
    s_pressed = 1;
    hr_allow_fit();
    shot("networks_saved");
    s_pressed = -1;
    s_network_page = 1;
    hr_allow_fit();
    shot("networks_page2");
    s_saved_configured = false;
    s_scan_pending = true;
    s_network_count = 0;
    s_network_page = 0;
    shot("networks_scanning");
    s_scan_pending = false;
    s_saved_configured = true;
    s_forget_confirm = true;
    snprintf(s_network_message, sizeof(s_network_message), "遗忘失败，请重试或取消");
    hr_allow_fit();
    shot("networks_forget");
    s_forget_confirm = false;

    s_view = TRANSFER_PASSWORD;
    s_selected = s_networks[0];
    snprintf(s_password, sizeof(s_password), "correct-horse-battery-staple-1234567890");
    s_password_visible = true;
    s_keyboard_mode = 0;
    snprintf(s_network_message, sizeof(s_network_message), "密码须 8–63 字符或 64 位十六进制");
    s_pressed = 42;
    hr_allow_fit();
    shot("password");
    s_keyboard_mode = 2;
    s_password_visible = false;
    s_network_message[0] = 0;
    s_pressed = -1;
    hr_allow_fit();
    shot("password_symbols");
}
