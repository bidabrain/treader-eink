/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 书城：番茄标签页的第二个分页。频道和分类 → 分类榜单 → 书籍详情 → 下载到书库。
 * 数据是 FanqieRankTracker 的榜单 JSON，联网交给 components/tomato 的后台任务，这里只读 TF 卡缓存。
 *
 * Bookstore: the Tomato tab's second segment. Channel and category -> rank list -> book detail -> download.
 * Data is FanqieRankTracker rank JSON; the components/tomato task does the networking and this page reads
 * the TF card cache only.
 *
 * 冻结（2026-11-17 改）：榜单页改成 3×3 封面网格，所以打开分类时会在后台一次联网取完这个分类缺的封面（以前只在进详情时取一张）。
 * 其余联网仍只在用户点“刷新”、打开没缓存的分类、点下载时发生；不自动刷新榜单。取封面让路给用户操作：要取榜单或下载时先取消它。
 * 列表只翻页不滚动：点“‹ ›”或左右/上下滑一下翻一整页。
 * Frozen (changed 2026-11-17): the rank page is now a 3x3 cover grid, so opening a category fetches that category's
 * missing covers in one background session (formerly one cover on the detail page). Other network use still
 * happens only on "Refresh", opening an uncached category, or downloading; ranks never refresh automatically.
 * Cover fetching yields to the user: a rank fetch or download cancels it first. Lists page, never scroll:
 * the pager or one swipe turns a whole page.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "book_cover.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "tomato.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_store"
#define POLL_MS 400
// 长度都随显示大小放大；封面图不放大。靠下的位置跟着标签栏或屏幕底走。/ Lengths scale with the display size; cover
// images do not. Positions near the bottom follow the tab bar or the screen bottom.
#define CHIP_H TR_S(62)
#define CHIP_GAP TR_S(12)
#define CHIP_STEP TR_S(76)
#define SEARCH_BAR (EpdRect){TR_MARGIN, TR_S(244), TR_W - 2 * TR_MARGIN, TR_S(80)}   // 搜索栏 / Search bar
#define SEARCH_DY 104                                                  // 搜索栏让出来的高度 / Room the bar takes
#define CHANNEL_Y TR_S(322 + SEARCH_DY)
#define CATS_Y TR_S(430 + SEARCH_DY)
#define RANK_TOP TR_S(176)
#define GRID_COLS 3
#define GRID_PER grid_per()              // 100% 时每页 3×3 / 3x3 per page at 100%
#define CELL_W 150
#define CELL_COVER_H 200                 // 3:4 封面 / 3:4 cover
#define CELL_TEXT_H TR_S(84)             // 封面下两行字 / The two text lines under a cover
#define CELL_GAP ((TR_W - 2 * TR_MARGIN - GRID_COLS * CELL_W) / (GRID_COLS - 1))
#define PAGER_Y (TR_TABS_Y - TR_S(54))
#define COVER_RECT (EpdRect){TR_MARGIN, TR_S(180), 220, 296}
// 详情页没有标签栏：按钮和下面的说明贴着屏幕底（留 80）。/ The detail page has no tab bar: the button and the note
// under it sit 80 px above the screen bottom.
#define DETAIL_BTN (EpdRect){TR_MARGIN, TR_H - 80 - TR_S(26 + 26 + 84), TR_W - 2 * TR_MARGIN, TR_S(84)}

extern const app_desc_t app_tr_tomato;
// 番茄页下次进来直接打开搜索，返回时回书城（定义在 app_tr_tomato.c）。/ Make the Tomato page open its search on the
// next entry and come back here on return (defined in app_tr_tomato.c).
void tr_tomato_open_search(void);
extern const app_desc_t app_tr_store;

typedef enum { V_HOME, V_RANK, V_DETAIL } view_t;
typedef enum { OP_NONE, OP_RANK, OP_COVER, OP_DOWNLOAD } op_t;

static view_t s_view;
static tomato_rank_cat_t* s_cats;      // PSRAM
static int s_cat_count = -1;           // -1：没有缓存 / -1: no cache
static char s_date[16];
static bool s_male = true;
static char s_cat_name[32];            // 选中的分类 / Selected category
static int s_cur = -1;                 // s_cats 里的下标 / Index into s_cats
static tomato_rank_book_t* s_books;    // PSRAM
static int s_book_count = -1;
static int s_page;
static int s_detail;
static tomato_book_t* s_shelf;         // PSRAM，判断“已在书架” / PSRAM, for "on the shelf"
static int s_shelf_count;

static op_t s_op, s_last;
static tomato_status_t s_status;
// 取封面被用户操作打断后要接着做的事。/ What to do once a cancelled cover fetch stops.
typedef enum { Q_NONE, Q_INDEX, Q_CATEGORY, Q_COVERS, Q_DOWNLOAD } queued_t;
static queued_t s_queued;
// 排队或正在下载的那本书（s_books 下标和 book_id）；进度和“取消”只显示在这本的详情上。
// The book queued or downloading (s_books index and book_id); progress and "cancel" show on its detail only.
static int s_dl_index = -1;
static char s_dl_id[sizeof(((tomato_rank_book_t*)0)->book_id)];
static uint32_t s_seen_revision;
static int64_t s_poll_ms;

/* ---- 数据 / Data ---- */

static void load_prefs(void) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t male = 1;
    if (nvs_get_u8(h, "store_ch", &male) == ESP_OK) s_male = male != 0;
    size_t len = sizeof(s_cat_name);
    if (nvs_get_str(h, "store_cat", s_cat_name, &len) != ESP_OK) s_cat_name[0] = 0;
    nvs_close(h);
}

