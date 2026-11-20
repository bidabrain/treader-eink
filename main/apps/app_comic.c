/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 漫画：书架（封面网格，系列和单本）、系列的话列表、一屏一页的 CBZ 阅读和工具条、详情（清进度、删除）。
 * 和书库分开管理，只看 TF 卡 comics/。版式见 docs/design/ui.html 的“漫画”一节。
 *
 * Comics: a shelf (cover grid of series and singles), a series' episode list, one-page-per-screen CBZ reading with a
 * toolbar, and a detail page (clear progress, delete). Managed apart from the library; only reads comics/ on the TF
 * card. Layout follows the "comics" section of docs/design/ui.html.
 *
 * 冻结：只解 JPEG / PNG（网页上传时已统一转成灰度 JPEG）；页面按整页缩放，不做滚动；解码放在主循环里一次一页。
 * Frozen: decode JPEG / PNG only (web uploads are converted to grayscale JPEG); pages fit the screen, no scrolling;
 * decoding runs on the main loop one page at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app.h"
#include "app_registry.h"
#include "auto_turn.h"
#include "book_cover.h"
#include "book_image.h"
#include "comic_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hupu_img.h"
#include "settings.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "zip_reader.h"

#define TAG "comic"
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* ---- 版式 / Layout ---- */

// 封面和漫画页是图片，不随显示大小放大；文字、按钮、行高和间距都放大。放大后书架一页一行、话列表一页少几行，
// 工具条从屏幕底往上排。
// Covers and comic pages are images and do not scale; text, buttons, row heights and gaps do. Scaled up, the shelf
// shows one row per page, the episode list fewer rows, and the toolbar stacks up from the screen bottom.
#define BIG (ui_scale_pct > 100)
#define CELL_W 150
#define COVER_H 200
#define MINI_W 70
#define MINI_H 96
#define PER_PAGE (3 * shelf_rows())
#define PAGER_Y (TR_TABS_Y - TR_S(54))
#define CARD (EpdRect){TR_MARGIN, TR_S(284), TR_W - 2 * TR_MARGIN, TR_S(136)}
#define SORT_PILL (EpdRect){TR_MARGIN, TR_S(212), TR_S(150), TR_S(54)}
#define IMPORT_X 592
#define GRID_TOP (TR_S(284) + TR_S(136) + TR_S(34))
#define GRID_PITCH (COVER_H + TR_S(94))
#define CELL_TEXT_H (TR_S(74) + (BIG ? TR_S(28) : 0))   // 放大后状态行可折成两行 / The status may fold to 2 lines
#define ROWS_TOP series_rows_top()
#define ROW_H TR_S(66)
#define ROWS_PER series_rows()
#define SERIES_PAGER_Y (TR_TABS_Y - TR_S(64))
#define CONTINUE_PILL continue_pill()
#define AREA_W TR_W
#define AREA_H 1160
#define LABEL_Y (TR_H - TR_S(46) > AREA_H ? TR_H - TR_S(46) : AREA_H)   // 页脚不压图 / The footer stays off the image
#define STEP_Y (TR_H - TR_S(140))
#define AUTO_Y (STEP_Y - TR_S(36) - TR_S(18) - AUTO_TURN_ROW_H)
#define TOOL_Y (AUTO_Y - TR_S(122))
#define SEEK_Y (TOOL_Y - TR_S(54))
#define BAR_TOP (SEEK_Y - TR_S(86))
#define FULL_EVERY 8
#define FLUSH_EVERY 10
#define COVER_SLOTS 12
#define PAGE_INPUT_MAX (8u * 1024u * 1024u)

static const int CELL_X[3] = {40, 267, 494};

// 书架一页几行：按翻页条上面剩下的高度算（正常档 2 行）。/ Shelf rows per page from the height above the pager (2 at
// normal size).
static int shelf_rows(void) {
    const int rows = (PAGER_Y - TR_S(16) - GRID_TOP + TR_S(26)) / GRID_PITCH;
    return rows < 1 ? 1 : rows > 2 ? 2 : rows;
}

static int cell_y(int k) { return GRID_TOP + (k / 3) * GRID_PITCH; }

// 画一行字；放不下且带“ · ”时从那里折成两行（不截短固定文字）。strong 为加粗黑字，否则深灰。返回行数。
// Draw one line; when it does not fit and has " · ", fold it there into two lines (fixed text is never cut). strong
// draws bold black, otherwise dark gray. Returns the line count.
static int split_lines(const char* text, int px, int w) {
    return ttf_text_width_px(px, text) > w && strstr(text, " · ") ? 2 : 1;
}

static int draw_split(uint8_t* fb, int x, int y, int px, int pitch, int w, const char* text, bool strong) {
    char a[128], b[128];
    const char* dot = strstr(text, " · ");
    const int lines = split_lines(text, px, w);
    if (lines == 1) snprintf(a, sizeof(a), "%s", text);
    else snprintf(a, sizeof(a), "%.*s", (int)(dot - text), text);
    if (lines == 2) snprintf(b, sizeof(b), "%s", dot + strlen(" · "));
    for (int i = 0; i < lines; i++) {
        const char* s = i ? b : a;
        if (strong) tr_text(fb, x, y + i * pitch, px, s, EPD_DRAW_ALIGN_LEFT, true);
        else tr_text_dim(fb, x, y + i * pitch, px, s, EPD_DRAW_ALIGN_LEFT);
    }
    return lines;
}

typedef enum { V_SHELF, V_SERIES, V_READ, V_DETAIL } view_t;

/* ---- 状态 / State ---- */

static view_t s_view;
static comic_entry_t* s_shelf;
static int s_count;
static bool s_sort_name;
static int s_page;
static char s_message[96];

// 继续看：最近那一话。/ Continue: the most recent episode.
static char s_last[COMIC_PATH_MAX];
static char s_last_title[200];
static char s_last_owner[COMIC_PATH_MAX];   // 所属系列或单本路径 / Owning series or single path
static uint16_t s_last_page, s_last_pages;

// 系列视图。/ Series view.
static int s_series = -1;
static char s_series_path[COMIC_PATH_MAX];   // 重扫后按路径找回系列 / Relocate the series by path after a rescan
static char (*s_vols)[COMIC_NAME_MAX];
static int s_vol_count;
static int s_vol_page;
// 系列页的统计，进系列和进度变了时算好，render 只画。/ Series stats, computed on entry and after progress changes so
// render only draws.
static int s_ser_done, s_ser_resume, s_ser_dir;
static uint16_t s_ser_page;

// 阅读器。/ Reader.
typedef struct {
    int idx;
    int w, h;
    uint8_t* px;
    bool failed;
} pagebuf_t;

static zip_reader_t* s_zip;
static int* s_pages;
static int s_page_count;
static int s_cur;
static char s_book[COMIC_PATH_MAX];
static char s_owner[COMIC_PATH_MAX];   // 方向按系列或单本记 / Direction is kept per series or single
static char s_book_title[160];
static bool s_rtl;
static int s_vol_index = -1;           // 在系列里第几话，单本为 -1 / Episode index in the series, -1 for a single
static bool s_toolbar;
static int s_turns;
static pagebuf_t s_buf[2];             // [0] 当前页，[1] 预解码的下一页 / [0] current, [1] prefetched next
// 自动翻页：档位、计时起点、到最后一页停下。/ Auto turn: level, countdown start, stopped at the very end.
static int s_auto_level;
static int64_t s_auto_ms;
static bool s_auto_done;
static bool s_auto_was_on;
static int64_t s_auto_tick;
static bool s_mark_done;   // 这一话翻到过最后一页 / This episode reached its last page

// 详情。/ Detail.
static char s_detail_path[COMIC_PATH_MAX];
static char s_detail_name[COMIC_NAME_MAX];
static bool s_detail_series;
static view_t s_detail_back;
static bool s_confirm_delete;
static char s_detail_info[2][96];

/* ---- 小工具 / Helpers ---- */

static void base_title(const char* path, char* out, size_t cap) { tr_title_from_path(path, out, cap); }

static bool parent_dir(const char* path, char* out, size_t cap) {
    const char* slash = strrchr(path, '/');
    if (!slash) return false;
    snprintf(out, cap, "%.*s", (int)(slash - path), path);
    return true;
}

static bool is_image_name(const char* name) {
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!*base || base[0] == '.' || strstr(name, "__MACOSX")) return false;
    const char* ext = strrchr(base, '.');
    return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png"));
}

// 追加 src，放不下就在字符边界截断。/ Append src, cutting on a character boundary when it does not fit.
static void utf8_append(char* out, size_t cap, const char* src) {
    size_t len = strlen(out), n = strlen(src);
    if (len + 1 >= cap) return;
    if (n > cap - 1 - len) {
        n = cap - 1 - len;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out + len, src, n);
    out[len + n] = 0;
}

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* ---- 封面缓存 / Cover cache ---- */

