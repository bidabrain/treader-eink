/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 书城里的微信读书：设置 › 内容来源 › 微信读书打开后，书城顶上多出“番茄 / 微信读书”切换（只开一个来源时没有切换，
 * 直接进那个）。第一次进来先看说明并确认；之后扫码登录 → 同步书架 → 书架封面网格 → 详情 → 整本下载成 EPUB 到书库。
 * 协议引擎是 components/read_pico_weread（来自 CrossMux），联网都在 tomato 后台任务里，这里只读快照和书架缓存。
 *
 * WeRead in the bookstore: once Settings > Sources > WeRead is on, the store gets a "番茄 / 微信读书" switch at the top
 * (with one source there is no switch and "书城" opens it directly). The first visit shows a notice to accept; then
 * QR sign-in -> shelf sync -> shelf cover grid -> detail -> whole-book EPUB into the library.
 * The protocol engine is components/read_pico_weread (from CrossMux); all networking runs on the tomato background
 * task and this page only reads the snapshot and the cached shelf.
 *
 * 冻结：只在用户点“同步”“扫码登录”“下载”时联网，不自动同步；列表只翻页不滚动；不上传进度。
 * Frozen: the network is used only on "Sync", "Sign in" or "Download", never automatically; lists page, never
 * scroll; no progress is uploaded.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "book_cover.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mem_reclaim.h"
#include "read_pico_weread.h"
#include "settings.h"
#include "tomato.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"

#define TAG "app_weread"
#define POLL_MS 400
// 下载和同步时进度最多这么久重画一次：整页刷屏会和解码抢 CPU 与 PSRAM。/ Progress redraws at most this often while
// downloading or syncing: a whole-page refresh competes with decoding for CPU and PSRAM.
#define BUSY_REDRAW_MS 3000
// 开始联网前 PSRAM 最大连续块至少这么大（TLS 连接放 PSRAM），不够先放掉能重建的缓存。/ The largest PSRAM block
// wanted before going online (TLS lives in PSRAM); below it, rebuildable caches are dropped first.
#define PSRAM_HEADROOM (256 * 1024)
#define CELL_W 150
#define CELL_COVER_H 200
#define CELL_TEXT_H TR_S(84)
#define GRID_COLS 3
#define GRID_TOP TR_S(250)
#define CELL_GAP ((TR_W - 2 * TR_MARGIN - GRID_COLS * CELL_W) / (GRID_COLS - 1))
#define PAGER_Y (TR_TABS_Y - TR_S(54))
// 顶上“‹ 书城”右边的来源切换。/ The source switch right of "‹ 书城" at the top.
#define SRC_SEG (EpdRect){TR_W - TR_MARGIN - TR_S(320), TR_S(84), TR_S(320), TR_S(64)}
#define STATUS_Y TR_S(186)
#define ACTION_RECT (EpdRect){TR_W - TR_MARGIN - TR_S(170), TR_S(170), TR_S(170), TR_S(64)}
#define CENTER_BTN (EpdRect){TR_MARGIN, TR_S(520), TR_W - 2 * TR_MARGIN, TR_S(84)}
#define QR_SIZE 360   // 二维码和图片一样不随显示大小放大 / The QR keeps its size like images do
#define COVER_RECT (EpdRect){TR_MARGIN, TR_S(180), 220, 296}
#define DETAIL_BTN (EpdRect){TR_MARGIN, TR_H - 80 - TR_S(26 + 26 + 84), TR_W - 2 * TR_MARGIN, TR_S(84)}

extern const app_desc_t app_book;
extern const app_desc_t app_tr_store;
extern const app_desc_t app_tr_weread;

typedef enum { V_NOTICE, V_SHELF, V_QR, V_DETAIL } view_t;
typedef enum { J_NONE, J_SYNC, J_DOWNLOAD } job_t;

static view_t s_view;
static job_t s_job, s_last;
static uint32_t s_job_seq;
static tomato_status_t s_status;
static weread_snapshot_t s_snap;
static char s_qr_url[sizeof(s_snap.qr)];
static uint32_t s_seen_rev;
static int64_t s_poll_ms, s_busy_draw_ms;
static char s_note[96];      // 开始不了时的原因 / Why something could not start
static bool s_tomato_on;     // 番茄服务器设了没有（决定有没有来源切换）/ Tomato server set (decides the switch)