static void save_prefs(void) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "store_ch", s_male ? 1 : 0);
    nvs_set_str(h, "store_cat", s_cat_name);
    nvs_commit(h);
    nvs_close(h);
}

static void load_index(void) {
    s_cat_count = s_cats ? tomato_rank_load_index(s_cats, TOMATO_RANK_CAT_MAX, s_date, sizeof(s_date)) : -1;
    s_cur = -1;
    for (int i = 0; i < s_cat_count; i++)
        if (s_cats[i].male == s_male && !strcmp(s_cats[i].name, s_cat_name)) s_cur = i;
}

static void load_books(void) {
    s_book_count = s_cur >= 0 && s_books ? tomato_rank_load_books(&s_cats[s_cur], s_books, TOMATO_RANK_BOOK_MAX) : -1;
}

static void load_shelf(void) {
    s_shelf_count = 0;
    if (s_shelf) tomato_shelf_load(s_shelf, TOMATO_SHELF_MAX, &s_shelf_count);
}

static int shelf_index_of(const char* book_id) {
    for (int i = 0; i < s_shelf_count; i++) if (!strcmp(s_shelf[i].book_id, book_id)) return i;
    return -1;
}

// 本页任务的序号；序号对不上就是别的页的任务。/ Our job's sequence; a mismatch means another page's job.
static uint32_t s_op_seq;

static bool start_op(op_t op, esp_err_t err) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start op %d: %s", op, esp_err_to_name(err));
        return false;
    }
    s_op = op;
    s_op_seq = tomato_sync_seq();
    s_last = OP_NONE;
    s_seen_revision = 0;
    tomato_sync_get_status(&s_status);
    return true;
}

// 刚结束的是本页的这个任务、之后也没有别的任务开始：这时全局状态才是它的结果。
// The op that just ended is ours and no other job started since: only then is the global status its result.
static bool last_is(op_t op) { return s_last == op && tomato_sync_seq() == s_op_seq; }

// 当前详情页这本书就是排队或正在下载的那本。/ The detail page shows the book that is queued or downloading.
static bool detail_is_dl(void) {
    return s_dl_index == s_detail && s_detail >= 0 && s_detail < s_book_count && !strcmp(s_books[s_detail].book_id, s_dl_id);
}

// 清掉排着的下载。/ Drop a queued download.
static void drop_queued_download(void) {
    if (s_queued == Q_DOWNLOAD) s_queued = Q_NONE;
}

static bool cover_file(const tomato_rank_book_t* b, char* out, size_t cap) {
    struct stat st;
    tomato_rank_cover_path(b->book_id, out, cap);
    return stat(out, &st) == 0 && st.st_size > 0;
}

// 网格几行、封面多高：字放大后三行放得下就把封面缩小一点（不低于 160），否则两行、封面原大。100% 时 3 行、200 高。
// Grid rows and cover height: with larger text keep three rows by shrinking covers a little (not below 160),
// otherwise two rows at full size. Three rows of 200 at 100%.
static int grid_rows(int* cover_h) {
    const int room = PAGER_Y - (RANK_TOP + TR_S(10));
    int rows = 3, h = room / 3 - CELL_TEXT_H;
    if (h < 160) {
        rows = 2;
        h = room / 2 - CELL_TEXT_H;
    }
    if (cover_h) *cover_h = h < CELL_COVER_H ? h : CELL_COVER_H;
    return rows;
}

static int grid_per(void) { return grid_rows(NULL) * GRID_COLS; }

static int rank_pages(void) {
    return s_book_count > 0 ? (s_book_count + GRID_PER - 1) / GRID_PER : 1;
}

// 第 k 格的封面位置（3:4，左对齐在格子里）。/ Cover rect of grid cell k (3:4, left-aligned in its cell).
static EpdRect cell_rect(int k) {
    int h;
    grid_rows(&h);
    return (EpdRect){TR_MARGIN + (k % GRID_COLS) * (CELL_W + CELL_GAP),
                     RANK_TOP + TR_S(10) + (k / GRID_COLS) * (h + CELL_TEXT_H), h * 3 / 4, h};
}

// 这个分类还有没下载的封面。/ Whether the category still misses covers.
static bool covers_missing(void) {
    char path[96];
    for (int i = 0; i < s_book_count; i++)
        if (s_books[i].cover[0] && !cover_file(&s_books[i], path, sizeof(path))) return true;
    return false;
}

// 后台空闲时一次取完本分类缺的封面。/ When idle, fetch all of this category's missing covers at once.
static void fetch_covers(void) {
    if (s_book_count > 0 && !tomato_sync_busy() && covers_missing())
        start_op(OP_COVER, tomato_sync_start_rank_covers(s_books, s_book_count));
}

// 后台空闲返回 true；在取封面就取消它，记下 q 等它停了再做。/ True when idle; cancels a cover fetch and queues q.
// 详情页点“下载到书库”没能开始时的原因。/ Why "下载到书库" on a detail page could not start.
static char s_detail_note[64];

// 刷新被下载或同步挡住时的提示。/ The note when a refresh is blocked by a download or sync.
static const char* s_blocked;