typedef struct {
    char key[COMIC_PATH_MAX];
    uint8_t* px;     // CELL_W × COVER_H
    uint8_t* mini;   // MINI_W × MINI_H
    bool failed;
    uint32_t use;
} cover_t;

static cover_t s_covers[COVER_SLOTS];
static uint32_t s_cover_clock;
static bool s_cover_built;

static void cover_free(cover_t* c) {
    heap_caps_free(c->px);
    heap_caps_free(c->mini);
    memset(c, 0, sizeof(*c));
}

static void covers_release(void) {
    for (int i = 0; i < COVER_SLOTS; i++) cover_free(&s_covers[i]);
}

static cover_t* cover_find(const char* key) {
    for (int i = 0; i < COVER_SLOTS; i++)
        if (s_covers[i].key[0] && !strcmp(s_covers[i].key, key)) {
            s_covers[i].use = ++s_cover_clock;
            return &s_covers[i];
        }
    return NULL;
}

static cover_t* cover_slot(const char* key) {
    cover_t* best = &s_covers[0];
    for (int i = 0; i < COVER_SLOTS; i++) {
        if (!s_covers[i].key[0]) { best = &s_covers[i]; break; }
        if (s_covers[i].use < best->use) best = &s_covers[i];
    }
    cover_free(best);
    snprintf(best->key, sizeof(best->key), "%s", key);
    best->use = ++s_cover_clock;
    return best;
}

static void cover_cache_path(const char* key, char* out, size_t cap) {
    snprintf(out, cap, COMIC_META_DIR "/covers/%08lx.g", (unsigned long)fnv(key));
}

// 第一张图在 zip 里的条目号（按自然顺序最小的那张）。/ The first image entry in natural order.
static int first_image(zip_reader_t* z) {
    int best = -1;
    char best_name[COMIC_PATH_MAX] = "";
    const size_t n = zip_entry_count(z);
    for (size_t i = 0; i < n; i++) {
        const char* name = zip_entry_name(z, (int)i);
        if (!name || !is_image_name(name)) continue;
        if (best < 0 || comic_name_cmp(name, best_name) < 0) {
            best = (int)i;
            snprintf(best_name, sizeof(best_name), "%s", name);
        }
    }
    return best;
}

// 解码 zip 里的一张图到灰度（不超过 648×1000）。/ Decode one zip image to gray (at most 648x1000).
static bool decode_entry(zip_reader_t* z, int entry, uint8_t** gray, uint16_t* w, uint16_t* h) {
    const size_t len = zip_entry_size(z, entry);
    // 大于 2 MB 的图要确认 PSRAM 放得下（原文件加解码还要的余量）。/ Images over 2 MB need room for the file plus decoding.
    if (!len || len > PAGE_INPUT_MAX) return false;
    if (len > 2u * 1024u * 1024u && heap_caps_get_largest_free_block(PSRAM_CAPS) < len + 2u * 1024u * 1024u) return false;
    uint8_t* data = heap_caps_malloc(len, PSRAM_CAPS);
    if (!data) return false;
    bool ok = zip_extract(z, entry, data, len) == ESP_OK && book_image_decode(data, len, 648u * 1000u, gray, w, h);
    heap_caps_free(data);
    return ok;
}

// 封面来源：系列取第一话，单本取自己。/ Cover source: a series' first episode, or the single itself.
static bool cover_source(const comic_entry_t* e, char* out, size_t cap) {
    if (!e->series) { snprintf(out, cap, "%s", e->path); return true; }
    if (!s_vols) return false;
    char (*names)[COMIC_NAME_MAX] = heap_caps_malloc(COMIC_NAME_MAX * COMIC_VOLUME_MAX, PSRAM_CAPS);
    if (!names) return false;
    const int n = comic_store_volumes(e->path, names, COMIC_VOLUME_MAX);
    if (n > 0) snprintf(out, cap, "%s/%s", e->path, names[0]);
    heap_caps_free(names);
    return n > 0;
}

static void cover_fill(cover_t* c, const uint8_t* gray, int gw, int gh) {
    c->px = heap_caps_malloc(CELL_W * COVER_H, PSRAM_CAPS);
    c->mini = heap_caps_malloc(MINI_W * MINI_H, PSRAM_CAPS);
    if (!c->px || !c->mini) {
        // 只放掉像素，保留 key 并记为失败，免得每轮都重做。/ Free the pixels only and keep the key marked failed so it is
        // not rebuilt every tick.
        heap_caps_free(c->px);
        heap_caps_free(c->mini);
        c->px = c->mini = NULL;
        c->failed = true;
        return;
    }
    book_image_fill(gray, gw, gh, c->px, CELL_W, COVER_H);
    book_image_fill(c->px, CELL_W, COVER_H, c->mini, MINI_W, MINI_H);
}

// 读缓存或从 CBZ 生成一张封面。/ Load a cover from the cache or build it from the CBZ.
static void cover_build(const comic_entry_t* e) {
    cover_t* c = cover_slot(e->path);
    char src[COMIC_PATH_MAX + COMIC_NAME_MAX];
    struct stat st;
    if (!cover_source(e, src, sizeof(src)) || stat(src, &st) != 0) { c->failed = true; return; }
    // 缓存名按来源文件和大小算，换了文件就重新生成。/ Name the cache by source and size so a replaced file rebuilds it.
    char key[COMIC_PATH_MAX + COMIC_NAME_MAX + 16];
    snprintf(key, sizeof(key), "%s#%ld", src, (long)st.st_size);
    char cache[96];
    cover_cache_path(key, cache, sizeof(cache));
    FILE* f = fopen(cache, "rb");
    if (f) {
        uint8_t* px = heap_caps_malloc(CELL_W * COVER_H, PSRAM_CAPS);
        bool ok = px && fread(px, 1, CELL_W * COVER_H, f) == CELL_W * COVER_H;
        fclose(f);
        if (ok) { cover_fill(c, px, CELL_W, COVER_H); heap_caps_free(px); return; }
        heap_caps_free(px);
    }
    zip_reader_t* z = NULL;
    uint8_t* gray = NULL;
    uint16_t gw = 0, gh = 0;
    int entry = -1;
    if (zip_open(src, &z) == ESP_OK) entry = first_image(z);
    const bool ok = entry >= 0 && decode_entry(z, entry, &gray, &gw, &gh);
    zip_close(z);
    if (!ok) { c->failed = true; return; }
    cover_fill(c, gray, gw, gh);
    free(gray);
    if (!c->px) { c->failed = true; return; }
    mkdir(COMIC_META_DIR, 0775);
    mkdir(COMIC_META_DIR "/covers", 0775);
    f = fopen(cache, "wb");
    if (f) {
        const bool written = fwrite(c->px, 1, CELL_W * COVER_H, f) == CELL_W * COVER_H;
        if (fclose(f) != 0 || !written) remove(cache);
    }
}

static void draw_cover(uint8_t* fb, EpdRect r, const char* key, const char* title, bool mini) {
    const cover_t* c = cover_find(key);
    const uint8_t* px = c ? (mini ? c->mini : c->px) : NULL;
    if (px) hupu_img_draw(fb, r.x, r.y, r.width, r.height, px, true);
    else tr_cover(fb, r, title, "", "CBZ", false);
}

/* ---- 书架 / Shelf ---- */

static int cmp_entry(const void* a, const void* b) {
    const comic_entry_t* x = a;
    const comic_entry_t* y = b;
    if (!s_sort_name && x->stamp != y->stamp) return x->stamp > y->stamp ? -1 : 1;
    return comic_name_cmp(x->name, y->name);
}

static int shelf_pages(void) { return s_count ? (s_count + PER_PAGE - 1) / PER_PAGE : 1; }

static void load_last(void) {
    s_last[0] = s_last_title[0] = s_last_owner[0] = 0;
    if (!comic_progress_last(s_last, sizeof(s_last))) { s_last[0] = 0; return; }
    char dir[COMIC_PATH_MAX], title[COMIC_NAME_MAX];
    base_title(s_last, title, sizeof(title));
    if (parent_dir(s_last, dir, sizeof(dir)) && strcmp(dir, COMIC_ROOT)) {
        // 文件夹名没有扩展名，直接取最后一段。/ Folder names have no extension; take the last segment as is.
        const char* slash = strrchr(dir, '/');
        utf8_append(s_last_title, sizeof(s_last_title), slash ? slash + 1 : dir);
        utf8_append(s_last_title, sizeof(s_last_title), " · ");
        utf8_append(s_last_title, sizeof(s_last_title), title);
        snprintf(s_last_owner, sizeof(s_last_owner), "%s", dir);
    } else {
        snprintf(s_last_title, sizeof(s_last_title), "%s", title);
        snprintf(s_last_owner, sizeof(s_last_owner), "%s", s_last);
    }
    comic_progress_t p = {0};
    comic_progress_get(s_last, &p);
    s_last_page = p.page;
    s_last_pages = p.pages;
}

