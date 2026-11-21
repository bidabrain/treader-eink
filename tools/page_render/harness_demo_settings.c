/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：设置页的演示截图（README / 网站预览用）：首页、底部标签、电源与设备，数据像一台真在用的设备。
 * English: Demo shots of Settings for the README / site previews: home, tab bar, power and device, with data that
 * looks like a device in real use.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_settings.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "nvs.h"
#include "esp_random.h"
#include "psa/crypto.h"

const char* hr_page = "demo_settings";

// 组件替身：返回演示数据。/ Component stand-ins returning demo data.
bool tr_lock_wallpaper_get(char* out, size_t cap) {
    snprintf(out, cap, "/sdcard/wallpapers/晨雾.jpg");
    return true;
}
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->present = info->mounted = true;
    info->capacity_bytes = 62537072640ull;
    info->free_bytes = 44238163558ull;   // 41.2 GB
    return ESP_OK;
}
esp_err_t tomato_store_url_load(char* out, size_t cap) { (void)out; (void)cap; return ESP_FAIL; }
void tomato_sync_get_status(tomato_status_t* out) { memset(out, 0, sizeof(*out)); }
int tomato_red_list_limit(void) { return 200; }
int tomato_wb_list_limit(void) { return 100; }
int tomato_x_list_limit(void) { return 300; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
bool tomato_config_has_builtin(void) { return false; }
bool tr_time_valid(void) { return true; }
static const esp_app_desc_t s_desc = {.version = "0.9.0", .date = "Nov 21 2026", .time = "15:43:00"};
const esp_app_desc_t* esp_app_get_description(void) { return &s_desc; }
// 演示二维码：三个定位角加伪随机的模块，看起来像一张真的二维码。/ Demo QR code: three finder squares plus
// pseudo-random modules, so it reads like a real code.
// 验证器：已设置、已上锁。/ Authenticator: set up and locked.
#ifndef HR_BASELINE   // 改动前的代码还没有验证器 / The pre-change code has no authenticator
tr_auth_state_t tr_auth_state(void) { return TR_AUTH_LOCKED; }
int tr_auth_count(void) { return -1; }
#endif
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    enum { N = 29 };
    const int m = area.width / (N + 4), ox = area.x + (area.width - N * m) / 2, oy = area.y + (area.height - N * m) / 2;
    epd_fill_rect(area, 0xFF, fb);
    uint32_t seed = 2463534242u;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            bool on = seed & 1;
            const int fr = r < 8 ? 0 : r >= N - 8 ? N - 7 : -1, fc = c < 8 ? 0 : c >= N - 8 ? N - 7 : -1;
            if (fr >= 0 && fc >= 0 && !(fr && fc)) {
                const int dr = r - fr, dc = c - fc;
                if (dr == 7 || dc == 7 || dr < 0 || dc < 0) on = false;   // 定位角外的白边 / Quiet ring
                else {
                    int d = dr < dc ? dr : dc;
                    if (6 - dr < d) d = 6 - dr;
                    if (6 - dc < d) d = 6 - dc;
                    on = d != 1;
                }
            }
            if (r == 6 && c >= 8 && c < N - 8) on = c % 2 == 0;   // 定时线 / Timing pattern
            if (c == 6 && r >= 8 && r < N - 8) on = r % 2 == 0;
            if (on) epd_fill_rect((EpdRect){ox + c * m, oy + r * m, m, m}, 0x00, fb);
        }
}

// 设置模块的存储替身：写入都算成功，好让锁屏密码显示“已开启”。/ Storage stand-ins for the settings module: writes
// succeed so the lock PIN shows as on.
esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char* k, const void* v, size_t n) {
    (void)h; (void)k; (void)v; (void)n;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* k) { (void)h; (void)k; return ESP_OK; }
void esp_fill_random(void* buf, size_t len) { memset(buf, 0x5A, len); }
psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
psa_status_t psa_hash_compute(int alg, const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* len) {
    (void)alg; (void)in; (void)n;
    memset(out, 0xA5, cap);
    *len = cap;
    return PSA_SUCCESS;
}

