/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TF 卡：显示状态和容量，重新挂载，格式化。格式化前有两步确认，做完建好 Treader 用的文件夹。
 * 从设置页进入，不在菜单里。
 *
 * TF card: status and capacity, remount and format. Formatting needs two confirmations and then creates the
 * folders Treader uses. Opened from Settings; not in the menu.
 *
 * 冻结：格式化只在用户两次确认后执行；后台同步在跑时拒绝；格式化前先切回内置字体，不留卡上的句柄。
 * Frozen: format only after two user confirmations; refuse while a sync runs; switch to the built-in font
 * first so no card handle stays open.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "book_cover.h"
#include "esp_log.h"
#include "pinyin_ime.h"
#include "read_pico_sd.h"
#include "tomato.h"
#include "tr_lock.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "tr_card"
// 长度随显示大小放大；按钮行放在说明下面，放大后跟着往下让。/ Lengths scale with the display size; the button
// row sits below the notes and moves down with them at larger sizes.
#define BTN_Y btn_y()
#define BTN_H TR_S(84)
#define BTN_W TR_S(290)

extern const app_desc_t app_tr_settings;

typedef enum { V_INFO, V_ASK, V_WORKING } view_t;

static view_t s_view;
static bool s_armed;          // 已经点过一次“清空并格式化” / "Erase" tapped once already
static bool s_remounting;
static char s_msg[96];
static read_pico_sd_info_t s_info;
static esp_err_t s_info_err;

// Treader 在卡上用到的文件夹。/ Folders Treader uses on the card.
static const char* const DIRS[] = {
    "/sdcard/books", "/sdcard/comics", "/sdcard/fonts", "/sdcard/ime", "/sdcard/wallpapers", "/sdcard/tomato",
    "/sdcard/update",
};

static void refresh(void) {
    memset(&s_info, 0, sizeof(s_info));
    s_info_err = read_pico_sd_get_info(&s_info);
}

static bool card_ok(void) {
    return s_info_err == ESP_OK && s_info.mounted;
}

// 正常档 900；放大后放在说明之后，但不低于屏幕下沿。/ 900 at the normal size; at larger sizes after the notes but
// never past the screen bottom.
static int btn_y(void) {
    const int low = TR_H - BTN_H - TR_S(40);
    return TR_S(900) < low ? TR_S(900) : low;
}

// 两个按钮并排：放大后宽度不超过半行。/ Two buttons side by side: at larger sizes no wider than half a row.
static int btn_w(void) {
    const int half = (TR_W - 2 * TR_MARGIN - TR_S(24)) / 2;
    return BTN_W < half ? BTN_W : half;
}

static void draw_buttons(uint8_t* fb, const char* left, bool left_filled, int left_px, const char* right,
                         bool right_filled, int right_px) {
    const int w = btn_w();
    tr_pill(fb, (EpdRect){TR_MARGIN, BTN_Y, w, BTN_H}, left, left_filled, left_px);
    tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - w, BTN_Y, w, BTN_H}, right, right_filled, right_px);
}

/* ---- 绘制 / Drawing ---- */

