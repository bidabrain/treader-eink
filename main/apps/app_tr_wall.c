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
 * 修订（1.2.8，用户要求）：右上角“手机上传”打开传书的网页服务（壁纸模式），手机浏览器把图缩成屏幕比例的灰度 JPEG
 * 传进 wallpapers；只收图片，点“完成”或离开这一页就停。原因：不用读卡器也能加壁纸。
 * Revision (1.2.8, user request): "Upload from phone" at the top right starts the transfer web service in wallpaper
 * mode; the phone's browser shrinks images to screen-ratio grayscale JPEGs and sends them into wallpapers; images
 * only, stopping on "Done" or leaving the page. Reason: add wallpapers without a card reader.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "app_loop.h"
#include "book_cover.h"
#include "book_store.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "read_pico_transfer.h"
#include "tomato.h"
#include "tr_lock.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"

#define WALL_DIR "/sdcard/wallpapers"
#define WALL_MAX 60
#define NAME_LEN 128
// 长度随显示大小放大（壁纸预览图本身不放大）；一页几行按翻页按钮上面剩下的高度算。/ Lengths scale with the
// display size (the wallpaper preview itself does not); rows per page follow the height left above the pager.
#define LIST_TOP TR_S(280)
#define ROW_H TR_S(132)
#define PAGER_Y (TR_H - TR_S(104))
#define ROWS ((PAGER_Y - TR_S(20) - LIST_TOP) / ROW_H)
#define BAND_Y (TR_H - TR_S(152))
#define TEXT_X (TR_MARGIN + TR_S(110))

// 右上角“手机上传”；上传界面的二维码不随显示大小放大（扫码不受影响）。/ "Upload from phone" at the top right;
// the upload view's QR code keeps its size (scanning is unaffected).
#define UPLOAD_RECT (EpdRect){TR_W - TR_MARGIN - TR_S(170), TR_S(78), TR_S(170), TR_S(56)}
#define UP_QR 220
#define UP_BOX_Y TR_S(180)
#define UP_POLL_MS 500

extern const app_desc_t app_tr_settings;
extern const char wall_upload_start[] asm("_binary_wall_upload_html_start");
extern const char wall_upload_end[] asm("_binary_wall_upload_html_end");

typedef enum { V_LIST, V_LOADING, V_PREVIEW, V_UPLOAD } view_t;

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

// 手机上传：网页服务的状态。/ Upload from phone: the web service state.
static bool s_up_on;
static book_store_root_t s_up_root;   // 算剩余空间用的 TF 卡根 / The TF card root used for free space
static read_pico_transfer_status_t s_up;
static int64_t s_up_poll_ms;

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

// 预览底栏三个按钮：放大后宽度收到一行放得下。/ The preview band's three buttons: at larger sizes their width
// shrinks so they fit one row.
static int band_btn_w(void) {
    const int fit = (TR_W - 2 * TR_MARGIN - 2 * TR_S(17)) / 3;
    return TR_S(190) < fit ? TR_S(190) : fit;
}

static int band_mid_x(void) {
    const int w = band_btn_w();
    return TR_MARGIN + w + (TR_W - 2 * TR_MARGIN - 3 * w) / 2;
}

static void free_preview(void) {
    heap_caps_free(s_preview);
    s_preview = NULL;
}

/* ---- 绘制 / Drawing ---- */