static int s_count;          // 书架总本数 / Shelf size
static int s_page;
static weread_book_t* s_books;   // 本页的书（PSRAM）/ This page's books (PSRAM)
static int s_page_n;
static weread_book_t s_detail;
static char s_dl_id[sizeof(s_detail.id)];

/* ---- 书城入口 / Store entry ---- */

static bool s_last_weread;   // 上次停在哪个来源 / Which source was used last

// 书库的“书城”打开哪一页：只有一个来源就是它；两个都有就回到上次用的。/ The page "书城" opens: the only source, or
// the one used last when both exist.
const app_desc_t* tr_store_entry(bool tomato_on) {
    if (!app_settings_weread()) return &app_tr_store;
    if (!tomato_on) return &app_tr_weread;
    return s_last_weread ? &app_tr_weread : &app_tr_store;
}

// 记下停在哪个来源，下次“书城”回到这里。/ Remember the source in use so "书城" returns to it.
void tr_store_mark(bool weread) { s_last_weread = weread; }

// 两个来源都有时在标题行右边画切换。/ With both sources, draw the switch at the right of the title row.
void tr_store_draw_source(uint8_t* fb, bool tomato_on, bool weread) {
    if (!tomato_on || !app_settings_weread()) return;
    static const char* const LABELS[2] = {"番茄", "微信读书"};
    tr_segment(fb, SRC_SEG, LABELS, 2, weread ? 1 : 0);
}

// 点了切换就换页，返回 true。/ Switch pages on a tap of the switch; true when handled.
bool tr_store_source_hit(app_ctx_t* ctx, bool tomato_on, bool weread, uint16_t x, uint16_t y) {
    if (!tomato_on || !app_settings_weread()) return false;
    const int seg = tr_segment_hit(SRC_SEG, 2, x, y);
    if (seg < 0) return false;
    s_last_weread = seg == 1;
    if ((seg == 1) != weread) ctx->request_app = seg == 1 ? &app_tr_weread : &app_tr_store;
    return true;
}

/* ---- 数据 / Data ---- */

static int grid_rows(int* cover_h) {
    const int room = PAGER_Y - (GRID_TOP + TR_S(10));
    int rows = 3, h = room / 3 - CELL_TEXT_H;
    if (h < 160) {
        rows = 2;
        h = room / 2 - CELL_TEXT_H;
    }
    if (cover_h) *cover_h = h < CELL_COVER_H ? h : CELL_COVER_H;
    return rows;
}

static int grid_per(void) { return grid_rows(NULL) * GRID_COLS; }

static int pages(void) { return s_count > 0 ? (s_count + grid_per() - 1) / grid_per() : 1; }

static EpdRect cell_rect(int k) {
    int h;
    grid_rows(&h);
    return (EpdRect){TR_MARGIN + (k % GRID_COLS) * (CELL_W + CELL_GAP),
                     GRID_TOP + TR_S(10) + (k / GRID_COLS) * (h + CELL_TEXT_H), h * 3 / 4, h};
}

// 读书架本数和当前页的书。/ Read the shelf size and this page's books.
static void load_page(void) {
    s_count = weread_shelf_count();
    if (s_page >= pages()) s_page = pages() - 1;
    if (s_page < 0) s_page = 0;
    s_page_n = 0;
    const int per = grid_per();
    for (int k = 0; k < per && s_books; k++) {
        const int i = s_page * per + k;
        if (i >= s_count || !weread_shelf_get(i, &s_books[k])) break;
        s_page_n++;
    }
}

static bool job_running(void) { return s_job != J_NONE && tomato_sync_busy() && tomato_sync_seq() == s_job_seq; }

// 刚结束的是本页的这个任务、之后也没有别的任务开始。/ The job that just ended is ours and nothing started since.
static bool last_is(job_t j) { return s_last == j && tomato_sync_seq() == s_job_seq; }

// 主任务上、开始联网前调：PSRAM 紧就先放缓存。/ Called on the main task before going online: drop caches when PSRAM
// is tight.
static void ensure_headroom(void) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < PSRAM_HEADROOM) mem_reclaim("weread start");
}

