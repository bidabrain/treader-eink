/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：设置页（各个二级页、键盘编辑）的渲染测试。/ English: Render bench for Settings (every sub-page and editor).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_settings.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "nvs.h"

const char* hr_page = "settings";

static bool s_more_stub;   // “往后翻接着拉”开关 / The keep-pulling switch
bool tomato_feed_more_on(void) { return s_more_stub; }
esp_err_t tomato_feed_more_set(bool on) { s_more_stub = on; return ESP_OK; }

// 组件替身：返回固定的假数据。/ Component stand-ins returning fixed mock data.
static bool s_mock_wall = true, s_mock_time = true, s_mock_busy, s_mock_builtin;
bool tr_lock_wallpaper_get(char* out, size_t cap) {
    if (!s_mock_wall) return false;
    snprintf(out, cap, "/sdcard/wallpapers/sunset-over-the-lake-2026.jpg");
    return true;
}
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->present = info->mounted = true;
    info->capacity_bytes = 31914983424ull;
    info->free_bytes = 12884901888ull;
    return ESP_OK;
}
esp_err_t tomato_store_url_load(char* out, size_t cap) { snprintf(out, cap, "https://rank.example.org"); return ESP_OK; }
void tomato_sync_get_status(tomato_status_t* out) { memset(out, 0, sizeof(*out)); }
int tomato_red_list_limit(void) { return 50; }
int tomato_wb_list_limit(void) { return 30; }
int tomato_x_list_limit(void) { return 20; }
int tomato_wt_lang(void) { return 0; }
const char* tomato_wt_lang_name(int lang) { (void)lang; return "繁体中文"; }
bool tomato_wt_simp(void) { return true; }
int tomato_wt_max(void) { return 10; }
bool tomato_sync_busy(void) { return s_mock_busy; }
bool tomato_sync_preemptible(void) { return false; }
bool tomato_config_has_builtin(void) { return s_mock_builtin; }
bool tr_time_valid(void) { return s_mock_time; }
static const esp_app_desc_t s_desc = {.version = "1.42.0", .date = "Nov 21 2026", .time = "15:43:00"};
const esp_app_desc_t* esp_app_get_description(void) { return &s_desc; }
// 验证器：已设置、已上锁。/ Authenticator: set up and locked.
tr_auth_state_t tr_auth_state(void) { return TR_AUTH_LOCKED; }
int tr_auth_count(void) { return -1; }
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    epd_fill_rect(area, 0x00, fb);
    epd_fill_rect((EpdRect){area.x + 12, area.y + 12, area.width - 24, area.height - 24}, 0xFF, fb);
}
#ifndef HR_BASELINE   // 改动前没有代理设置 / No proxy settings before this change
esp_err_t tomato_proxy_load(tomato_proxy_t* out) { memset(out, 0, sizeof(*out)); return ESP_OK; }
esp_err_t tomato_proxy_save(const tomato_proxy_t* cfg) { (void)cfg; return ESP_OK; }
bool tomato_proxy_parse(const char* text, tomato_proxy_t* cfg) { (void)text; (void)cfg; return true; }
#endif
esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }


static app_ctx_t s_ctx;

static void shot(const char* name) {
    render(&s_ctx, hr_fb);
    hr_shot(name);
}

static void page(page_t p, const char* name) {
    s_edit = E_NONE;
    s_page = p;
    shot(name);
}

static void edit(edit_t e, const char* text, const char* name) {
    tr_kbd_open(&s_kbd, text, false, EDIT[e].go);
    s_edit = e;
    shot(name);
    s_edit = E_NONE;
}

static void set_pins(const uint8_t* pins, int n, uint16_t hidden) {
    app_settings_set_tab_pins(pins, n);
    app_settings_set_hidden_tabs(hidden);
    tr_tabs_configure(app_settings_hidden_tabs(), pins, n);
}

