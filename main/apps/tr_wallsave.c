/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 看大图时的“存为壁纸”工具条：两个按钮“存为壁纸”“存并设为锁屏”，存好后弹两秒提示框，写文件名和去哪里选用。
 *
 * English: the "save as wallpaper" toolbar for image views: "save as wallpaper" and "save and set as lock screen";
 * after saving, a two-second notice names the file and where to pick it.
 *
 * 冻结：只往 TF 卡 wallpapers/ 复制，不改原图、不删任何文件；“存并设为锁屏”才改锁屏设置。
 * Frozen: only copies into wallpapers/ on the TF card, never alters or deletes files; only "save and set" changes the
 * lock-screen setting.
 */

#include "tr_wallsave.h"

#include <stdio.h>
#include <string.h>

#include "tr_lock.h"
#include "tr_ui.h"
#include "ui_kit.h"

#define NOTICE_MS 2000
#define BTN_Y (TR_WALLSAVE_TOP + 32)
#define BTN_H 80
#define BTN_W 290
#define SET_X (TR_W - TR_MARGIN - BTN_W)
#define BOX (EpdRect){60, 860, TR_W - 120, 190}

void tr_wallsave_draw(uint8_t* fb, const tr_wallsave_t* w) {
    if (w->shown) {
        epd_fill_rect((EpdRect){0, TR_WALLSAVE_TOP, TR_W, 184}, UI_GRAY_WHITE, fb);
        tr_hline(fb, TR_WALLSAVE_TOP, 0, TR_W, true);
        tr_pill(fb, (EpdRect){TR_MARGIN, BTN_Y, BTN_W, BTN_H}, "存为壁纸", true, 32);
        tr_pill(fb, (EpdRect){SET_X, BTN_Y, BTN_W, BTN_H}, "存并设为锁屏", false, 32);
        tr_text_dim(fb, TR_MARGIN, BTN_Y + BTN_H + 22, 24, "存到 TF 卡 wallpapers/，点图片中间收起", EPD_DRAW_ALIGN_LEFT);
    }
    if (w->title[0]) {
        const EpdRect r = BOX;
        epd_fill_rect(r, UI_GRAY_WHITE, fb);
        ui_draw_round_rect(fb, r, 22, UI_GRAY_BLACK);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), 21, UI_GRAY_BLACK);
        ui_draw_round_rect(fb, ui_inset_rect(r, 2), 20, UI_GRAY_BLACK);
        tr_text(fb, r.x + r.width / 2, r.y + 28, 32, w->title, EPD_DRAW_ALIGN_CENTER, true);
        tr_wrap(fb, r.x + 24, r.y + 88, r.width - 48, 24, 38, w->line, 2);
    }
}

static void notice(tr_wallsave_t* w, const char* title, const char* line, int64_t now_ms) {
    snprintf(w->title, sizeof(w->title), "%s", title);
    snprintf(w->line, sizeof(w->line), "%s", line);
    w->until_ms = now_ms + NOTICE_MS;
}

bool tr_wallsave_tap(tr_wallsave_t* w, int x, int y, const char* src, const char* source, const char* author, int n,
                     int64_t now_ms) {
    if (!w->shown || w->title[0] || y < BTN_Y || y >= BTN_Y + BTN_H) return false;
    const bool set = x >= SET_X;
    if (!set && x >= TR_MARGIN + BTN_W) return false;
    char path[180], line[160];
    const tr_wall_save_t r = tr_lock_wallpaper_save(src, source, author, n, path, sizeof(path));
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    w->shown = false;
    if (r == TR_WALL_FAILED) {
        notice(w, "没存下来", "图片还没下载完、超过 2 MB，或者 TF 卡写不进去", now_ms);
        return true;
    }
    if (set) {
        tr_lock_wallpaper_set(path);
        snprintf(line, sizeof(line), "%.120s\n下次锁屏就用它", base);
        notice(w, r == TR_WALL_EXISTS ? "已经存过，已设为锁屏" : "已存并设为锁屏", line, now_ms);
    } else {
        snprintf(line, sizeof(line), "%.100s\n到 设置 › 显示与阅读 › 锁屏壁纸 里选用", base);
        notice(w, r == TR_WALL_EXISTS ? "已经存过了" : "已存为壁纸", line, now_ms);
    }
    return true;
}

bool tr_wallsave_tick(tr_wallsave_t* w, int64_t now_ms) {
    if (!w->title[0] || now_ms < w->until_ms) return false;
    w->title[0] = 0;
    return true;
}

void tr_wallsave_open(tr_wallsave_t* w) {
    w->shown = true;
    w->title[0] = 0;
}

void tr_wallsave_close(tr_wallsave_t* w) {
    w->shown = false;
    w->title[0] = 0;
}
