/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB 传书页：开始前说明；开始后换内置字体、停后台、卸 TF 卡并交给电脑当 U 盘，设备只显示这一页（接管三键、
 * 占着 PMU 所以电源键不锁屏、不自动睡眠、没有标签栏和菜单）；点“结束传输”或拔线 3 秒后重启恢复。
 *
 * English: USB transfer page. An intro first; once started it switches to the built-in font, stops background work,
 * unmounts the TF card and hands it to the computer as a USB drive. The device then shows only this page (it owns the
 * three keys and the PMU, so the power key does not lock, idle sleep is off, and there is no tab bar or menu).
 * "End transfer", or 3 s after the cable is pulled, restarts the device to recover.
 *
 * 冻结：结束一律重启，不做原地恢复；电脑没弹出时结束要先确认；后台在下载或同步时不允许开始。
 * Frozen: ending always restarts, never an in-place recovery; ending before the computer ejects asks first; no start
 * while a download or sync runs.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "book_cover.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "pinyin_ime.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_sd.h"
#include "read_pico_usb.h"
#include "tomato.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_usb"
#define CW (TR_W - 2 * TR_MARGIN)
// 长度随显示大小放大；放大后说明页不画装饰图标、间距收紧，按钮离底边近一些，每一屏仍放得下。
// Lengths scale with the display size; larger sizes drop the intro's decorative icon, tighten gaps and move the button
// closer to the bottom so every view still fits.
#define BIG (ui_scale_pct > 100)
#define BUTTON_RECT button_rect()
#define DIALOG_RECT dialog_geom(NULL, NULL, NULL)
#define BACK_BTN back_btn()
#define FORCE_BTN force_btn()
#define METER_TOP (BIG ? TR_S(420) : 520)
#define METER_H TR_S(80)
#define DIALOG_TEXT "现在结束，电脑上还没写完的文件可能会坏掉。建议先在电脑上弹出。"
#define RESTART_SUB "重启后检查 TF 卡、重新扫描书库和漫画"
#define GONE_END_MS 3000
#define POLL_MS 1000

typedef enum { V_INTRO, V_RUN, V_CONFIRM, V_RESTART } view_t;

static view_t s_view;
static char s_msg[96];
static bool s_failed;                 // 交卡或启动 USB 失败：只能重启 / Handover or USB start failed: restart only
static read_pico_usb_status_t s_st;
static int64_t s_poll_ms, s_restart_at;
static bool s_power;                  // 有外部供电（USB 线还插着）/ External power present (cable still in)
static bool s_was_busy;               // 上次画的“正在读写” / Whether "reading/writing" was last drawn
static int64_t s_intro_ms;            // 开始前页面最近一次被碰 / Last touch on the intro

/* ---- 小工具 / Helpers ---- */

static EpdRect button_rect(void) {
    const int h = TR_S(96);
    return (EpdRect){TR_MARGIN, TR_H - (BIG ? TR_S(40) : 160) - h, CW, h};
}

// 按 tr_wrap 的断行规则数行数（不限行）。/ Count lines with tr_wrap's breaking rule (no line limit).
static int count_lines(const char* text, int px, int w) {
    int n = 0;
    for (const char* p = text; *p; n++) {
        p += tr_line_bytes(p, px, w);
        if (*p == '\n') p++;
    }
    return n;
}

// 确认框：放大后说明可能多一行，框跟着长高、保持上下居中。/ Confirm dialog: scaled up the text may take another
// line, so the box grows and stays centred.
static EpdRect dialog_geom(int* text_lines, EpdRect* back, EpdRect* force) {
    const int w = TR_W - 140, pad = TR_S(36);
    int lines = count_lines(DIALOG_TEXT, TR_S(28), w - 2 * pad);
    if (lines < 3) lines = 3;
    if (lines > 4) lines = 4;
    const int btn_off = TR_S(108) + lines * TR_S(42) + TR_S(44);
    const int h = btn_off + TR_S(84) + TR_S(38);
    const EpdRect d = {70, 420 - (h - 400) / 2, w, h};
    const int bw = (w - 2 * pad - TR_S(12)) / 2;
    if (text_lines) *text_lines = lines;
    if (back) *back = (EpdRect){d.x + pad, d.y + btn_off, bw, TR_S(84)};
    if (force) *force = (EpdRect){d.x + d.width - pad - bw, d.y + btn_off, bw, TR_S(84)};
    return d;
}