void hr_scenes(void) {
    s_ctx.fb = hr_fb;
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_wifi, sizeof(s_wifi), "HomeNetwork-5G");
    s_has_cfg = true;
    snprintf(s_cfg.url, sizeof(s_cfg.url), "http://192.168.1.10:18423");
    s_has_dav = true;
    snprintf(s_dav.url, sizeof(s_dav.url), "https://dav.jianguoyun.com/dav/treader");
    s_has_host[S_RED] = true;
    snprintf(s_host[S_RED].url, sizeof(s_host[S_RED].url), "http://192.168.1.20:8091");
    snprintf(s_host[S_RED].token, sizeof(s_host[S_RED].token), "abc");
    s_message[0] = 0;
    static const uint8_t pins4[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_HUPU};
    set_pins(pins4, 4, 1u << TR_TAB_X);

    hr_allow_fit();
    page(P_HOME, "home");
    snprintf(s_message, sizeof(s_message), "已保存");
    hr_allow_fit();
    page(P_HOME, "home_message");
    s_message[0] = 0;
#ifndef HR_BASELINE
    // 状态栏右上角的 Wi-Fi 名：普通的和 32 字节长的。/ The Wi-Fi name at the status bar's top right: a normal one and a
    // 32-byte one.
    s_wifi_count = 3;
    snprintf(hr_wifi_name, sizeof(hr_wifi_name), "HomeNetwork-5G");
    hr_allow_fit();
    page(P_HOME, "home_wifi_name");
    snprintf(hr_wifi_name, sizeof(hr_wifi_name), "家庭网络-长名字ABCDEFGHIJ");
    snprintf(s_wifi, sizeof(s_wifi), "%s", hr_wifi_name);
    hr_allow_fit();
    page(P_HOME, "home_wifi_long");
    hr_wifi_name[0] = 0;
    snprintf(s_wifi, sizeof(s_wifi), "HomeNetwork-5G");
    // 代理：关；开着的 SOCKS5；带用户名的 HTTP；填地址的键盘；没填地址就想开。/ Proxy: off; SOCKS5 on; HTTP with a user
    // name; the address keyboard; trying to switch on without an address.
    hr_allow_fit();
    page(P_PROXY, "proxy_off");
    snprintf(s_px.host, sizeof(s_px.host), "192.168.1.5");
    s_px.port = 7890;
    s_px.on = true;
    hr_allow_fit();
    page(P_HOME, "home_proxy_on");
    hr_allow_fit();
    page(P_PROXY, "proxy_socks5");
    s_px.type = TOMATO_PROXY_HTTP;
    snprintf(s_px.user, sizeof(s_px.user), "alice");
    snprintf(s_px.pass, sizeof(s_px.pass), "secret");
    hr_allow_fit();
    page(P_PROXY, "proxy_http_auth");
    edit(E_PX_ADDR, "192.168.1.5:7890", "proxy_edit_addr");
    edit(E_PX_PASS, "", "proxy_edit_pass");
    memset(&s_px, 0, sizeof(s_px));
    snprintf(s_message, sizeof(s_message), "先填代理地址");
    hr_allow_fit();
    page(P_PROXY, "proxy_need_addr");
    s_message[0] = 0;
#endif
    hr_allow_fit();
    page(P_DISPLAY, "display");
#ifndef HR_BASELINE   // 改动前没有翻页区域 / No turn-zone setting before this change
    page(P_TURN, "turn");
    app_settings_set_turn_zone(APP_TURN_LOWER);
    page(P_TURN, "turn_lower");
    page(P_DISPLAY, "display_turn_lower");
#ifndef HR_BASELINE   // 改动前没有文字深浅 / No text darkness before this change
    // 最深一档：设置首页和显示页。/ The darkest level: the settings home and the display page.
    tr_text_depth_apply(2);
    page(P_DISPLAY, "display_darker");
    hr_allow_fit();
    page(P_HOME, "home_darker");
    tr_text_depth_apply(0);
#endif
    app_settings_set_turn_zone(APP_TURN_SIDES);