static bool claim_sync(queued_t q) {
    s_blocked = NULL;
    if (!tomato_sync_busy()) return true;
    if (s_op == OP_COVER) {
        s_queued = q;
        tomato_sync_cancel();
        return false;
    }
    // 别的页留下的浏览任务：取消；还没停下就记下 q，停了马上做。/ A browsing job from another page: cancel it; if it
    // has not stopped yet, queue q and run it once the worker is free.
    if (s_op != OP_NONE) {
        if (s_op == OP_DOWNLOAD) s_blocked = "正在下载，完成后再刷新";
        return false;
    }
    const tomato_preempt_t room = tomato_sync_preempt();
    if (room == TOMATO_PREEMPT_STOPPING) s_queued = q;
    if (room == TOMATO_PREEMPT_BLOCKED) s_blocked = "后台正在下载或同步，完成后再刷新";
    return room == TOMATO_PREEMPT_FREE;
}

/* ---- 分类胶囊排版 / Category chip layout ---- */

// 分类胶囊当前在第几页。/ The chip page shown.
static int s_chip_page;

// 按顺序排胶囊，返回第 k 个本频道分类的位置和所在页；bottom 以下另起一页。返回总数，*pages 是页数。
// Lay chips out in order with the rect and page of the k-th channel category; a chip past bottom starts a new page.
// Returns the count; *pages gets the page count.
static int layout_pass(EpdRect* rects, int* idx, int* page, int max, int bottom, int* pages) {
    int n = 0, x = TR_MARGIN, y = CATS_Y, p = 0;
    for (int i = 0; i < s_cat_count && n < max; i++) {
        if (s_cats[i].male != s_male) continue;
        const int w = ttf_text_width_px(TR_S(30), s_cats[i].name) + TR_S(44);
        if (x + w > TR_W - TR_MARGIN + 10) { x = TR_MARGIN; y += CHIP_STEP; }
        if (y + CHIP_H > bottom && x == TR_MARGIN && y > CATS_Y) { y = CATS_Y; p++; }
        rects[n] = (EpdRect){x, y, w, CHIP_H};
        page[n] = p;
        idx[n++] = i;
        x += w + CHIP_GAP;
    }
    *pages = p + 1;
    return n;
}

// 放大后胶囊行数变多：标签栏上面放不下就分页，底下留出翻页按钮。/ Larger sizes need more chip rows: when they do not
// fit above the tab bar, page them and leave room for the pager.
static int layout_chips(EpdRect* rects, int* idx, int* page, int max, int* pages) {
    const int n = layout_pass(rects, idx, page, max, TR_TABS_Y - 8, pages);
    if (*pages == 1) return n;
    return layout_pass(rects, idx, page, max, PAGER_Y - TR_S(24), pages);
}

/* ---- 绘制 / Drawing ---- */

static void draw_segment(uint8_t* fb) {
    static const char* const LABELS[2] = {"书架", "书城"};
    tr_segment(fb, TR_SEGMENT_RECT, LABELS, 2, 1);
}

static const char* busy_text(void) {
    if (s_status.phase == TOMATO_CONNECTING) return "正在连接 Wi-Fi…";
    return "正在获取榜单…";
}