static void draw_list(uint8_t* fb) {
    tr_status_bar(fb);
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), "锁屏壁纸", EPD_DRAW_ALIGN_LEFT, true);
    if (!s_no_card) tr_pill(fb, UPLOAD_RECT, "手机上传", false, TR_S(28));
    char line[160];
    const char* cur = strrchr(s_current, '/');
    snprintf(line, sizeof(line), "当前：%.120s", s_current[0] ? (cur ? cur + 1 : s_current) : "书卡样式（不用壁纸）");
    tr_fit(line, TR_S(28), TR_W - 2 * TR_MARGIN - (s_current[0] ? TR_S(190) : 0));
    tr_text(fb, TR_MARGIN, TR_S(172), TR_S(28), line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_current[0])
        tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - TR_S(170), TR_S(160), TR_S(170), TR_S(56)}, "不用壁纸", false, TR_S(28));
    if (s_msg[0]) tr_text(fb, TR_MARGIN, TR_S(228), TR_S(26), s_msg, EPD_DRAW_ALIGN_LEFT, true);

    if (s_no_card) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP + TR_S(20), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46), "没有检测到 TF 卡。", 2);
        return;
    }
    if (!s_count) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP + TR_S(20), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "点右上角“手机上传”，用手机浏览器把图片传上来；也可以把 JPG 或 PNG 放进 TF 卡的 wallpapers 文件夹。建议竖图 684×1216，单张不超过 2 MB。", 5);
        return;
    }
    for (int row = 0; row < ROWS; row++) {
        int i = s_page * ROWS + row;
        if (i >= s_count) break;
        const int top = LIST_TOP + row * ROW_H;
        char path[NAME_LEN + 24];
        path_of(i, path, sizeof(path));
        tr_book_mini_cover(fb, (EpdRect){TR_MARGIN, top + TR_S(8), TR_S(87), TR_S(116)}, path, s_names[i], false);
        snprintf(line, sizeof(line), "%s", s_names[i]);
        tr_fit(line, TR_S(32), TR_W - TR_MARGIN - TEXT_X);
        tr_text(fb, TEXT_X, top + TR_S(22), TR_S(32), line, EPD_DRAW_ALIGN_LEFT, false);
        const bool current = !strcmp(path, s_current);
        snprintf(line, sizeof(line), "%.1f MB%s", s_sizes[i] / 1048576.0, current ? " · 当前壁纸" : "");
        if (current) tr_text(fb, TEXT_X, top + TR_S(72), TR_S(26), line, EPD_DRAW_ALIGN_LEFT, true);
        else tr_text_dim(fb, TEXT_X, top + TR_S(72), TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
        tr_hline(fb, top + ROW_H - 2, TEXT_X, TR_W - TR_MARGIN - TEXT_X, true);
    }
    tr_pager(fb, PAGER_Y, s_page, pages());
}

static void draw_preview(uint8_t* fb) {
    book_screen_draw(fb, s_preview);
    epd_fill_rect((EpdRect){0, BAND_Y, TR_W, TR_H - BAND_Y}, UI_GRAY_WHITE, fb);
    tr_hline(fb, BAND_Y, 0, TR_W, true);
    // 三个按钮：设为壁纸、删除（点一次变黑底“确认删除”）、返回。/ Three buttons: set, delete (one tap turns it into a
    // filled "confirm"), back.
    const int w = band_btn_w(), y = BAND_Y + TR_S(40), h = TR_S(80), px = TR_S(30);
    tr_pill(fb, (EpdRect){TR_MARGIN, y, w, h}, "设为壁纸", true, px);
    tr_pill(fb, (EpdRect){band_mid_x(), y, w, h}, s_confirm_del ? "确认删除" : "删除", s_confirm_del, px);
    tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - w, y, w, h}, "返回", false, px);
}

/* ---- 手机上传 / Upload from phone ---- */

static uint64_t up_free(void* ctx) { return book_store_upload_free_bytes(ctx); }

static void up_qr(void) {
    if (!s_up.network_ready) return;
    if (s_up.mode == READ_PICO_TRANSFER_MODE_AP) ui_wifi_qr_prepare(s_up.ssid, READ_PICO_TRANSFER_PASSWORD);
    else ui_wifi_qr_prepare_url(s_up.url);
}

