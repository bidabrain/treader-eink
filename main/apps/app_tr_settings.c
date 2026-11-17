/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 设置：分组清单，入口指向官方的字体、睡眠、传书（含 Wi-Fi）、概览页；
 * 番茄服务器和 WebDAV 的地址、账号在本页用键盘编辑；“立即同步”触发 WebDAV 同步。
 *
 * Settings: a grouped list linking to the official font, sleep, transfer (incl. Wi-Fi) and overview pages;
 * Tomato server and WebDAV credentials are edited here with the keyboard; "Sync now" runs WebDAV sync.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "book_progress.h"
#include "esp_heap_caps.h"
#include "read_pico_transfer.h"
#include "tomato.h"
#include "tr_kbd.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define ROW_H 76
#define POLL_MS 500

extern const app_desc_t app_font_pick;
extern const app_desc_t app_sleep;
extern const app_desc_t app_transfer;
extern const app_desc_t app_home;
extern const app_desc_t app_selftest;

typedef enum {
    R_FONT, R_SLEEP, R_WIFI, R_SERVER, R_IMPORT, R_DAV, R_DAV_SYNC, R_ABOUT, R_SELFTEST, R_COUNT
} row_id_t;

typedef struct {
    const char* section;   // 非空时在这一行上方画分组标题 / Section title above this row when set
    const char* label;
    int y;
} row_t;

static const row_t ROWS[R_COUNT] = {
    [R_FONT] = {"阅读", "字体", 200},
    [R_SLEEP] = {"电源", "睡眠与锁屏", 324},
    [R_WIFI] = {"连接", "Wi-Fi", 448},
    [R_SERVER] = {NULL, "番茄服务器", 524},
    [R_IMPORT] = {NULL, "导入书籍", 600},
    [R_DAV] = {"同步", "WebDAV", 724},
    [R_DAV_SYNC] = {NULL, "立即同步", 800},
    [R_ABOUT] = {"关于", "关于本机", 924},
    [R_SELFTEST] = {NULL, "设备自检", 1000},
};

// 编辑步骤：番茄（地址→密码）、WebDAV（地址→用户名→密码）。
// Edit steps: Tomato (URL, password) and WebDAV (URL, user, password).
typedef enum { E_NONE = 0, E_SRV_URL, E_SRV_PASS, E_DAV_URL, E_DAV_USER, E_DAV_PASS } edit_t;

static const struct { const char* title; const char* hint; const char* go; } EDIT[] = {
    [E_NONE] = {"", "", ""},
    [E_SRV_URL] = {"番茄服务器地址", "例如 http://192.168.1.10:8080", "下一步"},
    [E_SRV_PASS] = {"番茄服务器密码", "服务器没有设置密码就留空", "保存"},
    [E_DAV_URL] = {"WebDAV 目录地址", "例如 https://dav.jianguoyun.com/dav/treader", "下一步"},
    [E_DAV_USER] = {"WebDAV 用户名", "坚果云填登录邮箱", "下一步"},
    [E_DAV_PASS] = {"WebDAV 密码", "坚果云填“应用密码”，不是登录密码", "保存"},
};

static edit_t s_edit;
static char s_wifi[33];
static tomato_config_t s_cfg;
static tomato_webdav_config_t s_dav;
static bool s_has_cfg, s_has_dav;
static char s_message[64];
static tr_kbd_t s_kbd;
static char s_step1[160], s_step2[64];
static bool s_syncing;
static int64_t s_poll_ms;
static uint32_t s_seen_revision;

/* ---- WebDAV 进度读写 / Progress I/O for WebDAV ---- */

static int export_progress(tomato_progress_t* out, int max) {
    book_progress_recent_t* list = heap_caps_calloc((size_t)max, sizeof(*list), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!list) return 0;
    int n = book_progress_recent(list, max), count = 0;
    for (int i = 0; i < n; i++) {
        book_progress_t p;
        if (!book_progress_load(list[i].path, list[i].file_size, &p)) continue;
        tomato_progress_t* o = &out[count++];
        memset(o, 0, sizeof(*o));
        snprintf(o->path, sizeof(o->path), "%s", list[i].path);
        o->size = p.file_size;
        o->chapter = p.chapter;
        o->off = p.byte_off;
        o->px = p.px;
        o->pct = p.pct;
    }
    heap_caps_free(list);
    return count;
}

// 本机文件不小于记录时的大小才写回（番茄更新只追加章节）。/ Import only if the local file is no smaller (updates only append).
static bool import_progress(const tomato_progress_t* in) {
    struct stat st;
    if (!in->size || stat(in->path, &st) != 0 || (uint32_t)st.st_size < in->size) return false;
    book_progress_t p = {
        .file_size = (uint32_t)st.st_size,
        .chapter = in->chapter,
        .byte_off = in->off,
        .px = in->px >= 36 && in->px <= 72 && (in->px - 36) % 4 == 0 ? in->px : 40,
        .pct = in->pct > 100 ? 100 : in->pct,
    };
    return book_progress_save(in->path, &p) == ESP_OK;
}