static void scan(void) {
    s_count = s_shelf ? comic_store_scan(s_shelf, COMIC_SHELF_MAX) : 0;
    if (s_count > 1) qsort(s_shelf, (size_t)s_count, sizeof(*s_shelf), cmp_entry);
    if (s_page >= shelf_pages()) s_page = shelf_pages() - 1;
    // 排序变了，按路径找回当前系列。/ The order changed; find the current series again by path.
    s_series = -1;
    for (int i = 0; s_series_path[0] && i < s_count; i++)
        if (s_shelf[i].series && !strcmp(s_shelf[i].path, s_series_path)) s_series = i;
    load_last();
}

static void draw_series_stack(uint8_t* fb, int x, int y) {
    // 系列封面后面垫两张浅灰卡片，表示一叠。/ Two light gray cards behind a series cover show a stack.
    for (int k = 2; k >= 1; k--) {
        EpdRect r = {x + 5 * k, y - 5 * k, CELL_W, COVER_H};
        epd_fill_rect(r, k == 2 ? TR_GRAY_SELECTED : TR_GRAY_SURFACE, fb);
        epd_draw_rect(r, TR_GRAY_OUTLINE, fb);
    }
}

static void draw_shelf(uint8_t* fb) {
    tr_title(fb, "漫画");
    tr_icon(fb, TR_ICON_IMPORT, IMPORT_X + 20, TR_S(116), false);
    char line[96];
    snprintf(line, sizeof(line), "%d 部 · %s", s_count, s_sort_name ? "按名字" : "按最近");
    tr_text_dim(fb, TR_MARGIN, TR_S(156), TR_S(28), line, EPD_DRAW_ALIGN_LEFT);
    tr_pill(fb, SORT_PILL, s_sort_name ? "按名字" : "按最近", false, TR_S(28));
    if (s_message[0])
        tr_message(fb, TR_W - TR_MARGIN, TR_S(220), TR_S(210), 400, TR_S(26), s_message, EPD_DRAW_ALIGN_RIGHT, false);
    if (!s_count) {
        tr_wrap(fb, TR_MARGIN, TR_S(330), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "还没有漫画。设置 → Wi-Fi 与传书，用手机网页上传 CBZ；也可以用读卡器把 CBZ 放进 TF 卡的 comics 文件夹，"
                "一部漫画的多话放在同一个子文件夹里。", BIG ? 9 : 6);
        tr_tab_bar(fb, TR_TAB_COMIC);
        return;
    }
    // 继续看。/ Continue reading.
    const EpdRect card = CARD;
    if (s_last[0]) {
        // Material 3 填充卡片（漫画只用整页刷新，灰底安全）。/ Material 3 filled card (comics refresh whole pages, so
        // the gray is safe).
        ui_fill_round_rect(fb, card, TR_S(24), TR_GRAY_SURFACE);
        draw_cover(fb, (EpdRect){56, card.y + (card.height - MINI_H) / 2, MINI_W, MINI_H}, s_last_owner, s_last_title,
                   true);
        tr_text_on(fb, 148, card.y + TR_S(14), TR_S(24), "继续看", EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY,
                   TR_GRAY_SURFACE);
        tr_text_on(fb, TR_W - 60, card.y + TR_S(14), TR_S(24), "接着看 →", EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK,
                   TR_GRAY_SURFACE);
        char title[200];
        snprintf(title, sizeof(title), "%s", s_last_title);
        tr_fit(title, TR_S(32), TR_W - 60 - 148);
        tr_text_on(fb, 148, card.y + TR_S(46), TR_S(32), title, EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK,
                   TR_GRAY_SURFACE);
        const int pct = s_last_pages ? (s_last_page + 1) * 100 / s_last_pages : 0;
        snprintf(line, sizeof(line), "%u / %u", (unsigned)s_last_page + 1, (unsigned)s_last_pages);
        // 放大后进度条给页码让出宽度。/ Scaled up, the bar leaves room for the page count.
        const int bar_w = BIG ? TR_W - 60 - ttf_text_width_px(TR_S(26), line) - TR_S(16) - 148 : TR_W - 170 - 148;
        tr_progress(fb, 148, card.y + TR_S(108), bar_w, pct);
        tr_text_on(fb, TR_W - 60, card.y + TR_S(94), TR_S(26), line, EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK,
                   TR_GRAY_SURFACE);
    }
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = s_page * PER_PAGE + k;
        if (i >= s_count) break;
        const comic_entry_t* e = &s_shelf[i];
        const int x = CELL_X[k % 3], y = cell_y(k);
        if (e->series) draw_series_stack(fb, x, y);
        draw_cover(fb, (EpdRect){x, y, CELL_W, COVER_H}, e->path, e->name, false);
        char name[COMIC_NAME_MAX];
        snprintf(name, sizeof(name), "%s", e->name);
        tr_fit(name, TR_S(24), CELL_W);
        tr_text(fb, x, y + COVER_H + TR_S(8), TR_S(24), name, EPD_DRAW_ALIGN_LEFT, true);
        bool done = e->series ? e->done == e->volumes : e->done;
        if (e->series) {
            if (done) snprintf(line, sizeof(line), "%u 话 · 看完", (unsigned)e->volumes);
            else if (e->done) snprintf(line, sizeof(line), "%u 话 · 看完 %u", (unsigned)e->volumes, (unsigned)e->done);
            else snprintf(line, sizeof(line), "%u 话 · %s", (unsigned)e->volumes, e->stamp ? "在看" : "未看");
        } else {
            if (done) snprintf(line, sizeof(line), "单本 · 看完");
            else if (e->stamp) snprintf(line, sizeof(line), "单本 · %u%%", (unsigned)e->pct);
            else snprintf(line, sizeof(line), "单本 · 未看");
        }
        // 看完的加粗黑字，其余深灰；放大后放不下就在“ · ”处折行。/ Finished ones are bold black, the rest dark
        // gray; scaled up, a line that does not fit folds at " · ".
        if (BIG) {
            draw_split(fb, x, y + COVER_H + TR_S(46), TR_S(22), TR_S(28), CELL_W, line, done);
            continue;
        }
        tr_fit(line, 22, CELL_W);
        if (done) tr_text(fb, x, y + 246, 22, line, EPD_DRAW_ALIGN_LEFT, true);
        else tr_text_dim(fb, x, y + 246, 22, line, EPD_DRAW_ALIGN_LEFT);
    }
    tr_pager(fb, PAGER_Y, s_page, shelf_pages());
    tr_tab_bar(fb, TR_TAB_COMIC);
}

/* ---- 系列 / Series ---- */

// 系列页上半部分的排布。正常档沿用原坐标；放大后右边文字依次往下排，“继续看”按钮放在封面和文字都下面。
// Series header layout. Normal size keeps the original coordinates; scaled up the text beside the cover stacks
// downward and the continue button sits below both the cover and the text.
typedef struct {
    int stats_y, resume_y, dir_y, rule_y, rows_top;
    EpdRect pill;
} series_geo_t;

static void series_stats_line(char* out, size_t cap) {
    snprintf(out, cap, "共 %d 话 · 看完 %d 话", s_vol_count, s_ser_done);
}

static series_geo_t series_geo(void) {
    if (!BIG) return (series_geo_t){204, 256, 304, 520, 536, {222, 400, TR_W - TR_MARGIN - 222, 76}};
    series_geo_t g;
    char line[96];
    series_stats_line(line, sizeof(line));
    g.stats_y = TR_S(204);
    g.resume_y = g.stats_y + (split_lines(line, TR_S(30), TR_W - TR_MARGIN - 222) - 1) * TR_S(42) + TR_S(52);
    g.dir_y = g.resume_y + TR_S(48);
    int pill_y = TR_S(196) + COVER_H + TR_S(4);
    if (g.dir_y + TR_S(44) > pill_y) pill_y = g.dir_y + TR_S(44);
    g.pill = (EpdRect){222, pill_y, TR_W - TR_MARGIN - 222, TR_S(76)};
    g.rule_y = pill_y + g.pill.height + TR_S(20);
    g.rows_top = g.rule_y + TR_S(16);
    return g;
}

static EpdRect continue_pill(void) { return series_geo().pill; }
static int series_rows_top(void) { return series_geo().rows_top; }

// 话列表一页几行：按翻页条上面剩下的高度算（正常档 7 行）。/ Episode rows per page from the height above the pager
// (7 at normal size).
static int series_rows(void) {
    const int rows = (SERIES_PAGER_Y - TR_S(32) - series_rows_top()) / ROW_H;
    return rows < 1 ? 1 : rows > 7 ? 7 : rows;
}

static int vol_pages(void) { return s_vol_count ? (s_vol_count + ROWS_PER - 1) / ROWS_PER : 1; }

static void vol_path(int i, char* out, size_t cap) { snprintf(out, cap, "%s/%s", s_shelf[s_series].path, s_vols[i]); }

static int list_vols(void) {
    s_vol_count = s_vols && s_series >= 0 ? comic_store_volumes(s_shelf[s_series].path, s_vols, COMIC_VOLUME_MAX) : 0;
    return s_vol_count;
}