// 有保存的 Wi-Fi 就连它，否则开设备热点。/ Join the saved Wi-Fi if any, else open the device hotspot.
static app_redraw_t up_start(void) {
    if (tomato_sync_preempt() != TOMATO_PREEMPT_FREE) {
        snprintf(s_msg, sizeof(s_msg), "后台正在下载或同步，完成后再试");
        return APP_REDRAW_PAGE;
    }
    if (book_store_upload_root(&s_up_root) != ESP_OK || s_up_root.is_flash) {
        snprintf(s_msg, sizeof(s_msg), "没有 TF 卡，传不了壁纸");
        return APP_REDRAW_PAGE;
    }
    bool configured = false;
    char ssid[33];
    read_pico_transfer_get_saved_wifi(ssid, &configured);
    const read_pico_transfer_cfg_t cfg = {
        .mode = configured ? READ_PICO_TRANSFER_MODE_STA : READ_PICO_TRANSFER_MODE_AP,
        .root_dir = WALL_DIR,
        .file_limit = 4 * 1024 * 1024,
        .free_bytes_cb = up_free,
        .free_bytes_ctx = &s_up_root,
        .images = true,
        .page_html = wall_upload_start,
        .page_html_len = (size_t)(wall_upload_end - wall_upload_start - 1),
    };
    mkdir(WALL_DIR, 0775);
    // 后台任务做完还留着的 Wi-Fi 先断开，热点才开得起来。/ Drop Wi-Fi lingering after a background job so the hotspot
    // can start.
    tomato_net_idle_off();
    const esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "打不开网页服务（%s）", esp_err_to_name(err));
        return APP_REDRAW_PAGE;
    }
    s_up_on = true;
    memset(&s_up, 0, sizeof(s_up));
    read_pico_transfer_get_status(&s_up);
    up_qr();
    app_loop_hold_awake(true);
    s_msg[0] = 0;
    s_view = V_UPLOAD;
    return APP_REDRAW_PAGE;
}

static void up_stop(void) {
    if (!s_up_on) return;
    read_pico_transfer_stop();
    ui_wifi_qr_clear();
    s_up_on = false;
    app_loop_hold_awake(false);
}

// 停下服务回列表，告诉收到几张。/ Stop the service, back to the list, saying how many arrived.
static app_redraw_t up_done(void) {
    read_pico_transfer_get_status(&s_up);
    const unsigned got = s_up.done_count;
    up_stop();
    scan();
    s_page = 0;
    if (got) snprintf(s_msg, sizeof(s_msg), "收到 %u 张新壁纸", got);
    else s_msg[0] = 0;
    s_view = V_LIST;
    return APP_REDRAW_PAGE;
}

static EpdRect up_done_btn(void) {
    return (EpdRect){TR_MARGIN, TR_H - TR_S(150), TR_W - 2 * TR_MARGIN, TR_S(80)};
}