#endif
    page(P_TABS, "tabs");
    // 最坏：八行三段，常用超出这一档。/ Worst case: eight rows in three sections, pins beyond this size's room.
    static const uint8_t pins3[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS};
    set_pins(pins3, 3, (1u << TR_TAB_X) | (1u << TR_TAB_HUPU));
    snprintf(s_message, sizeof(s_message), "底部满了，先移出一个");
    page(P_TABS, "tabs_worst");
    s_message[0] = 0;
    set_pins(pins4, 4, 0);
    page(P_TABS, "tabs_none_hidden");
    set_pins(NULL, 0, 0);
    page(P_TABS, "tabs_empty");
    set_pins(pins4, 4, 1u << TR_TAB_X);
    hr_allow_fit();
    page(P_TRANSFER, "transfer");
    s_mock_busy = true;
    hr_allow_fit();
    page(P_TRANSFER, "transfer_busy");
    s_mock_busy = false;
    hr_allow_fit();
    page(P_SOURCES, "sources");
    page(P_SRC_HUPU, "hupu");
    s_hupu_ask = true;
    page(P_SRC_HUPU, "hupu_ask");
    s_hupu_ask = false;
    page(P_SRC_TOMATO, "src_tomato");
    s_srv = S_RED;
    page(P_SRC_SERVER, "server");
    snprintf(s_test_result[S_RED], sizeof(s_test_result[0]), "连接正常，服务器版本 0.9.3，已登录");
    hr_allow_fit();
    page(P_SRC_SERVER, "server_tested");
    s_test_result[S_RED][0] = 0;
    s_srv = S_X;
    page(P_SRC_SERVER, "server_x_empty");
#ifndef HR_BASELINE   // 改动前没有漫画服务器 / No comic server before this change
    s_srv = S_WT;
    page(P_SRC_SERVER, "server_comic");
#endif
    s_srv = S_RED;
    s_web_on = true;
    memset(&s_web_status, 0, sizeof(s_web_status));
    page(P_SRC_SERVER, "web_starting");
    s_web_status.network_ready = true;
    s_web_status.mode = READ_PICO_TRANSFER_MODE_STA;
    snprintf(s_web_status.wifi_ssid, sizeof(s_web_status.wifi_ssid), "HomeNetwork-5G");
    snprintf(s_web_status.url, sizeof(s_web_status.url), "http://192.168.1.123");
    page(P_SRC_SERVER, "web_sta");
    s_web_status.mode = READ_PICO_TRANSFER_MODE_AP;
    snprintf(s_web_status.ssid, sizeof(s_web_status.ssid), "Treader-1A2B");
    snprintf(s_web_note, sizeof(s_web_note), "收到地址，正在保存…");
    page(P_SRC_SERVER, "web_ap");
    s_web_note[0] = 0;
#ifndef HR_BASELINE   // 改动前番茄没有网页填写 / No Tomato web form before this change
    s_web_status.mode = READ_PICO_TRANSFER_MODE_STA;
    s_web_tomato = true;
    page(P_SRC_TOMATO, "web_tomato");
    s_web_tomato = false;
#endif
    // WebDAV 的网页填写在同步页。/ The WebDAV web form lives on the sync page.
    s_web_status.mode = READ_PICO_TRANSFER_MODE_STA;
    s_web_dav = true;
    page(P_SYNC, "web_dav");
    s_web_dav = false;
    s_web_on = false;
    hr_allow_fit();
    page(P_SYNC, "sync");
    s_syncing = true;
    hr_allow_fit();
    page(P_SYNC, "sync_running");
    s_syncing = false;
    hr_allow_fit();
    page(P_POWER, "power");
    hr_no_bar_check();
    page(P_ABOUT, "about");
#ifndef HR_BASELINE
    // 放大后的第二页（开源致谢、后面的时区）。/ Second pages at larger sizes (credits, later zones).
    if (about_paged()) {
        s_about_page = 1;
        hr_no_bar_check();
        page(P_ABOUT, "about_credits");
        s_about_page = 0;
    }
#endif
    page(P_TIME, "time");
    page(P_TZ, "tz");
#ifndef HR_BASELINE
    if (tz_pages() > 1) {
        s_tz_page = 1;
        page(P_TZ, "tz_page2");
        s_tz_page = 0;
    }
#endif
    tset_begin();
    page(P_TSET, "tset");
    s_page = P_SRC_SERVER;
    edit(E_HOST_URL, "http://192.168.1.20:8091", "edit_host_url");
    edit(E_DAV_PASS, "secret-password", "edit_dav_pass");
    snprintf(s_message, sizeof(s_message), "地址要以 http:// 或 https:// 开头");
    edit(E_HOST_URL, "192.168.1.20", "edit_message");
    s_message[0] = 0;
    edit(E_STORE_URL, "", "edit_store");
}