// 系列里接着看哪一话：最近看过的那话；它已经看完且后面还有就下一话；都没看过就第一话。
// Which episode to continue: the latest read; the one after it if that is finished; else the first.
static void series_stats(void) {
    s_ser_done = s_ser_resume = 0;
    s_ser_page = 0;
    s_ser_dir = s_series >= 0 ? comic_direction_get(s_shelf[s_series].path) : -1;
    uint32_t stamp = 0;
    bool latest_done = false;
    char path[COMIC_PATH_MAX + COMIC_NAME_MAX];
    for (int i = 0; i < s_vol_count; i++) {
        vol_path(i, path, sizeof(path));
        comic_progress_t p;
        if (!comic_progress_get(path, &p)) continue;
        if (p.done) s_ser_done++;
        if (p.stamp > stamp) { stamp = p.stamp; s_ser_resume = i; s_ser_page = p.page; latest_done = p.done; }
    }
    if (latest_done && s_ser_resume + 1 < s_vol_count) { s_ser_resume++; s_ser_page = 0; }
}

static void open_series(int index) {
    s_series = index;
    snprintf(s_series_path, sizeof(s_series_path), "%s", s_shelf[index].path);
    list_vols();
    series_stats();
    s_vol_page = s_ser_resume / ROWS_PER;
    s_view = V_SERIES;
}