static void draw_upload(uint8_t* fb) {
    tr_status_bar(fb);
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), "手机上传壁纸", EPD_DRAW_ALIGN_LEFT, true);
    const int bx = TR_MARGIN, by = UP_BOX_Y, bw = TR_W - 2 * TR_MARGIN, pad = TR_S(28);
    char line[200];
    int y = by + pad;
    if (!s_up.network_ready) {
        y += tr_wrap(fb, bx + pad, y, bw - 2 * pad, TR_S(28), TR_S(42),
                     s_up.state == READ_PICO_TRANSFER_ERROR ? "连不上 Wi-Fi。检查设置 › Wi-Fi 里保存的网络，或点“完成”再试。"
                                                           : "正在打开网页服务…", 4) * TR_S(42);
    } else {
        const EpdRect qr = {bx + pad, y, UP_QR, UP_QR};
        ui_wifi_qr_draw(fb, qr);
        const int tx = qr.x + qr.width + TR_S(20), tw = bx + bw - pad - tx;
        const bool sta = s_up.mode == READ_PICO_TRANSFER_MODE_STA;
        if (sta) snprintf(line, sizeof(line), "手机连上同一个 Wi-Fi（%s），扫码或在浏览器打开：", s_up.wifi_ssid);
        else snprintf(line, sizeof(line), "扫码连上设备热点 %s（口令 %s），再在浏览器打开：", s_up.ssid,
                      READ_PICO_TRANSFER_PASSWORD);
        const int n = tr_wrap(fb, tx, y + TR_S(8), tw, TR_S(26), TR_S(38), line, 6);
        // 网址是内容：放不下就把字号缩到放得下。/ The address is content: shrink its size until it fits.
        const char* url = sta ? s_up.url : READ_PICO_TRANSFER_URL;
        const char* host = strstr(url, "://") ? strstr(url, "://") + 3 : url;
        int px = TR_S(32);
        while (px > 24 && ttf_text_width_px(px, host) > tw) px -= 2;
        const int uy = y + TR_S(8) + n * TR_S(38) + TR_S(8);
        tr_text(fb, tx, uy, px, host, EPD_DRAW_ALIGN_LEFT, true);
        y = uy + TR_S(56) > qr.y + qr.height + TR_S(16) ? uy + TR_S(56) : qr.y + qr.height + TR_S(16);
    }
    tr_hline(fb, y, bx + pad, bw - 2 * pad, true);
    y += TR_S(20);
    if (s_up.state == READ_PICO_TRANSFER_UPLOADING && s_up.cur_name[0]) {
        snprintf(line, sizeof(line), "正在接收：%s", s_up.cur_name);
        tr_fit(line, TR_S(28), bw - 2 * pad);
    } else if (s_up.done_count) {
        snprintf(line, sizeof(line), "已收到 %u 张", s_up.done_count);
    } else {
        snprintf(line, sizeof(line), "等待手机上传…");
    }
    tr_text(fb, bx + pad, y, TR_S(28), line, EPD_DRAW_ALIGN_LEFT, s_up.done_count > 0);
    y += TR_S(52);
    y += tr_wrap(fb, bx + pad, y, bw - 2 * pad, TR_S(24), TR_S(36),
                 "网页会把图片缩成屏幕大小的灰度图再传，可选铺满（裁边）或完整（留白）。传完点“完成”，在列表里点一张设为壁纸。"
                 "离开这一页就停止。", 6) * TR_S(36);
    const EpdRect b = {bx, by, bw, y + TR_S(20) - by};
    ui_draw_round_rect(fb, b, TR_S(24), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(b, 1), TR_S(24) - 1, TR_GRAY_OUTLINE);
    tr_pill(fb, up_done_btn(), "完成", true, TR_S(32));
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_view == V_UPLOAD) draw_upload(fb);
    else if (s_view == V_PREVIEW && s_preview) draw_preview(fb);
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
    if (s_view == V_UPLOAD) {
        if ((y < TR_S(160) && x < TR_S(120)) || ui_rect_hit(up_done_btn(), x, y)) return up_done();
        return APP_REDRAW_NONE;
    }
    if (s_view == V_PREVIEW) {
        if (y < BAND_Y + TR_S(20)) {
            // 点图片别处取消“确认删除”。/ Tapping the image cancels a pending delete.
            if (!s_confirm_del) return APP_REDRAW_NONE;
            s_confirm_del = false;
            return APP_REDRAW_PAGE;
        }
        const int mid = band_mid_x();
        if (x >= mid - 10 && x < mid + band_btn_w() + 10) {
            if (s_confirm_del) return delete_selected();
            s_confirm_del = true;
            return APP_REDRAW_PAGE;
        }
        if (x < mid - 10) {
            char path[NAME_LEN + 24];
            path_of(s_sel, path, sizeof(path));
            tr_lock_wallpaper_set(path);
            snprintf(s_current, sizeof(s_current), "%s", path);
            snprintf(s_msg, sizeof(s_msg), "已设为锁屏壁纸");
        }
        return back_to_list();
    }
    if (y < TR_S(160) && x < TR_S(120)) { leave(ctx); return APP_REDRAW_NONE; }
    if (!s_no_card && ui_rect_hit(UPLOAD_RECT, x, y)) return up_start();
    s_msg[0] = 0;
    if (s_current[0] && y >= TR_S(150) && y < TR_S(226) && x > TR_W - TR_MARGIN - TR_S(190)) {
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
    if (s_view == V_UPLOAD) return up_done();
    if (s_view == V_PREVIEW) return back_to_list();
    leave(ctx);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    tr_time_poll();
    if (s_view == V_UPLOAD) {
        // 网络好了就做二维码；收到新图、开始或结束接收时重画。/ Make the QR code once the network is up; redraw
        // when a picture arrives or a receive starts or ends.
        if (ctx->now_ms - s_up_poll_ms < UP_POLL_MS) return APP_REDRAW_NONE;
        s_up_poll_ms = ctx->now_ms;
        const read_pico_transfer_status_t old = s_up;
        read_pico_transfer_get_status(&s_up);
        if (s_up.network_ready && !old.network_ready) up_qr();
        const bool changed = s_up.network_ready != old.network_ready || s_up.state != old.state ||
                             s_up.done_count != old.done_count || strcmp(s_up.cur_name, old.cur_name) ||
                             strcmp(s_up.url, old.url);
        return changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
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
    up_stop();
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