/* ---- 数据 / Data ---- */

static void load(void) {
    bool configured = false;
    if (read_pico_transfer_get_saved_wifi(s_wifi, &configured) != ESP_OK || !configured) s_wifi[0] = 0;
    s_has_cfg = tomato_config_load(&s_cfg) == ESP_OK;
    s_has_dav = tomato_webdav_config_load(&s_dav) == ESP_OK;
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
    case R_IMPORT: snprintf(out, cap, tomato_sync_busy() ? "同步中，稍后再试" : "浏览器上传 ›"); break;
    case R_DAV: snprintf(out, cap, "%.150s ›", s_has_dav ? host_of(s_dav.url) : "未设置"); break;
    case R_DAV_SYNC: {
        tomato_status_t st;
        tomato_sync_get_status(&st);
        if (s_syncing) snprintf(out, cap, "%s…", st.phase == TOMATO_CONNECTING ? "连接 Wi-Fi" : "同步中");
        else snprintf(out, cap, s_has_dav ? "进度和番茄书架 ›" : "先设置 WebDAV");
        break;
    }
    case R_ABOUT: snprintf(out, cap, "电量、存储、版本 ›"); break;
    case R_SELFTEST: snprintf(out, cap, "屏幕、触摸、电源 ›"); break;
    default: out[0] = 0; break;
    }
}

/* ---- 绘制 / Drawing ---- */

static void draw_list(uint8_t* fb) {
    tr_title(fb, "设置");
    if (s_message[0]) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s", s_message);
        tr_fit(msg, 26, 420);
        tr_text(fb, TR_W - TR_MARGIN, 104, 26, msg, EPD_DRAW_ALIGN_RIGHT, false);
    }
    for (int i = 0; i < R_COUNT; i++) {
        const row_t* r = &ROWS[i];
        if (r->section) tr_text(fb, TR_MARGIN, r->y - 36, 26, r->section, EPD_DRAW_ALIGN_LEFT, false);
        tr_text(fb, TR_MARGIN, r->y + 22, 32, r->label, EPD_DRAW_ALIGN_LEFT, false);
        char value[160];
        row_value((row_id_t)i, value, sizeof(value));
        tr_fit(value, 28, TR_W - 2 * TR_MARGIN - 200);
        tr_text(fb, TR_W - TR_MARGIN, r->y + 26, 28, value, EPD_DRAW_ALIGN_RIGHT, false);
        bool last_in_group = i + 1 == R_COUNT || ROWS[i + 1].section;
        if (!last_in_group) tr_hline(fb, r->y + ROW_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_tab_bar(fb, TR_TAB_SETTINGS);
}

static void draw_edit(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 96, 100, 36, EDIT[s_edit].title, EPD_DRAW_ALIGN_LEFT, true);
    EpdRect f = {TR_MARGIN, 176, TR_W - 2 * TR_MARGIN, 84};
    ui_draw_round_rect(fb, f, 14, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(f, 1), 13, UI_GRAY_BLACK);
    bool secret = s_edit == E_SRV_PASS || s_edit == E_DAV_PASS;
    char shown[TR_KBD_TEXT_MAX];
    if (secret) {
        // 密码只露最后一个字符。/ Passwords show only the last character.
        size_t n = strlen(s_kbd.text), k = 0;
        for (size_t i = 0; i + 1 < n && k < sizeof(shown) - 2; i++) shown[k++] = '*';
        if (n && k < sizeof(shown) - 1) shown[k++] = s_kbd.text[n - 1];
        shown[k] = 0;
    } else {
        snprintf(shown, sizeof(shown), "%s", s_kbd.text);
    }
    const char* tail = shown;
    while (*tail && ttf_text_width_px(32, tail) > f.width - 48) tail++;
    tr_text(fb, f.x + 20, f.y + 22, 32, tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + 22 + ttf_text_width_px(32, tail), f.y + 18, 3, 46}, UI_GRAY_BLACK, fb);
    tr_text(fb, TR_MARGIN, 286, 26, EDIT[s_edit].hint, EPD_DRAW_ALIGN_LEFT, false);
    tr_kbd_draw(fb, &s_kbd);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    if (s_edit == E_NONE) draw_list(fb);
    else draw_edit(fb);
}

/* ---- 编辑 / Editing ---- */

static void begin_edit(edit_t step, const char* initial) {
    s_edit = step;
    tr_kbd_open(&s_kbd, initial, false, EDIT[step].go);
}