static void draw_series(uint8_t* fb) {
    const comic_entry_t* e = &s_shelf[s_series];
    const series_geo_t g = series_geo();
    const int rows_per = ROWS_PER;
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    char title[COMIC_NAME_MAX];
    snprintf(title, sizeof(title), "%s", e->name);
    char line[96];
    snprintf(line, sizeof(line), "%d 话", s_vol_count);
    // 标题让出右边话数的宽度。/ The title leaves room for the episode count on the right.
    tr_fit(title, TR_S(40), BIG ? TR_W - TR_S(96) - TR_MARGIN - ttf_text_width_px(TR_S(28), line) - TR_S(16)
                                : TR_W - 96 - 160);
    tr_text(fb, TR_S(96), TR_S(96), TR_S(40), title, EPD_DRAW_ALIGN_LEFT, true);
    tr_text_dim(fb, TR_W - TR_MARGIN, TR_S(104), TR_S(28), line, EPD_DRAW_ALIGN_RIGHT);
    tr_hline(fb, TR_S(170), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    draw_cover(fb, (EpdRect){TR_MARGIN, TR_S(196), CELL_W, COVER_H}, e->path, e->name, false);
    char path[COMIC_PATH_MAX + COMIC_NAME_MAX];
    series_stats_line(line, sizeof(line));
    if (BIG) draw_split(fb, 222, g.stats_y, TR_S(30), TR_S(42), TR_W - TR_MARGIN - 222, line, true);
    else tr_text(fb, 222, 204, 30, line, EPD_DRAW_ALIGN_LEFT, true);
    const int resume = s_ser_resume;
    char vol[COMIC_NAME_MAX], text[COMIC_NAME_MAX + 48];
    if (s_vol_count) {
        vol_path(resume, path, sizeof(path));
        base_title(path, vol, sizeof(vol));
        snprintf(line, sizeof(line), " 第 %u 页", (unsigned)s_ser_page + 1);
        text[0] = 0;
        utf8_append(text, sizeof(text), "接着看：");
        utf8_append(text, sizeof(text), vol);
        utf8_append(text, sizeof(text), line);
        tr_fit(text, TR_S(28), TR_W - TR_MARGIN - 222);
        tr_text_dim(fb, 222, g.resume_y, TR_S(28), text, EPD_DRAW_ALIGN_LEFT);
    }
    tr_text_dim(fb, 222, g.dir_y, TR_S(28), s_ser_dir == 1 ? "翻页：从右往左" : "翻页：从左往右", EPD_DRAW_ALIGN_LEFT);
    if (s_vol_count) {
        text[0] = 0;
        utf8_append(text, sizeof(text), "继续看 ");
        utf8_append(text, sizeof(text), vol);
        tr_fit(text, TR_S(30), g.pill.width - TR_S(40));
        tr_pill(fb, g.pill, text, true, TR_S(30));
    }
    tr_hline(fb, g.rule_y, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    for (int k = 0; k < rows_per; k++) {
        const int i = s_vol_page * rows_per + k;
        if (i >= s_vol_count) break;
        const int y = g.rows_top + k * ROW_H;
        vol_path(i, path, sizeof(path));
        base_title(path, vol, sizeof(vol));
        comic_progress_t p;
        const bool has = comic_progress_get(path, &p);
        const bool now = has && i == resume && !p.done;
        char state[32];
        if (!has) snprintf(state, sizeof(state), "未看");
        else if (p.done) snprintf(state, sizeof(state), "看完");
        else snprintf(state, sizeof(state), "%s%u / %u", now ? "在看 " : "", (unsigned)p.page + 1, (unsigned)p.pages);
        // 话名让出右边状态的宽度。/ The episode name leaves room for the state on the right.
        tr_fit(vol, TR_S(32), BIG ? TR_W - 2 * TR_MARGIN - ttf_text_width_px(TR_S(28), state) - TR_S(20)
                                  : TR_W - 2 * TR_MARGIN - 200);
        tr_text(fb, TR_MARGIN, y + TR_S(14), TR_S(32), vol, EPD_DRAW_ALIGN_LEFT, now);
        if (now) tr_text(fb, TR_W - TR_MARGIN, y + TR_S(18), TR_S(28), state, EPD_DRAW_ALIGN_RIGHT, true);
        else tr_text_dim(fb, TR_W - TR_MARGIN, y + TR_S(18), TR_S(28), state, EPD_DRAW_ALIGN_RIGHT);
        tr_hline(fb, y + ROW_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(fb, SERIES_PAGER_Y, s_vol_page, vol_pages());
    tr_tab_bar(fb, TR_TAB_COMIC);
}

/* ---- 阅读器 / Reader ---- */

static void page_free(pagebuf_t* b) {
    heap_caps_free(b->px);
    memset(b, 0, sizeof(*b));
    b->idx = -1;
}

static void reader_close(void) {
    page_free(&s_buf[0]);
    page_free(&s_buf[1]);
    zip_close(s_zip);
    s_zip = NULL;
    heap_caps_free(s_pages);
    s_pages = NULL;
    s_page_count = 0;
}

static void save_progress(bool flush) {
    if (!s_book[0] || !s_page_count) return;
    comic_progress_t p = {0};
    comic_progress_get(s_book, &p);
    p.page = (uint16_t)s_cur;
    p.pages = (uint16_t)s_page_count;
    p.stamp = 0;
    p.done = p.done || s_mark_done;
    comic_progress_set(s_book, &p);
    if (flush) comic_progress_flush();
}

// 把一页解码并缩到屏幕里（保持比例）。/ Decode one page and fit it to the screen, keeping the aspect ratio.
static void decode_page(pagebuf_t* b, int idx) {
    page_free(b);
    b->idx = idx;
    uint8_t* gray = NULL;
    uint16_t gw = 0, gh = 0;
    if (!s_zip || idx < 0 || idx >= s_page_count || !decode_entry(s_zip, s_pages[idx], &gray, &gw, &gh)) {
        b->failed = true;
        return;
    }
    float s = (float)AREA_W / gw;
    if ((float)AREA_H / gh < s) s = (float)AREA_H / gh;
    b->w = (int)(gw * s);
    b->h = (int)(gh * s);
    if (b->w < 1) b->w = 1;
    if (b->h < 1) b->h = 1;
    b->px = heap_caps_malloc((size_t)b->w * b->h, PSRAM_CAPS);
    if (b->px) book_image_fill(gray, gw, gh, b->px, b->w, b->h);
    else b->failed = true;
    free(gray);
}

// 把 s_cur 准备好：预解码过就直接换过来。/ Make s_cur ready, swapping in the prefetched page when it matches.
static void ensure_current(void) {
    if (s_buf[0].idx == s_cur && (s_buf[0].px || s_buf[0].failed)) return;
    if (s_buf[1].idx == s_cur) {
        pagebuf_t t = s_buf[0];
        s_buf[0] = s_buf[1];
        s_buf[1] = t;
        page_free(&s_buf[1]);
        return;
    }
    page_free(&s_buf[1]);
    decode_page(&s_buf[0], s_cur);
}

static const char* const* s_sort_names;
static int cmp_page(const void* a, const void* b) {
    return comic_name_cmp(s_sort_names[*(const int*)a], s_sort_names[*(const int*)b]);
}

// ComicInfo.xml 写了 Manga=YesAndRightToLeft 就默认从右往左。/ ComicInfo.xml Manga=YesAndRightToLeft means right-to-left.
static bool comicinfo_rtl(zip_reader_t* z) {
    const size_t n = zip_entry_count(z);
    for (size_t i = 0; i < n; i++) {
        const char* name = zip_entry_name(z, (int)i);
        const char* base = name ? strrchr(name, '/') : NULL;
        base = base ? base + 1 : name;
        if (!base || strcasecmp(base, "ComicInfo.xml")) continue;
        const size_t len = zip_entry_size(z, (int)i);
        if (!len || len > 65536) return false;
        char* xml = heap_caps_malloc(len + 1, PSRAM_CAPS);
        if (!xml) return false;
        bool rtl = false;
        if (zip_extract(z, (int)i, xml, len) == ESP_OK) {
            xml[len] = 0;
            rtl = strstr(xml, "YesAndRightToLeft") != NULL;
        }
        heap_caps_free(xml);
        return rtl;
    }
    return false;
}

// 打开一话：列出图片、按自然顺序排、定方向和起始页。新的一话打开成功才换掉当前这话，失败时原来的照常能读。
// start：-1 按进度（看完的从头），-2 最后一页（往回翻进上一话），其余为指定页。
// Open an episode: list images in natural order, set direction and the starting page. The current episode is only
// replaced once the new one opened, so a failure leaves it readable. start: -1 from progress (finished ones restart),
// -2 the last page (turning back into the previous episode), otherwise that page.
static bool reader_open(const char* path, const char* owner, int vol_index, int start) {
    zip_reader_t* z = NULL;
    if (zip_open(path, &z) != ESP_OK) {
        snprintf(s_message, sizeof(s_message), "打不开这个 CBZ（文件损坏或不是 ZIP）");
        return false;
    }
    const size_t n = zip_entry_count(z);
    int* pages = heap_caps_malloc((n ? n : 1) * sizeof(int), PSRAM_CAPS);
    char** names = heap_caps_calloc(n ? n : 1, sizeof(char*), PSRAM_CAPS);
    int count = 0;
    if (pages && names) {
        for (size_t i = 0; i < n; i++) {
            const char* name = zip_entry_name(z, (int)i);
            if (!name || !is_image_name(name)) continue;
            names[i] = heap_caps_malloc(strlen(name) + 1, PSRAM_CAPS);
            if (!names[i]) continue;
            strcpy(names[i], name);
            pages[count++] = (int)i;
        }
        s_sort_names = (const char* const*)names;
        qsort(pages, (size_t)count, sizeof(int), cmp_page);
    }
    if (names) for (size_t i = 0; i < n; i++) heap_caps_free(names[i]);
    heap_caps_free(names);
    if (!count) {
        heap_caps_free(pages);
        zip_close(z);
        snprintf(s_message, sizeof(s_message), "这个 CBZ 里没有能显示的图片（支持 JPEG / PNG）");
        return false;
    }
    if (s_page_count) save_progress(true);
    reader_close();
    covers_release();
    s_message[0] = 0;
    s_zip = z;
    s_pages = pages;
    s_page_count = count;
    s_mark_done = false;
    snprintf(s_book, sizeof(s_book), "%s", path);
    snprintf(s_owner, sizeof(s_owner), "%s", owner);
    base_title(path, s_book_title, sizeof(s_book_title));
    s_vol_index = vol_index;
    const int d = comic_direction_get(owner);
    s_rtl = d >= 0 ? d == 1 : comicinfo_rtl(s_zip);
    comic_progress_t p;
    if (start == -2) s_cur = s_page_count - 1;
    else if (start >= 0) s_cur = start < s_page_count ? start : s_page_count - 1;
    else if (comic_progress_get(path, &p) && p.page < s_page_count && !(p.done && p.page + 1 >= s_page_count)) s_cur = p.page;
    else s_cur = 0;
    s_toolbar = false;
    s_turns = 0;
    s_view = V_READ;
    ensure_current();
    save_progress(true);
    return true;
}

static bool open_volume(int vol, int start) {
    if (s_series < 0 || vol < 0 || vol >= s_vol_count) return false;
    char path[COMIC_PATH_MAX + COMIC_NAME_MAX];
    vol_path(vol, path, sizeof(path));
    return reader_open(path, s_shelf[s_series].path, vol, start);
}

static int vol_index_of(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int v = 0; v < s_vol_count; v++)
        if (!strcmp(s_vols[v], base)) return v;
    return -1;
}

// 打开某个路径（继续看用）：属于系列就先定位系列。/ Open a path (for "continue"), locating its series first.
static bool open_path(const char* path) {
    char dir[COMIC_PATH_MAX];
    if (parent_dir(path, dir, sizeof(dir)) && strcmp(dir, COMIC_ROOT)) {
        for (int i = 0; i < s_count; i++) {
            if (!s_shelf[i].series || strcmp(s_shelf[i].path, dir)) continue;
            open_series(i);
            int v = vol_index_of(path);
            if (v < 0) break;
            // 最近那话已看完且后面还有：直接看下一话。/ The latest episode is finished and more follow: open the next.
            comic_progress_t p;
            if (comic_progress_get(path, &p) && p.done && v + 1 < s_vol_count) return open_volume(v + 1, 0);
            return open_volume(v, -1);
        }
        snprintf(s_message, sizeof(s_message), "找不到上次看的那一话");
        s_view = V_SHELF;
        return false;
    }
    s_series = -1;
    s_series_path[0] = 0;
    return reader_open(path, path, -1, -1);
}

static void leave_reader(void) {
    save_progress(true);
    reader_close();
    const bool series = s_vol_index >= 0;
    scan();
    s_view = series && s_series >= 0 ? V_SERIES : V_SHELF;
    if (s_view == V_SERIES) {
        list_vols();
        series_stats();
        s_vol_page = s_vol_index / ROWS_PER;
    }
}

static app_redraw_t step_redraw(void) {
    return ++s_turns % FULL_EVERY == 0 ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
}

// 翻到相对的一页；越界就进上一话 / 下一话。/ Turn by delta; crossing the ends moves to the previous / next episode.
static app_redraw_t turn(int delta) {
    if (!s_page_count) return APP_REDRAW_NONE;
    const int next = s_cur + delta;
    if (next >= 0 && next < s_page_count) {
        s_cur = next;
        if (s_cur + 1 >= s_page_count) s_mark_done = true;
        ensure_current();
        save_progress(s_turns % FLUSH_EVERY == FLUSH_EVERY - 1);
        return step_redraw();
    }
    if (next >= s_page_count) {
        s_cur = s_page_count - 1;
        s_mark_done = true;
        save_progress(true);
        if (s_vol_index >= 0 && s_vol_index + 1 < s_vol_count) {
            // 打不开就停在这话，reader_open 已经写好原因。/ On failure stay here; reader_open set the reason.
            return open_volume(s_vol_index + 1, 0) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
        }
        snprintf(s_message, sizeof(s_message), "已经是最后一页");
        return APP_REDRAW_PAGE;
    }
    if (s_vol_index > 0) return open_volume(s_vol_index - 1, -2) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
    snprintf(s_message, sizeof(s_message), "已经是第一页");
    return APP_REDRAW_PAGE;
}

// 屏幕左边 / 左键：从右往左时是下一页。/ Left side / left key: the next page when reading right-to-left.
static app_redraw_t turn_left(void) { return turn(s_rtl ? 1 : -1); }
static app_redraw_t turn_right(void) { return turn(s_rtl ? -1 : 1); }

static void draw_reader(uint8_t* fb) {
    const pagebuf_t* b = &s_buf[0];
    if (b->px) hupu_img_draw(fb, (AREA_W - b->w) / 2, (AREA_H - b->h) / 2, b->w, b->h, b->px, false);
    else tr_wrap(fb, TR_MARGIN, TR_S(520), TR_W - 2 * TR_MARGIN, TR_S(32), TR_S(48), "这一页设备解不了（渐进式 JPEG 太大，或格式不支持）。用传书网页重新上传这部漫画会自动转换。左右翻页继续。", BIG ? 6 : 4);
    char label[200];
    snprintf(label, sizeof(label), "%d / %d", s_cur + 1, s_page_count);
    char name[200];
    snprintf(name, sizeof(name), "%s", s_book_title);
    tr_fit(name, TR_S(24), BIG ? TR_W - 2 * TR_MARGIN - ttf_text_width_px(TR_S(24), label) - TR_S(24)
                               : TR_W - 2 * TR_MARGIN - 160);
    tr_text(fb, TR_MARGIN, LABEL_Y, TR_S(24), name, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, LABEL_Y, TR_S(24), label, EPD_DRAW_ALIGN_RIGHT, false);
    if (s_message[0] && !s_toolbar) {
        epd_fill_rect((EpdRect){0, LABEL_Y - TR_S(50), TR_W, TR_S(44)}, UI_GRAY_WHITE, fb);
        tr_text(fb, TR_W / 2, LABEL_Y - TR_S(46), TR_S(26), s_message, EPD_DRAW_ALIGN_CENTER, true);
    }
    if (!s_toolbar) return;
    // 工具条：顶部返回和标题，底部页码、进度、按钮。/ Toolbar: back and title on top; pages, progress and buttons below.
    epd_fill_rect((EpdRect){0, 0, TR_W, TR_S(120)}, UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_S(120) - 2, 0, TR_W, true);
    tr_text(fb, TR_MARGIN, TR_S(40), TR_S(32), "‹ 漫画", EPD_DRAW_ALIGN_LEFT, false);
    char title[200];
    if (s_vol_index >= 0 && s_series >= 0) snprintf(title, sizeof(title), "%.80s · %.80s", s_shelf[s_series].name, s_book_title);
    else snprintf(title, sizeof(title), "%s", s_book_title);
    tr_fit(title, TR_S(34), TR_W - 2 * TR_S(170));
    tr_text(fb, TR_W / 2, TR_S(38), TR_S(34), title, EPD_DRAW_ALIGN_CENTER, true);
    // Material 3 底部面板：圆角上沿和拖动条。/ Material 3 bottom sheet: rounded top edge and a drag handle.
    epd_fill_rect((EpdRect){0, BAR_TOP, TR_W, TR_H - BAR_TOP}, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, (EpdRect){0, BAR_TOP, TR_W, TR_S(120)}, TR_S(32), TR_GRAY_LINE);
    ui_draw_round_rect(fb, (EpdRect){0, BAR_TOP + 1, TR_W, TR_S(120)}, TR_S(32), TR_GRAY_LINE);
    epd_fill_rect((EpdRect){2, BAR_TOP + TR_S(40), TR_W - 4, TR_S(82)}, UI_GRAY_WHITE, fb);
    ui_fill_round_rect(fb, (EpdRect){TR_W / 2 - TR_S(36), BAR_TOP + TR_S(8), TR_S(72), TR_S(8)}, TR_S(4), TR_GRAY_SELECTED);
    snprintf(label, sizeof(label), "第 %d / %d 页", s_cur + 1, s_page_count);
    tr_text_dim(fb, TR_MARGIN, BAR_TOP + TR_S(32), TR_S(28), label, EPD_DRAW_ALIGN_LEFT);
    if (s_vol_index >= 0) {
        snprintf(label, sizeof(label), "第 %d / %d 话", s_vol_index + 1, s_vol_count);
        tr_text_dim(fb, TR_W - TR_MARGIN, BAR_TOP + TR_S(32), TR_S(28), label, EPD_DRAW_ALIGN_RIGHT);
    }
    tr_progress(fb, TR_MARGIN, SEEK_Y, TR_W - 2 * TR_MARGIN, s_page_count ? (s_cur + 1) * 100 / s_page_count : 0);
    const int col = (TR_W - 2 * TR_MARGIN) / 3;
    for (int i = 0; i < 3; i++) {
        const int cx = TR_MARGIN + col * i + col / 2;
        // 每个按钮垫一块浅灰圆角方块。/ Each tool sits on a light gray rounded tile.
        ui_fill_round_rect(fb, (EpdRect){TR_MARGIN + col * i + 10, TOOL_Y - TR_S(12), col - 20, TR_S(108)}, TR_S(24),
                           TR_GRAY_SURFACE);
        if (i == 0) {
            tr_icon_on(fb, TR_ICON_TOC, cx, TOOL_Y + TR_S(24), UI_GRAY_BLACK, TR_GRAY_SURFACE);
            tr_text_on(fb, cx, TOOL_Y + TR_S(56), TR_S(28), s_vol_index >= 0 ? "话列表" : "详情", EPD_DRAW_ALIGN_CENTER,
                       false, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        } else if (i == 1) {
            tr_text_on(fb, cx, TOOL_Y + TR_S(4), TR_S(36), s_rtl ? "←" : "→", EPD_DRAW_ALIGN_CENTER, true, UI_GRAY_BLACK,
                       TR_GRAY_SURFACE);
            tr_text_on(fb, cx, TOOL_Y + TR_S(56), TR_S(28), s_rtl ? "右→左" : "左→右", EPD_DRAW_ALIGN_CENTER, false,
                       UI_GRAY_BLACK, TR_GRAY_SURFACE);
        } else {
            tr_icon_on(fb, TR_ICON_COMIC, cx, TOOL_Y + TR_S(24), UI_GRAY_BLACK, TR_GRAY_SURFACE);
            tr_text_on(fb, cx, TOOL_Y + TR_S(56), TR_S(28), "漫画", EPD_DRAW_ALIGN_CENTER, false, UI_GRAY_BLACK,
                       TR_GRAY_SURFACE);
        }
    }
    auto_turn_draw_row(fb, AUTO_Y, s_auto_level);
    tr_hline(fb, AUTO_Y + AUTO_TURN_ROW_H + TR_S(18), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    if (s_vol_index >= 0) {
        const int w = (TR_W - 2 * TR_MARGIN - TR_S(24)) / 2;
        tr_pill(fb, (EpdRect){TR_MARGIN, STEP_Y, w, TR_S(76)}, "‹ 上一话", false, TR_S(30));
        tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - w, STEP_Y, w, TR_S(76)}, "下一话 ›", false, TR_S(30));
    }
    if (s_message[0]) tr_text(fb, TR_W / 2, TR_H - TR_S(40), TR_S(24), s_message, EPD_DRAW_ALIGN_CENTER, false);
}

/* ---- 详情 / Detail ---- */

static void open_detail(const char* path, const char* name, bool series, view_t back) {
    snprintf(s_detail_path, sizeof(s_detail_path), "%s", path);
    snprintf(s_detail_name, sizeof(s_detail_name), "%s", name);
    s_detail_series = series;
    s_detail_back = back;
    s_confirm_delete = false;
    s_message[0] = 0;
    s_view = V_DETAIL;
    // 文件信息在这里查好，render 只画。/ Look up file info here so render only draws.
    struct stat st;
    s_detail_info[1][0] = 0;
    if (series) {
        char (*names)[COMIC_NAME_MAX] = heap_caps_malloc(COMIC_NAME_MAX * COMIC_VOLUME_MAX, PSRAM_CAPS);
        const int n = names ? comic_store_volumes(path, names, COMIC_VOLUME_MAX) : 0;
        heap_caps_free(names);
        snprintf(s_detail_info[0], sizeof(s_detail_info[0]), "系列 · %d 话", n);
    } else {
        if (stat(path, &st) == 0) snprintf(s_detail_info[0], sizeof(s_detail_info[0]), "CBZ · %.1f MB", st.st_size / 1048576.0);
        else snprintf(s_detail_info[0], sizeof(s_detail_info[0]), "CBZ");
        comic_progress_t p;
        if (comic_progress_get(path, &p)) {
            if (p.done) snprintf(s_detail_info[1], sizeof(s_detail_info[1]), "已看完");
            else snprintf(s_detail_info[1], sizeof(s_detail_info[1]), "看到第 %u / %u 页", (unsigned)p.page + 1, (unsigned)p.pages);
        }
    }
}

#define DETAIL_CLEAR (EpdRect){TR_MARGIN, TR_S(640), TR_W - 2 * TR_MARGIN, TR_S(84)}
#define DETAIL_DELETE (EpdRect){TR_MARGIN, TR_S(748), TR_W - 2 * TR_MARGIN, TR_S(84)}

static void draw_detail(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_S(96), TR_S(100), TR_S(36), s_detail_series ? "系列详情" : "漫画详情", EPD_DRAW_ALIGN_LEFT, true);
    tr_wrap(fb, TR_MARGIN, TR_S(190), TR_W - 2 * TR_MARGIN, TR_S(40), TR_S(56), s_detail_name, 3);
    tr_text_dim(fb, TR_MARGIN, TR_S(380), TR_S(30), s_detail_info[0], EPD_DRAW_ALIGN_LEFT);
    if (s_detail_info[1][0]) tr_text_dim(fb, TR_MARGIN, TR_S(430), TR_S(30), s_detail_info[1], EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(fb, TR_MARGIN, TR_S(520), TR_S(26),
                s_detail_series ? "删除会删掉这个文件夹里的全部 CBZ。" : "删除只删这一个 CBZ 文件。", EPD_DRAW_ALIGN_LEFT);
    tr_pill(fb, DETAIL_CLEAR, s_detail_series ? "清除这部的全部进度" : "清除进度", false, TR_S(32));
    tr_pill(fb, DETAIL_DELETE, s_confirm_delete ? "再点一次确认删除" : "删除", s_confirm_delete, TR_S(32));
    if (s_message[0]) tr_text(fb, TR_W / 2, TR_S(870), TR_S(28), s_message, EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t detail_back(void) {
    s_view = s_detail_back;
    if (s_view == V_SERIES && s_series >= 0) {
        if (!list_vols()) s_view = V_SHELF;
        else series_stats();
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_detail(uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(160)) return detail_back();
    if (ui_rect_hit(DETAIL_CLEAR, x, y)) {
        comic_progress_forget(s_detail_path);
        snprintf(s_message, sizeof(s_message), comic_progress_flush() == ESP_OK ? "进度已清除" : "保存失败，检查 TF 卡");
        // 内存里的表已经清掉；按它重算“继续看”，卡片不再指向刚清的那一话。
        // The in-memory table is already cleared; recompute "continue" from it so the card no longer points at the cleared episode.
        load_last();
        s_detail_info[1][0] = 0;
        s_confirm_delete = false;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(DETAIL_DELETE, x, y)) {
        if (!s_confirm_delete) { s_confirm_delete = true; return APP_REDRAW_PAGE; }
        const esp_err_t err = comic_store_delete(s_detail_path);
        s_confirm_delete = false;
        // 删了单本或系列回书架；删了系列里的一话回话列表。/ Deleting a single or series returns to the shelf; an
        // episode returns to the list.
        const bool episode = s_detail_back == V_SERIES && !s_detail_series;
        scan();
        snprintf(s_message, sizeof(s_message), err == ESP_OK ? "已删除" : "删除没有完成，检查 TF 卡");
        if (episode && s_series >= 0 && s_series < s_count) {
            s_detail_back = V_SERIES;
            return detail_back();
        }
        s_view = V_SHELF;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

/* ---- 触摸 / Touch ---- */

static int shelf_hit(uint16_t x, uint16_t y) {
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = s_page * PER_PAGE + k;
        if (i >= s_count) break;
        const int cx = CELL_X[k % 3], cy = cell_y(k);
        if (x >= cx && x < cx + CELL_W && y + 12 >= cy && y < cy + COVER_H + CELL_TEXT_H) return i;
    }
    return -1;
}

static app_redraw_t open_entry(int i) {
    s_message[0] = 0;
    if (s_shelf[i].series) { open_series(i); return APP_REDRAW_PAGE; }
    s_series = -1;
    s_series_path[0] = 0;
    return reader_open(s_shelf[i].path, s_shelf[i].path, -1, -1) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
}

static app_redraw_t tap_shelf(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, TR_TAB_COMIC)) return APP_REDRAW_NONE;
    if (y >= TR_S(86) && y < TR_S(150) && x >= IMPORT_X - 10) {
        // 先到“设置 › 传书”选 Wi-Fi 还是 USB。/ Go to Settings > Transfer to pick Wi-Fi or USB.
        app_tr_settings_request_transfer();
        ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(SORT_PILL, x, y)) {
        s_sort_name = !s_sort_name;
        s_page = 0;
        scan();
        return APP_REDRAW_PAGE;
    }
    if (!s_count) return APP_REDRAW_NONE;
    if (s_last[0] && ui_rect_hit(CARD, x, y)) {
        s_message[0] = 0;
        return open_path(s_last) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
    }
    const int pg = tr_pager_hit(PAGER_Y, x, y);
    if (pg) {
        const int next = s_page + pg;
        if (next < 0 || next >= shelf_pages()) return APP_REDRAW_NONE;
        s_page = next;
        return APP_REDRAW_PAGE;
    }
    const int i = shelf_hit(x, y);
    return i >= 0 ? open_entry(i) : APP_REDRAW_NONE;
}

static app_redraw_t tap_series(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, TR_TAB_COMIC)) return APP_REDRAW_NONE;
    if (y < TR_S(160) && x < TR_S(160)) { s_view = V_SHELF; s_message[0] = 0; return APP_REDRAW_PAGE; }
    if (s_vol_count && ui_rect_hit(CONTINUE_PILL, x, y)) {
        return open_volume(s_ser_resume, s_ser_page) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
    }
    const int pg = tr_pager_hit(SERIES_PAGER_Y, x, y);
    if (pg) {
        const int next = s_vol_page + pg;
        if (next < 0 || next >= vol_pages()) return APP_REDRAW_NONE;
        s_vol_page = next;
        return APP_REDRAW_PAGE;
    }
    const int top = ROWS_TOP, rows = ROWS_PER;
    if (y >= top && y < top + rows * ROW_H) {
        const int i = s_vol_page * rows + (y - top) / ROW_H;
        if (i < s_vol_count) return open_volume(i, -1) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t toggle_direction(void) {
    s_rtl = !s_rtl;
    comic_direction_set(s_owner, s_rtl);
    comic_progress_flush();
    s_ser_dir = s_rtl ? 1 : 0;
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap_reader(uint16_t x, uint16_t y) {
    if (!s_toolbar) {
        s_message[0] = 0;
        if (x < TR_W / 3) return turn_left();
        if (x >= TR_W * 2 / 3) return turn_right();
        s_toolbar = true;
        return APP_REDRAW_PAGE;
    }
    if (y < TR_S(120)) {
        if (x < TR_S(200)) { leave_reader(); return APP_REDRAW_FULL; }
        return APP_REDRAW_NONE;
    }
    if (y < BAR_TOP) { s_toolbar = false; s_message[0] = 0; return APP_REDRAW_PAGE; }
    if (y >= SEEK_Y - TR_S(30) && y < SEEK_Y + TR_S(34) && x >= TR_MARGIN && x < TR_W - TR_MARGIN) {
        int page = (x - TR_MARGIN) * s_page_count / (TR_W - 2 * TR_MARGIN);
        if (page >= s_page_count) page = s_page_count - 1;
        s_cur = page;
        ensure_current();
        save_progress(false);
        return APP_REDRAW_PAGE;
    }
    const int step = auto_turn_row_hit(AUTO_Y, x, y);
    if (step) {
        s_auto_level = auto_turn_step(s_auto_level, step);
        app_settings_set_auto_turn(true, (uint8_t)s_auto_level);
        return APP_REDRAW_PAGE;
    }
    if (y >= TOOL_Y - TR_S(10) && y < TOOL_Y + TR_S(100)) {
        const int col = (x - TR_MARGIN) * 3 / (TR_W - 2 * TR_MARGIN);
        if (col == 0) {
            save_progress(true);
            if (s_vol_index >= 0) { leave_reader(); return APP_REDRAW_FULL; }
            const view_t back = V_SHELF;
            char name[COMIC_NAME_MAX];
            snprintf(name, sizeof(name), "%.120s", s_book_title);
            char path[COMIC_PATH_MAX];
            snprintf(path, sizeof(path), "%s", s_book);
            reader_close();
            scan();
            open_detail(path, name, false, back);
            return APP_REDRAW_FULL;
        }
        if (col == 1) return toggle_direction();
        if (col == 2) {
            leave_reader();
            s_view = V_SHELF;
            return APP_REDRAW_FULL;
        }
    }
    if (s_vol_index >= 0 && y >= STEP_Y && y < STEP_Y + TR_S(76)) {
        const int vol = x < TR_W / 2 ? s_vol_index - 1 : s_vol_index + 1;
        if (vol < 0 || vol >= s_vol_count) {
            snprintf(s_message, sizeof(s_message), vol < 0 ? "已经是第一话" : "已经是最后一话");
            return APP_REDRAW_PAGE;
        }
        save_progress(true);
        return open_volume(vol, -1) ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t list_turn(int dir) {
    if (s_view == V_SHELF) {
        const int next = s_page + dir;
        if (next < 0 || next >= shelf_pages()) return APP_REDRAW_NONE;
        s_page = next;
        return APP_REDRAW_PAGE;
    }
    if (s_view == V_SERIES) {
        const int next = s_vol_page + dir;
        if (next < 0 || next >= vol_pages()) return APP_REDRAW_NONE;
        s_vol_page = next;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    const uint16_t x = ev->x0, y = ev->y0;
    // 碰屏幕就重新计时。/ Any touch restarts the auto-turn countdown.
    s_auto_ms = ctx->now_ms;
    s_auto_done = false;
    if (ev->type == UI_GESTURE_LONG_PRESS) {
        if (s_view == V_SHELF) {
            const int i = shelf_hit(x, y);
            if (i >= 0) { open_detail(s_shelf[i].path, s_shelf[i].name, s_shelf[i].series, V_SHELF); return APP_REDRAW_PAGE; }
        } else if (s_view == V_SERIES && y >= ROWS_TOP && y < ROWS_TOP + ROWS_PER * ROW_H) {
            const int i = s_vol_page * ROWS_PER + (y - ROWS_TOP) / ROW_H;
            if (i < s_vol_count) {
                char path[COMIC_PATH_MAX + COMIC_NAME_MAX], name[COMIC_NAME_MAX];
                vol_path(i, path, sizeof(path));
                base_title(path, name, sizeof(name));
                open_detail(path, name, false, V_SERIES);
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP) {
        switch (s_view) {
        case V_SHELF: return tap_shelf(ctx, x, y);
        case V_SERIES: return tap_series(ctx, x, y);
        case V_READ: return tap_reader(x, y);
        case V_DETAIL: return touch_detail(x, y);
        }
        return APP_REDRAW_NONE;
    }
    // 滑动：列表翻整页；阅读时左右滑翻页，方向跟着阅读方向走。/ Swipes turn whole list pages; while reading, horizontal
    // swipes turn pages following the reading direction.
    if (s_view == V_READ) {
        if (s_toolbar) return APP_REDRAW_NONE;
        if (ev->type == UI_GESTURE_SWIPE_L) return turn_right();
        if (ev->type == UI_GESTURE_SWIPE_R) return turn_left();
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U) return list_turn(1);
    if (ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D) return list_turn(-1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    s_auto_ms = ctx->now_ms;
    s_auto_done = false;
    if (s_view == V_READ) {
        if (key == UI_KEY_2) { s_toolbar = !s_toolbar; s_message[0] = 0; return APP_REDRAW_PAGE; }
        if (s_toolbar) {
            if (key == UI_KEY_1) { s_toolbar = false; return APP_REDRAW_PAGE; }
            if (key == UI_KEY_3) { s_toolbar = false; save_progress(true); ctx->request_menu = true; return APP_REDRAW_NONE; }
            return APP_REDRAW_NONE;
        }
        s_message[0] = 0;
        if (key == UI_KEY_1) return turn_left();
        if (key == UI_KEY_3) return turn_right();
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_2) {
        // 中键回主页（书库）。/ The home key returns to the library.
        ctx->request_app = app_home_page();
        return APP_REDRAW_NONE;
    }
    if (s_view == V_DETAIL) return key == UI_KEY_1 ? detail_back() : APP_REDRAW_NONE;
    if (s_view == V_SERIES && key == UI_KEY_1 && s_vol_page == 0) { s_view = V_SHELF; return APP_REDRAW_PAGE; }
    if (key == UI_KEY_3 && s_view == V_SHELF && s_page + 1 >= shelf_pages()) { ctx->request_menu = true; return APP_REDRAW_NONE; }
    return list_turn(key == UI_KEY_1 ? -1 : 1);
}

/* ---- 生命周期 / Lifecycle ---- */

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_view == V_READ) { draw_reader(fb); return; }
    tr_status_bar(fb);
    if (s_view == V_SERIES && s_series >= 0) draw_series(fb);
    else if (s_view == V_DETAIL) draw_detail(fb);
    else draw_shelf(fb);
}

// 书架上缺的封面一次生成一张，看得见的都齐了整页刷新一次；阅读时预解码下一页。
// Build one missing visible cover per tick and redraw once they are all there; while reading, prefetch the next page.
static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (ctx->touch && ctx->touch->touched) { s_auto_ms = ctx->now_ms; return APP_REDRAW_NONE; }
    if (ctx->consumed) return APP_REDRAW_NONE;
    tr_time_poll();
    if (s_view == V_READ) {
        // 定时自动翻页：工具条开着暂停；最后一话的最后一页停下，碰一下屏幕再继续。
        // Timed auto turn: paused by the toolbar; stops on the last page of the last episode until the next touch.
        const bool auto_on = s_zip && !s_toolbar && s_auto_level > 0 && !s_auto_done;
        // 刚开始、或隔了很久才又跑到这里（锁屏、菜单）：从现在重新计时，不立刻翻。
        // Just started, or a long gap since the last tick (lock screen, menu): restart the countdown instead of turning.
        if (auto_on && (!s_auto_was_on || ctx->now_ms - s_auto_tick > 1500)) s_auto_ms = ctx->now_ms;
        s_auto_was_on = auto_on;
        s_auto_tick = ctx->now_ms;
        auto_turn_set_active(auto_on);
        if (auto_on && ctx->now_ms - s_auto_ms >= auto_turn_ms(s_auto_level)) {
            s_auto_ms = ctx->now_ms;
            const bool last = s_cur + 1 >= s_page_count && !(s_vol_index >= 0 && s_vol_index + 1 < s_vol_count);
            if (last) {
                s_auto_done = true;
                auto_turn_set_active(false);
                snprintf(s_message, sizeof(s_message), "已到最后一页，自动翻页停了");
                return APP_REDRAW_PAGE;
            }
            s_message[0] = 0;
            return turn(1);
        }
        if (s_toolbar || !s_zip) return APP_REDRAW_NONE;
        const int want = s_cur + 1;
        if (want < s_page_count && s_buf[1].idx != want) decode_page(&s_buf[1], want);
        return APP_REDRAW_NONE;
    }
    auto_turn_set_active(false);
    s_auto_was_on = false;
    if (s_view != V_SHELF && s_view != V_SERIES) return APP_REDRAW_NONE;
    const comic_entry_t* need = NULL;
    if (s_view == V_SHELF) {
        if (s_last[0] && !cover_find(s_last_owner)) {
            for (int i = 0; i < s_count && !need; i++)
                if (!strcmp(s_shelf[i].path, s_last_owner)) need = &s_shelf[i];
        }
        for (int k = 0; k < PER_PAGE && !need; k++) {
            const int i = s_page * PER_PAGE + k;
            if (i < s_count && !cover_find(s_shelf[i].path)) need = &s_shelf[i];
        }
    } else if (s_series >= 0 && !cover_find(s_shelf[s_series].path)) {
        need = &s_shelf[s_series];
    }
    if (need) {
        cover_build(need);
        s_cover_built = true;
        return APP_REDRAW_NONE;
    }
    if (!s_cover_built) return APP_REDRAW_NONE;
    s_cover_built = false;
    return APP_REDRAW_PAGE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_shelf) s_shelf = heap_caps_calloc(COMIC_SHELF_MAX, sizeof(*s_shelf), PSRAM_CAPS);
    if (!s_vols) s_vols = heap_caps_calloc(COMIC_VOLUME_MAX, COMIC_NAME_MAX, PSRAM_CAPS);
    s_buf[0].idx = s_buf[1].idx = -1;
    s_message[0] = 0;
    s_auto_level = app_settings_auto_turn(true);
    s_auto_ms = ctx->now_ms;
    s_auto_done = false;
    s_auto_was_on = false;
    // 进度表每次进页都从卡上重读，换过卡也不会把旧表写到新卡上。/ Reload the progress table from the card on every
    // entry so a swapped card never receives the old table.
    comic_progress_drop();
    scan();
    // 显示大小可能变了，每页行数跟着变：话列表页码夹回范围。/ The display size may have changed and with it the rows
    // per page: clamp the episode page.
    if (s_vol_page >= vol_pages()) s_vol_page = vol_pages() - 1;
    if (s_vol_page < 0) s_vol_page = 0;
    // 锁屏或切页回来时按路径找回那一话，接着刚才那一页。/ Coming back from the lock screen or another page finds the
    // episode by path and resumes the same page.
    if (s_view == V_READ && s_book[0]) {
        char book[COMIC_PATH_MAX], owner[COMIC_PATH_MAX];
        snprintf(book, sizeof(book), "%s", s_book);
        snprintf(owner, sizeof(owner), "%s", s_owner);
        const int page = s_cur;
        s_book[0] = 0;
        s_page_count = 0;
        if (s_series >= 0 && s_series < s_count && s_shelf[s_series].series && list_vols()) {
            const int v = vol_index_of(book);
            if (v >= 0 && open_volume(v, page)) return;
        } else if (reader_open(book, owner, -1, page)) {
            return;
        }
        s_view = V_SHELF;
    }
    if (s_view == V_SERIES && (s_series < 0 || s_series >= s_count || !s_shelf[s_series].series)) s_view = V_SHELF;
    if (s_view == V_SERIES) { list_vols(); series_stats(); }
    if (s_view == V_DETAIL) s_view = V_SHELF;
}

static void comic_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    auto_turn_set_active(false);
    save_progress(false);
    comic_progress_flush();
    // 保留 s_view / s_book，回来时接着读；文件和缓冲都放掉。/ Keep s_view / s_book to resume; release files and buffers.
    page_free(&s_buf[0]);
    page_free(&s_buf[1]);
    zip_close(s_zip);
    s_zip = NULL;
    heap_caps_free(s_pages);
    s_pages = NULL;
    s_page_count = 0;
    covers_release();
}

// 每次锁屏前（含浅睡）把正在看的这一页写回卡上；不画屏、不关文件。没在看就什么也不做。
// Before every lock, light sleep included, write the current page to the card; no drawing, files stay open. Nothing to do
// when not reading.
static void comic_on_lock(app_ctx_t* ctx) {
    (void)ctx;
    if (s_view != V_READ || !s_page_count) return;
    save_progress(true);
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    // 先尽量把上次落盘后翻过的页写回去，失败也照常收尾；卡已失效时写入只会失败，不会写到别的卡上。
    // Try to save pages turned since the last flush first and close as usual on failure; with the card gone the write just fails.
    save_progress(false);
    (void)comic_progress_flush();
    reader_close();
    covers_release();
    comic_progress_drop();
    s_book[0] = 0;
    s_view = V_SHELF;
    s_series = -1;
    s_series_path[0] = 0;
    s_count = 0;
    s_last[0] = 0;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){0, 0, TR_W, TR_H};
}

const app_desc_t app_comic = {
    .title = "漫画",
    .detail = "TF 卡上的 CBZ",
    .enter_full = true,
    .owns_keys = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = comic_on_exit,
    .on_media_lost = on_media_lost,
    .on_lock = comic_on_lock,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
