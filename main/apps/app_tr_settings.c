/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 设置：分组清单，入口指向官方的字体、睡眠、传书（含 Wi-Fi）、概览页；番茄服务器地址和密码在本页用键盘编辑。
 * Settings: a grouped list linking to the official font, sleep, transfer (incl. Wi-Fi) and overview pages;
 * the Tomato server address and password are edited here with the keyboard.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "read_pico_transfer.h"
#include "tomato.h"
#include "tr_kbd.h"
#include "tr_nav.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define ROW_H 88

extern const app_desc_t app_font_pick;
extern const app_desc_t app_sleep;
extern const app_desc_t app_transfer;
extern const app_desc_t app_home;

typedef enum { V_LIST = 0, V_EDIT_URL, V_EDIT_PASS } view_t;

typedef enum { R_FONT, R_SLEEP, R_WIFI, R_SERVER, R_WEBDAV, R_IMPORT, R_ABOUT, R_COUNT } row_id_t;

typedef struct {
    const char* section;   // 非空时在这一行上方画分组标题 / Section title above this row when set
    const char* label;
    int y;
} row_t;

static const row_t ROWS[R_COUNT] = {
    [R_FONT] = {"阅读", "字体", 226},
    [R_SLEEP] = {"电源", "睡眠与锁屏", 372},
    [R_WIFI] = {"连接", "Wi-Fi", 518},
    [R_SERVER] = {NULL, "番茄服务器", 606},
    [R_WEBDAV] = {NULL, "WebDAV 同步", 694},
    [R_IMPORT] = {NULL, "导入书籍", 782},
    [R_ABOUT] = {"关于", "关于本机", 928},
};

static view_t s_view;
static char s_wifi[33];
static tomato_config_t s_cfg;
static bool s_has_cfg;
static char s_message[48];
static tr_kbd_t s_kbd;
static char s_new_url[sizeof(s_cfg.url)];

static void load(void) {
    bool configured = false;
    if (read_pico_transfer_get_saved_wifi(s_wifi, &configured) != ESP_OK || !configured) s_wifi[0] = 0;
    s_has_cfg = tomato_config_load(&s_cfg) == ESP_OK;
}

// 去掉 http:// 只显示主机。/ Drop the scheme and show the host only.
static const char* host_of(const char* url) {
    const char* p = strstr(url, "://");
    return p ? p + 3 : url;
}

static void row_value(row_id_t id, char* out, size_t cap) {
    switch (id) {
    case R_FONT: snprintf(out, cap, "%s ›", ttf_font_is_builtin() ? "内置字体" : ttf_font_display_name()); break;
    case R_SLEEP: snprintf(out, cap, "›"); break;
    case R_WIFI: snprintf(out, cap, "%s ›", s_wifi[0] ? s_wifi : "未设置"); break;
    case R_SERVER: snprintf(out, cap, "%s ›", s_has_cfg ? host_of(s_cfg.url) : "未设置"); break;
    case R_WEBDAV: snprintf(out, cap, "即将加入"); break;
    case R_IMPORT: snprintf(out, cap, tomato_sync_busy() ? "同步中，稍后再试" : "浏览器上传 ›"); break;
    case R_ABOUT: snprintf(out, cap, "电量、存储、版本 ›"); break;
    default: out[0] = 0; break;
    }
}

