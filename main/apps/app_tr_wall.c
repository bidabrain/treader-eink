/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏壁纸：只列 TF 卡 wallpapers 文件夹里的 JPG/PNG，点一张先全屏预览，再设为壁纸、删除（点两次）或返回。
 * 选中的路径存 NVS，锁屏时读它（见 tr_lock.c）。从设置页进入，不在菜单里。
 *
 * Lock wallpaper: lists only JPG/PNG in the TF card's wallpapers folder; tapping one shows a full-screen preview,
 * then sets it, deletes it (two taps) or goes back. The choice lives in NVS and the lock screen reads it (see
 * tr_lock.c). Opened from Settings; not in the menu.
 *
 * 冻结：不扫描其他目录；解码只在 on_tick 里做，render 只画缓存。
 * Frozen: never scan other folders; decode only in on_tick, render paints caches only.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "book_cover.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "tr_lock.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define WALL_DIR "/sdcard/wallpapers"
#define WALL_MAX 60
#define NAME_LEN 128
#define LIST_TOP 280
#define ROW_H 132
#define ROWS 6
#define PAGER_Y 1112
#define BAND_Y 1064

extern const app_desc_t app_tr_settings;

typedef enum { V_LIST, V_LOADING, V_PREVIEW } view_t;

EXT_RAM_BSS_ATTR static char s_names[WALL_MAX][NAME_LEN];
static uint32_t s_sizes[WALL_MAX];
static int s_count;
static int s_page;
static int s_sel = -1;
static view_t s_view;
static bool s_no_card;
static char s_msg[64];
static char s_current[288];
static uint8_t* s_preview;   // 预览用全屏缓冲（PSRAM）/ Full-screen preview buffer (PSRAM)
static bool s_confirm_del;   // “删除”点过一次，再点才删 / "Delete" tapped once; the next tap deletes

/* ---- 数据 / Data ---- */

static void path_of(int i, char* out, size_t cap) {
    snprintf(out, cap, WALL_DIR "/%.127s", s_names[i]);
}

static int name_cmp(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void scan(void) {
    s_count = 0;
    mkdir(WALL_DIR, 0775);   // 第一次进来顺手建好文件夹 / Create the folder on first visit
    DIR* d = opendir(WALL_DIR);
    s_no_card = d == NULL;
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && s_count < WALL_MAX) {
        if (e->d_name[0] == '.' || !book_image_name(e->d_name) || strlen(e->d_name) >= NAME_LEN) continue;
        snprintf(s_names[s_count++], NAME_LEN, "%s", e->d_name);
    }
    closedir(d);
    qsort(s_names, (size_t)s_count, NAME_LEN, name_cmp);
    for (int i = 0; i < s_count; i++) {
        char path[NAME_LEN + 24];
        struct stat st;
        path_of(i, path, sizeof(path));
        s_sizes[i] = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    }
}

static int pages(void) {
    return s_count ? (s_count + ROWS - 1) / ROWS : 1;
}

static void free_preview(void) {
    heap_caps_free(s_preview);
    s_preview = NULL;
}

/* ---- 绘制 / Drawing ---- */