static void info_row(uint8_t* fb, int y, const char* label, const char* value) {
    tr_text(fb, TR_MARGIN, y, TR_S(30), label, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, y + TR_S(2), TR_S(28), value, EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, y + TR_S(54), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static const char* state_text(void) {
    if (s_info_err == ESP_ERR_NOT_FINISHED) return "正在检测…";
    if (!s_info.present) return "没有插卡";
    if (s_info.mounted) return "正常";
    if (s_info.needs_format) return "无法读取，可能需要格式化";
    return "已插入，未挂载";
}

static void draw_info(uint8_t* fb) {
    char line[96];
    info_row(fb, TR_S(200), "状态", state_text());
    if (card_ok()) {
        snprintf(line, sizeof(line), "%.1f GB", s_info.capacity_bytes / 1073741824.0);
        info_row(fb, TR_S(276), "容量", line);
        snprintf(line, sizeof(line), "%.1f GB", s_info.free_bytes / 1073741824.0);
        info_row(fb, TR_S(352), "可用", line);
    }
    // 放大后文件夹说明往上提、行距收紧，给提示和按钮留出位置。/ At larger sizes the folder notes move up with
    // tighter lines, leaving room for the message and buttons.
    const bool tight = ui_scale_pct != 100;
    const int head = TR_S(460) - (tight ? TR_S(40) : 0), step = tight ? TR_S(34) : TR_S(40);
    tr_text_dim(fb, TR_MARGIN, head, TR_S(26), "Treader 用到的文件夹", EPD_DRAW_ALIGN_LEFT);
    static const char* const LINES[] = {
        "books　书籍（TXT、EPUB，番茄下载也放这里）",
        "comics　漫画（CBZ，一部多话放一个子文件夹）",
        "fonts　中文字体（TTF）",
        "ime　拼音词库",
        "wallpapers　锁屏壁纸（JPG、PNG）",
        "tomato　番茄书架、封面和设置",
    };
    // 文件夹说明放进浅灰卡片（这一页整页刷新）。/ The folder notes sit on a light gray card (whole-page refresh).
    const int card_y = head + TR_S(34), card_h = 6 * step + TR_S(20);
    ui_fill_round_rect(fb, (EpdRect){TR_MARGIN - TR_S(16), card_y, TR_W - 2 * TR_MARGIN + 2 * TR_S(16), card_h},
                       TR_S(24), TR_GRAY_SURFACE);
    for (int i = 0; i < 6; i++)
        tr_text_on(fb, TR_MARGIN, card_y + TR_S(12) + i * step, TR_S(28), LINES[i], EPD_DRAW_ALIGN_LEFT, false,
                   UI_GRAY_BLACK, TR_GRAY_SURFACE);
    const int note = card_y + card_h + TR_S(6);
    tr_text_dim(fb, TR_MARGIN, note, TR_S(26), "书、图片和缓存只存在 TF 卡上，不占系统存储。", EPD_DRAW_ALIGN_LEFT);
    if (s_msg[0]) tr_wrap(fb, TR_MARGIN, note + TR_S(46), TR_W - 2 * TR_MARGIN, TR_S(28), TR_S(40), s_msg, 2);
    if (s_info.present) draw_buttons(fb, "重新挂载", false, TR_S(30), "格式化 TF 卡", true, TR_S(30));
}

static void draw_ask(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(200), TR_S(44), "格式化 TF 卡？", EPD_DRAW_ALIGN_LEFT, true);
    tr_wrap(fb, TR_MARGIN, TR_S(290), TR_W - 2 * TR_MARGIN, TR_S(32), TR_S(48),
            "TF 卡上的所有文件都会被删除，无法恢复：书籍和阅读用的文件、字体、拼音词库、壁纸、番茄书架和服务器设置。", 5);
    tr_wrap(fb, TR_MARGIN, TR_S(560), TR_W - 2 * TR_MARGIN, TR_S(28), TR_S(42),
            "格式化后会建好 Treader 需要的文件夹。字体和拼音词库要从电脑重新复制到 fonts 和 ime。", 3);
    draw_buttons(fb, "取消", true, TR_S(32), s_armed ? "再点一次确认" : "清空并格式化", false, TR_S(30));
    if (!s_armed) return;
    // 按钮下面放不下这行提醒时放到按钮上面。/ When the warning does not fit below the buttons it goes above them.
    int y = BTN_Y + BTN_H + TR_S(30);
    if (y + TR_S(28) > TR_H - TR_S(8)) y = BTN_Y - TR_S(30) - TR_S(28);
    tr_text(fb, TR_W / 2, y, TR_S(28), "这一步不能撤销", EPD_DRAW_ALIGN_CENTER, true);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    if (s_view != V_WORKING) tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), "TF 卡", EPD_DRAW_ALIGN_LEFT, true);
    if (s_view == V_ASK) draw_ask(fb);
    else if (s_view == V_WORKING)
        tr_text(fb, TR_W / 2, TR_S(560), TR_S(36), "正在格式化，请勿拔卡…", EPD_DRAW_ALIGN_CENTER, true);
    else draw_info(fb);
}

/* ---- 操作 / Actions ---- */

static bool make_dirs(void) {
    bool ok = true;
    for (size_t i = 0; i < sizeof(DIRS) / sizeof(DIRS[0]); i++) {
        struct stat st;
        if (mkdir(DIRS[i], 0775) != 0 && !(stat(DIRS[i], &st) == 0 && S_ISDIR(st.st_mode))) ok = false;
    }
    FILE* f = fopen("/sdcard/README.txt", "w");
    if (f) {
        fputs("Treader TF card\n\n"
              "books/       TXT and EPUB books; Tomato downloads go here too\n"
              "comics/      CBZ comics; one subfolder per series\n"
              "fonts/       Chinese TTF font (copy sdcard/fonts from the repo)\n"
              "ime/         pinyin lexicon (copy sdcard/ime from the repo)\n"
              "wallpapers/  lock-screen wallpapers, JPG or PNG\n"
              "tomato/      Tomato shelf, covers and settings (managed by the device)\n"
              "update/      firmware for TF-card updates (Read_Pico.bin or treader-full.bin)\n"
              "hupu/, rednote/, weibo/, .treader/  caches made by the device; safe to delete\n", f);
        fclose(f);
    }
    return ok;
}