static void draw_list(uint8_t* fb) {
    tr_title(fb, "设置");
    if (s_message[0]) tr_text(fb, TR_W - TR_MARGIN, 100, 28, s_message, EPD_DRAW_ALIGN_RIGHT, false);
    for (int i = 0; i < R_COUNT; i++) {
        const row_t* r = &ROWS[i];
        if (r->section) tr_text(fb, TR_MARGIN, r->y - 42, 28, r->section, EPD_DRAW_ALIGN_LEFT, false);
        tr_text(fb, TR_MARGIN, r->y + 24, 32, r->label, EPD_DRAW_ALIGN_LEFT, false);
        char value[160];
        row_value((row_id_t)i, value, sizeof(value));
        tr_fit(value, 28, TR_W - 2 * TR_MARGIN - 220);
        tr_text(fb, TR_W - TR_MARGIN, r->y + 28, 28, value, EPD_DRAW_ALIGN_RIGHT, false);
        bool last_in_group = i + 1 == R_COUNT || ROWS[i + 1].section;
        if (!last_in_group) tr_hline(fb, r->y + ROW_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_tab_bar(fb, TR_TAB_SETTINGS);
}

static void draw_edit(uint8_t* fb) {
    bool url = s_view == V_EDIT_URL;
    tr_text(fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 96, 100, 36, url ? "番茄服务器地址" : "访问密码", EPD_DRAW_ALIGN_LEFT, true);
    EpdRect f = {TR_MARGIN, 176, TR_W - 2 * TR_MARGIN, 84};
    ui_draw_round_rect(fb, f, 14, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(f, 1), 13, UI_GRAY_BLACK);
    const char* tail = s_kbd.text;
    while (*tail && ttf_text_width_px(32, tail) > f.width - 48) tail++;
    tr_text(fb, f.x + 20, f.y + 22, 32, tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + 22 + ttf_text_width_px(32, tail), f.y + 18, 3, 46}, UI_GRAY_BLACK, fb);
    tr_text(fb, TR_MARGIN, 286, 26,
            url ? "例如 http://192.168.1.10:8080，保存在 TF 卡 tomato/config.json" : "服务器没有设置密码就留空",
            EPD_DRAW_ALIGN_LEFT, false);
    tr_kbd_draw(fb, &s_kbd);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    if (s_view == V_LIST) draw_list(fb);
    else draw_edit(fb);
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_view = V_LIST;
    s_message[0] = 0;
    load();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_view = V_LIST;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){0, 160, TR_W, TR_H - 160};
}

static app_redraw_t touch_list(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)x;
    for (int i = 0; i < R_COUNT; i++) {
        if (y < ROWS[i].y || y >= ROWS[i].y + ROW_H) continue;
        switch ((row_id_t)i) {
        case R_FONT: ctx->request_app = &app_font_pick; break;
        case R_SLEEP: ctx->request_app = &app_sleep; break;
        case R_WIFI:
        case R_IMPORT:
            if (!tomato_sync_busy()) ctx->request_app = &app_transfer;
            break;
        case R_SERVER:
            // 地址只用英文和符号。/ The address takes English letters and symbols only.
            tr_kbd_open(&s_kbd, s_has_cfg ? s_cfg.url : "http://", false, "下一步");
            s_view = V_EDIT_URL;
            return APP_REDRAW_PAGE;
        case R_ABOUT: ctx->request_app = &app_home; break;
        default: break;
        }
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_edit(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)ctx;
    if (y < 160 && x < 120) { s_view = V_LIST; return APP_REDRAW_PAGE; }
    tr_kbd_result_t res = tr_kbd_touch(&s_kbd, x, y);
    if (res == TR_KBD_CHANGED) {
        render(ctx, ctx->fb);
        return APP_REDRAW_AREA;
    }
    if (res != TR_KBD_SUBMIT) return APP_REDRAW_NONE;
    if (s_view == V_EDIT_URL) {
        snprintf(s_new_url, sizeof(s_new_url), "%s", s_kbd.text);
        tr_kbd_open(&s_kbd, s_has_cfg ? s_cfg.password : "", false, "保存");
        s_view = V_EDIT_PASS;
        return APP_REDRAW_PAGE;
    }
    tomato_config_t next = {0};
    snprintf(next.url, sizeof(next.url), "%s", s_new_url);
    snprintf(next.password, sizeof(next.password), "%.63s", s_kbd.text);
    esp_err_t err = tomato_config_save(&next);
    memset(&next, 0, sizeof(next));
    memset(&s_kbd, 0, sizeof(s_kbd));
    snprintf(s_message, sizeof(s_message), err == ESP_OK ? "已保存" : "保存失败，检查 TF 卡");
    load();
    s_view = V_LIST;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    if (s_view == V_LIST) {
        if (tr_nav_touch(ctx, touch->x, touch->y, TR_TAB_SETTINGS)) return APP_REDRAW_NONE;
        return touch_list(ctx, touch->x, touch->y);
    }
    return touch_edit(ctx, touch->x, touch->y);
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1 || s_view == V_LIST) return APP_REDRAW_NONE;
    s_view = V_LIST;
    return APP_REDRAW_PAGE;
}

const app_desc_t app_tr_settings = {
    .title = "设置",
    .detail = "字体、睡眠、Wi-Fi、番茄服务器",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_touch = on_touch,
    .on_key = on_key,
    .area_hint = area_hint,
};