static EpdRect back_btn(void) {
    EpdRect r;
    dialog_geom(NULL, &r, NULL);
    return r;
}

static EpdRect force_btn(void) {
    EpdRect r;
    dialog_geom(NULL, NULL, &r);
    return r;
}

// 在底色上逐行排字，项目符号的续行缩到符号后面；fb 为空只数行。
// Wrap text on a filled background; bullet continuation lines indent past the bullet. A NULL fb only counts lines.
static int wrap_on(uint8_t* fb, int x, int y, int w, int px, int line_h, const char* text, bool bold, uint8_t bg) {
    const int indent = strncmp(text, "· ", strlen("· ")) == 0 ? ttf_text_width_px(px, "· ") : 0;
    int n = 0;
    for (const char* p = text; *p; n++) {
        const int lx = n ? x + indent : x;
        const size_t take = tr_line_bytes(p, px, w - (lx - x));
        char line[160];
        const size_t k = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
        memcpy(line, p, k);
        line[k] = 0;
        if (fb) tr_text_on(fb, lx, y + n * line_h, px, line, EPD_DRAW_ALIGN_LEFT, bold, UI_GRAY_BLACK, bg);
        p += take;
        while (*p == ' ' || *p == '\n') p++;
    }
    return n;
}

// 居中的一句，放不下就折成两行；返回行数。/ A centred line that folds into two when it does not fit; returns lines.
static int center_text(uint8_t* fb, int y, int px, int line_h, const char* text, bool dim) {
    char line[160];
    const char* p = text;
    int n = 0;
    for (; *p && n < 2; n++) {
        const size_t take = ttf_text_width_px(px, p) <= CW ? strlen(p) : tr_line_bytes(p, px, CW);
        const size_t k = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
        memcpy(line, p, k);
        line[k] = 0;
        if (dim) tr_text_dim(fb, TR_W / 2, y + n * line_h, px, line, EPD_DRAW_ALIGN_CENTER);
        else tr_text(fb, TR_W / 2, y + n * line_h, px, line, EPD_DRAW_ALIGN_CENTER, true);
        p += take;
    }
    return n;
}

static void fmt_bytes(uint64_t n, char* out, size_t cap) {
    if (n < 1024 * 1024) snprintf(out, cap, "%llu KB", (unsigned long long)(n / 1024));
    else if (n < 10ull * 1024 * 1024 * 1024) snprintf(out, cap, "%.1f MB", n / 1048576.0);
    else snprintf(out, cap, "%.2f GB", n / 1073741824.0);
}

static bool card_ready(uint64_t* free_bytes) {
    read_pico_sd_info_t info = {0};
    const bool ok = read_pico_sd_get_info(&info) == ESP_OK && info.mounted;
    if (free_bytes) *free_bytes = info.free_bytes;
    return ok;
}

// USB 图标：竖杆、三叉、方块和圆点（按 ui.html 里的 i-usb 画）。/ USB glyph: shaft, fork, square and dots.
static void draw_usb_icon(uint8_t* fb, int cx, int top, int size) {
    const int s = TR_S(size / 40);
    const int x = cx - TR_S(size) / 2;
    for (int d = -s; d <= s; d++) {
        epd_draw_line(x + 20 * s + d, top + 4 * s, x + 20 * s + d, top + 30 * s, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 15 * s, top + 9 * s + d, x + 20 * s, top + 4 * s + d, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 25 * s, top + 9 * s + d, x + 20 * s, top + 4 * s + d, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 20 * s, top + 22 * s + d, x + 12 * s, top + 17 * s + d, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 12 * s + d, top + 17 * s, x + 12 * s + d, top + 12 * s, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 20 * s, top + 26 * s + d, x + 28 * s, top + 21 * s + d, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 28 * s + d, top + 21 * s, x + 28 * s + d, top + 17 * s, UI_GRAY_BLACK, fb);
    }
    epd_fill_rect((EpdRect){x + 25 * s, top + 13 * s, 6 * s, 5 * s}, UI_GRAY_BLACK, fb);
    epd_fill_circle(x + 12 * s, top + 11 * s, 3 * s, UI_GRAY_BLACK, fb);
    epd_fill_circle(x + 20 * s, top + 33 * s, 4 * s, UI_GRAY_BLACK, fb);
}