// 格式化：先让出所有卡上的句柄和缓存，再格式化、建目录。/ Release card handles and caches, then format and create folders.
static void do_format(void) {
    if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
    book_cover_release();
    pinyin_ime_unload();
    esp_err_t err = read_pico_sd_format();
    if (err == ESP_OK) {
        bool dirs = make_dirs();
        tr_lock_wallpaper_set(NULL);   // 原壁纸已被删除 / The old wallpaper is gone
        snprintf(s_msg, sizeof(s_msg), dirs ? "格式化完成，文件夹已建好。请从电脑复制字体和词库。"
                                            : "格式化完成，但有文件夹没建好，请重新挂载后再试。");
    } else {
        snprintf(s_msg, sizeof(s_msg), "格式化失败（%s），请检查卡或换一张卡。", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "format: %s", esp_err_to_name(err));
    refresh();
}

/* ---- 事件 / Events ---- */

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    const int x = touch->x, y = touch->y;
    if (s_view == V_WORKING) return APP_REDRAW_NONE;
    if (y < TR_S(160) && x < TR_S(120)) {
        if (s_view == V_ASK) { s_view = V_INFO; s_armed = false; return APP_REDRAW_PAGE; }
        ctx->request_app = &app_tr_settings;
        return APP_REDRAW_NONE;
    }
    if (y < BTN_Y || y >= BTN_Y + BTN_H) return APP_REDRAW_NONE;
    const bool left = x < TR_W / 2;
    if (s_view == V_ASK) {
        if (left) { s_view = V_INFO; s_armed = false; return APP_REDRAW_PAGE; }
        if (!s_armed) { s_armed = true; return APP_REDRAW_PAGE; }
        s_armed = false;
        s_view = V_WORKING;
        return APP_REDRAW_PAGE;
    }
    if (!s_info.present) return APP_REDRAW_NONE;
    if (tomato_sync_busy()) {
        snprintf(s_msg, sizeof(s_msg), "番茄或 WebDAV 同步还在进行，等它结束再操作。");
        return APP_REDRAW_PAGE;
    }
    if (left) {
        // 重新挂载前同样先放开卡上的字体。/ Release the card font before remounting too.
        if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
        book_cover_release();
        pinyin_ime_unload();
        s_remounting = read_pico_sd_remount() == ESP_ERR_NOT_FINISHED;
        snprintf(s_msg, sizeof(s_msg), s_remounting ? "正在挂载…" : "挂载失败");
        refresh();
        return APP_REDRAW_PAGE;
    }
    s_msg[0] = 0;
    s_view = V_ASK;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1 || s_view == V_WORKING) return APP_REDRAW_NONE;
    if (s_view == V_ASK) { s_view = V_INFO; s_armed = false; return APP_REDRAW_PAGE; }
    ctx->request_app = &app_tr_settings;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    (void)ctx;
    tr_time_poll();
    if (s_view == V_WORKING) {
        do_format();
        s_view = V_INFO;
        return APP_REDRAW_FULL;
    }
    // 插拔卡或挂载结束时更新状态。/ Update when the card is inserted, removed or finishes mounting.
    read_pico_sd_info_t before = s_info;
    esp_err_t before_err = s_info_err;
    refresh();
    if (s_remounting && s_info_err != ESP_ERR_NOT_FINISHED) {
        s_remounting = false;
        snprintf(s_msg, sizeof(s_msg), card_ok() ? "已挂载" : "挂载失败，可以试试格式化");
        return APP_REDRAW_PAGE;
    }
    if (before.present != s_info.present || before.mounted != s_info.mounted || before_err != s_info_err)
        return s_view == V_INFO ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    return APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_view = V_INFO;
    s_armed = false;
    s_remounting = false;
    s_msg[0] = 0;
    refresh();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    s_view = V_INFO;
    s_armed = false;
}

const app_desc_t app_tr_card = {
    .title = "TF 卡",
    .detail = "状态、重新挂载与格式化",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
};