static void draw_list(uint8_t* fb) {
    tr_status_bar(fb);
    tr_text(fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 96, 100, 36, "锁屏壁纸", EPD_DRAW_ALIGN_LEFT, true);
    char line[160];
    const char* cur = strrchr(s_current, '/');
    snprintf(line, sizeof(line), "当前：%.120s", s_current[0] ? (cur ? cur + 1 : s_current) : "书卡样式（不用壁纸）");
    tr_fit(line, 28, TR_W - 2 * TR_MARGIN - (s_current[0] ? 190 : 0));
    tr_text(fb, TR_MARGIN, 172, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_current[0]) tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - 170, 160, 170, 56}, "不用壁纸", false, 28);
    if (s_msg[0]) tr_text(fb, TR_MARGIN, 228, 26, s_msg, EPD_DRAW_ALIGN_LEFT, true);

    if (s_no_card) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP + 20, TR_W - 2 * TR_MARGIN, 30, 46, "没有检测到 TF 卡。", 2);
        return;
    }
    if (!s_count) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP + 20, TR_W - 2 * TR_MARGIN, 30, 46,
                "把 JPG 或 PNG 图片放进 TF 卡的 wallpapers 文件夹，再回到这里。建议竖图 684×1216，单张不超过 2 MB。", 5);
        return;
    }
    for (int row = 0; row < ROWS; row++) {
        int i = s_page * ROWS + row;
        if (i >= s_count) break;
        const int top = LIST_TOP + row * ROW_H;
        char path[NAME_LEN + 24];
        path_of(i, path, sizeof(path));
        tr_book_mini_cover(fb, (EpdRect){TR_MARGIN, top + 8, 87, 116}, path, s_names[i], false);
        snprintf(line, sizeof(line), "%s", s_names[i]);
        tr_fit(line, 32, TR_W - TR_MARGIN - 150);
        tr_text(fb, 150, top + 22, 32, line, EPD_DRAW_ALIGN_LEFT, false);
        const bool current = !strcmp(path, s_current);
        snprintf(line, sizeof(line), "%.1f MB%s", s_sizes[i] / 1048576.0, current ? " · 当前壁纸" : "");
        tr_text(fb, 150, top + 72, 26, line, EPD_DRAW_ALIGN_LEFT, current);
        tr_hline(fb, top + ROW_H - 2, 150, TR_W - TR_MARGIN - 150, true);
    }
    tr_pager(fb, PAGER_Y, s_page, pages());
}

static void draw_preview(uint8_t* fb) {
    book_screen_draw(fb, s_preview);
    epd_fill_rect((EpdRect){0, BAND_Y, TR_W, TR_H - BAND_Y}, UI_GRAY_WHITE, fb);
    tr_hline(fb, BAND_Y, 0, TR_W, false);
    // 三个按钮：设为壁纸、删除（点一次变黑底“确认删除”）、返回。/ Three buttons: set, delete (one tap turns it into a
    // filled "confirm"), back.
    tr_pill(fb, (EpdRect){TR_MARGIN, BAND_Y + 40, 190, 80}, "设为壁纸", true, 30);
    tr_pill(fb, (EpdRect){TR_MARGIN + 207, BAND_Y + 40, 190, 80}, s_confirm_del ? "确认删除" : "删除", s_confirm_del, 30);
    tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - 190, BAND_Y + 40, 190, 80}, "返回", false, 30);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_view == V_PREVIEW && s_preview) draw_preview(fb);
    else draw_list(fb);
}

/* ---- 事件 / Events ---- */

static void leave(app_ctx_t* ctx) {
    ctx->request_app = &app_tr_settings;
}

static app_redraw_t back_to_list(void) {
    free_preview();
    s_confirm_del = false;
    s_view = V_LIST;
    return APP_REDRAW_FULL;
}

// 删掉预览中的这张；是当前壁纸就改回书卡样式。它的全屏缓存离页时一起清掉。/ Delete the previewed image; if it is
// the current wallpaper, go back to the book card. Its screen cache is pruned on exit.
static app_redraw_t delete_selected(void) {
    char path[NAME_LEN + 24];
    path_of(s_sel, path, sizeof(path));
    if (remove(path) != 0) {
        snprintf(s_msg, sizeof(s_msg), "删不掉这张图，检查 TF 卡");
        return back_to_list();
    }
    if (!strcmp(path, s_current)) {
        tr_lock_wallpaper_set(NULL);
        s_current[0] = 0;
        snprintf(s_msg, sizeof(s_msg), "已删除，锁屏改回书卡样式");
    } else {
        snprintf(s_msg, sizeof(s_msg), "已删除");
    }
    scan();
    if (s_page >= pages()) s_page = pages() - 1;
    return back_to_list();
}