static bool start_job(job_t j, esp_err_t err) {
    s_note[0] = 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start job %d: %s", j, esp_err_to_name(err));
        snprintf(s_note, sizeof(s_note), "%s", err == ESP_ERR_INVALID_STATE ? "后台正在下载或同步，完成后再试" : "开始失败");
        return false;
    }
    s_job = j;
    s_job_seq = tomato_sync_seq();
    s_last = J_NONE;
    s_seen_rev = 0;
    tomato_sync_get_status(&s_status);
    return true;
}

// 后台空着就开始；在跑浏览类任务就让它停下（最多等一会儿）。/ Start when idle; stop a browsing job first (briefly).
static bool make_room(void) {
    if (!tomato_sync_busy()) return true;
    if (tomato_sync_preempt() == TOMATO_PREEMPT_FREE) return true;
    snprintf(s_note, sizeof(s_note), "%s", "后台正在下载或同步，完成后再试");
    return false;
}

static void start_sync(void) {
    if (!make_room()) return;
    ensure_headroom();
    start_job(J_SYNC, weread_start_sync());
}

static void start_download(void) {
    if (!make_room()) return;
    ensure_headroom();
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", s_detail.id);
    start_job(J_DOWNLOAD, weread_start_download(s_detail.id, app_settings_weread_epub()));
}

/* ---- 绘制 / Drawing ---- */

static void draw_header(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_S(96), TR_S(96), TR_S(40), "书城", EPD_DRAW_ALIGN_LEFT, true);
    tr_store_draw_source(fb, s_tomato_on, true);
}

static void draw_notice(uint8_t* fb) {
    draw_header(fb);
    tr_text(fb, TR_MARGIN, TR_S(190), TR_S(36), "微信读书", EPD_DRAW_ALIGN_LEFT, true);
    const int lh = TR_S(46);
    int lines = tr_wrap(fb, TR_MARGIN, TR_S(256), TR_W - 2 * TR_MARGIN, TR_S(30), lh,
                        "用手机微信扫码登录后，可以同步你在微信读书里的书架，把书整本下载成 EPUB 放进书库，离线阅读。", 4);
    lines += tr_wrap(fb, TR_MARGIN, TR_S(256) + (lines + 1) * lh, TR_W - 2 * TR_MARGIN, TR_S(30), lh,
                     "这里用的是微信读书网页版的非官方接口，微信读书改版后可能用不了；只能下你的账号能读的书。"
                     "设备只读取，不会上传阅读进度和笔记。", 5) + 1;
    tr_wrap(fb, TR_MARGIN, TR_S(256) + (lines + 1) * lh, TR_W - 2 * TR_MARGIN, TR_S(26), TR_S(40),
            "协议引擎来自开源项目 CrossMux（MIT 许可），感谢原作者。", 2);
    tr_pill(fb, DETAIL_BTN, "同意并继续", true, TR_S(32));
}

static const char* stage_text(void) {
    if (s_status.phase == TOMATO_CONNECTING) return "正在连接 Wi-Fi";
    switch (s_snap.stage) {
    case WEREAD_STAGE_QR: return "等待扫码";
    case WEREAD_STAGE_SHELF: return "正在同步书架";
    case WEREAD_STAGE_COVERS: return "正在取封面";
    case WEREAD_STAGE_CHAPTERS: return "正在下载章节";
    case WEREAD_STAGE_IMAGES: return "正在下载插图";
    case WEREAD_STAGE_PACKAGING: return "正在打包 EPUB";
    default: return s_job == J_DOWNLOAD ? "正在下载" : "正在同步";
    }
}

// 带进度的一句话。/ A line with progress.
static void busy_line(char* out, size_t cap) {
    if (s_snap.total) snprintf(out, cap, "%s %u / %u", stage_text(), (unsigned)s_snap.done, (unsigned)s_snap.total);
    else snprintf(out, cap, "%s…", stage_text());
}

static int busy_pct(void) {
    if (!s_snap.total) return 10;
    const int pct = (int)((uint64_t)s_snap.done * 100 / s_snap.total);
    return pct > 99 ? 99 : pct;
}