/* ---- 绘制 / Drawing ---- */

static void draw_intro(uint8_t* fb) {
    tr_status_bar(fb);
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_S(96), TR_S(100), TR_S(36), "USB 传书", EPD_DRAW_ALIGN_LEFT, true);
    // 放大后去掉装饰图标，给文字让地方。/ Larger sizes drop the decorative icon to make room for text.
    if (!BIG) draw_usb_icon(fb, TR_W / 2, 190, 120);
    static const char* const STEPS[4] = {"用数据线连上电脑", "点“开始”，电脑上出现 U 盘", "拷完在电脑上“弹出”这个 U 盘",
                                         "回到设备点“结束传输”"};
    int y = BIG ? TR_S(158) : 350;
    for (int i = 0; i < 4; i++) {
        epd_fill_circle(TR_MARGIN + TR_S(21), y + TR_S(21), TR_S(21), UI_GRAY_BLACK, fb);
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        tr_text_inv(fb, TR_MARGIN + TR_S(21), y + TR_S(6), TR_S(24), n, EPD_DRAW_ALIGN_CENTER, true);
        const int lines =
            tr_wrap(fb, TR_MARGIN + TR_S(58), y + TR_S(2), CW - TR_S(58), TR_S(28), TR_S(36), STEPS[i], 2);
        y += TR_S(46) + (lines - 1) * TR_S(36);
    }
    // 注意事项放在浅灰填充卡片里（说明页整页刷新）；放大后一条可能折两行，卡片跟着长高。
    // The notes sit on a light gray filled card (the intro refreshes as a whole page); scaled up a note may take two
    // lines and the card grows with it.
    static const char* const NOTES[4] = {"开始后：", "· 设备其他功能全部暂停，只显示传输页", "· 换成内置字体，结束后自动换回",
                                         "· Wi-Fi 和后台加载都会停掉"};
    const int pad = TR_S(26), inner = CW - 2 * pad, lh = TR_S(42);
    int note_lines = 0;
    for (int i = 0; i < 4; i++) note_lines += wrap_on(NULL, 0, 0, inner, TR_S(28), lh, NOTES[i], i == 0, 0);
    const EpdRect w = {TR_MARGIN, y + (BIG ? TR_S(16) : 46), CW, TR_S(20) + note_lines * lh + TR_S(22)};
    ui_fill_round_rect(fb, w, TR_S(24), TR_GRAY_SURFACE);
    for (int i = 0, ny = w.y + TR_S(20); i < 4; i++)
        ny += lh * wrap_on(fb, w.x + pad, ny, inner, TR_S(28), lh, NOTES[i], i == 0, TR_GRAY_SURFACE);
    uint64_t free_bytes = 0;
    const bool card = card_ready(&free_bytes);
    char line[200], size[24];
    fmt_bytes(free_bytes, size, sizeof(size));
    if (card)
        snprintf(line, sizeof(line), "TF 卡可用 %s。书放 books/，漫画放 comics/（一部一个文件夹），字体放 fonts/，壁纸放 wallpapers/。", size);
    else
        snprintf(line, sizeof(line), "没有可用的 TF 卡，插好卡再来。");
    tr_wrap(fb, TR_MARGIN, w.y + w.height + (BIG ? TR_S(16) : 46), CW, TR_S(24), TR_S(36), s_msg[0] ? s_msg : line,
            BIG ? 4 : 3);
    const EpdRect b = BUTTON_RECT;
    if (card) {
        tr_pill(fb, b, "开始", true, TR_S(34));
    } else {
        ui_draw_round_rect(fb, b, b.height / 2, TR_GRAY_LINE);
        tr_text(fb, TR_W / 2, b.y + TR_S(30), TR_S(34), "没有 TF 卡", EPD_DRAW_ALIGN_CENTER, false);
    }
}

