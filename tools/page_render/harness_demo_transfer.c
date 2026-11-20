/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：传书页的演示图：设备热点开着，手机正在上传一本书。/ English: Demo shot of the transfer page: the hotspot is on
 * and a phone is uploading a book.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_transfer.c"   // 按 -I 找：当前源码 / Found via -I: current sources
#include "page_render.h"
#include "../../managed_components/espressif__qrcode/qrcodegen.h"

const char* hr_page = "demo_transfer";

#define DEMO_SSID "ReadPico-3A7F"

// 二维码：用真实的 qrcodegen 编出 WiFi 码，按固件的缩放规则画。/ QR: encode the real WiFi payload with qrcodegen and
// draw it with the firmware's scale rule.
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    epd_fill_rect(area, 0xff, fb);
    if (!qrcodegen_encodeText("WIFI:T:WPA;S:" DEMO_SSID ";P:" READ_PICO_TRANSFER_PASSWORD ";;", tmp, qr,
                              qrcodegen_Ecc_LOW, 1, 10, qrcodegen_Mask_AUTO, true))
        return;
    const int side = qrcodegen_getSize(qr);
    const int width = area.width < area.height ? area.width : area.height;
    const int scale = width / (side + 8);
    if (scale < 1) return;
    const int x0 = area.x + (area.width - side * scale) / 2;
    const int y0 = area.y + (area.height - side * scale) / 2;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            if (qrcodegen_getModule(qr, x, y))
                epd_fill_rect((EpdRect){x0 + x * scale, y0 + y * scale, scale, scale}, 0, fb);
}

// 热点模式、手机在传一个文件；32 GB 卡的预留是容量的 5%。/ Hotspot mode with a phone uploading one file; a 32 GB
// card reserves 5% of its capacity.
static void home(const char* name, uint64_t done, uint64_t total, unsigned files) {
    memset(&s_status, 0, sizeof(s_status));
    s_view = TRANSFER_HOME;
    s_mode = READ_PICO_TRANSFER_MODE_AP;
    s_start_pending = false;
    s_media_lost = false;
    s_status.mode = s_mode;
    s_status.network_ready = true;
    s_status.state = READ_PICO_TRANSFER_UPLOADING;
    snprintf(s_status.ssid, sizeof(s_status.ssid), DEMO_SSID);
    snprintf(s_status.url, sizeof(s_status.url), "http://192.168.4.1");
    s_status.sta_count = 1;
    s_status.done_count = files;
    snprintf(s_status.cur_name, sizeof(s_status.cur_name), "%s", name);
    s_status.cur_bytes = done;
    s_status.cur_total = total;
    s_status.last_error = ESP_OK;
    s_qr_ready = true;
    s_qr_url = false;
    s_free = 26732ull * 1048576 + 419430;
    s_reserve = 1519ull * 1048576;
    s_root.is_flash = false;
    s_pressed = -1;
}

typedef struct {
    const char* ssid;
    int rssi;
    bool supported, password;
} demo_net_t;

static const demo_net_t NETS[] = {
    {"Home-2.4G", -42, true, true},      {"TP-LINK_5A3C", -55, true, true}, {"ChinaNet-xY7k", -61, true, true},
    {"Cafe_Guest", -67, true, false},    {"Xiaomi_88F2", -72, true, true},  {"Company-Staff", -78, false, true},
    {"HUAWEI-B3C1", -83, true, true},    {"CMCC-9F2D", -88, true, true},
};

static void ui_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    home("三体全集.epub", 3 * 1048576 + 734003, 5 * 1048576 + 943718, 3);
    render(&ctx, hr_fb);
    hr_shot("ui10");

    // 选择 WiFi：已保存一个网络，扫到 8 个，第 1 页。/ Pick WiFi: one saved network, 8 found, page 1.
    home("", 0, 0, 0);
    s_view = TRANSFER_NETWORKS;
    s_forget_confirm = false;
    s_scan_pending = false;
    s_saved_configured = true;
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "Home-2.4G");
    s_network_count = sizeof(NETS) / sizeof(NETS[0]);
    s_network_page = 0;
    s_network_message[0] = 0;
    for (size_t i = 0; i < s_network_count; i++) {
        memset(&s_networks[i], 0, sizeof(s_networks[i]));
        snprintf(s_networks[i].ssid, sizeof(s_networks[i].ssid), "%s", NETS[i].ssid);
        s_networks[i].rssi = NETS[i].rssi;
        s_networks[i].supported = NETS[i].supported;
        s_networks[i].requires_password = NETS[i].password;
    }
    render(&ctx, hr_fb);
    hr_shot("ui11");

    // 接收漫画：网页转好的 CBZ 显示成“系列 / 文件名”。/ Receiving a comic: the converted CBZ shows as "series / name".
    home("海边的少年 / 第03话 防波堤.cbz", 1 * 1048576 + 996147, 3 * 1048576 + 104857, 2);
    render(&ctx, hr_fb);
    hr_shot("ui14");
}

void hr_scenes(void) {
    ui_scenes();
    memset(&s_status, 0, sizeof(s_status));
    s_view = TRANSFER_HOME;
    s_mode = READ_PICO_TRANSFER_MODE_AP;
    s_start_pending = false;
    s_media_lost = false;
    s_status.mode = s_mode;
    s_status.network_ready = true;
    s_status.state = READ_PICO_TRANSFER_UPLOADING;
    snprintf(s_status.ssid, sizeof(s_status.ssid), DEMO_SSID);
    snprintf(s_status.url, sizeof(s_status.url), "http://192.168.4.1");
    s_status.sta_count = 1;
    s_status.done_count = 3;
    snprintf(s_status.cur_name, sizeof(s_status.cur_name), "三体全集.epub");
    s_status.cur_bytes = 3 * 1048576 + 734003;
    s_status.cur_total = 5 * 1048576 + 943718;
    s_status.last_error = ESP_OK;
    s_qr_ready = true;
    s_qr_url = false;
    s_free = 21468ull * 1048576 + 314572;
    s_reserve = 64ull * 1048576;
    s_root.is_flash = false;
    s_pressed = -1;
    app_ctx_t ctx = {.fb = hr_fb};
    render(&ctx, hr_fb);
    hr_shot("home");
}