static void draw_shelf(uint8_t* fb) {
    draw_header(fb);
    char line[200];
    const bool syncing = job_running() && s_job == J_SYNC;
    if (syncing) busy_line(line, sizeof(line));
    else if (s_note[0]) snprintf(line, sizeof(line), "%s", s_note);
    else if (last_is(J_SYNC) && s_status.phase == TOMATO_FAILED) snprintf(line, sizeof(line), "%s", s_status.message);
    else if (s_count > 0) snprintf(line, sizeof(line), "书架 %d 本", s_count);
    else snprintf(line, sizeof(line), "%s", weread_logged_in() ? "还没同步书架" : "还没登录");
    tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - TR_S(12), TR_W - 2 * TR_MARGIN - TR_S(190), TR_S(28), line,
               EPD_DRAW_ALIGN_LEFT, false);
    const EpdRect act = ACTION_RECT;
    if (syncing) {
        tr_text(fb, TR_W - TR_MARGIN, act.y + TR_S(16), TR_S(30), "取消", EPD_DRAW_ALIGN_RIGHT, true);
    } else if (weread_logged_in()) {
        const int off = ttf_text_width_px(TR_S(30), "同步") + TR_S(30);
        tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - (off > TR_S(96) ? off : TR_S(96)), act.y + TR_S(34), false);
        tr_text(fb, TR_W - TR_MARGIN, act.y + TR_S(16), TR_S(30), "同步", EPD_DRAW_ALIGN_RIGHT, false);
    }
    tr_hline(fb, TR_S(240), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    if (syncing) tr_progress(fb, TR_MARGIN, TR_S(232), TR_W - 2 * TR_MARGIN, busy_pct());
    if (!weread_logged_in() && !syncing) {
        tr_wrap(fb, TR_MARGIN, TR_S(300), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "点下面的按钮，设备上会出现二维码，用手机微信扫一扫并在手机上确认。登录后自动同步书架。", 4);
        tr_pill(fb, CENTER_BTN, "扫码登录", true, TR_S(32));
        return;
    }
    if (s_count <= 0) {
        if (!syncing)
            tr_wrap(fb, TR_MARGIN, TR_S(300), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                    "点右上角“同步”取你在微信读书里的书架。想下的书先在手机微信读书里加到书架。", 4);
        return;
    }
    for (int k = 0; k < s_page_n; k++) {
        const weread_book_t* b = &s_books[k];
        const EpdRect r = cell_rect(k);
        tr_book_cover(fb, r, b->cover[0] ? b->cover : NULL, b->title, "", "", false);
        snprintf(line, sizeof(line), "%.*s", (int)sizeof(b->title) - 1, b->title);
        tr_fit(line, TR_S(24), CELL_W);
        tr_text(fb, r.x, r.y + r.height + TR_S(8), TR_S(24), line, EPD_DRAW_ALIGN_LEFT, true);
        // 已下载加粗黑字，否则作者用深灰。/ "已下载" is bold black; otherwise the author in dark gray.
        if (b->local[0]) {
            tr_text(fb, r.x, r.y + r.height + TR_S(46), TR_S(22), "已下载", EPD_DRAW_ALIGN_LEFT, true);
        } else {
            snprintf(line, sizeof(line), "%.*s", (int)sizeof(b->author) - 1, b->author[0] ? b->author : "—");
            tr_fit(line, TR_S(22), CELL_W);
            tr_text_dim(fb, r.x, r.y + r.height + TR_S(46), TR_S(22), line, EPD_DRAW_ALIGN_LEFT);
        }
    }
    tr_pager(fb, PAGER_Y, s_page, pages());
}

static void draw_qr(uint8_t* fb) {
    draw_header(fb);
    tr_text(fb, TR_W / 2, TR_S(200), TR_S(36), "微信扫码登录", EPD_DRAW_ALIGN_CENTER, true);
    const EpdRect qr = {(TR_W - QR_SIZE) / 2, TR_S(270), QR_SIZE, QR_SIZE};
    ui_wifi_qr_draw(fb, qr);
    const int y = qr.y + qr.height + TR_S(30);
    tr_wrap(fb, TR_MARGIN, y, TR_W - 2 * TR_MARGIN, TR_S(28), TR_S(42),
            "用手机微信扫一扫，在手机上点“确认登录”。二维码过一会儿会失效，失效了点取消再登录一次。", 3);
    tr_pill(fb, DETAIL_BTN, "取消", false, TR_S(32));
}

