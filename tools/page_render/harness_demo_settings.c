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
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) { epd_fill_rect(area, 0xFF, fb); }

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
}