static void draw_meters(uint8_t* fb) {
    char a[24], b[24];
    fmt_bytes(s_st.write_bytes, a, sizeof(a));
    fmt_bytes(s_st.read_bytes, b, sizeof(b));
    const bool busy = esp_timer_get_time() / 1000 - s_st.io_ms < 2000 && s_st.io_ms;
    s_was_busy = busy;
    const char* state = s_st.ejected ? "已弹出" : busy ? "正在读写…" : "空闲";
    const char* labels[3] = {"电脑写入", "电脑读取", "状态"};
    const char* values[3] = {a, b, state};
    for (int i = 0; i < 3; i++) {
        const int y = METER_TOP + i * METER_H;
        epd_fill_rect((EpdRect){0, y, TR_W, METER_H}, UI_GRAY_WHITE, fb);
        tr_text(fb, TR_MARGIN, y + TR_S(20), TR_S(30), labels[i], EPD_DRAW_ALIGN_LEFT, false);
        tr_text(fb, TR_W - TR_MARGIN, y + TR_S(20), TR_S(30), values[i], EPD_DRAW_ALIGN_RIGHT, true);
        tr_hline(fb, y + METER_H - 2, TR_MARGIN, CW, true);
    }
}

static void draw_run(uint8_t* fb) {
    tr_status_bar(fb);
    tr_title(fb, "USB 传书中");
    // 放大后图标、状态和读数靠得紧一些。/ Scaled up, the icon, status and meters sit closer together.
    draw_usb_icon(fb, TR_W / 2, BIG ? TR_S(150) : 200, 140);
    const int big_y = BIG ? TR_S(296) : 370;
    const bool gone = s_st.state == READ_PICO_USB_GONE;
    const char* big = s_failed ? "USB 没能启动" :
                      s_st.ejected ? "电脑已弹出，可以结束" :
                      s_st.state == READ_PICO_USB_WAITING ? "等待连接电脑…" :
                      s_st.state == READ_PICO_USB_CONNECTED ? "已连接电脑" :
                      s_power ? "电脑暂停了 U 盘" : "数据线已拔下";
    center_text(fb, big_y, TR_S(40), TR_S(48), big, false);
    const char* sub = s_failed ? s_msg :
                      s_st.ejected ? "点下面的按钮恢复设备" :
                      s_st.state == READ_PICO_USB_WAITING ? "用数据线连上电脑，电脑上会出现 U 盘" :
                      s_st.state == READ_PICO_USB_CONNECTED ? "在电脑上打开新出现的 U 盘" :
                      s_power ? "电脑可能在睡眠或省电，等它恢复" : "马上自动结束…";
    center_text(fb, big_y + TR_S(60), TR_S(28), TR_S(36), sub, true);
    (void)gone;
    if (!s_failed) draw_meters(fb);
    tr_wrap(fb, TR_MARGIN, BIG ? METER_TOP + 3 * METER_H + TR_S(24) : 800, CW, TR_S(26), TR_S(40),
            s_failed ? "TF 卡已经从设备上卸下，点“结束传输”重启恢复。" :
            "拷完先在电脑上“弹出”U 盘（Windows 右下角“安全删除硬件”，Mac 拖到废纸篓或点 ⏏），再点下面的按钮。",
            BIG ? 4 : 3);
    tr_pill(fb, BUTTON_RECT, "结束传输", s_failed || s_st.ejected || s_st.state == READ_PICO_USB_WAITING, TR_S(34));
}

static void draw_confirm(uint8_t* fb) {
    draw_run(fb);
    // Material 3 对话框：白底大圆角、深灰细框。/ Material 3 dialog: white, large radius, thin dark outline.
    int lines;
    EpdRect back, force;
    const EpdRect d = dialog_geom(&lines, &back, &force);
    epd_fill_rect(d, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, d, TR_S(28), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(d, 1), TR_S(28) - 1, TR_GRAY_OUTLINE);
    tr_text(fb, TR_W / 2, d.y + TR_S(40), TR_S(36), "电脑还没弹出 U 盘", EPD_DRAW_ALIGN_CENTER, true);
    tr_wrap(fb, d.x + TR_S(36), d.y + TR_S(108), d.width - 2 * TR_S(36), TR_S(28), TR_S(42), DIALOG_TEXT, lines);
    tr_pill(fb, back, "返回", false, TR_S(30));
    tr_pill(fb, force, "仍然结束", true, TR_S(30));
}