static void draw_home(uint8_t* fb) {
    tr_title(fb, "番茄小说");
    draw_segment(fb);
    char line[96];
    const bool fetching = tomato_sync_busy() && s_op == OP_RANK;
    if (fetching) snprintf(line, sizeof(line), "%s", busy_text());
    else if (s_blocked) snprintf(line, sizeof(line), "%s", s_blocked);
    else if (last_is(OP_RANK) && s_status.phase == TOMATO_FAILED) snprintf(line, sizeof(line), "%s", s_status.message);
    else if (s_cat_count >= 0) snprintf(line, sizeof(line), "榜单 %s", s_date[0] ? s_date : "已缓存");
    else snprintf(line, sizeof(line), "还没有榜单");
    // 搜索栏（Material 3 灰底胶囊），点了进番茄搜索页。书城只用整页刷新，灰底安全。
    // Search bar (Material 3 gray capsule) that opens the Tomato search; the bookstore only refreshes whole pages.
    const EpdRect bar = SEARCH_BAR;
    ui_fill_round_rect(fb, bar, bar.height / 2, TR_GRAY_SURFACE);
    tr_icon_on(fb, TR_ICON_SEARCH, bar.x + TR_S(50), bar.y + TR_S(40), UI_GRAY_BLACK, TR_GRAY_SURFACE);
    tr_text_on(fb, bar.x + TR_S(86), bar.y + TR_S(22), TR_S(30), "搜索书名或作者", EPD_DRAW_ALIGN_LEFT, false,
               TR_GRAY_SECONDARY, TR_GRAY_SURFACE);
    tr_message(fb, TR_MARGIN, TR_S(258 + SEARCH_DY), TR_S(246 + SEARCH_DY), TR_W - 2 * TR_MARGIN - TR_S(160), TR_S(28),
               line, EPD_DRAW_ALIGN_LEFT, false);
    // 刷新一直在（自己在取榜单时除外）：后台在跑别的，点了由 claim_sync 让路或说明原因。原来后台一忙就藏起来，
    // 别的页留下的任务结束后这页不重画，按钮就一直不见。
    // Refresh always shows (except while this page fetches ranks): with other work running, claim_sync makes room or
    // says why. It used to hide whenever the worker was busy and never came back, as this page does not redraw when
    // another page's job ends.
    if (!fetching) {
        tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - TR_S(96), TR_S(274 + SEARCH_DY), false);
        tr_text(fb, TR_W - TR_MARGIN, TR_S(254 + SEARCH_DY), TR_S(30), "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    }
    tr_pill(fb, (EpdRect){TR_MARGIN, CHANNEL_Y, TR_S(120), CHIP_H}, "男频", s_male, TR_S(30));
    tr_pill(fb, (EpdRect){TR_MARGIN + TR_S(134), CHANNEL_Y, TR_S(120), CHIP_H}, "女频", !s_male, TR_S(30));
    tr_hline(fb, TR_S(404 + SEARCH_DY), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    if (s_cat_count < 0) {
        tr_wrap(fb, TR_MARGIN, CATS_Y + TR_S(10), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "点右上角“刷新”联网获取榜单。之后打开会直接显示上次取到的内容，不用联网。", 4);
        return;
    }
    EpdRect rects[TOMATO_RANK_CAT_MAX];
    int idx[TOMATO_RANK_CAT_MAX], page[TOMATO_RANK_CAT_MAX], pages;
    int n = layout_chips(rects, idx, page, TOMATO_RANK_CAT_MAX, &pages);
    if (s_chip_page >= pages) s_chip_page = pages - 1;
    for (int k = 0; k < n; k++) {
        if (page[k] == s_chip_page) tr_pill(fb, rects[k], s_cats[idx[k]].name, idx[k] == s_cur, TR_S(30));
    }
    tr_pager(fb, PAGER_Y, s_chip_page, pages);
}

static void draw_rank(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    char line[160];
    if (s_cur >= 0) {
        snprintf(line, sizeof(line), "%s · %s", s_cats[s_cur].male ? "男频" : "女频", s_date);
        tr_text_dim(fb, TR_W - TR_MARGIN, TR_S(104), TR_S(28), line, EPD_DRAW_ALIGN_RIGHT);
        // 放大后分类名和右边的日期挤：分类名先缩字号，再不够才截断。/ Scaled up, the name crowds the date on the
        // right: the name steps its size down first and is cut only after that.
        const int name_x = TR_S(96), name_w = TR_W - TR_MARGIN - ttf_text_width_px(TR_S(28), line) - TR_S(16) - name_x;
        char name[sizeof(s_cats[0].name)];
        snprintf(name, sizeof(name), "%s", s_cats[s_cur].name);
        int px = TR_S(40);
        while (px > TR_S(30) && ttf_text_width_px(px, name) > name_w) px -= 2;
        tr_fit(name, px, name_w);
        tr_text(fb, name_x, TR_S(96) + (TR_S(40) - px) / 2, px, name, EPD_DRAW_ALIGN_LEFT, true);
    }
    tr_hline(fb, TR_S(170), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    const bool fetching = tomato_sync_busy() && s_op == OP_RANK;
    if (fetching || s_book_count <= 0) {
        const char* msg = fetching ? busy_text()
                        : last_is(OP_RANK) && s_status.phase == TOMATO_FAILED ? s_status.message
                        : s_book_count < 0 ? "还没取到这个分类。后台有别的同步在跑时取不了，返回后再点一次分类。"
                        : "这个分类没有书";
        tr_wrap(fb, TR_MARGIN, RANK_TOP + TR_S(30), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46), msg, 3);
        return;
    }
    const int per = GRID_PER;
    for (int k = 0; k < per; k++) {
        const int i = s_page * per + k;
        if (i >= s_book_count) break;
        const tomato_rank_book_t* b = &s_books[i];
        const EpdRect r = cell_rect(k);
        // 封面上不叠任何字，名次和“已在书架”放在封面下面两行，免得和文字封面的书名重叠。
        // Nothing overlays the cover; rank and "on shelf" go in the two lines below so they never hit a text cover's title.
        char cover[96];
        tr_book_cover(fb, r, cover_file(b, cover, sizeof(cover)) ? cover : NULL, b->title, "", "", false);
        snprintf(line, sizeof(line), "%d %s", i + 1, b->title);
        tr_fit(line, TR_S(24), CELL_W);
        tr_text(fb, r.x, r.y + r.height + TR_S(8), TR_S(24), line, EPD_DRAW_ALIGN_LEFT, true);
        const bool owned = shelf_index_of(b->book_id) >= 0;
        if (owned) snprintf(line, sizeof(line), "已在书架");
        else snprintf(line, sizeof(line), "在读 %s", b->reads[0] ? b->reads : "—");
        tr_fit(line, TR_S(22), CELL_W);
        // 已在书架加粗黑字，在读人数深灰。/ "On shelf" is bold black; reader counts are dark gray.
        if (owned) tr_text(fb, r.x, r.y + r.height + TR_S(46), TR_S(22), line, EPD_DRAW_ALIGN_LEFT, true);
        else tr_text_dim(fb, r.x, r.y + r.height + TR_S(46), TR_S(22), line, EPD_DRAW_ALIGN_LEFT);
    }
    tr_pager(fb, PAGER_Y, s_page, rank_pages());
}

static void draw_detail(uint8_t* fb) {
    const tomato_rank_book_t* b = &s_books[s_detail];
    char line[160];
    tr_text(fb, TR_MARGIN, TR_S(92), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%s · 第 %d 名", s_cur >= 0 ? s_cats[s_cur].name : "", s_detail + 1);
    tr_text_dim(fb, TR_S(96), TR_S(100), TR_S(30), line, EPD_DRAW_ALIGN_LEFT);
    char cover[96];
    const EpdRect cr = COVER_RECT;
    tr_book_cover(fb, cr, cover_file(b, cover, sizeof(cover)) ? cover : NULL, b->title, b->author, "番茄", false);
    const int x = 292, w = TR_W - TR_MARGIN - x;
    // 放大后书名最多两行，给简介多留一行。/ Scaled up, the title takes at most two lines to leave the synopsis a line.
    const int ty = TR_S(188), tlh = TR_S(52);
    int lines = tr_wrap(fb, x, ty, w, TR_S(40), tlh, b->title, ui_scale_pct > 100 ? 2 : 3);
    int y = ty + lines * tlh + TR_S(18);
    snprintf(line, sizeof(line), "%s", b->author);
    tr_fit(line, TR_S(30), w);
    tr_text(fb, x, y, TR_S(30), line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "在读 %s", b->reads[0] ? b->reads : "—");
    tr_text_dim(fb, x, y + TR_S(50), TR_S(28), line, EPD_DRAW_ALIGN_LEFT);
    int text_end = y + TR_S(50) + TR_S(28);
    if (s_cur >= 0) {
        snprintf(line, sizeof(line), "%s · %s", s_cats[s_cur].male ? "男频" : "女频", s_cats[s_cur].name);
        tr_fit(line, TR_S(28), w);
        tr_text_dim(fb, x, y + TR_S(94), TR_S(28), line, EPD_DRAW_ALIGN_LEFT);
        text_end = y + TR_S(94) + TR_S(28);
    }
    // 分隔线在封面和右边文字都下面；简介按按钮上面剩下的高度定行数（100% 时 8 行）。
    // The rule sits below both the cover and the text beside it; the synopsis gets the lines that fit above the
    // button (8 at 100%).
    EpdRect btn = DETAIL_BTN;
    int rule = cr.y + cr.height + TR_S(34);
    if (text_end + TR_S(26) > rule) rule = text_end + TR_S(26);
    tr_hline(fb, rule, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    const int intro_y = rule + TR_S(26), intro_lh = TR_S(48);
    int intro_lines = (btn.y - TR_S(76) - 4 - intro_y) / intro_lh;
    if (intro_lines > 8) intro_lines = 8;
    if (intro_lines > 0)
        tr_wrap(fb, TR_MARGIN, intro_y, TR_W - 2 * TR_MARGIN, TR_S(30), intro_lh, b->intro[0] ? b->intro : "暂无简介",
                intro_lines);

    const bool downloading = detail_is_dl() && ((tomato_sync_busy() && s_op == OP_DOWNLOAD) || s_queued == Q_DOWNLOAD);
    if (downloading) {
        const char* what = s_queued == Q_DOWNLOAD ? "正在准备"
                         : s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi"
                         : s_status.phase == TOMATO_QUEUED ? "服务器正在生成 TXT" : "正在下载";
        if (s_status.phase == TOMATO_DOWNLOADING) snprintf(line, sizeof(line), "%s · %.1f MB", what, s_status.bytes / 1048576.0);
        else snprintf(line, sizeof(line), "%s…", what);
        tr_text(fb, TR_MARGIN, btn.y - TR_S(4), TR_S(28), line, EPD_DRAW_ALIGN_LEFT, false);
        tr_progress(fb, TR_MARGIN, btn.y + TR_S(54), btn.width - TR_S(110),
                    s_status.phase == TOMATO_DOWNLOADING ? 80 : s_status.phase == TOMATO_QUEUED ? 40 : 10);
        tr_text(fb, TR_W - TR_MARGIN, btn.y + TR_S(38), TR_S(30), "取消", EPD_DRAW_ALIGN_RIGHT, false);
        return;
    }
    if (s_detail_note[0])
        tr_message(fb, TR_MARGIN, btn.y - TR_S(50), btn.y - TR_S(76), TR_W - 2 * TR_MARGIN, TR_S(28), s_detail_note,
                   EPD_DRAW_ALIGN_LEFT, false);
    else if (last_is(OP_DOWNLOAD) && detail_is_dl() && s_status.phase == TOMATO_FAILED)
        tr_message(fb, TR_MARGIN, btn.y - TR_S(50), btn.y - TR_S(76), TR_W - 2 * TR_MARGIN, TR_S(28), s_status.message,
                   EPD_DRAW_ALIGN_LEFT, false);
    const bool owned = shelf_index_of(b->book_id) >= 0;
    tr_pill(fb, btn, owned ? "开始阅读" : "下载到书库", true, TR_S(32));
    // 放大后说明一行放不下就居中分两行。/ When the note no longer fits on one line once scaled, it takes two centered lines.
    const char* note = owned ? "已在番茄书架和书库里" : "经番茄服务器生成 TXT，下载后在书库和番茄书架里都能看到";
    const int npx = TR_S(26), ny = btn.y + btn.height + TR_S(26), nw = TR_W - 32;
    if (ttf_text_width_px(npx, note) <= nw) {
        tr_text_dim(fb, TR_W / 2, ny, npx, note, EPD_DRAW_ALIGN_CENTER);
    } else {
        char first[128];
        size_t cut = tr_line_bytes(note, npx, TR_W - 2 * TR_MARGIN);
        if (cut >= sizeof(first)) cut = sizeof(first) - 1;
        memcpy(first, note, cut);
        first[cut] = 0;
        tr_text_dim(fb, TR_W / 2, ny, npx, first, EPD_DRAW_ALIGN_CENTER);
        tr_text_dim(fb, TR_W / 2, ny + npx + TR_S(8), npx, note + cut, EPD_DRAW_ALIGN_CENTER);
    }
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    switch (s_view) {
    case V_HOME: draw_home(fb); break;
    case V_RANK: draw_rank(fb); break;
    case V_DETAIL: draw_detail(fb); break;
    }
    if (s_view != V_DETAIL) tr_tab_bar(fb, TR_TAB_TOMATO);
}

/* ---- 操作 / Actions ---- */

static app_redraw_t open_category(int i) {
    s_cur = i;
    snprintf(s_cat_name, sizeof(s_cat_name), "%s", s_cats[i].name);
    save_prefs();
    s_page = 0;
    s_last = OP_NONE;
    s_view = V_RANK;
    if (tomato_rank_cat_cached(&s_cats[i])) {
        load_books();
        // 换了分类：停掉上一个分类的封面，改取这个分类的。/ New category: stop the previous covers, fetch this one's.
        if (claim_sync(Q_COVERS)) fetch_covers();
    } else {
        s_book_count = -1;
        if (claim_sync(Q_CATEGORY)) start_op(OP_RANK, tomato_sync_start_rank(&s_cats[i]));
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_detail(int i) {
    s_detail = i;
    s_last = OP_NONE;
    s_detail_note[0] = 0;
    s_view = V_DETAIL;
    // 封面多半在网格里已经取过；还缺就补这一本。/ The grid usually fetched the cover already; fill in this one if not.
    char cover[96];
    if (!cover_file(&s_books[i], cover, sizeof(cover)) && s_books[i].cover[0] && !tomato_sync_busy())
        start_op(OP_COVER, tomato_sync_start_rank_covers(&s_books[i], 1));
    return APP_REDRAW_PAGE;
}

static app_redraw_t start_download(int i);

static app_redraw_t download_or_read(app_ctx_t* ctx) {
    const tomato_rank_book_t* b = &s_books[s_detail];
    int owned = shelf_index_of(b->book_id);
    if (owned >= 0) {
        char path[BOOK_STORE_PATH_MAX];
        snprintf(path, sizeof(path), TOMATO_BOOKS_DIR "/%s", s_shelf[owned].file);
        struct stat st;
        if (s_shelf[owned].file[0] && stat(path, &st) == 0) tr_nav_open_book(ctx, path);
        return APP_REDRAW_NONE;
    }
    if (s_queued == Q_DOWNLOAD) return APP_REDRAW_NONE;
    return start_download(s_detail);
}

// 下载第 i 本；后台在取封面就排队，记下是哪本，停下后下的还是这本。/ Download book i; while covers are being fetched,
// queue it with the book recorded so the same book downloads once the worker stops.
static app_redraw_t start_download(int i) {
    const tomato_rank_book_t* b = &s_books[i];
    // 另一本还在下：不动它的记录（否则它的进度会显示到这本上），提示等它下完。
    // Another book is still downloading: keep its record (or its progress would show on this book) and say to wait.
    if (tomato_sync_busy() && s_op == OP_DOWNLOAD) {
        snprintf(s_detail_note, sizeof(s_detail_note), "%s", "正在下载另一本，下完再点");
        return APP_REDRAW_PAGE;
    }
    const int prev_index = s_dl_index;
    char prev_id[sizeof(s_dl_id)];
    snprintf(prev_id, sizeof(prev_id), "%s", s_dl_id);
    s_dl_index = i;
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", b->book_id);
    if (!claim_sync(Q_DOWNLOAD)) {
        if (s_queued == Q_DOWNLOAD) { s_detail_note[0] = 0; return APP_REDRAW_PAGE; }
        // 别的页面的下载或同步在跑：恢复原来的记录，说原因。/ A download or sync from another page is running:
        // restore the previous record and say why.
        s_dl_index = prev_index;
        snprintf(s_dl_id, sizeof(s_dl_id), "%s", prev_id);
        snprintf(s_detail_note, sizeof(s_detail_note), "%s", "后台正在下载或同步，完成后再试");
        return APP_REDRAW_PAGE;
    }
    s_detail_note[0] = 0;
    // 榜单书转成搜索结果的格式，走同一条下载路。/ Turn the rank book into a search hit and use the same download path.
    tomato_hit_t hit = {0};
    snprintf(hit.book_id, sizeof(hit.book_id), "%s", b->book_id);
    snprintf(hit.title, sizeof(hit.title), "%s", b->title);
    snprintf(hit.author, sizeof(hit.author), "%s", b->author);
    snprintf(hit.thumb, sizeof(hit.thumb), "%s", b->cover);
    start_op(OP_DOWNLOAD, tomato_sync_start_download(&hit));
    return APP_REDRAW_PAGE;
}

/* ---- 事件 / Events ---- */

// 分类胶囊翻一整页；只有一页或到头不动。/ Turn one whole chip page; nothing when there is one page or at the ends.
static app_redraw_t turn_chips(int turn) {
    if (s_cat_count <= 0) return APP_REDRAW_NONE;
    EpdRect rects[TOMATO_RANK_CAT_MAX];
    int idx[TOMATO_RANK_CAT_MAX], page[TOMATO_RANK_CAT_MAX], pages;
    layout_chips(rects, idx, page, TOMATO_RANK_CAT_MAX, &pages);
    const int next = s_chip_page + turn;
    if (next < 0 || next >= pages) return APP_REDRAW_NONE;
    s_chip_page = next;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_home(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_segment_hit(TR_SEGMENT_RECT, 2, x, y) == 0) { ctx->request_app = &app_tr_tomato; return APP_REDRAW_NONE; }
    if (ui_rect_hit(SEARCH_BAR, x, y)) {
        tr_tomato_open_search();
        ctx->request_app = &app_tr_tomato;
        return APP_REDRAW_NONE;
    }
    if (y >= TR_S(240 + SEARCH_DY) && y < TR_S(310 + SEARCH_DY) && x > TR_W - TR_MARGIN - TR_S(160) &&
        !(tomato_sync_busy() && s_op == OP_RANK)) {
        if (claim_sync(Q_INDEX)) start_op(OP_RANK, tomato_sync_start_rank(NULL));
        return APP_REDRAW_PAGE;
    }
    if (y >= CHANNEL_Y && y < CHANNEL_Y + CHIP_H && x < TR_MARGIN + TR_S(260)) {
        bool male = x < TR_MARGIN + TR_S(127);
        if (male == s_male) return APP_REDRAW_NONE;
        s_male = male;
        s_chip_page = 0;
        save_prefs();
        load_index();
        return APP_REDRAW_PAGE;
    }
    if (s_cat_count <= 0) return APP_REDRAW_NONE;
    EpdRect rects[TOMATO_RANK_CAT_MAX];
    int idx[TOMATO_RANK_CAT_MAX], page[TOMATO_RANK_CAT_MAX], pages;
    int n = layout_chips(rects, idx, page, TOMATO_RANK_CAT_MAX, &pages);
    if (pages > 1) {
        int turn = tr_pager_hit(PAGER_Y, x, y);
        if (turn) return turn_chips(turn);
    }
    for (int k = 0; k < n; k++)
        if (page[k] == s_chip_page && ui_rect_hit(rects[k], x, y)) return open_category(idx[k]);
    return APP_REDRAW_NONE;
}

static app_redraw_t turn_page(int turn) {
    int next = s_page + turn;
    if (next < 0 || next >= rank_pages()) return APP_REDRAW_NONE;
    s_page = next;
    // 上一页还在排队的缩略图不用再做。/ Drop thumbnails still queued for the previous page.
    book_cover_drop_pending();
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_rank(uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(140)) { s_view = V_HOME; return APP_REDRAW_PAGE; }
    int turn = tr_pager_hit(PAGER_Y, x, y);
    if (turn) return turn_page(turn);
    const int per = GRID_PER;
    for (int k = 0; k < per && s_book_count > 0; k++) {
        EpdRect r = cell_rect(k);
        r.width = CELL_W;                             // 整个格子宽 / The whole cell width
        r.height += CELL_TEXT_H - TR_S(10);           // 书名两行也算 / The title lines count too
        const int i = s_page * per + k;
        if (i < s_book_count && ui_rect_hit(r, x, y)) return open_detail(i);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t touch_detail(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(300)) {
        drop_queued_download();
        s_view = V_RANK;
        return APP_REDRAW_PAGE;
    }
    EpdRect btn = DETAIL_BTN;
    const bool cancel_hit = y >= btn.y && y < btn.y + btn.height && x > TR_W - TR_MARGIN - TR_S(140);
    if (detail_is_dl() && tomato_sync_busy() && s_op == OP_DOWNLOAD) {
        if (cancel_hit) tomato_sync_cancel();
        return APP_REDRAW_NONE;
    }
    // 还在排队：“取消”就是不下了。/ Still queued: "cancel" just drops it.
    if (detail_is_dl() && s_queued == Q_DOWNLOAD) {
        if (!cancel_hit) return APP_REDRAW_NONE;
        drop_queued_download();
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(btn, x, y)) return download_or_read(ctx);
    return APP_REDRAW_NONE;
}

static app_redraw_t tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_view != V_DETAIL && tr_nav_touch(ctx, x, y, TR_TAB_TOMATO)) {
        // 从书城点“番茄”标签回到书架。/ Tapping the Tomato tab from the store returns to the shelf.
        if (!ctx->request_app) ctx->request_app = &app_tr_tomato;
        return APP_REDRAW_NONE;
    }
    switch (s_view) {
    case V_HOME: return touch_home(ctx, x, y);
    case V_RANK: return touch_rank(x, y);
    case V_DETAIL: return touch_detail(ctx, x, y);
    }
    return APP_REDRAW_NONE;
}

// 轻点照旧；榜单页和分页的分类胶囊左滑 / 上滑下一页，右滑 / 下滑上一页。/ Taps as before; on the rank page and
// paged category chips, swipes turn whole pages.
static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    if (s_view == V_DETAIL) return APP_REDRAW_NONE;
    const int turn = ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U ? 1
                   : ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D ? -1 : 0;
    if (!turn) return APP_REDRAW_NONE;
    return s_view == V_RANK ? turn_page(turn) : turn_chips(turn);
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    switch (s_view) {
    case V_DETAIL: drop_queued_download(); s_view = V_RANK; return APP_REDRAW_PAGE;
    case V_RANK: s_view = V_HOME; return APP_REDRAW_PAGE;
    default: ctx->request_app = &app_tr_tomato; return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view != V_HOME && !(ctx->touch && ctx->touch->touched) && book_cover_tick()) return APP_REDRAW_PAGE;
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tr_time_poll();
    if (s_op != OP_NONE && tomato_sync_seq() != s_op_seq) {
        // 序号变了：本页的任务早结束了，后台在跑别人的。/ The sequence moved on: our job ended, another runs.
        s_op = OP_NONE;
        load_shelf();
        return APP_REDRAW_PAGE;
    }
    if (s_op == OP_NONE) {
        // 等别的页的任务停下后，做排着的事。/ Once another page's job stops, run what was queued.
        if (s_queued == Q_NONE || tomato_sync_busy()) return APP_REDRAW_NONE;
        s_op = OP_COVER;   // 借用下面“操作结束”的分支处理队列 / Reuse the "done" branch below to run the queue
        s_op_seq = tomato_sync_seq();
    }
    tomato_sync_get_status(&s_status);
    if (!tomato_sync_busy()) {
        // 操作结束：重读缓存和书架，整页刷新一次。/ Done: reload caches and the shelf, refresh the page once.
        s_last = s_op;
        s_op = OP_NONE;
        if (s_last == OP_RANK) {
            char keep[32];
            snprintf(keep, sizeof(keep), "%s", s_cat_name);
            load_index();
            if (s_view != V_HOME) load_books();
            snprintf(s_cat_name, sizeof(s_cat_name), "%s", keep);
        }
        if (s_last == OP_DOWNLOAD) load_shelf();
        // 取封面被打断：接着做用户要的事。/ Cover fetch interrupted: carry on with what the user asked for.
        const queued_t q = s_queued;
        s_queued = Q_NONE;
        if (q == Q_INDEX) { start_op(OP_RANK, tomato_sync_start_rank(NULL)); return APP_REDRAW_PAGE; }
        if (q == Q_CATEGORY && s_cur >= 0) { start_op(OP_RANK, tomato_sync_start_rank(&s_cats[s_cur])); return APP_REDRAW_PAGE; }
        // 下排队时记下的那本，不是当前看的那本。/ Download the book recorded at queue time, not whatever is shown.
        if (q == Q_DOWNLOAD && s_dl_index >= 0 && s_dl_index < s_book_count &&
            !strcmp(s_books[s_dl_index].book_id, s_dl_id))
            return start_download(s_dl_index);
        if (q == Q_COVERS && s_view == V_RANK) { fetch_covers(); return APP_REDRAW_NONE; }
        if (s_last == OP_RANK && s_view == V_RANK && s_status.phase == TOMATO_DONE) {
            fetch_covers();
            return APP_REDRAW_PAGE;
        }
        if (s_last == OP_COVER) {
            // 封面到了不立刻整页刷：让本页缩略图排队生成，生成完 book_cover_tick 再刷一次。
            // Covers arrived: queue this page's thumbnails; book_cover_tick redraws once they are built.
            if (s_view == V_RANK) {
                char cover[96];
                for (int k = 0; k < GRID_PER; k++) {
                    const int i = s_page * GRID_PER + k;
                    if (i < s_book_count && cover_file(&s_books[i], cover, sizeof(cover))) book_cover_load(cover);
                }
            } else if (s_view == V_DETAIL) {
                char cover[96];
                if (cover_file(&s_books[s_detail], cover, sizeof(cover))) book_cover_load(cover);
            }
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_PAGE;
    }
    if (s_status.revision == s_seen_revision || s_view != V_DETAIL || s_op != OP_DOWNLOAD || !detail_is_dl())
        return APP_REDRAW_NONE;
    s_seen_revision = s_status.revision;
    return APP_REDRAW_PAGE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_cats) s_cats = heap_caps_calloc(TOMATO_RANK_CAT_MAX, sizeof(*s_cats), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_books) s_books = heap_caps_calloc(TOMATO_RANK_BOOK_MAX, sizeof(*s_books), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_shelf) s_shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*s_shelf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static bool prefs_loaded;
    if (!prefs_loaded) { load_prefs(); prefs_loaded = true; }
    tr_nav_set_tomato_page(&app_tr_store);
    book_cover_drop_pending();
    load_shelf();
    load_index();
    s_detail_note[0] = 0;
    // 分类胶囊分页时，翻到选中分类所在的那页。/ With paged chips, show the page holding the selected category.
    s_chip_page = 0;
    if (s_cur >= 0 && s_cat_count > 0) {
        EpdRect rects[TOMATO_RANK_CAT_MAX];
        int idx[TOMATO_RANK_CAT_MAX], page[TOMATO_RANK_CAT_MAX], pages;
        const int n = layout_chips(rects, idx, page, TOMATO_RANK_CAT_MAX, &pages);
        for (int k = 0; k < n; k++)
            if (idx[k] == s_cur) s_chip_page = page[k];
    }
    tomato_sync_get_status(&s_status);
    // 之后有别的任务开始过：上次结果的状态已被盖掉，不再显示。/ Another job started since: the last result's status
    // was overwritten, so stop showing it.
    if (s_last != OP_NONE && tomato_sync_seq() != s_op_seq) s_last = OP_NONE;
    // 后台还在跑就保留原视图，否则回到书城首页。/ Keep the view while work runs; otherwise start at the store home.
    if (!(tomato_sync_busy() && tomato_sync_seq() == s_op_seq) || s_op == OP_NONE) { s_view = V_HOME; s_op = OP_NONE; }
    if (s_view != V_HOME) load_books();
    if (s_view == V_DETAIL && s_detail >= s_book_count) s_view = V_HOME;
    // 显示大小变了每页本数也变：榜单页码夹回范围。/ Books per page follow the display size: clamp the rank page.
    if (s_page >= rank_pages()) s_page = rank_pages() - 1;
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    book_cover_drop_pending();
    s_queued = Q_NONE;
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    tomato_sync_cancel();
    s_view = V_HOME;
    s_cat_count = -1;
    s_book_count = -1;
    s_shelf_count = 0;
}

const app_desc_t app_tr_store = {
    .title = "书城",
    .detail = "番茄榜单",
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