static app_ctx_t s_ctx;

static void page(page_t p, const char* name) {
    s_edit = E_NONE;
    s_page = p;
    render(&s_ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    s_ctx.fb = hr_fb;
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_wifi, sizeof(s_wifi), "Home-5G");
    s_has_cfg = true;
    snprintf(s_cfg.url, sizeof(s_cfg.url), "http://192.168.1.10:18423");
    s_has_dav = true;
    snprintf(s_dav.url, sizeof(s_dav.url), "https://dav.jianguoyun.com/dav/treader");
    s_has_host[S_RED] = s_has_host[S_WB] = true;
    snprintf(s_host[S_RED].url, sizeof(s_host[S_RED].url), "http://192.168.1.10:8091");
    snprintf(s_host[S_WB].url, sizeof(s_host[S_WB].url), "http://192.168.1.10:8092");
    s_message[0] = 0;
    // 默认排法：书库固定，漫画 番茄 小红书 虎扑 放底部，微博和设置在“全部”，X 隐藏。/ Default arrangement: library
    // fixed; comics, Tomato, Xiaohongshu, Hupu pinned; Weibo and Settings on All; X hidden.
    static const uint8_t pins[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU};
    app_settings_set_tab_pins(pins, 4);
    app_settings_set_hidden_tabs(1u << TR_TAB_X);
    tr_tabs_configure(app_settings_hidden_tabs(), pins, 4);
    app_settings_set_lock_idle(1);   // 2 分钟 / 2 minutes
    app_settings_pin_set("2580");

    hr_allow_fit();
    page(P_HOME, "home");
    page(P_TABS, "tabs");
    hr_allow_fit();
    page(P_POWER, "power");

    // 设计文档 ui.html 的图：编号对应 docs/local/ui-figures.json。/ Figures for the design doc ui.html; numbers follow
    // docs/local/ui-figures.json.
    hr_allow_fit();
    page(P_TRANSFER, "ui17");
    hr_allow_fit();
    page(P_HOME, "ui49");
    page(P_TABS, "ui50");
    hr_allow_fit();
    page(P_SOURCES, "ui51");
    // 微博来源页：已填地址和令牌，刚测试过。/ Weibo source page: address and token filled, just tested.
    snprintf(s_host[S_WB].token, sizeof(s_host[S_WB].token), "wb-7f3a9c21e5d84b06");
    snprintf(s_test_result[S_WB], sizeof(s_test_result[0]), "正常 · 关注 86 条");
    s_srv = S_WB;
    hr_allow_fit();
    page(P_SRC_SERVER, "ui52");
    // 手机网页填写打开后（小红书页，连着家里的 Wi-Fi）。/ Web form switched on (Xiaohongshu page, on home Wi-Fi).
    s_srv = S_RED;
    s_web_on = true;
    memset(&s_web_status, 0, sizeof(s_web_status));
    s_web_status.network_ready = true;
    s_web_status.mode = READ_PICO_TRANSFER_MODE_STA;
    snprintf(s_web_status.wifi_ssid, sizeof(s_web_status.wifi_ssid), "Home-5G");
    snprintf(s_web_status.url, sizeof(s_web_status.url), "http://192.168.1.37");
    page(P_SRC_SERVER, "ui53");
    s_web_on = false;
    hr_no_bar_check();
    page(P_ABOUT, "ui55");
    hr_allow_fit();
    page(P_POWER, "ui62");
    // 时间与时区：画关掉自动校时后的样子。/ Time and zone, drawn with automatic time switched off.
    app_settings_set_time_auto(false);
    page(P_TIME, "ui66");
    tset_begin();
    page(P_TSET, "ui67");
    app_settings_set_time_auto(true);

    // 文档新加的图。/ New figures for the doc.
    hr_allow_fit();
    page(P_DISPLAY, "new_display");
    app_settings_set_display_size(2);   // 特大 / Largest
    ui_set_scale(130);
    hr_begin();
    hr_allow_fit();
    page(P_HOME, "new_display_large");
    app_settings_set_display_size(0);
    ui_set_scale(100);
    hr_begin();
}