static void draw_restart(uint8_t* fb) {
    tr_text(fb, TR_W / 2, TR_S(470), TR_S(44), "正在恢复…", EPD_DRAW_ALIGN_CENTER, true);
    const int n = center_text(fb, TR_S(550), TR_S(28), TR_S(36), RESTART_SUB, true);
    tr_text_dim(fb, TR_W / 2, TR_S(594) + (n - 1) * TR_S(36), TR_S(28), "约 5 秒", EPD_DRAW_ALIGN_CENTER);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    switch (s_view) {
    case V_INTRO: draw_intro(fb); break;
    case V_RUN: draw_run(fb); break;
    case V_CONFIRM: draw_confirm(fb); break;
    case V_RESTART: draw_restart(fb); break;
    }
}

/* ---- 动作 / Actions ---- */

static app_redraw_t start(void) {
    s_msg[0] = 0;
    if (!card_ready(NULL)) return APP_REDRAW_PAGE;
    // 浏览类后台任务让路；在下载或同步就不开始。/ Browsing jobs yield; refuse while downloading or syncing.
    const tomato_preempt_t room = tomato_sync_preempt();
    if (room != TOMATO_PREEMPT_FREE) {
        snprintf(s_msg, sizeof(s_msg), room == TOMATO_PREEMPT_STOPPING ? "正在停下后台加载，请再点一次“开始”。"
                                                                       : "后台正在下载或同步，完成后再用 USB 传书。");
        return APP_REDRAW_PAGE;
    }
    // 先放下所有卡上的东西：字体、封面、词库。/ Let go of everything on the card: font, covers, lexicon.
    if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
    book_cover_release();
    pinyin_ime_unload();
    void* card = NULL;
    esp_err_t err = read_pico_sd_take_for_usb(&card);
    if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_NOT_FOUND) {
        snprintf(s_msg, sizeof(s_msg), err == ESP_ERR_NOT_FOUND ? "没有 TF 卡。" : "TF 卡正忙，稍后再试。");
        return APP_REDRAW_PAGE;
    }
    if (err == ESP_OK) err = read_pico_usb_start(card);
    s_view = V_RUN;
    s_failed = err != ESP_OK;
    if (s_failed) snprintf(s_msg, sizeof(s_msg), "（%s）", esp_err_to_name(err));
    memset(&s_st, 0, sizeof(s_st));
    ESP_LOGI(TAG, "usb transfer start: %s", esp_err_to_name(err));
    return APP_REDRAW_FULL;
}

static app_redraw_t finish(app_ctx_t* ctx) {
    s_view = V_RESTART;
    s_restart_at = ctx->now_ms + 400;   // 先把“正在恢复”刷上屏 / Let "Restoring" reach the panel first
    return APP_REDRAW_FULL;
}

// 电脑还挂着 U 盘（没弹出，且连着或只是挂起）就先确认。/ Ask first while the computer may still hold the drive.
static bool host_may_hold(void) {
    return !s_failed && !s_st.ejected &&
           (s_st.state == READ_PICO_USB_CONNECTED || (s_st.state == READ_PICO_USB_GONE && s_power));
}

static app_redraw_t ask_finish(app_ctx_t* ctx) {
    if (host_may_hold()) { s_view = V_CONFIRM; return APP_REDRAW_PAGE; }
    return finish(ctx);
}