static bool detail_is_dl(void) { return s_dl_id[0] && !strcmp(s_dl_id, s_detail.id); }

static void draw_detail(uint8_t* fb) {
    const weread_book_t* b = &s_detail;
    char line[200];
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text_dim(fb, TR_S(96), TR_S(100), TR_S(30), "微信读书书架", EPD_DRAW_ALIGN_LEFT);
    const EpdRect cr = COVER_RECT;
    tr_book_cover(fb, cr, b->cover[0] ? b->cover : NULL, b->title, b->author, "微信读书", false);
    const int x = 292, w = TR_W - TR_MARGIN - x;
    const int ty = TR_S(188), tlh = TR_S(52);
    const int lines = tr_wrap(fb, x, ty, w, TR_S(40), tlh, b->title, ui_scale_pct > 100 ? 2 : 3);
    const int y = ty + lines * tlh + TR_S(18);
    snprintf(line, sizeof(line), "%.*s", (int)sizeof(b->author) - 1, b->author);
    tr_fit(line, TR_S(30), w);
    tr_text(fb, x, y, TR_S(30), line, EPD_DRAW_ALIGN_LEFT, false);
    tr_text_dim(fb, x, y + TR_S(50), TR_S(28), b->local[0] ? "已在书库" : "还没下载", EPD_DRAW_ALIGN_LEFT);
    const EpdRect btn = DETAIL_BTN;
    const bool downloading = detail_is_dl() && job_running() && s_job == J_DOWNLOAD;
    if (downloading) {
        busy_line(line, sizeof(line));
        tr_text(fb, TR_MARGIN, btn.y - TR_S(4), TR_S(28), line, EPD_DRAW_ALIGN_LEFT, false);
        tr_progress(fb, TR_MARGIN, btn.y + TR_S(54), btn.width - TR_S(110), busy_pct());
        tr_text(fb, TR_W - TR_MARGIN, btn.y + TR_S(38), TR_S(30), "取消", EPD_DRAW_ALIGN_RIGHT, false);
        return;
    }
    const char* note = s_note[0] ? s_note
                     : last_is(J_DOWNLOAD) && detail_is_dl() && s_status.phase != TOMATO_DONE ? s_status.message
                     : last_is(J_DOWNLOAD) && detail_is_dl() ? s_status.message : "";
    if (note[0])
        tr_message(fb, TR_MARGIN, btn.y - TR_S(50), btn.y - TR_S(76), TR_W - 2 * TR_MARGIN, TR_S(28), note,
                   EPD_DRAW_ALIGN_LEFT, false);
    tr_pill(fb, btn, b->local[0] ? "开始阅读" : "下载到书库", true, TR_S(32));
    const char* sub = b->local[0] ? "点这里重新下载（覆盖书库里的这本）"
                    : app_settings_weread_epub() ? "整本下载成 EPUB，带插图，比较慢" : "整本下载成 TXT，只要文字，比较快";
    tr_text_dim(fb, TR_W / 2, btn.y + btn.height + TR_S(26), TR_S(26), sub, EPD_DRAW_ALIGN_CENTER);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    switch (s_view) {
    case V_NOTICE: draw_notice(fb); break;
    case V_SHELF: draw_shelf(fb); break;
    case V_QR: draw_qr(fb); break;
    case V_DETAIL: draw_detail(fb); break;
    }
    if (s_view == V_SHELF) tr_tab_bar(fb, TR_TAB_LIBRARY);
}

/* ---- 事件 / Events ---- */