static app_redraw_t finish_edit(void) {
    esp_err_t err;
    if (s_edit == E_SRV_PASS) {
        tomato_config_t next = {0};
        snprintf(next.url, sizeof(next.url), "%.127s", s_step1);
        snprintf(next.password, sizeof(next.password), "%.63s", s_kbd.text);
        err = tomato_config_save(&next);
        memset(&next, 0, sizeof(next));
    } else {
        tomato_webdav_config_t next = {0};
        snprintf(next.url, sizeof(next.url), "%.159s", s_step1);
        snprintf(next.user, sizeof(next.user), "%.63s", s_step2);
        snprintf(next.password, sizeof(next.password), "%.63s", s_kbd.text);
        err = tomato_webdav_config_save(&next);
        memset(&next, 0, sizeof(next));
    }
    memset(&s_kbd, 0, sizeof(s_kbd));
    memset(s_step2, 0, sizeof(s_step2));
    snprintf(s_message, sizeof(s_message), err == ESP_OK ? "已保存" : "保存失败，检查 TF 卡");
    load();
    s_edit = E_NONE;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_edit(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 160 && x < 120) { s_edit = E_NONE; return APP_REDRAW_PAGE; }
    tr_kbd_result_t res = tr_kbd_touch(&s_kbd, x, y);
    if (res == TR_KBD_CHANGED) {
        render(ctx, ctx->fb);
        return APP_REDRAW_AREA;
    }
    if (res != TR_KBD_SUBMIT) return APP_REDRAW_NONE;
    switch (s_edit) {
    case E_SRV_URL:
        snprintf(s_step1, sizeof(s_step1), "%.159s", s_kbd.text);
        begin_edit(E_SRV_PASS, s_has_cfg ? s_cfg.password : "");
        return APP_REDRAW_PAGE;
    case E_DAV_URL:
        snprintf(s_step1, sizeof(s_step1), "%.159s", s_kbd.text);
        begin_edit(E_DAV_USER, s_has_dav ? s_dav.user : "");
        return APP_REDRAW_PAGE;
    case E_DAV_USER:
        snprintf(s_step2, sizeof(s_step2), "%.63s", s_kbd.text);
        begin_edit(E_DAV_PASS, s_has_dav ? s_dav.password : "");
        return APP_REDRAW_PAGE;
    default:
        return finish_edit();
    }
}

static app_redraw_t touch_list(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)x;
    for (int i = 0; i < R_COUNT; i++) {
        if (y < ROWS[i].y || y >= ROWS[i].y + ROW_H) continue;
        s_message[0] = 0;
        switch ((row_id_t)i) {
        case R_FONT: ctx->request_app = &app_font_pick; break;
        case R_SLEEP: ctx->request_app = &app_sleep; break;
        case R_WIFI:
        case R_IMPORT:
            if (!tomato_sync_busy()) ctx->request_app = &app_transfer;
            break;
        case R_SERVER:
            // 地址只用英文和符号。/ Addresses take English letters and symbols only.
            begin_edit(E_SRV_URL, s_has_cfg ? s_cfg.url : "http://");
            return APP_REDRAW_PAGE;
        case R_DAV:
            begin_edit(E_DAV_URL, s_has_dav ? s_dav.url : "https://");
            return APP_REDRAW_PAGE;
        case R_DAV_SYNC:
            if (!s_has_dav || tomato_sync_busy()) return APP_REDRAW_NONE;
            tomato_sync_set_progress_io(export_progress, import_progress);
            if (tomato_sync_start_webdav() == ESP_OK) s_syncing = true;
            return APP_REDRAW_PAGE;
        case R_ABOUT: ctx->request_app = &app_home; break;
        case R_SELFTEST: ctx->request_app = &app_selftest; break;
        default: break;
        }
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

/* ---- 生命周期 / Lifecycle ---- */

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_edit = E_NONE;
    s_message[0] = 0;
    // 离开期间同步可能已结束。/ The sync may have finished while away.
    if (s_syncing && !tomato_sync_busy()) s_syncing = false;
    load();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_dav, 0, sizeof(s_dav));
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_edit = E_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    if (s_edit == E_NONE) return (EpdRect){0, ROWS[R_DAV_SYNC].y, TR_W, ROW_H};
    return (EpdRect){0, 160, TR_W, TR_H - 160};
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    if (s_edit == E_NONE) {
        if (tr_nav_touch(ctx, touch->x, touch->y, TR_TAB_SETTINGS)) return APP_REDRAW_NONE;
        return touch_list(ctx, touch->x, touch->y);
    }
    return touch_edit(ctx, touch->x, touch->y);
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1 || s_edit == E_NONE) return APP_REDRAW_NONE;
    s_edit = E_NONE;
    return APP_REDRAW_PAGE;
}

// 同步进行中只刷新“立即同步”这一行；结束后整页显示结果。
// While syncing, refresh only the "Sync now" row; when done, show the result with a page refresh.
static app_redraw_t on_tick(app_ctx_t* ctx) {
    tr_time_poll();
    if (!s_syncing || s_edit != E_NONE || ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tomato_status_t st;
    tomato_sync_get_status(&st);
    if (!tomato_sync_busy()) {
        s_syncing = false;
        snprintf(s_message, sizeof(s_message), "%s", st.message);
        load();
        return APP_REDRAW_PAGE;
    }
    if (st.revision == s_seen_revision) return APP_REDRAW_NONE;
    s_seen_revision = st.revision;
    render(ctx, ctx->fb);
    return APP_REDRAW_AREA;
}

const app_desc_t app_tr_settings = {
    .title = "设置",
    .detail = "字体、睡眠、Wi-Fi、服务器与同步",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