static app_redraw_t leave(app_ctx_t* ctx) {
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    const uint16_t x = touch->x, y = touch->y;
    switch (s_view) {
    case V_INTRO:
        s_intro_ms = ctx->now_ms;
        if (y < TR_S(160) && x < TR_S(160)) return leave(ctx);
        if (ui_rect_hit(BUTTON_RECT, x, y)) return start();
        return APP_REDRAW_NONE;
    case V_RUN:
        return ui_rect_hit(BUTTON_RECT, x, y) ? ask_finish(ctx) : APP_REDRAW_NONE;
    case V_CONFIRM:
        if (ui_rect_hit(FORCE_BTN, x, y)) return finish(ctx);
        if (ui_rect_hit(BACK_BTN, x, y) || !ui_rect_hit(DIALOG_RECT, x, y)) { s_view = V_RUN; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    default:
        return APP_REDRAW_NONE;
    }
}

// 接管三键：开始前 KEY1 返回；传输中三键都不做事（中键长按的整屏刷新由主循环处理）。
// Owns the keys: KEY1 goes back before starting; during a transfer no key acts (the loop still does the KEY2 hold refresh).
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (s_view == V_INTRO && key == UI_KEY_1) return leave(ctx);
    if (s_view == V_CONFIRM && key == UI_KEY_1) { s_view = V_RUN; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){0, METER_TOP, TR_W, 3 * METER_H};
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view == V_RESTART) {
        if (ctx->now_ms >= s_restart_at) {
            read_pico_usb_stop();
            esp_restart();
        }
        return APP_REDRAW_NONE;
    }
    // 开始前的页面接管了电源键和睡眠：放着不动就按平时的空闲时间退出。/ The intro holds the power key and sleep:
    // left alone for the usual idle time, it backs out.
    if (s_view == V_INTRO) return ctx->now_ms - s_intro_ms >= APP_AUTO_SLEEP_MS ? leave(ctx) : APP_REDRAW_NONE;
    if (s_failed || ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    const read_pico_usb_status_t before = s_st;
    const bool power_before = s_power;
    read_pico_usb_get_status(&s_st);
    // 外部供电：本页占着 PMU，主循环不轮询，自己刷一次。读不到就当有电（不自动结束）。
    // External power: this page holds the PMU so the loop does not poll it; refresh here. Unknown counts as powered.
    s_power = true;
    if (read_pico_pmu_ready() && read_pico_pmu_refresh() == ESP_OK) {
        const pmu_snapshot_t* p = read_pico_pmu_get();
        if (p) s_power = p->charge_state == PMU_CHARGE_CHARGING || p->charge_state == PMU_CHARGE_FULL_INFERRED ||
                         (p->flags & PMU_STATUS_CHARGING_ACTIVE);
    }
    // 拔线（没有外部供电）或电脑已弹出后断开，3 秒后自动结束；只是挂起（还有电）就等。
    // Unplugged (no external power), or detached after an eject: end after 3 s. A mere suspend (still powered) waits.
    if (s_st.state == READ_PICO_USB_GONE && (s_st.ejected || !s_power) && ctx->now_ms - s_st.gone_ms >= GONE_END_MS)
        return finish(ctx);
    const bool changed = s_st.state != before.state || s_st.ejected != before.ejected || s_power != power_before;
    if (changed) {
        if (s_view == V_CONFIRM && !host_may_hold()) return finish(ctx);
        return APP_REDRAW_PAGE;
    }
    const bool busy = s_st.io_ms && ctx->now_ms - s_st.io_ms < 2000;
    if (s_view == V_RUN && (s_st.read_bytes + s_st.write_bytes != before.read_bytes + before.write_bytes ||
                            busy != s_was_busy)) {
        draw_meters(ctx->fb);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    s_intro_ms = ctx->now_ms;
    s_view = V_INTRO;
    s_msg[0] = 0;
    s_failed = false;
}

const app_desc_t app_usb = {
    .title = "USB 传书",
    .detail = "TF 卡当 U 盘连电脑",
    .enter_full = true,
    .owns_keys = true,     // 中键不回书库、KEY3 不开菜单 / The middle key does not go home; KEY3 opens no menu
    .holds_pmu = true,     // 电源键不锁屏、不自动睡眠 / The power key does not lock; no idle sleep
    .tab_nav = true,       // 右下角菜单把手让出来（本页不画标签栏）/ Yield the corner menu handle (no tab bar here)
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