static app_redraw_t turn_page(int turn) {
    const int next = s_page + turn;
    if (next < 0 || next >= pages()) return APP_REDRAW_NONE;
    s_page = next;
    book_cover_drop_pending();
    load_page();
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_shelf(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(150) && x < TR_S(300)) { ctx->request_app = &app_book; return APP_REDRAW_NONE; }
    if (tr_store_source_hit(ctx, s_tomato_on, true, x, y)) return APP_REDRAW_NONE;
    const bool syncing = job_running() && s_job == J_SYNC;
    if (ui_rect_hit(ACTION_RECT, x, y)) {
        if (syncing) tomato_sync_cancel();
        else if (weread_logged_in()) start_sync();
        return APP_REDRAW_PAGE;
    }
    if (!weread_logged_in() && !syncing) {
        if (ui_rect_hit(CENTER_BTN, x, y)) start_sync();
        return ui_rect_hit(CENTER_BTN, x, y) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    const int turn = tr_pager_hit(PAGER_Y, x, y);
    if (turn) return turn_page(turn);
    for (int k = 0; k < s_page_n; k++) {
        EpdRect r = cell_rect(k);
        r.width = CELL_W;
        r.height += CELL_TEXT_H - TR_S(10);
        if (!ui_rect_hit(r, x, y)) continue;
        s_detail = s_books[k];
        s_note[0] = 0;
        s_view = V_DETAIL;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_detail(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(300)) { s_view = V_SHELF; return APP_REDRAW_PAGE; }
    const EpdRect btn = DETAIL_BTN;
    if (detail_is_dl() && job_running() && s_job == J_DOWNLOAD) {
        if (y >= btn.y && y < btn.y + btn.height && x > TR_W - TR_MARGIN - TR_S(140)) tomato_sync_cancel();
        return APP_REDRAW_NONE;
    }
    // 已下载：按钮读书，下面那行重新下载。/ Downloaded: the button reads, the line under it downloads again.
    const EpdRect again = {TR_MARGIN, btn.y + btn.height + TR_S(10), TR_W - 2 * TR_MARGIN, TR_S(60)};
    if (s_detail.local[0] && ui_rect_hit(again, x, y)) {
        if (job_running()) return APP_REDRAW_NONE;
        start_download();
        return APP_REDRAW_PAGE;
    }
    if (!ui_rect_hit(btn, x, y)) return APP_REDRAW_NONE;
    if (s_detail.local[0]) {
        tr_nav_open_book(ctx, s_detail.local);
        return APP_REDRAW_NONE;
    }
    if (job_running() && s_job == J_DOWNLOAD) {
        snprintf(s_note, sizeof(s_note), "%s", "正在下载另一本，下完再点");
        return APP_REDRAW_PAGE;
    }
    start_download();
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_view == V_SHELF && tr_nav_touch(ctx, x, y, TR_TAB_LIBRARY)) {
        if (!ctx->request_app) ctx->request_app = &app_book;
        return APP_REDRAW_NONE;
    }
    switch (s_view) {
    case V_NOTICE:
        if (y < TR_S(150) && x < TR_S(300)) { ctx->request_app = &app_book; return APP_REDRAW_NONE; }
        if (tr_store_source_hit(ctx, s_tomato_on, true, x, y)) return APP_REDRAW_NONE;
        if (!ui_rect_hit(DETAIL_BTN, x, y)) return APP_REDRAW_NONE;
        if (!weread_accept_disclaimer()) {
            snprintf(s_note, sizeof(s_note), "%s", "写不了 TF 卡，检查卡后再试");
            return APP_REDRAW_PAGE;
        }
        s_view = V_SHELF;
        load_page();
        return APP_REDRAW_PAGE;
    case V_SHELF: return touch_shelf(ctx, x, y);
    case V_QR:
        if (ui_rect_hit(DETAIL_BTN, x, y) || (y < TR_S(150) && x < TR_S(300))) {
            tomato_sync_cancel();
            s_view = V_SHELF;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case V_DETAIL: return touch_detail(ctx, x, y);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    if (s_view != V_SHELF) return APP_REDRAW_NONE;
    const int turn = ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U ? 1
                   : ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D ? -1 : 0;
    return turn ? turn_page(turn) : APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_DETAIL: s_view = V_SHELF; return APP_REDRAW_PAGE;
    case V_QR: tomato_sync_cancel(); s_view = V_SHELF; return APP_REDRAW_PAGE;
    default: ctx->request_app = &app_book; return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view == V_SHELF && !(ctx->touch && ctx->touch->touched) && book_cover_tick()) return APP_REDRAW_PAGE;
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tr_time_poll();
    if (s_job == J_NONE) return APP_REDRAW_NONE;
    if (tomato_sync_seq() != s_job_seq) {
        // 序号变了：本页的任务早结束了。/ The sequence moved on: our job ended long ago.
        s_job = J_NONE;
        load_page();
        return APP_REDRAW_PAGE;
    }
    tomato_sync_get_status(&s_status);
    weread_snapshot(&s_snap);
    if (!tomato_sync_busy()) {
        // 任务结束：重读书架，详情页的书也更新“已下载”。/ Job done: reload the shelf and refresh the detail book.
        s_last = s_job;
        s_job = J_NONE;
        ui_wifi_qr_clear();
        s_qr_url[0] = 0;
        if (s_view == V_QR) s_view = V_SHELF;
        // 因为内存不够失败：放掉能重建的缓存，让下一次能成。/ Failed for lack of memory: drop rebuildable caches so the
        // next try can work.
        if (s_status.phase == TOMATO_FAILED && s_status.error == ESP_ERR_NO_MEM) {
            mem_reclaim("weread failed");
            snprintf(s_note, sizeof(s_note), "%s", "内存不足，已清理缓存，请再点一次");
        }
        load_page();
        if (s_last == J_DOWNLOAD) {
            // 重新下载会换掉同名文件：旧的封面缩略图作废。/ A re-download replaces the file: drop its old thumbnail.
            weread_snapshot(&s_snap);
            if (s_snap.output[0]) book_cover_forget(s_snap.output);
            book_store_notify_changed();
            for (int i = 0; i < s_count; i++) {
                weread_book_t b;
                if (weread_shelf_get(i, &b) && !strcmp(b.id, s_detail.id)) { s_detail = b; break; }
            }
        }
        return APP_REDRAW_PAGE;
    }
    // 出现二维码就切过去；登录成功后回书架看同步进度。/ Switch to the QR when it appears; back to the shelf once signed in.
    if (s_snap.stage == WEREAD_STAGE_QR && strcmp(s_qr_url, s_snap.qr)) {
        snprintf(s_qr_url, sizeof(s_qr_url), "%s", s_snap.qr);
        ui_wifi_qr_prepare_https(s_qr_url);
        s_view = V_QR;
        return APP_REDRAW_PAGE;
    }
    if (s_view == V_QR && s_snap.stage != WEREAD_STAGE_QR) {
        ui_wifi_qr_clear();
        s_qr_url[0] = 0;
        s_view = V_SHELF;
        return APP_REDRAW_PAGE;
    }
    if (s_snap.revision == s_seen_rev || ctx->now_ms - s_busy_draw_ms < BUSY_REDRAW_MS) return APP_REDRAW_NONE;
    s_seen_rev = s_snap.revision;
    s_busy_draw_ms = ctx->now_ms;
    // 进度只在看得见的页上刷。/ Redraw progress only where it shows.
    const bool visible = (s_view == V_SHELF && s_job == J_SYNC) || (s_view == V_DETAIL && detail_is_dl());
    return visible ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    tr_store_mark(true);
    tomato_config_t cfg;
    s_tomato_on = tomato_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    if (!s_books) s_books = heap_caps_calloc(9, sizeof(weread_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    book_cover_drop_pending();
    s_note[0] = 0;
    weread_configure();
    if (s_last != J_NONE && tomato_sync_seq() != s_job_seq) s_last = J_NONE;
    if (!job_running()) s_job = J_NONE;
    if (!weread_disclaimer_accepted()) s_view = V_NOTICE;
    else if (s_view == V_NOTICE || (s_view == V_QR && !job_running()) || (s_view == V_DETAIL && !job_running()))
        s_view = V_SHELF;
    load_page();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    book_cover_drop_pending();
    heap_caps_free(s_books);
    s_books = NULL;
    s_page_n = 0;
    // 扫码等着没意义了：离页就停登录。下载照常在后台跑。/ Waiting for a scan is pointless off the page: stop the sign-in.
    // Downloads keep running in the background.
    if (job_running() && s_job == J_SYNC && s_snap.stage == WEREAD_STAGE_QR) tomato_sync_cancel();
    // 没在下载：没下完的书的续传缓存没用了，离页就删。/ No download running: resume data for unfinished books is useless
    // off the page, so delete it.
    if (!weread_busy()) weread_drop_partial();
    ui_wifi_qr_clear();
    s_qr_url[0] = 0;
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    if (job_running()) tomato_sync_cancel();
    s_view = V_SHELF;
    s_count = 0;
    s_page_n = 0;
}

const app_desc_t app_tr_weread = {
    .title = "书城",
    .detail = "微信读书",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
};