static app_redraw_t turn_page(int turn) {
    if (pages() <= 1) return APP_REDRAW_NONE;
    s_page = (s_page + turn + pages()) % pages();
    book_cover_drop_pending();
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap(app_ctx_t* ctx, int x, int y) {
    if (s_view == V_LOADING) return APP_REDRAW_NONE;
    if (s_view == V_PREVIEW) {
        if (y < BAND_Y + 20) {
            // 点图片别处取消“确认删除”。/ Tapping the image cancels a pending delete.
            if (!s_confirm_del) return APP_REDRAW_NONE;
            s_confirm_del = false;
            return APP_REDRAW_PAGE;
        }
        if (x >= TR_MARGIN + 197 && x < TR_MARGIN + 407) {
            if (s_confirm_del) return delete_selected();
            s_confirm_del = true;
            return APP_REDRAW_PAGE;
        }
        if (x < TR_MARGIN + 197) {
            char path[NAME_LEN + 24];
            path_of(s_sel, path, sizeof(path));
            tr_lock_wallpaper_set(path);
            snprintf(s_current, sizeof(s_current), "%s", path);
            snprintf(s_msg, sizeof(s_msg), "已设为锁屏壁纸");
        }
        return back_to_list();
    }
    if (y < 160 && x < 120) { leave(ctx); return APP_REDRAW_NONE; }
    s_msg[0] = 0;
    if (s_current[0] && y >= 150 && y < 226 && x > TR_W - TR_MARGIN - 190) {
        tr_lock_wallpaper_set(NULL);
        s_current[0] = 0;
        snprintf(s_msg, sizeof(s_msg), "锁屏改回书卡样式");
        return APP_REDRAW_PAGE;
    }
    int turn = tr_pager_hit(PAGER_Y, (uint16_t)x, (uint16_t)y);
    if (turn) return turn_page(turn);
    if (y >= LIST_TOP && (y - LIST_TOP) / ROW_H < ROWS) {
        int i = s_page * ROWS + (y - LIST_TOP) / ROW_H;
        if (i >= s_count) return APP_REDRAW_NONE;
        s_sel = i;
        s_view = V_LOADING;
        snprintf(s_msg, sizeof(s_msg), "正在处理图片，请稍候…");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

// 松手才算轻点；列表左滑 / 上滑下一页，右滑 / 下滑上一页。/ Taps on release; list swipes turn whole pages.
static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    if (s_view != V_LIST) return APP_REDRAW_NONE;
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U) return turn_page(1);
    if (ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D) return turn_page(-1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1 || s_view == V_LOADING) return APP_REDRAW_NONE;
    if (s_view == V_PREVIEW) return back_to_list();
    leave(ctx);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    tr_time_poll();
    if (s_view == V_LOADING) {
        // 第一次选这张图时做全屏缓存，之后锁屏直接读。/ Build the full-screen cache once; the lock reads it later.
        char path[NAME_LEN + 24];
        path_of(s_sel, path, sizeof(path));
        free_preview();
        if (book_screen_build(path)) s_preview = book_screen_load(path);
        if (s_preview) {
            s_msg[0] = 0;
            s_confirm_del = false;
            s_view = V_PREVIEW;
            return APP_REDRAW_FULL;
        }
        snprintf(s_msg, sizeof(s_msg), "这张图打不开：太大（超过 2 MB）或格式不支持");
        s_view = V_LIST;
        return APP_REDRAW_PAGE;
    }
    if (s_view == V_LIST && !(ctx->touch && ctx->touch->touched) && book_cover_tick()) return APP_REDRAW_PAGE;
    return APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_view = V_LIST;
    s_page = 0;
    s_sel = -1;
    s_msg[0] = 0;
    free_preview();
    book_cover_drop_pending();
    if (!tr_lock_wallpaper_get(s_current, sizeof(s_current))) s_current[0] = 0;
    scan();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    // 预览过但没选的图，缓存（每张约 400 KB）离开时删掉。/ Drop caches (~400 KB each) of previewed but unchosen images.
    book_screen_prune(s_current[0] ? s_current : NULL);
    free_preview();
    book_cover_drop_pending();
    s_view = V_LIST;
}

const app_desc_t app_tr_wall = {
    .title = "锁屏壁纸",
    .detail = "TF 卡 wallpapers 文件夹里的图片",
    .enter_full = true,
    .tab_nav = true,   // 预览底部的按钮不让给菜单把手 / Keep the preview's bottom buttons away from the menu handle
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
};
