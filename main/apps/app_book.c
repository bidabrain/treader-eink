/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 图书书架、阅读、目录与页内进度；文件解析和排版由book模块负责。
 * Book shelf, reader, TOC and progress; book modules own parsing and pagination.
 *
 * 冻结：Phase4b统一手势入口并接管三键为上页/工具条/下页；工具条保留强刷，长按中键或把手打开演示菜单。屏幕翻页在抬起提交，不画按下态。
 * 晃动实验默认关，只翻下一页；离页关闭AOI2并休眠。render只绘图。
 * 预渲染回调返回前收齐，避免菜单/锁屏绕过页内TTF锁。
 * 普通翻页正文与页脚分区刷新；页脚只驱动变化像素，整屏强刷仍清全屏。
 * 中键按下切工具条，持续按住500ms返回演示菜单（用户新增快捷出口）。
 * 用户授权基础管理：长按书架先看完整详情，清进度与删文件分别确认；失败保留待重试记录，不自动回收其他书进度。
 * 用户修订：单本管理为书架弹窗；管理页用于批量操作。分页和排序保留勾选，筛选/应用搜索及重扫清除勾选。
 * 失败进度仅按变更路径失效；删除后的清理重试保留到本次开机结束，不随切页释放。
 * 卡失效时先保存进度并关闭阅读资源，再由主循环回退字体；禁止自动续读失效挂载。
 * Treader 修订：阅读工具条的“清残影”换成“排版”（中键长按就是整屏刷新），排版面板调行距、段距、页边距、首行缩进和行下虚线，
 * 改了立即按当前位置重排并存进设置。/ Treader revision: the reading toolbar's full-refresh tool becomes Layout (a
 * middle-key hold already refreshes); its panel sets line and paragraph spacing, margins, first-line indent and rules
 * under lines, re-paginating at the current position at once and saving to settings.
 * Treader 修订（2026-11-17）：首页并入书库，书库是开机页。原来的续读弹窗换成书架顶部常驻的“继续阅读”卡片，点了才打开，
 * 大书照样不会在进书架时阻塞；番茄有更新时状态行右侧提示，点了去番茄页。中键（home）在书架上回第一页，已在第一页则整屏刷新。
 * 用户修订：图片点击后才加载，预览返回不改变正文分页；只缓存当前章节最近查看的一幅图，已读重复位置不冒充全书首次位置。
 * Treader 修订（1.2.0，用户要求番茄照漫画的样子并进书库）：番茄不再是单独的标签。设置了番茄服务器时，书库右上角多出“书城”，
 * 状态行右侧原来跳番茄页的更新提示改成就地的“检查更新 / 下载 N 本更新”，进度和“取消”也在这里；长按番茄书的详情里多出
 * “检查这本更新”。来源胶囊叫“全部 / 本地 / 番茄”。检查和下载仍只在点了以后运行。
 * 用户修订（状态行的字容易按错）：番茄更新改成筛选条最右边的胶囊，和漫画的“检查更新”一样；在跑时点一下变“再点取消”，再点才取消。
 * Frozen: Phase4b uses the shared gesture entry and owns previous/tools/next keys; the toolbar keeps full refresh and the middle-key hold or handle opens the demo menu. Screen turns commit on release without pressed decoration.
 * Shake is experimental, off by default, forward only; exit disables AOI2 and sleeps it. Render only paints.
 * Join preparation before returning callbacks so menus/lock cannot race the page-local TTF lock.
 * Ordinary turns refresh body and footer separately; the footer drives changed pixels only, while full refresh still clears the whole screen.
 * Middle-key press toggles tools; holding for 500ms opens the demo menu, as requested.
 * User-authorized management shows full details before separate clear/delete confirmations; retain failed saves for retry without pruning other books.
 * User revision: single-book actions use a shelf dialog; full management is for batches. Paging/sorting preserve selection; filtering/applied search and rescanning clear it.
 * Invalidate failed progress only for changed paths; retain deletion cleanup retries across page exits for this boot.
 * Lost media saves progress and closes reader resources before global font fallback; never auto-resume an invalid mount.
 * Treader revision (2026-11-17): Home merged into the library, which is now the boot page. The resume dialog is replaced
 * by a persistent "continue reading" card at the top of the shelf that opens only on a tap, so a large book still never
 * blocks shelf entry; pending Tomato updates show on the right of the status line and lead to the Tomato page. On the
 * shelf the home key returns to page one, or does a full refresh when already there.
 * Treader revision (1.2.0, the user asked for Tomato to join the library the way comics work): Tomato is no longer a
 * tab of its own. With a Tomato server set, "书城" appears at the library's top right; the status-line hint that used
 * to lead to the Tomato page becomes in-place "检查更新 / 下载 N 本更新" with progress and "取消" right there; a Tomato
 * book's long-press detail gains "检查这本更新". The source pill reads All / Local / Tomato. Checks and downloads still
 * run only on a tap.
 * User revision (the status-line text was easy to mis-tap): the Tomato update became a pill at the right end of the chip
 * row, like the comics' "检查更新"; while running one tap shows "再点取消" and a second cancels.
 * User revision: load images only after a tap and return without repagination; cache one viewed image in the current chapter and distinguish visited-chapter origins from the first occurrence in the whole book.
 * Treader 修订：书架和阅读工具条改成 RickyOS 风格（docs/design/ui.html），书架带底部标签栏；工具条的晃动开关换成“菜单”入口，
 * 去掉阅读页常驻菜单把手。番茄下载的书显示书名、作者和新章节数。功能和交互逻辑不变。
 * Treader revision: shelf and reading toolbar restyled after RickyOS (docs/design/ui.html) with a tab bar on the shelf;
 * the toolbar shake toggle becomes a Menu entry and the reader drops its persistent menu handle. Tomato books show
 * title, author and new chapters. Behavior and interaction logic are unchanged.
 * Treader 修订（2026-11-20，用户要求按 Material 3 灰阶重做界面）：书架、继续阅读卡片、详情和确认弹窗、工具条与目录换成 Material 3
 * 灰阶样式；按钮位置、点击区域和刷新方式都不变。会被 DU 局部刷新的地方（按下态、搜索框、筛选胶囊、排版选项）只用黑白，
 * 灰色只做整页刷新的底色，按下的描边画在灰块外面。
 * Treader revision (2026-11-20, the user asked for a Material 3 gray redesign): the shelf, continue card, detail and
 * confirm dialogs, toolbar and TOC take the Material 3 gray style; control positions, hit areas and refresh modes are
 * unchanged. Anything DU updates (pressed states, the search field, filter pills, layout choices) stays black and white;
 * grays are only backgrounds for page refreshes, and pressed outlines are drawn outside gray blocks.
 * Treader 修订（1.0.2，用户要求）：正文点按的翻页区域可在设置里选：左右（默认，原行为）、中下翻页、点哪都翻；插图、滑动、
 * 长按和三键不变。
 * Treader revision (1.0.2, user request): the reader's tap zones are a setting: sides (default, the old behavior),
 * lower or anywhere; images, swipes, long press and the keys are unchanged.
 * Treader 修订（1.2.2，用户要求）：目录右上角一个跳转按钮，不在最后一页时写“最后一章”、点了翻到目录末页，在末页时写
 * “第一章”、点了回第一页；目录始终正序，只有一页时没有这个按钮。替换 1.2.1 的“正序 / 倒序”。
 * Treader revision (1.2.2, user request): one jump button at the TOC's top right reads "最后一章" before the last page
 * and turns to it, and "第一章" on the last page and returns to page one; the TOC stays in order and a one-page TOC has
 * no button. Replaces 1.2.1's "正序 / 倒序".
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "app_registry.h"
#include "book_layout.h"
#include "book_policy.h"
#include "book_cover.h"
#include "book_progress.h"
#include "book_source.h"
#include "book_store.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "read_pico_init.h"
#include "read_pico_sd.h"
#include "read_pico_search.h"
#include "settings.h"
#include "tomato.h"
#include "tr_kbd.h"
#include "tr_nav.h"
#include "auto_turn.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_gesture.h"
#include "ui_menu.h"

// 界面长度随显示大小放大（阅读正文另有字号，不跟着变）；书架和目录一页几行按剩下的高度算。
// UI lengths scale with the display size (the reading body keeps its own font size); shelf and TOC rows per page
// follow the height that is left.
#define BOOK_ROWS shelf_rows()
#define TOC_NAV_Y (TR_H - TR_S(106))
#define BOOK_TOC_ROWS ((TOC_NAV_Y - TR_S(210)) / TR_S(88))
#define BOOK_PX_MIN 36
#define BOOK_PX_MAX 72
#define BOOK_PX_STEP 4
#define BOOK_SHAKE_THS_MG 192
#define BOOK_SHAKE_DUR 2
#define BOOK_SIZE_SETTLE_MS 400
#define BOOK_TOOL_COUNT 11
#define SHELF_ROW_TOP TR_S(290)
#define SHELF_ROW_H TR_S(140)
#define SHELF_PAGER_Y (TR_TABS_Y - TR_S(84))
#define CARD_TOP TR_S(284)
#define CARD_H TR_S(136)
#define CARD_ROWS_TOP TR_S(436)   // 有卡片时书目从这里开始 / Rows start here when the card shows
#define CONTROL_CARD 130
#define CONTROL_TOMATO_HINT 117
#define CONTROL_STORE 119
#define CONTROL_TOC_JUMP 131
#define TOOL_PANEL_Y (UI_BAR_TOP - TR_S(416))   // 下面多了一行自动翻页 / One more row below for auto turn
#define AUTO_ROW_Y (TOOL_PANEL_Y + TR_S(326))
// 续读和批量确认弹窗的按钮行。/ Button row of the resume and batch-confirm dialogs.
#define DIALOG_BTN_Y TR_S(620)
#define BOOK_TOOL_ROWS 2

typedef enum { SHELF, READING, TOC, MANAGE, BULK, SEARCH } book_view_t;
typedef struct {
    char name[256];
    char path[BOOK_STORE_PATH_MAX];
    uint32_t size;
    bool is_flash;
    bool has_progress;
    uint8_t pct;
    uint32_t recent;
    bool selected, removed, search_match;
    bool tomato;               ///< 番茄下载的书 / Downloaded from Tomato
    uint16_t new_chapters;     ///< 番茄检查到、还没下载的新章节数 / New chapters found by a Tomato check, not downloaded yet
    uint16_t fresh_chapters;   ///< 已下载、打开后还没看的新章节数 / New chapters downloaded but not opened since
    char title[96];            ///< 番茄书名，空则用文件名 / Tomato title; empty means use the file name
    char author[48];
    char book_id[TOMATO_ID_LEN];   ///< 番茄书号（“检查这本更新”用）/ Tomato book id (for "检查这本更新")
} shelf_entry_t;

static const char* TAG = "book";
extern const app_desc_t app_transfer;
extern const app_desc_t app_tr_store;
static book_view_t s_view;
static shelf_entry_t* s_shelf;
static size_t s_shelf_capacity;
static int s_count, s_visible_count, s_filter;
static bool s_recent_sort;
static shelf_entry_t s_managed;
static bool s_delete_confirm, s_file_removed;
static char s_shelf_warning[128], s_manage_message[128];
static char s_query[65], s_search_draft[65], s_batch_message[128];
static book_view_t s_search_parent;
EXT_RAM_BSS_ATTR static tr_kbd_t s_search_kbd;
static bool s_batch_confirm, s_batch_delete;
typedef struct pending_progress {
    char path[BOOK_STORE_PATH_MAX];
    book_progress_t value;
    bool dirty, progress_saved;
    book_progress_watch_t* watch;
    struct pending_progress* next;
} pending_progress_t;
static pending_progress_t* s_pending;
typedef struct delete_retry {
    shelf_entry_t entry;
    struct delete_retry* next;
} delete_retry_t;
static delete_retry_t* s_delete_retries;
static char s_latest_path[BOOK_STORE_PATH_MAX];
static bool s_save_failed;
static bool s_pending_invalidated;
// 阅读排版：设置里存的样式，和排版面板是否打开。/ Reading layout: the stored style and whether the panel is open.
static app_book_style_t s_style = {.line = 1, .para = 1, .margin = 1};
static bool s_typeset;
static const int LINE_PCT[3] = {130, 150, 180};
static const int MARGIN_PX[3] = {24, 40, 64};
#define TYPESET_Y (UI_BAR_TOP - TR_S(552))
#define TYPESET_ROWS 6
#define TYPESET_ROW0 (TYPESET_Y + TR_S(84))
#define TYPESET_ROW_H TR_S(76)
static void apply_style(void) {
    book_layout_set_style(LINE_PCT[s_style.line], s_style.para, s_style.indent, s_style.underline, !s_style.keep_blank);
}
static int64_t s_save_retry_ms;
static unsigned s_store_revision;
static bool s_scan_pending, s_resume_pending, s_toolbar, s_clear_confirm;
// 自动翻页：档位、上次计时起点、到书尾停下。/ Auto turn: level, countdown start, stopped at the end.
static int s_auto_level;
static int64_t s_auto_ms;
static bool s_auto_done;
static bool s_auto_was_on;
static int64_t s_auto_tick;
static char s_resume_path[BOOK_STORE_PATH_MAX], s_resume_name[256];
// 最近读的那本，扫描书架时从进度记录取。/ Most recently read book, taken from progress records at each scan.
static char s_continue[BOOK_STORE_PATH_MAX];
// 别的页面要求直接打开的书（首页“接着读”、番茄书架）。/ Book another page asked to open directly (Home, Tomato).
static char s_open_request[BOOK_STORE_PATH_MAX];
// 番茄并进书库：本页发起的检查或下载更新、它的序号和结束后的结果。/ Tomato in the library: the check or update this
// page started, its sequence and the outcome once it ends.
typedef enum { TOM_NONE = 0, TOM_CHECK, TOM_UPDATE, TOM_ONE } tom_op_t;
static tom_op_t s_tom_op;
static uint32_t s_tom_seq, s_tom_rev;
static bool s_tom_cfg;
static char s_tom_note[96];
// 更新中点了一下“更新中”胶囊，等第二下才取消；点别处复原。/ The running pill was tapped once; a second tap cancels and
// any other tap resets it.
static bool s_tom_confirm_cancel, s_tom_cancelling;
static int64_t s_tom_poll_ms, s_tom_draw_ms;
static uint32_t s_tom_bytes;
#define TOM_BYTES_REDRAW_MS 2000   // 下载中按字节刷进度的间隔 / Interval for byte-driven progress redraws while downloading

void app_book_request_open(const char* path) {
    snprintf(s_open_request, sizeof(s_open_request), "%s", path ? path : "");
}
// 正在阅读的书的路径，给同步任务查询；用自旋锁保护跨任务读写。
// Path of the book being read, queried by the sync task; a spinlock guards cross-task access.
static portMUX_TYPE s_open_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_open_shared[BOOK_STORE_PATH_MAX];
static void publish_open_path(const char* path) {
    portENTER_CRITICAL(&s_open_lock);
    snprintf(s_open_shared, sizeof(s_open_shared), "%s", path);
    portEXIT_CRITICAL(&s_open_lock);
}
bool app_book_path_is_open(const char* path) {
    portENTER_CRITICAL(&s_open_lock);
    bool open = s_open_shared[0] && !strcmp(s_open_shared, path);
    portEXIT_CRITICAL(&s_open_lock);
    return open;
}
static char s_message[128], s_storage[128], s_path[BOOK_STORE_PATH_MAX], s_title[128];
static char s_font_path[192];
static char* s_text;
static blk_t* s_blocks;
static size_t s_block_count;
static size_t s_text_len, s_chapter, s_page;
static bool s_image_open;
static uint8_t* s_image_pixels;
static uint16_t s_image_width, s_image_height;
static size_t s_image_block = SIZE_MAX;
static const char* s_image_error;
static char s_image_origin[224];
static uint32_t s_file_size;
static int s_px, s_turns, s_unsaved;
static int64_t s_poll_ms, s_last_turn_ms, s_size_settle_ms, s_sensor_ms;
static bool s_shake_enabled, s_sensor_on;
static bool s_sensor_saved;
static sc7a20h_sensor_config_t s_sensor_config;
static book_shake_gate_t s_shake;
static EpdRect s_area;
static enum EpdDrawMode s_mode = MODE_GL16;
// 本次局刷用的波形表；快速翻页换成 8 灰阶表，刷完复原。/ Waveform for this area push; fast turns use the 8-gray
// table and it resets after the push.
static const EpdWaveform* s_wave = &E0470_WAVEFORM;
static bool s_full;
static bool s_reader_split;
static int s_pressed_control = -1;
static int64_t s_du_ms;
static unsigned s_du_count;
static EpdRect s_du_area;
static SemaphoreHandle_t s_draw_lock, s_prep_done;
static TaskHandle_t s_prep_task;
static uint8_t* s_next_fb;
static int s_next_page = -1, s_prep_page = -1;

static void render(app_ctx_t* ctx, uint8_t* fb);
static void scan_shelf(app_ctx_t* ctx);
static void free_book(void);
static void save_progress(void);
static void invalidate_prep(void);
static void sort_shelf(app_ctx_t* ctx);
static int shelf_rows(void);
static void draw_control(uint8_t* fb, EpdRect rect, const char* label, int id) {
    if (id == 112 && s_query[0]) {
        ui_fill_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2,
                   UI_PX_BTN, label, EPD_DRAW_ALIGN_CENTER, true);
        return;
    }
    if (s_pressed_control == id) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
    ui_draw_button(fb, rect, label, false);
}
static void lock_draw(void) { if (s_draw_lock) xSemaphoreTake(s_draw_lock, portMAX_DELAY); }
static void unlock_draw(void) { if (s_draw_lock) xSemaphoreGive(s_draw_lock); }
static size_t fb_bytes(void) { return (size_t)epd_width() * epd_height() / 2; }
// 旋转后差分列向32像素对齐，分界必须落在对齐边上才不会驱动进度条。
// Rotated diff columns expand to 32 pixels; align the boundary so body updates cannot drive the track.
// 页脚（工具条收起时）：上面一条胶囊进度条，下面一行小字（章名、页码和百分比）。正文和翻页局刷区到页脚上沿为止。
// / Footer (toolbar closed): a capsule track above one small line (chapter, page and percent). The body and the
// page-turn refresh area stop at its top edge.
#define FOOTER_H TR_S(52)
static int footer_top(void) { return UI_LOCK_HEIGHT - FOOTER_H; }
static EpdRect reader_area(void) { return (EpdRect){0, 0, UI_LOCK_WIDTH, (footer_top() / 32) * 32}; }
static EpdRect body_rect(void) {
    int top = ttf_font_is_builtin() ? TR_S(140) : 24;
    const int m = MARGIN_PX[s_style.margin];
    // 底边不超过翻页局刷区（按 32 对齐）：放大后页脚变高，否则最后一行落在局刷区外、翻页留残影。
    // Keep the bottom inside the 32-aligned page-turn refresh area: with a taller scaled footer the last line would
    // otherwise fall outside it and keep the previous page's ink.
    const int bottom = footer_top() - 8 < reader_area().height ? footer_top() - 8 : reader_area().height;
    return (EpdRect){m, top, UI_LOCK_WIDTH - 2 * m, bottom - top};
}
// 排版面板：六行选项，前三行三选一，后三行二选一。/ Layout panel: six rows; three choices each for the first three, two
// for the rest.
static int typeset_choices(int row) { return row < 3 ? 3 : 2; }
static EpdRect typeset_rect(int row, int opt) {
    const int n = typeset_choices(row), gap = TR_S(12), x0 = 224;
    const int w = (UI_LOCK_WIDTH - UI_MARGIN - x0 - gap * (n - 1)) / n;
    return (EpdRect){x0 + opt * (w + gap), TYPESET_ROW0 + row * TYPESET_ROW_H, w, TR_S(56)};
}
static EpdRect typeset_done_rect(void) {
    return (EpdRect){UI_LOCK_WIDTH - UI_MARGIN - TR_S(140), TYPESET_Y + TR_S(14), TR_S(140), TR_S(56)};
}
static int typeset_value(int row) {
    return row == 0 ? s_style.line : row == 1 ? s_style.para : row == 2 ? s_style.margin : row == 3 ? s_style.indent :
           row == 4 ? s_style.underline : s_style.keep_blank;
}
static void draw_typeset(uint8_t* fb) {
    static const char* const NAMES[TYPESET_ROWS] = {"行距", "段距", "页边距", "首行缩进", "下划线", "空行"};
    static const char* const CHOICES[TYPESET_ROWS][3] = {
        {"紧凑", "标准", "宽松"}, {"无", "小", "大"}, {"窄", "中", "宽"}, {"按原文", "缩进两字", ""}, {"无", "有", ""},
        {"忽略", "保留", ""},
    };
    epd_fill_rect((EpdRect){0, TYPESET_Y, UI_LOCK_WIDTH, UI_BAR_TOP - TYPESET_Y}, UI_GRAY_WHITE, fb);
    tr_hline(fb, TYPESET_Y, 0, UI_LOCK_WIDTH, false);
    tr_text(fb, UI_MARGIN, TYPESET_Y + TR_S(24), TR_S(32), "排版", EPD_DRAW_ALIGN_LEFT, true);
    tr_pill(fb, typeset_done_rect(), "完成", true, TR_S(28));
    for (int row = 0; row < TYPESET_ROWS; ++row) {
        tr_text(fb, UI_MARGIN, TYPESET_ROW0 + row * TYPESET_ROW_H + TR_S(14), TR_S(28), NAMES[row], EPD_DRAW_ALIGN_LEFT,
                false);
        for (int opt = 0; opt < typeset_choices(row); ++opt)
            tr_pill(fb, typeset_rect(row, opt), CHOICES[row][opt], typeset_value(row) == opt, TR_S(28));
    }
}
static EpdRect progress_rect(void) {
    EpdRect b = ui_bar_rect(0, 1);
    return (EpdRect){b.x, UI_BAR_TOP, b.width, UI_BAR_H};
}
// 工具条开着时页脚两边放“上一章”“下一章”（宽度不放大，只放大高度），进度条夹在中间；收起时进度条占满整行。
// With the toolbar open the footer gets "previous" and "next chapter" at the sides (widths stay, heights scale) and
// the track sits between them; closed, the track spans the whole row.
#define CHAPTER_BTN_W 150
#define CHAPTER_BTN_GAP 16
// 工具条开着时页脚占满左右页边距之间（收起时右边留给老的菜单把手位置，不动）。/ With the toolbar open the footer spans
// the full width between the side margins (closed, it keeps the old handle's space on the right untouched).
static EpdRect chapter_btn_rect(int dir) {
    const int x = dir < 0 ? UI_MARGIN : UI_LOCK_WIDTH - UI_MARGIN - CHAPTER_BTN_W;
    return (EpdRect){x, UI_BAR_TOP + TR_S(8), CHAPTER_BTN_W, TR_S(48)};
}
static EpdRect track_rect(void) {
    EpdRect t = progress_rect();
    if (!s_toolbar) return t;
    t.x = UI_MARGIN + CHAPTER_BTN_W + CHAPTER_BTN_GAP;
    t.width = UI_LOCK_WIDTH - 2 * t.x;
    return t;
}
// 这一方向还有没有章可跳。/ Whether there is a chapter to jump to in this direction.
static bool chapter_jump_ok(int dir) {
    return dir < 0 ? s_chapter > 0 : s_chapter + 1 < book_chapter_count();
}
// “继续阅读”卡片对应的书在 s_shelf 里的下标，没有返回 -1。/ Shelf index of the continue-card book, or -1.
static int continue_index(void) {
    if (!s_continue[0]) return -1;
    for (int i = 0; i < s_count; ++i)
        if (!s_shelf[i].removed && !strcmp(s_shelf[i].path, s_continue)) return i;
    return -1;
}
// 有“继续阅读”的书、又没在搜索：书架按有卡片的布局排。/ A continue book and no search: the shelf uses the card layout.
static bool card_layout(void) { return !s_query[0] && continue_index() >= 0; }
// 卡片只在普通书架、没在搜索时显示。/ The card shows on the plain shelf only, not while searching.
static bool card_shown(void) {
    return s_view == SHELF && card_layout();
}
// 书架一页几本：列表是翻页条上面放得下几行（最多 4 行），网格是行数乘列数；书架、管理、详情弹窗用同一个数，页码不乱。
// Books per shelf page: rows above the pager in the list (at most 4), rows times columns in the grid; shelf, manage
// mode and the detail dialog share the number so page numbers stay put.
// 网格（只在书架和管理里）：三列封面，下面书名和进度；行数按剩下的高度算，封面不小于整列宽时的七成才多排一行。
// / Grid (shelf and manage only): three columns of covers with the title and progress below; rows follow the height
// left, and one more row is taken only while covers stay at least 70% of their full-column size.
#define GRID_COLS 3
#define GRID_GAP TR_S(20)
#define GRID_TEXT_H TR_S(70)
static bool shelf_grid(void) { return (s_view == SHELF || s_view == BULK) && app_settings_shelf_grid(); }
typedef struct { int rows, cell_w, cell_h, cover_w, cover_h; } grid_t;
static grid_t grid_layout(void) {
    grid_t g;
    const int top = card_layout() ? CARD_ROWS_TOP : SHELF_ROW_TOP, avail = SHELF_PAGER_Y - TR_S(24) - top;
    g.cell_w = (ui_content_width() - GRID_GAP * (GRID_COLS - 1)) / GRID_COLS;
    const int full = g.cell_w * 4 / 3, min = full * 7 / 10;
    g.rows = (avail + GRID_GAP) / (min + GRID_TEXT_H + GRID_GAP);
    if (g.rows < 1) g.rows = 1;
    if (g.rows > 3) g.rows = 3;
    g.cell_h = (avail - GRID_GAP * (g.rows - 1)) / g.rows;
    g.cover_h = g.cell_h - GRID_TEXT_H;
    if (g.cover_h > full) g.cover_h = full;
    g.cover_w = g.cover_h * 3 / 4;
    return g;
}
static int shelf_rows(void) {
    if (shelf_grid()) return grid_layout().rows * GRID_COLS;
    const int top = card_layout() ? CARD_ROWS_TOP : SHELF_ROW_TOP;
    const int n = (SHELF_PAGER_Y - TR_S(20) - top + TR_S(8)) / SHELF_ROW_H;
    return n < 1 ? 1 : n > 4 ? 4 : n;
}
static EpdRect card_rect(void) { return (EpdRect){UI_MARGIN, CARD_TOP, ui_content_width(), CARD_H}; }
// 筛选条最右边的番茄更新胶囊，和漫画的“检查更新”一样。/ The Tomato update pill at the right end of the chip row,
// like the comics' "检查更新".
#define TOMATO_PILL_X 426
static EpdRect tomato_hint_rect(void) {
    return (EpdRect){TOMATO_PILL_X, TR_S(212), UI_LOCK_WIDTH - UI_MARGIN - TOMATO_PILL_X, TR_S(54)};
}
// 番茄任务在跑时，状态行、下面的进度条和更新胶囊一起局刷。/ While a Tomato job runs, the status line, the bar under it
// and the update pill refresh together.
static EpdRect tomato_busy_rect(void) { return (EpdRect){0, TR_S(146), UI_LOCK_WIDTH, TR_S(126)}; }
static EpdRect view_icon_rect(void);
// “书城”在列表 / 网格切换图标左边（设置了番茄服务器才有）。/ "书城" left of the list / grid switch (only with a
// Tomato server set).
static EpdRect store_rect(void) {
    const EpdRect v = view_icon_rect();
    const int w = ttf_text_width_px(TR_S(30), "书城") + TR_S(24);
    return (EpdRect){v.x - w, v.y, w, v.height};
}
// 番茄书架上有新章节的本数。/ Tomato books with new chapters.
static int tomato_pending(void) {
    int n = 0;
    for (int i = 0; i < s_count; ++i) n += s_shelf[i].new_chapters > 0;
    return n;
}
// 书库里有没有番茄下载的书。/ Whether the library holds any Tomato book.
static bool tomato_any(void) {
    for (int i = 0; i < s_count; ++i) if (s_shelf[i].tomato && !s_shelf[i].removed) return true;
    return false;
}
// 本页发起的番茄任务还在跑。/ The Tomato job this page started is still running.
static bool tom_running(void) { return s_tom_op != TOM_NONE && tomato_sync_busy() && tomato_sync_seq() == s_tom_seq; }
static app_redraw_t tomato_action(const char* book_id);
// 番茄更新胶囊：设了服务器、书库里有番茄书（或本页任务在跑），不在管理和搜索时出现。/ The Tomato update pill: with a
// server set and Tomato books in the library (or this page's job running), outside manage mode and search.
static bool tomato_hint_shown(void);
static bool tomato_pill_room(void);
static EpdRect row_rect(int row, bool toc) {
    if (toc) return (EpdRect){UI_MARGIN, TR_S(210) + row * TR_S(88), ui_content_width(), TR_S(84)};
    const int top = card_shown() ? CARD_ROWS_TOP : SHELF_ROW_TOP;
    if (shelf_grid()) {
        // 网格：第 row 本所在的格子（封面加两行字）。封面封顶后格子比内容高，高度只算到字下面，选中框才贴着书。
        // Grid: the cell of the row-th book (cover plus two lines). Once the cover is capped the cell is taller than
        // its content, so the height stops below the text and the selection outline hugs the book.
        const grid_t g = grid_layout();
        const int col = row % GRID_COLS, line = row / GRID_COLS;
        const int h = g.cover_h + GRID_TEXT_H < g.cell_h ? g.cover_h + GRID_TEXT_H : g.cell_h;
        return (EpdRect){UI_MARGIN + col * (g.cell_w + GRID_GAP), top + line * (g.cell_h + GRID_GAP), g.cell_w, h};
    }
    return (EpdRect){UI_MARGIN, top + row * SHELF_ROW_H, ui_content_width(), SHELF_ROW_H - TR_S(12)};
}
// 工具条：0 目录、1 字号−、2 字号+、3 菜单、4 排版、5 书架、6 顶部返回、7 自动翻页−、8 自动翻页+、9 上一章、10 下一章。
// Toolbar: 0 TOC, 1 size-, 2 size+, 3 menu, 4 layout, 5 shelf, 6 top back, 7 auto turn -, 8 auto turn +,
// 9 previous chapter, 10 next chapter.
// 宽度占满内容区不放大，只放大高度；自动翻页那行的热区跟着 auto_turn 放大后的按钮。
// Widths fill the content area and stay; heights scale. The auto-turn row's press areas follow auto_turn's scaled
// buttons.
static EpdRect tool_rect(int i) {
    static const int slot[BOOK_TOOL_COUNT] = {0, -1, -2, 1, 2, 3, -3, -4, -5, -6, -7};
    int s = slot[i];
    if (s == -6 || s == -7) return chapter_btn_rect(s == -6 ? -1 : 1);
    if (s >= 0) return (EpdRect){UI_MARGIN + s * 151, TOOL_PANEL_Y + TR_S(80), 151, TR_S(120)};
    if (s == -1) return (EpdRect){UI_MARGIN, TOOL_PANEL_Y + TR_S(226), 200, TR_S(84)};
    if (s == -2) return (EpdRect){UI_LOCK_WIDTH - UI_MARGIN - 200, TOOL_PANEL_Y + TR_S(226), 200, TR_S(84)};
    if (s == -4) return (EpdRect){290, AUTO_ROW_Y - TR_S(8), AUTO_TURN_BTN_W + 20, AUTO_TURN_ROW_H + TR_S(16)};
    if (s == -5)
        return (EpdRect){UI_LOCK_WIDTH - UI_MARGIN - AUTO_TURN_BTN_W - 10, AUTO_ROW_Y - TR_S(8), AUTO_TURN_BTN_W + 20,
                         AUTO_TURN_ROW_H + TR_S(16)};
    return (EpdRect){0, 0, 320, TR_S(110)};
}
// 书架筛选条：0 来源、1 排序、2 管理/完成、3 清除搜索。有番茄更新胶囊时前三个收窄给它腾地方。
// Shelf chips: 0 source, 1 sort, 2 manage/done, 3 clear search. With the Tomato update pill the first three narrow to
// make room for it.
static EpdRect chip_rect(int i) {
    static const int xs[4] = {40, 202, 364, 496}, ws[4] = {150, 150, 120, 148};
    static const int xs_tom[3] = {40, 172, 304}, ws_tom[3] = {120, 120, 110};
    if (i < 3 && tomato_pill_room()) return (EpdRect){xs_tom[i], TR_S(212), ws_tom[i], TR_S(54)};
    return (EpdRect){xs[i], TR_S(212), ws[i], TR_S(54)};
}
// 胶囊里的字放不下就缩小。/ Shrink a pill's label until it fits.
static int chip_px(EpdRect r, const char* label, int px) {
    while (px > 20 && ttf_text_width_px(px, label) > r.width - TR_S(24)) px -= 2;
    return px;
}
// 两个图标靠右排，格子随显示大小放大。/ The two icons sit at the right; their cells scale with the display size.
static EpdRect import_icon_rect(void) {
    return (EpdRect){UI_LOCK_WIDTH - UI_MARGIN + 4 - TR_S(72), TR_S(80), TR_S(72), TR_S(72)};
}
static EpdRect search_icon_rect(void) {
    const EpdRect r = import_icon_rect();
    return (EpdRect){r.x - r.width, r.y, r.width, r.height};
}
// 列表 / 网格切换，在搜索左边；画的是点了会换成的样子。/ List / grid switch left of search; it shows the view a tap
// switches to.
static EpdRect view_icon_rect(void) {
    const EpdRect r = search_icon_rect();
    return (EpdRect){r.x - r.width, r.y, r.width, r.height};
}
static EpdRect save_retry_rect(void) { return (EpdRect){UI_MARGIN, TR_S(150), ui_content_width(), TR_S(44)}; }
// 翻页：目录沿用官方底栏，书架用“‹ 1 / 5 ›”左右两半。/ Paging: TOC keeps the official bar; the shelf uses the pager halves.
static EpdRect nav_rect(int i) {
    if (s_view == TOC) {
        static const int xs[3] = {40, 222, 474}, ws[3] = {170, 240, 170};
        return (EpdRect){xs[i], TOC_NAV_Y, ws[i], TR_S(80)};
    }
    if (i == 1) return (EpdRect){0, 0, 0, 0};
    return (EpdRect){i == 0 ? UI_MARGIN : UI_LOCK_WIDTH / 2, SHELF_PAGER_Y - TR_S(16), UI_LOCK_WIDTH / 2 - UI_MARGIN,
                     TR_S(72)};
}
static int nav_hit(uint16_t x, uint16_t y) {
    for (int i = 0; i < 3; ++i) {
        EpdRect r = nav_rect(i);
        if (r.width && ui_rect_hit(r, x, y)) return i;
    }
    return -1;
}
static int leaves(void) {
    int count = s_view == TOC ? (int)book_chapter_count() : s_visible_count;
    int rows = s_view == TOC ? BOOK_TOC_ROWS : BOOK_ROWS;
    return count ? 1 + (count - 1) / rows : 1;
}
// 当前章节所在的目录页。/ TOC page holding the current chapter.
static int toc_leaf(void) { return (int)s_chapter / BOOK_TOC_ROWS; }
// 目录右上角的跳转按钮：不在最后一页写“最后一章”，在最后一页写“第一章”；目录只有一页时没有。
// The TOC jump button at the top right: "最后一章" before the last page, "第一章" on it; absent for a one-page TOC.
static bool toc_jump_shown(void) { return leaves() > 1; }
static const char* toc_jump_label(int leaf) { return leaf + 1 < leaves() ? "最后一章" : "第一章"; }
static EpdRect toc_jump_rect(void) {
    const int w = ttf_text_width_px(TR_S(30), "最后一章") + TR_S(24);
    return (EpdRect){UI_LOCK_WIDTH - UI_MARGIN + 4 - w, TR_S(80), w, TR_S(72)};
}

// 截断必须停在UTF8字符边界。/ Truncation must stop at a UTF8 character boundary.
static void copy_text(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}
static void fit_text(char* text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}
static uint32_t chapter_end(void) {
    return s_chapter + 1 < book_chapter_count()
        ? book_chapter_byte_offset(s_chapter + 1) : book_total_bytes();
}
static unsigned percent(size_t page) {
    uint32_t total = book_total_bytes();
    if (!total) return 0;
    if (book_layout_complete() && s_chapter + 1 == book_chapter_count() && page + 1 == book_layout_page_count()) return 100;
    uint32_t off = book_position_bytes(book_chapter_byte_offset(s_chapter), chapter_end(),
                                       book_layout_page_start_offset(page), s_text_len);
    return (unsigned)((uint64_t)off * 100 / total);
}
static pending_progress_t* pending_find(const char* path) {
    for (pending_progress_t* p = s_pending; p; p = p->next) if (!strcmp(p->path, path)) return p;
    return NULL;
}
static int layout_name(uint8_t* fb, const char* name, int y, bool draw);
// 番茄书的详情多一行“检查这本更新”（确认和重试时不显示）。/ A Tomato book's detail gets one more row, "检查这本更新"
// (not while confirming or retrying).
static bool manage_check_shown(void) {
    return s_tom_cfg && s_managed.tomato && s_managed.book_id[0] && !s_clear_confirm && !s_file_removed;
}
// 弹窗在标题下面到标签栏上面之间居中。/ The dialog centers between the header and the tab bar.
static EpdRect manage_panel(void) {
    int height = layout_name(NULL, s_managed.name, 0, false) + TR_S(420) + (manage_check_shown() ? TR_S(96) : 0);
    const int avail = TR_TABS_Y - TR_S(218);
    return (EpdRect){UI_MARGIN - 16, TR_S(190) + (avail - height) / 2, ui_content_width() + 32, height};
}
static EpdRect manage_rect(int index, int count) {
    EpdRect panel = manage_panel();
    return ui_row_rect(index, count, panel.y + panel.height - TR_S(94), TR_S(76));
}
static EpdRect manage_check_rect(void) {
    const EpdRect panel = manage_panel();
    return ui_row_rect(0, 1, panel.y + panel.height - TR_S(190), TR_S(76));
}
// 管理模式不显示标签栏，批量按钮放在底部。/ Manage mode hides the tab bar; batch buttons sit at the bottom.
static EpdRect batch_rect(int id) {
    if (id < 3) return ui_row_rect(id, 3, TR_H - TR_S(116), TR_S(48));
    return ui_row_rect(id - 3, 2, TR_H - TR_S(58), TR_S(48));
}
static size_t selected_count(void) {
    size_t selected = 0;
    for (int i = 0; i < s_count; ++i) if (s_shelf[i].selected) ++selected;
    return selected;
}
static void clear_selection(void) {
    for (int i = 0; i < s_count; ++i) s_shelf[i].selected = false;
}
static void toggle_selection(int index) {
    if (index >= 0 && index < s_visible_count) s_shelf[index].selected = !s_shelf[index].selected;
}
static void select_page(int page) {
    for (int i = page * BOOK_ROWS; i < s_visible_count && i < (page + 1) * BOOK_ROWS; ++i) s_shelf[i].selected = true;
}
static void search_begin(void) {
    s_search_parent = s_view;
    memcpy(s_search_draft, s_query, sizeof(s_query));
    // 文件名搜索多用首字母，默认英文；切到拼音时才加载词库。
    // File-name search mostly uses initials: start in English and load the lexicon only when pinyin is chosen.
    tr_kbd_open(&s_search_kbd, s_query, false, "搜索");
    s_view = SEARCH;
}
static void refresh_search_matches(void) {
    for (int i = 0; i < s_count; ++i)
        s_shelf[i].search_match = read_pico_search_match(s_shelf[i].name, s_query);
}
static void search_finish(app_ctx_t* ctx, bool apply) {
    s_view = s_search_parent;
    if (apply) copy_text(s_search_draft, sizeof(s_search_draft), s_search_kbd.text);
    memset(&s_search_kbd, 0, sizeof(s_search_kbd));
    if (apply) {
        memcpy(s_query, s_search_draft, sizeof(s_query));
        refresh_search_matches();
        clear_selection();
        sort_shelf(ctx);
        s_batch_message[0] = 0;
    }
    memset(s_search_draft, 0, sizeof(s_search_draft));
}
static bool pending_reserve(const char* path) {
    if (pending_find(path)) return true;
    pending_progress_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return false;
    memset(p, 0, sizeof(*p));
    copy_text(p->path, sizeof(p->path), path);
    // 阅读页内注册；传书 HTTP 尚未启动，退出后也不注销失败项。
    // Register in reading before transfer HTTP starts; failed records survive page exit.
    p->watch = book_progress_watch_create(path);
    if (!p->watch) { free(p); return false; }
    pending_progress_t** tail = &s_pending;
    while (*tail) tail = &(*tail)->next;
    *tail = p;
    return true;
}
static bool pending_restore(const char* path, uint32_t size, book_progress_t* out) {
    pending_progress_t* pending = pending_find(path);
    if (!pending || !pending->dirty || pending->value.file_size != size) return false;
    *out = pending->value;
    return true;
}
static void pending_discard(const char* path) {
    pending_progress_t** p = &s_pending;
    while (*p) {
        if (!strcmp((*p)->path, path)) {
            pending_progress_t* old = *p;
            *p = old->next;
            book_progress_watch_destroy(old->watch);
            free(old);
            break;
        }
        p = &(*p)->next;
    }
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}
static void pending_mark_latest(const char* path) {
    pending_progress_t** item = &s_pending;
    while (*item && strcmp((*item)->path, path)) item = &(*item)->next;
    if (*item) {
        pending_progress_t* current = *item;
        *item = current->next;
        current->next = NULL;
        pending_progress_t** tail = &s_pending;
        while (*tail) tail = &(*tail)->next;
        *tail = current;
    }
    copy_text(s_latest_path, sizeof(s_latest_path), path);
}
static void pending_drop_invalidated(void) {
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        if (book_progress_watch_invalidated(p->watch)) {
            if (p->dirty) s_pending_invalidated = true;
            pending_discard(p->path);
        }
        p = next;
    }
}
static delete_retry_t* delete_retry_find(const char* path) {
    for (delete_retry_t* p = s_delete_retries; p; p = p->next)
        if (!strcmp(p->entry.path, path)) return p;
    return NULL;
}
static bool pending_flush(pending_progress_t* p) {
    if (!p->dirty) return true;
    if (!p->progress_saved) {
        if (book_progress_save(p->path, &p->value) != ESP_OK) return false;
        p->progress_saved = true;
    }
    if (!strcmp(p->path, s_latest_path) && book_progress_set_last_path(p->path) != ESP_OK) return false;
    p->dirty = false;
    return true;
}
static void retry_progress(void) {
    s_save_failed = false;
    bool had_dirty = false;
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        bool dirty = p->dirty;
        had_dirty |= dirty;
        if (s_text && !strcmp(p->path, s_path)) { p = next; continue; }
        if (!pending_flush(p)) s_save_failed = true;
        else if (strcmp(p->path, s_path)) pending_discard(p->path);
        else if (dirty) s_unsaved = 0;
        p = next;
    }
    if (s_text && (s_unsaved || had_dirty)) save_progress();
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}

/* ---- 管理详情 / Management details ---- */
static int layout_name(uint8_t* fb, const char* name, int y, bool draw) {
    const char* at = name;
    while (*at) {
        char line[256];
        size_t used = 0;
        while (at[used]) {
            unsigned char first = (unsigned char)at[used];
            size_t n = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            size_t remain = strlen(at + used);
            if (n > remain) n = 1;
            if (used + n >= sizeof(line)) break;
            memcpy(line + used, at + used, n);
            line[used + n] = 0;
            if (ttf_text_width_px(UI_PX_CAPTION, line) > ui_content_width() && used) break;
            used += n;
        }
        if (!used) break;
        line[used] = 0;
        if (draw) ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        y += TR_S(36);
        at += used;
    }
    return y;
}
static int draw_wrapped_name(uint8_t* fb, const char* name, int y) {
    return layout_name(fb, name, y, true);
}
// Material 3 对话框：白底、大圆角、深灰细框（墨水屏上没有阴影）。/ Material 3 dialog: white, large radius, thin dark
// outline (no shadows on e-ink).
static void draw_dialog(uint8_t* fb, EpdRect panel) {
    ui_fill_round_rect(fb, panel, TR_S(28), UI_GRAY_WHITE);
    ui_draw_round_rect(fb, panel, TR_S(28), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(panel, 1), TR_S(28) - 1, TR_GRAY_OUTLINE);
}
static void draw_manage(uint8_t* fb) {
    EpdRect panel = manage_panel();
    draw_dialog(fb, panel);
    tr_text(fb, UI_MARGIN, panel.y + TR_S(18), TR_S(38),
            s_clear_confirm ? (s_delete_confirm ? "确认删除文件？" : "确认清除进度？") : "图书详情", EPD_DRAW_ALIGN_LEFT, true);
    int y = draw_wrapped_name(fb, s_managed.name, panel.y + TR_S(74)) + TR_S(12);
    char info[96];
    snprintf(info, sizeof(info), "%s · %s · %.2f MB", s_managed.is_flash ? "内置存储" : "TF 卡",
             strrchr(s_managed.name, '.') ? strrchr(s_managed.name, '.') + 1 : "", s_managed.size / 1048576.0);
    tr_text_dim(fb, UI_MARGIN, y, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT);
    snprintf(info, sizeof(info), s_managed.has_progress ? "阅读进度 %u%%" : "尚无阅读进度", s_managed.pct);
    tr_text_dim(fb, UI_MARGIN, y + TR_S(44), UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(fb, UI_MARGIN, y + TR_S(88), UI_PX_CAPTION, s_managed.is_flash ? "/flash/books" : "/sdcard/books", EPD_DRAW_ALIGN_LEFT);
    const int msg_dy = manage_check_shown() ? TR_S(294) : TR_S(198);
    if (s_manage_message[0]) ui_text(fb, UI_MARGIN, panel.y + panel.height - msg_dy, UI_PX_CAPTION, s_manage_message, EPD_DRAW_ALIGN_LEFT, false);
    if (manage_check_shown()) tr_pill(fb, manage_check_rect(), tom_running() ? "后台正在检查更新" : "检查这本更新", false, TR_S(30));
    if (s_clear_confirm) {
        ui_text(fb, UI_MARGIN, panel.y + panel.height - TR_S(142), UI_PX_CAPTION, s_delete_confirm ? "删除后文件无法恢复" : "仅清阅读进度，保留图书文件", EPD_DRAW_ALIGN_LEFT, false);
        tr_pill(fb, manage_rect(0, 2), "取消", false, TR_S(30));
        tr_pill(fb, manage_rect(1, 2), s_delete_confirm ? "确认删除" : "确认清除", true, TR_S(30));
    } else if (s_file_removed) {
        tr_pill(fb, manage_rect(0, 2), "关闭", false, TR_S(30));
        tr_pill(fb, manage_rect(1, 2), "重试清理", true, TR_S(30));
    } else {
        tr_pill(fb, manage_rect(0, 3), "关闭", false, TR_S(30));
        tr_pill(fb, manage_rect(1, 3), "清进度", false, TR_S(30));
        tr_pill(fb, manage_rect(2, 3), "删除文件", true, TR_S(30));
    }
}
// 书库搜索：输入框加 Treader 键盘，默认英文（匹配拼音首字母、全拼和英文），可切全拼输入汉字。
// Library search: field plus the Treader keyboard, English by default (initials, full pinyin, English); pinyin can enter Chinese.
static void draw_search(uint8_t* fb) {
    ui_clear_page(fb);
    tr_status_bar(fb);
    tr_text(fb, UI_MARGIN, TR_S(94), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect f = {96, TR_S(84), UI_LOCK_WIDTH - 96 - UI_MARGIN, TR_S(80)};
    tr_pill(fb, f, "", false, TR_S(30));
    tr_icon(fb, TR_ICON_SEARCH, f.x + TR_S(44), f.y + TR_S(40), false);
    const char* tail = tr_tail_fit(s_search_kbd.text, TR_S(34), f.width - TR_S(120));
    tr_text(fb, f.x + TR_S(78), f.y + TR_S(20), TR_S(34), tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + TR_S(80) + ttf_text_width_px(TR_S(34), tail), f.y + TR_S(18), 3, TR_S(44)},
                  UI_GRAY_BLACK, fb);
    tr_text_dim(fb, UI_MARGIN, TR_S(200), TR_S(28), "拼音首字母、全拼或英文都能搜", EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(fb, UI_MARGIN, TR_S(244), TR_S(26), "要输入汉字就把键盘切到“全拼”", EPD_DRAW_ALIGN_LEFT);
    tr_kbd_draw(fb, &s_search_kbd);
}
static void draw_batch_confirmation(uint8_t* fb) {
    if (!s_batch_confirm) return;
    EpdRect panel = {UI_MARGIN - 12, TR_S(410), ui_content_width() + 24, TR_S(330)};
    draw_dialog(fb, panel);
    char title[96];
    snprintf(title, sizeof(title), "%s %u 本图书？", s_batch_delete ? "删除" : "清除进度：", (unsigned)selected_count());
    tr_text(fb, UI_MARGIN, TR_S(438), TR_S(38), title, EPD_DRAW_ALIGN_LEFT, true);
    tr_text_dim(fb, UI_MARGIN, TR_S(508), UI_PX_CAPTION, s_batch_delete ? "删除文件不可撤销，失败项可重试" : "仅清阅读进度，所有文件保留", EPD_DRAW_ALIGN_LEFT);
    tr_pill(fb, ui_row_rect(0, 2, DIALOG_BTN_Y, UI_BTN_H), "取消", false, TR_S(32));
    tr_pill(fb, ui_row_rect(1, 2, DIALOG_BTN_Y, UI_BTN_H), "确认", true, TR_S(32));
}
// 番茄书在 books 目录下，更新只在末尾追加章节。/ Tomato books live in the books dir; updates only append.
static const char* tomato_file_of(const char* path) {
    const size_t n = strlen(TOMATO_BOOKS_DIR);
    return !strncmp(path, TOMATO_BOOKS_DIR, n) && path[n] == '/' ? path + n + 1 : NULL;
}
static void save_progress(void) {
    if (!s_text || !s_path[0] || !book_layout_page_count()) return;
    bool was_failed = s_save_failed;
    pending_progress_t* pending = pending_find(s_path);
    if (!pending) { s_save_failed = true; return; }
    book_progress_t p = {
        .file_size = s_file_size, .chapter = (uint16_t)s_chapter,
        .byte_off = (uint32_t)book_layout_page_start_offset(s_page),
        .px = (uint8_t)s_px, .pct = (uint8_t)percent(s_page),
        .last_open_s = 0,
    };
    if (!pending->dirty || pending->value.file_size != p.file_size || pending->value.chapter != p.chapter ||
        pending->value.byte_off != p.byte_off || pending->value.px != p.px || pending->value.pct != p.pct) {
        pending->value = p;
        pending->progress_saved = false;
    }
    pending->dirty = true;
    if (pending_flush(pending)) {
        s_unsaved = 0;
        s_save_failed = false;
        for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
    }
    else { if (!s_unsaved) s_unsaved = 1; s_save_failed = true; }
    if (was_failed != s_save_failed) invalidate_prep();
}
static void invalidate_prep(void) { s_next_page = s_prep_page = -1; }

// 正文上点一下：-1 上一页、1 下一页、0 开关工具条，按设置里的翻页区域分。工具条开着时，中下翻页和点哪都翻
// 只用点正文收起工具条，不翻页。
// A tap on the page: -1 back, 1 forward, 0 toggles the toolbar, split by the turn-zone setting. With the toolbar
// open, the lower and anywhere layouts only close it on a body tap instead of turning.
static int tap_zone(int x, int y) {
    const uint8_t mode = app_settings_turn_zone();
    if (mode == APP_TURN_SIDES) {
        if (x < UI_LOCK_WIDTH * 3 / 10) return -1;
        return x >= UI_LOCK_WIDTH * 7 / 10 ? 1 : 0;
    }
    if (s_toolbar || y < UI_LOCK_HEIGHT / 4) return 0;
    if (mode == APP_TURN_LOWER && x < UI_LOCK_WIDTH * 3 / 10) return -1;
    return 1;
}
static void free_image(void) {
    free(s_image_pixels); s_image_pixels = NULL;
    s_image_width = s_image_height = 0;
    s_image_block = SIZE_MAX; s_image_open = false;
    s_image_error = NULL; s_image_origin[0] = 0;
}
static app_redraw_t close_image(void) {
    s_image_open = false;
    invalidate_prep();
    return APP_REDRAW_PAGE;
}

/* ---- 绘制与预渲染 / Drawing and preparation ---- */
// 胶囊进度条：圆头浅灰底，已读部分圆头黑色。/ Capsule track: a round-ended light gray track and a round-ended black fill.
static void capsule(uint8_t* fb, int x, int y, int w, int h, unsigned pct) {
    ui_fill_round_rect(fb, (EpdRect){x, y, w, h}, h / 2, UI_GRAY_LIGHT);
    int fw = w * (int)pct / 100;
    if (fw > 0 && fw < h) fw = h;
    if (fw > 0) ui_fill_round_rect(fb, (EpdRect){x, y, fw, h}, h / 2, UI_GRAY_BLACK);
}
// 页脚刷新区：从 32 对齐的翻页局刷区下沿到屏幕底，翻页时单独差分刷。/ Footer refresh area: from the bottom of the
// 32-aligned page-turn area to the screen bottom, refreshed on its own when turning pages.
static EpdRect footer_rect(void) {
    const int top = reader_area().height;
    return (EpdRect){0, top, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - top};
}
static void draw_footer(uint8_t* fb, size_t page) {
    const int top = footer_top(), x0 = UI_MARGIN, x1 = UI_LOCK_WIDTH - UI_MARGIN, px = TR_S(22), h = TR_S(8);
    char pages[48], name[96];
    if (book_layout_complete())
        snprintf(pages, sizeof(pages), "%u/%u · %u%%", (unsigned)page + 1, (unsigned)book_layout_page_count(),
                 percent(page));
    else snprintf(pages, sizeof(pages), "%u/… · %u%%", (unsigned)page + 1, percent(page));
    copy_text(name, sizeof(name), s_save_failed ? "进度未保存，稍后重试" : s_title);
    capsule(fb, x0, top + TR_S(2), x1 - x0, h, percent(page));
    fit_text(name, px, x1 - x0 - ttf_text_width_px(px, pages) - TR_S(24));
    tr_text(fb, x0, top + TR_S(18), px, name, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, x1, top + TR_S(18), px, pages, EPD_DRAW_ALIGN_RIGHT, false);
}
static void draw_reader(uint8_t* fb, size_t page) {
    ui_clear_page(fb);
    book_layout_draw_page(fb, page, body_rect(), s_px);
    if (ttf_font_is_builtin()) {
        ui_text(fb, UI_MARGIN, TR_S(16), UI_PX_CAPTION, "内建字体缺字，请选择 TF 卡字体", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){UI_MARGIN, TR_S(52), ui_content_width(), TR_S(72)},
                       "打开字体页 / 无卡正文可能缺字", false);
    }
    if (!s_toolbar) { draw_footer(fb, page); return; }
    // 工具条开着时页脚更高，先把压在下面的正文擦掉。/ With the toolbar open the footer is taller; clear the body
    // text under it first.
    epd_fill_rect((EpdRect){0, UI_BAR_TOP, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - UI_BAR_TOP}, UI_GRAY_WHITE, fb);
    EpdRect track = track_rect();
    // 胶囊进度条，章节分界是条上的白色细缝；进度条短，缝太密（间距小于 12）就不画。
    // / A capsule track with chapter boundaries as thin white gaps; the track is short, so skip the gaps when they
    // would be closer than 12 px.
    const int cap_h = TR_S(10), cap_y = track.y + TR_S(17);
    capsule(fb, track.x, cap_y, track.width, cap_h, percent(page));
    size_t chapters = book_chapter_count();
    if (chapters <= 40 && book_total_bytes() && track.width >= (int)chapters * TR_S(12)) {
        for (size_t i = 1; i < chapters; ++i) {
            int x = track.x + (uint64_t)book_chapter_byte_offset(i) * track.width / book_total_bytes();
            epd_fill_rect((EpdRect){x, cap_y, 2, cap_h}, UI_GRAY_WHITE, fb);
        }
    }
    char name[96], suffix[64], line[176];
    if (book_layout_complete())
        snprintf(suffix, sizeof(suffix), " · 本章 %u/%u · 全书 %u%%", (unsigned)page + 1,
                 (unsigned)book_layout_page_count(), percent(page));
    else snprintf(suffix, sizeof(suffix), " · 本章 %u/… · 全书 %u%%", (unsigned)page + 1, percent(page));
    if (s_toolbar) {
        // 章名已在面板顶行，这里只留页码和百分比，居中在两个按钮之间，放不下就缩字号。
        // The chapter name is already on the panel's top line; keep only the page and percent, centered between the
        // buttons, shrinking the size if needed.
        snprintf(line, sizeof(line), "%s%s", s_save_failed ? "进度未保存" : "", s_save_failed ? suffix : suffix + 4);
        int px = UI_PX_CAPTION;
        while (px > TR_S(22) && ttf_text_width_px(px, line) > track.width) px -= 2;
        // 还放不下：去掉“本章”“全书”，只留数字；没保存时只说没保存。/ Still too wide: drop the words and keep the
        // numbers; when unsaved, say only that.
        if (ttf_text_width_px(px, line) > track.width) {
            if (s_save_failed) copy_text(line, sizeof(line), "进度未保存");
            else if (book_layout_complete())
                snprintf(line, sizeof(line), "%u/%u · %u%%", (unsigned)page + 1, (unsigned)book_layout_page_count(),
                         percent(page));
            else snprintf(line, sizeof(line), "%u/… · %u%%", (unsigned)page + 1, percent(page));
        }
        fit_text(line, px, track.width);
        ui_text(fb, track.x + track.width / 2, track.y + TR_S(36), px, line, EPD_DRAW_ALIGN_CENTER, false);
        for (int dir = -1; dir <= 1; dir += 2) {
            const EpdRect r = chapter_btn_rect(dir);
            const char* label = dir < 0 ? "‹ 上一章" : "下一章 ›";
            if (chapter_jump_ok(dir)) {
                tr_pill(fb, r, label, s_pressed_control == (dir < 0 ? 209 : 210), TR_S(26));
            } else {
                // 没有可跳的章：浅灰框、灰字。/ Nothing to jump to: a light frame and gray text.
                ui_draw_round_rect(fb, r, r.height / 2, TR_GRAY_LINE);
                tr_text_dim(fb, r.x + r.width / 2, r.y + (r.height - TR_S(26)) / 2 - 2, TR_S(26), label,
                            EPD_DRAW_ALIGN_CENTER);
            }
        }
    } else {
        copy_text(name, sizeof(name), s_save_failed ? "进度未保存，稍后重试" : s_title);
        fit_text(name, UI_PX_CAPTION, track.width - ttf_text_width_px(UI_PX_CAPTION, suffix));
        snprintf(line, sizeof(line), "%s%s", name, suffix);
        ui_text(fb, track.x, track.y + TR_S(36), UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    if (s_toolbar) {
        // 顶部：返回书库与书名。/ Top: back to the shelf and the title.
        epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, TR_S(110)}, UI_GRAY_WHITE, fb);
        tr_hline(fb, TR_S(110), 0, UI_LOCK_WIDTH, true);
        tr_text(fb, UI_MARGIN, TR_S(40), TR_S(32), "‹ 书库", EPD_DRAW_ALIGN_LEFT, false);
        char head[128];
        copy_text(head, sizeof(head), s_title);
        fit_text(head, TR_S(34), 340);
        tr_text(fb, UI_LOCK_WIDTH / 2 + 40, TR_S(38), TR_S(34), head, EPD_DRAW_ALIGN_CENTER, true);
        // 底部面板（Material 3 底部动作面板：圆角上沿和拖动条），压在页脚进度条上方。
        // Bottom panel (a Material 3 bottom sheet: rounded top edge and a drag handle) above the footer track.
        epd_fill_rect((EpdRect){0, TOOL_PANEL_Y, UI_LOCK_WIDTH, UI_BAR_TOP - TOOL_PANEL_Y}, UI_GRAY_WHITE, fb);
        ui_draw_round_rect(fb, (EpdRect){0, TOOL_PANEL_Y, UI_LOCK_WIDTH, TR_S(120)}, TR_S(32), TR_GRAY_LINE);
        ui_draw_round_rect(fb, (EpdRect){0, TOOL_PANEL_Y + 1, UI_LOCK_WIDTH, TR_S(120)}, TR_S(32), TR_GRAY_LINE);
        epd_fill_rect((EpdRect){2, TOOL_PANEL_Y + TR_S(40), UI_LOCK_WIDTH - 4, TR_S(82)}, UI_GRAY_WHITE, fb);
        ui_fill_round_rect(fb, (EpdRect){UI_LOCK_WIDTH / 2 - TR_S(36), TOOL_PANEL_Y + TR_S(8), TR_S(72), TR_S(8)},
                           TR_S(4), TR_GRAY_SELECTED);
        char chapter[128];
        if (book_chapter_title(s_chapter, chapter, sizeof(chapter)) != ESP_OK)
            snprintf(chapter, sizeof(chapter), "第 %u 节", (unsigned)s_chapter + 1);
        fit_text(chapter, TR_S(28), 420);
        tr_text_dim(fb, UI_MARGIN, TOOL_PANEL_Y + TR_S(24), TR_S(28), chapter, EPD_DRAW_ALIGN_LEFT);
        char size_hint[24];
        snprintf(size_hint, sizeof(size_hint), "字号 %d", s_px);
        tr_text_dim(fb, UI_LOCK_WIDTH - UI_MARGIN, TOOL_PANEL_Y + TR_S(24), TR_S(28), size_hint, EPD_DRAW_ALIGN_RIGHT);
        static const int tools[4] = {0, 3, 4, 5};
        static const tr_icon_t icons[4] = {TR_ICON_TOC, TR_ICON_MENU, TR_ICON_REFRESH, TR_ICON_BOOKS};
        static const char* const labels[4] = {"目录", "菜单", "排版", "书库"};
        for (int k = 0; k < 4; ++k) {
            EpdRect r = tool_rect(tools[k]);
            // 灰底方块缩进 10 像素，按下的描边落在外面的白边上，DU 刷不坏灰底。
            // The gray tile is inset 10 px so the pressed outline lands on the white margin and DU never touches the gray.
            ui_fill_round_rect(fb, ui_inset_rect(r, TR_S(10)), TR_S(24), TR_GRAY_SURFACE);
            if (s_pressed_control == 200 + tools[k]) ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
            // 排版用“Aa”当图标。/ Layout uses "Aa" as its icon.
            if (tools[k] == 4) tr_text_on(fb, r.x + r.width / 2, r.y + TR_S(16), TR_S(36), "Aa", EPD_DRAW_ALIGN_CENTER,
                                          true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
            else tr_icon_on(fb, icons[k], r.x + r.width / 2, r.y + TR_S(36), UI_GRAY_BLACK, TR_GRAY_SURFACE);
            tr_text_on(fb, r.x + r.width / 2, r.y + TR_S(70), TR_S(28), labels[k], EPD_DRAW_ALIGN_CENTER, false, UI_GRAY_BLACK,
                       TR_GRAY_SURFACE);
        }
        tr_pill(fb, tool_rect(1), "A−", s_pressed_control == 201, TR_S(34));
        tr_pill(fb, tool_rect(2), "A+", s_pressed_control == 202, TR_S(34));
        snprintf(size_hint, sizeof(size_hint), "%d", s_px);
        tr_text(fb, UI_LOCK_WIDTH / 2, tool_rect(1).y + TR_S(20), TR_S(40), size_hint, EPD_DRAW_ALIGN_CENTER, true);

        auto_turn_draw_row(fb, AUTO_ROW_Y, s_auto_level);
        if (s_typeset) draw_typeset(fb);
    }
}
static void prep_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lock_draw();
        // 主循环可能正在换字体（换卡后重载 TF 卡字体）：拿字体锁，画字期间不让它卸载。
        // The main loop may be switching fonts (reloading the TF-card font after a swap): hold the font lock so it
        // cannot unload mid-draw.
        ttf_font_lock();
        if (s_prep_page >= 0 && s_text && s_next_fb) {
            draw_reader(s_next_fb, (size_t)s_prep_page);
            s_next_page = s_prep_page;
        }
        ttf_font_unlock();
        unlock_draw();
        xSemaphoreGive(s_prep_done);
    }
}
static void ensure_prep(void) {
    if (!s_draw_lock) s_draw_lock = xSemaphoreCreateMutex();
    if (!s_prep_done) s_prep_done = xSemaphoreCreateBinary();
    if (!s_next_fb) s_next_fb = heap_caps_aligned_alloc(16, fb_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prep_task && s_draw_lock && s_prep_done && s_next_fb) {
        if (xTaskCreatePinnedToCore(prep_task, "book_prep", 12 * 1024, NULL, 3, &s_prep_task, 1) != pdPASS)
            s_prep_task = NULL;
    }
}
static bool kick_prep(void) {
    if (!s_prep_task || s_view != READING || s_image_open || s_toolbar || s_clear_confirm ||
        s_page + 1 >= book_layout_page_count() || s_next_page == (int)s_page + 1) return false;
    s_prep_page = (int)s_page + 1;
    xSemaphoreTake(s_prep_done, 0);
    xTaskNotifyGive(s_prep_task);
    return true;
}
static void draw_image(uint8_t* fb) {
    ui_clear_page(fb);
    bool title_image = s_image_block < s_block_count && s_blocks[s_image_block].image_title;
    ui_draw_header(fb, s_image_error ? "图片未加载" : title_image ? "标题图预览" : "图片预览", "点击图片或返回按钮回到正文");
    int top = UI_CONTENT_TOP;
    if (s_image_origin[0]) {
        char line[sizeof(s_image_origin)]; copy_text(line, sizeof(line), s_image_origin);
        fit_text(line, UI_PX_CAPTION, ui_content_width());
        ui_text(fb, UI_MARGIN, top, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        top += UI_PX_CAPTION + UI_GAP;
    }
    EpdRect area = {UI_MARGIN, top, ui_content_width(), UI_CONTENT_BOTTOM - top};
    if (s_image_pixels && s_image_width && s_image_height) {
        int width = s_image_width, height = s_image_height;
        if (width > area.width) { height = height * area.width / width; width = area.width; }
        if (height > area.height) { width = width * area.height / height; height = area.height; }
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        int left = area.x + (area.width - width) / 2, y0 = area.y + (area.height - height) / 2;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            size_t src = (size_t)((int64_t)y * s_image_height / height) * s_image_width +
                         (size_t)((int64_t)x * s_image_width / width);
            epd_draw_pixel(left + x, y0 + y, s_image_pixels[src], fb);
        }
    } else ui_text(fb, area.x, area.y + area.height / 2, UI_PX_CAPTION,
                   s_image_error ? s_image_error : "图片不可用", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, ui_bar_rect(0, 1), "返回正文", false);
}
/* ---- Treader 书架 / Treader shelf ---- */
static void shelf_display_title(const shelf_entry_t* e, char* out, size_t cap) {
    if (e->title[0]) { copy_text(out, cap, e->title); return; }
    copy_text(out, cap, e->name);
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}
// 状态行右边的番茄操作：设置了番茄服务器、书库里有番茄书（或本页的任务在跑）才有。/ The Tomato action on the right of
// the status line: only with a Tomato server set and Tomato books in the library (or this page's job running).
// 筛选条给更新胶囊留位置：管理时胶囊不画，但前三个胶囊不挪。/ The chip row keeps room for the update pill: manage mode
// does not draw the pill, yet the first three chips stay put.
static bool tomato_pill_room(void) { return !s_query[0] && s_tom_cfg && (tomato_any() || tom_running()); }
static bool tomato_hint_shown(void) { return s_view == SHELF && tomato_pill_room(); }
// 在跑：进度，点一下变“再点取消”；有待下载的：下载；否则检查。/ Running: progress, turning into "再点取消" after one
// tap; updates pending: download them; otherwise check.
static void tomato_hint_label(char* out, size_t cap) {
    if (tom_running()) {
        tomato_status_t st;
        tomato_sync_get_status(&st);
        const char* what = s_tom_op == TOM_UPDATE ? "更新中" : "检查中";
        if (s_tom_cancelling) copy_text(out, cap, "正在取消…");
        else if (s_tom_confirm_cancel) copy_text(out, cap, "再点取消");
        else if (st.total > 0) snprintf(out, cap, "%s %d/%d", what, st.current, st.total);
        else snprintf(out, cap, "%s…", what);
    } else if (tomato_pending() > 0) {
        snprintf(out, cap, "下载 %d 本更新", tomato_pending());
    } else {
        copy_text(out, cap, "检查更新");
    }
}
// 番茄任务进度：前面几本算做完，当前这本按已收字节占预计大小算（不知道大小就不算），最多到 99%。
// Tomato job progress: earlier books count as done, the current one by bytes over its expected size (nothing when
// unknown), capped at 99%.
static int tomato_busy_pct(const tomato_status_t* st) {
    if (st->total <= 0 || st->current <= 0) return 0;
    int part = 0;
    if (st->phase == TOMATO_DOWNLOADING && st->expect)
        part = st->bytes >= st->expect ? 99 : (int)((uint64_t)st->bytes * 100 / st->expect);
    const int pct = ((st->current - 1) * 100 + part) / st->total;
    return pct > 99 ? 99 : pct;
}
// 番茄任务在跑时状态行左边写正在做哪一步，下载时带书名和 MB。/ While a Tomato job runs the left of the status line
// says which step is under way, with the title and MB while downloading.
static void tomato_busy_line(const tomato_status_t* st, char* out, size_t cap) {
    if (st->phase == TOMATO_CONNECTING) copy_text(out, cap, "正在连接 Wi-Fi…");
    else if (st->phase == TOMATO_QUEUED) copy_text(out, cap, "服务器正在生成 TXT…");
    else if (st->phase == TOMATO_DOWNLOADING && st->expect)
        snprintf(out, cap, "%s · %.1f / %.1f MB", st->message, st->bytes / 1048576.0, st->expect / 1048576.0);
    else if (st->phase == TOMATO_DOWNLOADING) snprintf(out, cap, "%s · %.1f MB", st->message, st->bytes / 1048576.0);
    else copy_text(out, cap, "正在检查更新…");
}
// 网格的一格：封面（新章节小胶囊压在右上角），下面书名，再下面进度条加百分比或“未读 / 读完”；管理时选中的描粗框。
// / One grid cell: the cover (a new-chapter badge on its top right), the title below, then a progress bar and percent
// or 未读 / 读完; in manage mode a selected book gets a thick outline.
static void draw_grid_cell(uint8_t* fb, const shelf_entry_t* e, EpdRect r, int row) {
    const grid_t g = grid_layout();
    if (s_pressed_control == row || (s_view == BULK && e->selected))
        ui_draw_selected_round_rect(fb, ui_inset_rect(r, -TR_S(6)), UI_BTN_RADIUS);
    char title[128], mark[32] = "", meta[16];
    shelf_display_title(e, title, sizeof(title));
    const bool done = e->has_progress && e->pct >= 100;
    const char* ext = strrchr(e->name, '.');
    const EpdRect cover = {r.x + (r.width - g.cover_w) / 2, r.y, g.cover_w, g.cover_h};
    tr_book_cover(fb, cover, e->path, title, e->tomato ? e->author : "",
                  e->tomato ? "番茄" : ext && !strcasecmp(ext, ".epub") ? "EPUB" : "TXT", done);
    if (s_view == BULK) copy_text(mark, sizeof(mark), e->selected ? "已选" : "");
    else if (e->new_chapters) snprintf(mark, sizeof(mark), "+%u", e->new_chapters);
    else if (e->fresh_chapters) snprintf(mark, sizeof(mark), "+%u", e->fresh_chapters);
    if (e->removed) copy_text(mark, sizeof(mark), "已删");
    if (mark[0])
        tr_badge(fb, cover.x + cover.width - TR_S(6), cover.y + TR_S(6), mark,
                 s_view == BULK || e->removed || e->new_chapters > 0, TR_S(22));
    const int ty = r.y + g.cover_h + TR_S(8);
    tr_fit(title, TR_S(24), r.width);
    tr_text(fb, r.x, ty, TR_S(24), title, EPD_DRAW_ALIGN_LEFT, true);
    if (e->has_progress && !done) {
        snprintf(meta, sizeof(meta), "%u%%", e->pct);
        const int pw = ttf_text_width_px(TR_S(22), meta) + TR_S(10);
        tr_progress(fb, r.x, ty + TR_S(46), r.width - pw, e->pct);
        tr_text_dim(fb, r.x + r.width, ty + TR_S(34), TR_S(22), meta, EPD_DRAW_ALIGN_RIGHT);
    } else {
        tr_text_dim(fb, r.x, ty + TR_S(34), TR_S(22), done ? "读完" : "未读", EPD_DRAW_ALIGN_LEFT);
    }
}
static void draw_shelf(app_ctx_t* ctx, uint8_t* fb) {
    ui_clear_page(fb);
    tr_status_bar(fb);
    tr_title(fb, s_view == BULK ? "管理书库" : "书库");
    EpdRect si = search_icon_rect(), ii = import_icon_rect(), vi = view_icon_rect();
    tr_icon(fb, app_settings_shelf_grid() ? TR_ICON_TOC : TR_ICON_MENU, vi.x + vi.width / 2, vi.y + vi.height / 2,
            false);
    tr_icon(fb, TR_ICON_SEARCH, si.x + si.width / 2, si.y + si.height / 2, false);
    tr_icon(fb, TR_ICON_IMPORT, ii.x + ii.width / 2, ii.y + ii.height / 2, false);

    char line[192];
    const char* note = s_view == BULK && s_batch_message[0] ? s_batch_message : s_message[0] ? s_message : s_shelf_warning;
    if (s_save_failed && s_view == SHELF) copy_text(line, sizeof(line), "进度未保存：点此重试");
    else if (note[0]) copy_text(line, sizeof(line), note);
    else if (s_view == BULK) snprintf(line, sizeof(line), "已选 %u 本 · 筛选或重扫会清除勾选", (unsigned)selected_count());
    else if (s_tom_note[0]) snprintf(line, sizeof(line), "%d 本 · %s", s_visible_count, s_tom_note);
    else snprintf(line, sizeof(line), "%d 本 · %s", s_visible_count, s_recent_sort ? "按最近阅读" : "按书名");
    if (s_tom_cfg && s_view == SHELF) {
        const EpdRect st = store_rect();
        tr_text(fb, st.x + st.width - TR_S(12), st.y + TR_S(18), TR_S(30), "书城", EPD_DRAW_ALIGN_RIGHT, true);
    }
    int line_w = ui_content_width();
    if (tomato_hint_shown()) {
        char hint[48];
        tomato_hint_label(hint, sizeof(hint));
        const EpdRect pill = tomato_hint_rect();
        tr_pill(fb, pill, hint, !tom_running() || s_tom_confirm_cancel, chip_px(pill, hint, TR_S(28)));
        // 在跑且状态行没有别的提示：左边换成进行到哪一步，状态行下面加一条进度条。/ Running with nothing else on the
        // status line: the left says where it is and a progress bar goes under the status line.
        if (tom_running() && !s_save_failed && !note[0]) {
            tomato_status_t st;
            tomato_sync_get_status(&st);
            if (!s_tom_note[0]) tomato_busy_line(&st, line, sizeof(line));
            tr_progress(fb, UI_MARGIN, TR_S(196), ui_content_width(), tomato_busy_pct(&st));
        }
    }
    tr_fit(line, TR_S(28), line_w);
    // 出错和待重试的提示用黑字，平常的数量行用深灰。/ Errors and retries stay black; the plain count line is dark gray.
    if (s_save_failed || note[0]) tr_text(fb, UI_MARGIN, TR_S(156), TR_S(28), line, EPD_DRAW_ALIGN_LEFT, false);
    else tr_text_dim(fb, UI_MARGIN, TR_S(156), TR_S(28), line, EPD_DRAW_ALIGN_LEFT);

    // 书都在 TF 卡上，筛选改成按来源。/ All books are on the card, so filter by source instead.
    const char* const chips[3] = {s_filter == 0 ? "全部" : s_filter == 1 ? "本地" : "番茄",
                                  s_recent_sort ? "按最近" : "按书名", s_view == BULK ? "完成" : "管理"};
    tr_pill(fb, chip_rect(0), chips[0], s_filter != 0, chip_px(chip_rect(0), chips[0], TR_S(28)));
    tr_pill(fb, chip_rect(1), chips[1], false, chip_px(chip_rect(1), chips[1], TR_S(28)));
    tr_pill(fb, chip_rect(2), chips[2], s_view == BULK, chip_px(chip_rect(2), chips[2], TR_S(28)));
    if (s_query[0]) {
        char chip[96];
        snprintf(chip, sizeof(chip), "%s ×", s_query);
        tr_fit(chip, TR_S(26), chip_rect(3).width - TR_S(24));
        tr_pill(fb, chip_rect(3), chip, true, TR_S(26));
    }

    if (card_shown()) {
        // 继续阅读卡片：小封面、书名、进度。/ Continue card: small cover, title, progress.
        // Material 3 填充卡片：浅灰底；按下的描边画在卡片外圈的白边上（DU 不碰灰底）。
        // Material 3 filled card on light gray; the pressed outline goes on the white margin around it (DU leaves
        // the gray alone).
        const shelf_entry_t* e = &s_shelf[continue_index()];
        const EpdRect c = card_rect();
        ui_fill_round_rect(fb, c, TR_S(24), TR_GRAY_SURFACE);
        if (s_pressed_control == CONTROL_CARD) ui_draw_selected_round_rect(fb, ui_inset_rect(c, -TR_S(8)), TR_S(30));
        char title[128], pct[16];
        shelf_display_title(e, title, sizeof(title));
        const bool done = e->has_progress && e->pct >= 100;
        tr_book_mini_cover(fb, (EpdRect){c.x + TR_S(16), c.y + TR_S(20), TR_S(70), TR_S(96)}, e->path, title, done);
        const int tx = c.x + TR_S(108), right = c.x + c.width - TR_S(20), tw = right - tx;
        tr_text_on(fb, tx, c.y + TR_S(14), TR_S(24), "继续阅读", EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY,
                   TR_GRAY_SURFACE);
        tr_text_on(fb, right, c.y + TR_S(14), TR_S(24), "接着读 →", EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK,
                   TR_GRAY_SURFACE);
        tr_fit(title, TR_S(32), tw);
        tr_text_on(fb, tx, c.y + TR_S(46), TR_S(32), title, EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
        snprintf(pct, sizeof(pct), "%u%%", e->has_progress ? e->pct : 0);
        tr_progress(fb, tx, c.y + TR_S(108), tw - TR_S(80), e->has_progress ? e->pct : 0);
        tr_text_on(fb, right, c.y + TR_S(94), TR_S(26), pct, EPD_DRAW_ALIGN_RIGHT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    }
    if (s_count && !s_visible_count)
        tr_text_dim(fb, UI_MARGIN, row_rect(0, false).y + TR_S(20), TR_S(30), "当前筛选没有图书", EPD_DRAW_ALIGN_LEFT);
    for (int row = 0; row < BOOK_ROWS; ++row) {
        int i = ctx->leaf * BOOK_ROWS + row;
        if (i >= s_visible_count) break;
        const shelf_entry_t* e = &s_shelf[i];
        EpdRect r = row_rect(row, false);
        if (shelf_grid()) { draw_grid_cell(fb, e, r, row); continue; }
        if (s_pressed_control == row) ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
        if (s_view == BULK && e->selected) ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
        char title[128], mark[32] = "", meta[160];
        shelf_display_title(e, title, sizeof(title));
        bool done = e->has_progress && e->pct >= 100;
        tr_book_mini_cover(fb, (EpdRect){r.x + (s_view == BULK ? TR_S(8) : 0), r.y + TR_S(8), TR_S(70), TR_S(96)},
                           e->path, title, done);
        if (s_view == BULK) copy_text(mark, sizeof(mark), e->selected ? "已选" : "未选");
        if (e->removed) copy_text(mark, sizeof(mark), "已删·待清理");
        else if (s_view != BULK && e->new_chapters) snprintf(mark, sizeof(mark), "+%u 章 待下载", e->new_chapters);
        else if (s_view != BULK && e->fresh_chapters) snprintf(mark, sizeof(mark), "+%u 章", e->fresh_chapters);
        // 新章节是小胶囊：待下载黑底，已下载没看灰底；其他标记是文字。胶囊离右边 10 像素，避开按下的描边。
        // New chapters are badges: black when pending, gray when downloaded but unopened; other marks are text. Badges
        // sit 10 px from the right so the pressed outline does not cross them.
        const bool badge = s_view != BULK && !e->removed && (e->new_chapters || e->fresh_chapters);
        int mark_w = !mark[0] ? 0
                     : badge  ? ttf_text_width_px(TR_S(24), mark) + TR_S(24) + TR_S(26)
                              : ttf_text_width_px(TR_S(28), mark) + TR_S(16);
        const int tx = r.x + TR_S(96), tw = r.x + r.width - tx;
        tr_fit(title, TR_S(34), tw - mark_w);
        tr_text(fb, tx, r.y + TR_S(4), TR_S(34), title, EPD_DRAW_ALIGN_LEFT, true);
        if (badge) tr_badge(fb, r.x + r.width - TR_S(10), r.y + TR_S(10), mark, e->new_chapters > 0, TR_S(24));
        else if (mark[0]) tr_text_dim(fb, r.x + r.width, r.y + TR_S(8), TR_S(28), mark, EPD_DRAW_ALIGN_RIGHT);
        const char* ext = strrchr(e->name, '.');
        if (e->tomato) snprintf(meta, sizeof(meta), "番茄%s%s", e->author[0] ? " · " : "", e->author);
        else snprintf(meta, sizeof(meta), "%s%s", ext && !strcasecmp(ext, ".epub") ? "EPUB" : "TXT", e->is_flash ? " · 内置" : "");
        tr_fit(meta, TR_S(26), tw);
        tr_text_dim(fb, tx, r.y + TR_S(52), TR_S(26), meta, EPD_DRAW_ALIGN_LEFT);
        if (e->has_progress && !done) {
            tr_progress(fb, tx, r.y + TR_S(98), tw - TR_S(90), e->pct);
            snprintf(meta, sizeof(meta), "%u%%", e->pct);
            tr_text_dim(fb, r.x + r.width, r.y + TR_S(86), TR_S(26), meta, EPD_DRAW_ALIGN_RIGHT);
        } else {
            tr_text_dim(fb, tx, r.y + TR_S(88), TR_S(26), done ? "读完" : "未读", EPD_DRAW_ALIGN_LEFT);
        }
        tr_hline(fb, r.y + r.height + TR_S(4), UI_MARGIN, ui_content_width(), true);
    }
    tr_pager(fb, SHELF_PAGER_Y, ctx->leaf, leaves());
    if (s_view == BULK) {
        const char* labels[] = {"本页全选", "清除勾选", "重新扫描", "删除所选", "清进度"};
        for (int i = 0; i < 5; ++i) draw_control(fb, batch_rect(i), labels[i], 610 + i);
        draw_batch_confirmation(fb);
    } else {
        tr_tab_bar(fb, TR_TAB_LIBRARY);
    }
}
static void render(app_ctx_t* ctx, uint8_t* fb) {
    lock_draw();
    if (s_image_open) { draw_image(fb); unlock_draw(); return; }
    if (s_view == SEARCH) { draw_search(fb); unlock_draw(); return; }
    if (s_view == READING && s_text) {
        draw_reader(fb, s_page);
    } else if (s_view == TOC) {
        ui_clear_page(fb);
        tr_status_bar(fb);
        tr_title(fb, "目录");
        if (toc_jump_shown()) {
            const EpdRect jump = toc_jump_rect();
            if (s_pressed_control == CONTROL_TOC_JUMP) ui_draw_selected_round_rect(fb, jump, UI_BTN_RADIUS);
            tr_text(fb, jump.x + jump.width - TR_S(12), jump.y + TR_S(18), TR_S(30), toc_jump_label(ctx->leaf),
                    EPD_DRAW_ALIGN_RIGHT, true);
        }
        char head[128], page_no[32];
        snprintf(page_no, sizeof(page_no), " · %d / %d", ctx->leaf + 1, leaves());
        copy_text(head, sizeof(head), s_title);
        fit_text(head, TR_S(28), ui_content_width() - ttf_text_width_px(TR_S(28), page_no));
        strncat(head, page_no, sizeof(head) - strlen(head) - 1);
        tr_text_dim(fb, UI_MARGIN, TR_S(156), TR_S(28), head, EPD_DRAW_ALIGN_LEFT);
        if (s_message[0]) tr_text(fb, UI_MARGIN, TR_S(220), TR_S(28), s_message, EPD_DRAW_ALIGN_LEFT, false);
        else {
            for (int row = 0; row < BOOK_TOC_ROWS; ++row) {
                int i = ctx->leaf * BOOK_TOC_ROWS + row;
                if (i >= (int)book_chapter_count()) break;
                EpdRect r = row_rect(row, true);
                char name[128];
                if (book_chapter_title(i, name, sizeof(name)) != ESP_OK) snprintf(name, sizeof(name), "第 %d 节", i + 1);
                bool current = i == (int)s_chapter;
                fit_text(name, TR_S(32), r.width - (current ? TR_S(110) : TR_S(16)));
                if (s_pressed_control == row) ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
                tr_text(fb, r.x, r.y + TR_S(22), TR_S(32), name, EPD_DRAW_ALIGN_LEFT, current);
                if (current) tr_badge(fb, r.x + r.width - TR_S(10), r.y + TR_S(22), "在读", true, TR_S(24));
                tr_hline(fb, r.y + r.height, UI_MARGIN, ui_content_width(), true);
            }
        }
        if (ctx->leaf > 0)
            tr_text(fb, nav_rect(0).x, TOC_NAV_Y + TR_S(22), TR_S(30), "‹ 上一页", EPD_DRAW_ALIGN_LEFT, false);
        tr_pill(fb, nav_rect(1), "返回阅读", true, TR_S(30));
        if (ctx->leaf + 1 < leaves())
            tr_text(fb, nav_rect(2).x + nav_rect(2).width, TOC_NAV_Y + TR_S(22), TR_S(30), "下一页 ›", EPD_DRAW_ALIGN_RIGHT,
                    false);
    } else {
        draw_shelf(ctx, fb);
        if (s_view == MANAGE) draw_manage(fb);
        if (s_resume_path[0]) {
            EpdRect panel = {UI_MARGIN - 12, TR_S(380), ui_content_width() + 24, TR_S(350)};
            draw_dialog(fb, panel);
            tr_text(fb, UI_MARGIN, TR_S(410), TR_S(38), "继续上次阅读？", EPD_DRAW_ALIGN_LEFT, true);
            char name[256];
            copy_text(name, sizeof(name), s_resume_name);
            fit_text(name, UI_PX_CAPTION, ui_content_width());
            ui_text(fb, UI_MARGIN, TR_S(478), UI_PX_CAPTION, name, EPD_DRAW_ALIGN_LEFT, false);
            tr_text_dim(fb, UI_MARGIN, TR_S(530), UI_PX_CAPTION, "确认后才加载图书", EPD_DRAW_ALIGN_LEFT);
            tr_pill(fb, ui_row_rect(0, 2, DIALOG_BTN_Y, UI_BTN_H), "留在书库", false, TR_S(32));
            tr_pill(fb, ui_row_rect(1, 2, DIALOG_BTN_Y, UI_BTN_H), "继续阅读", true, TR_S(32));
        }
    }
    unlock_draw();
}
static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE || redraw == APP_REDRAW_DONE) return true;
    int64_t start = esp_timer_get_time();
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) render(ctx, ctx->fb);
    bool prep = kick_prep();
    int64_t drawn = esp_timer_get_time();
    enum EpdDrawError err;
    if (redraw == APP_REDRAW_FULL || s_full) err = update_display_full(ctx->hl);
    else if (redraw == APP_REDRAW_AREA) {
        err = update_display_area_with(ctx->hl, s_wave, s_mode, s_area);
        if (s_reader_split) {
            // 页码即时更新；百分比不变时轨道像素不变，DU不会擦掉整条轨道。
            // Page numbers stay current; unchanged percentages leave the track unchanged, so DU never wipes the whole track.
            err = (enum EpdDrawError)(err | update_display_area_with(ctx->hl, &E0470_FOLLOW_WAVEFORM,
                                                                     MODE_DU, footer_rect()));
            ESP_LOGI(TAG, "reader regions body_h=%d footer=diff pct=%u", s_area.height, percent(s_page));
        }
    }
    else err = update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE);
    int64_t displayed = esp_timer_get_time();
    if (prep) xSemaphoreTake(s_prep_done, portMAX_DELAY);
    if (redraw == APP_REDRAW_AREA && s_mode == MODE_DU && !s_full) {
        s_du_area = s_du_count ? ui_rect_union(s_du_area, s_area) : s_area;
        ++s_du_count;
        s_du_ms = esp_timer_get_time() / 1000;
    } else s_du_count = 0;
    ESP_LOGI(TAG, "present draw=%lld display=%lld join=%lld ms", (drawn - start) / 1000,
             (displayed - drawn) / 1000, (esp_timer_get_time() - displayed) / 1000);
    guard_draw_result(ctx->hl, err);
    s_full = false;
    s_reader_split = false;
    s_mode = MODE_GL16;
    s_wave = &E0470_WAVEFORM;
    return true;
}
static app_redraw_t paint_reading(app_ctx_t* ctx, enum EpdDrawMode mode) {
    int64_t started = esp_timer_get_time();
    lock_draw();
    bool cached = !s_toolbar && !s_clear_confirm && s_next_fb && s_next_page == (int)s_page;
    if (cached)
        memcpy(ctx->fb, s_next_fb, fb_bytes());
    else draw_reader(ctx->fb, s_page);
    unlock_draw();
    ESP_LOGI(TAG, "paint cached=%d ms=%lld", cached, (esp_timer_get_time() - started) / 1000);
    s_mode = mode;
    if (mode == MODE_GL16 && app_settings_fast_turn() && !s_toolbar && !s_clear_confirm) {
        // 快速档：正文和页脚一次刷完，用 8 灰阶 GL16。/ Fast: body and footer in one pass with the 8-gray GL16.
        s_area = (EpdRect){0, 0, UI_LOCK_WIDTH, UI_LOCK_HEIGHT};
        s_reader_split = false;
        s_wave = &E0470_GRAY8_WAVEFORM;
    } else {
        s_area = reader_area();
        s_reader_split = true;
    }
    return APP_REDRAW_AREA;
}
static void loading_detail(app_ctx_t* ctx, const char* text, const char* detail) {
    lock_draw();
    EpdRect r = {UI_MARGIN, UI_BAR_TOP + TR_S(8), ui_bar_rect(0, 1).width, TR_S(80)};
    ui_clear_rect_fast(ctx->fb, r);
    ui_text(ctx->fb, r.x, r.y, UI_PX_CAPTION, text, EPD_DRAW_ALIGN_LEFT, false);
    if (detail) ui_text(ctx->fb, r.x, r.y + TR_S(36), UI_PX_CAPTION, detail, EPD_DRAW_ALIGN_LEFT, false);
    unlock_draw();
    guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_DU, r));
}
static app_redraw_t open_image(app_ctx_t* ctx, size_t block_index) {
    if (block_index >= s_block_count || !s_blocks[block_index].image_src) return APP_REDRAW_NONE;
    blk_t* block = &s_blocks[block_index];
    if (s_image_block != block_index || !s_image_pixels) {
        free_image();
        invalidate_prep();
        loading_detail(ctx, "正在加载图片…", "加载后可返回原阅读位置");
        lock_draw();
        free(s_next_fb); s_next_fb = NULL;
        esp_err_t err = book_chapter_load_image(s_chapter, block->image_src, &s_image_pixels, &s_image_width, &s_image_height);
        unlock_draw();
        ensure_prep();
        s_image_block = block_index;
        if (err != ESP_OK) {
            s_image_error = err == ESP_ERR_NO_MEM ? "内存不足，请返回后重试" :
                            err == ESP_ERR_NOT_FOUND ? "图片文件缺失" :
                            err == ESP_ERR_INVALID_SIZE ? "图片超出大小限制或文件损坏" : "图片损坏或格式暂不支持";
        }
        if (block->image_repeated && !block->image_title) {
            char title[160] = {0};
            (void)book_chapter_title(block->image_first_chapter, title, sizeof(title));
            snprintf(s_image_origin, sizeof(s_image_origin), "已读最早：第 %u 节 · %s", (unsigned)block->image_first_chapter + 1, title);
        }
    }
    s_image_open = true;
    s_size_settle_ms = 0;
    invalidate_prep();
    return APP_REDRAW_PAGE;
}

static const char* book_error_message(esp_err_t err) {
    if (err == ESP_ERR_NO_MEM) return "解析失败：内存不足，请稍后重试";
    if (err == ESP_ERR_INVALID_SIZE) return "超出上限：32768 项或资源过大";
    if (err == ESP_ERR_NOT_SUPPORTED) return "文件格式、压缩或加密暂不支持";
    if (err == ESP_ERR_NOT_FOUND) return "书籍或章节缺失，请检查文件和卡";
    return "解析失败：文件异常，请重新传入";
}

/* ---- 文件与进度 / Files and progress ---- */
static void free_book(void) {
    // 退出时再记一次“已看”：正在看的时候后台可能刚下完新章节。/ Mark seen again on close: new chapters may have landed while reading.
    if (s_text && tomato_file_of(s_path)) tomato_shelf_mark_seen(tomato_file_of(s_path));
    pending_progress_t* pending = pending_find(s_path);
    if (pending && !pending->dirty) pending_discard(s_path);
    invalidate_prep();
    free_image();
    book_layout_free();
    free(s_text);
    html_blocks_free(s_blocks, s_block_count);
    s_blocks = NULL;
    s_block_count = 0;
    s_text = NULL;
    s_text_len = 0;
    book_close();
    s_path[0] = 0;
    publish_open_path("");
}
static bool load_chapter(app_ctx_t* ctx, size_t chapter, size_t offset, bool last_page) {
    loading_detail(ctx, "正在加载和排版…", "长章节需要更多时间，请稍候");
    html_text_t loaded = {0};
    esp_err_t err = book_chapter_load_blocks(chapter, &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chapter load failed: %s", esp_err_to_name(err));
        copy_text(s_message, sizeof(s_message), book_error_message(err));
        return false;
    }
    lock_draw();
    invalidate_prep();
    bool ok = book_layout_begin_blocks(loaded.utf8, loaded.len, loaded.blocks, loaded.count, body_rect(), s_px);
    // 先完成续读页；其余页由 tick 分批排版，上一章末页仍须定位到末尾。
    // Finish the resume page first; ticks paginate the rest, while previous-chapter navigation needs its end.
    while (ok && !book_layout_complete() &&
           (last_page || book_layout_page_start_offset(book_layout_page_count()) <= offset))
        ok = book_layout_extend(2);
    if (!ok) {
        html_text_free(&loaded);
        bool restored = s_text && book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            ctx->leaf = 0;
        }
        copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
        return false;
    }
    free(s_text);
    html_blocks_free(s_blocks, s_block_count);
    free_image();
    s_text = loaded.utf8;
    s_text_len = loaded.len;
    s_blocks = loaded.blocks;
    s_block_count = loaded.count;
    s_chapter = chapter;
    s_page = last_page ? book_layout_page_count() - 1 : book_layout_page_for_offset(offset);
    if (book_chapter_title(chapter, s_title, sizeof(s_title)) != ESP_OK)
        snprintf(s_title, sizeof(s_title), "第 %u 节", (unsigned)chapter + 1);
    copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
    s_message[0] = 0;
    unlock_draw();
    return true;
}
static bool open_book(app_ctx_t* ctx, const char* path) {
    // 封面缩略图约 1 MB PSRAM，读书时让给排版。/ Thumbnails hold ~1 MB of PSRAM; give it to layout while reading.
    book_cover_release();
    if (!strncmp(path, "/sdcard/", 8)) {
        read_pico_sd_info_t sd = {0};
        read_pico_sd_get_info(&sd);
        if (!sd.present || !sd.mounted) {
            copy_text(s_message, sizeof(s_message), "TF 卡不可用，请重新挂载后打开");
            return false;
        }
    }
    // 自动续读也不能绕过同路径旧文件的清理重试。
    // Automatic resume must also finish cleanup of the old file at this path.
    if (delete_retry_find(path)) {
        copy_text(s_message, sizeof(s_message), "文件已删除，进度清理失败，请重试");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
        copy_text(s_message, sizeof(s_message), "无法读取文件，请检查存储卡");
        return false;
    }
    if (strncmp(path, "/flash/", 7) == 0 && st.st_size > BOOK_STORE_FLASH_FILE_MAX) {
        copy_text(s_message, sizeof(s_message), "内置存储单本限 1 MB");
        return false;
    }
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法保留待保存进度");
        return false;
    }
    save_progress();
    free_book();
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法打开图书");
        return false;
    }
    loading_detail(ctx, "正在解析图书…", "大书需要更多时间，请稍候");
    // 持绘制锁释放下一页缓存，降低解析峰值。/ Release the next-page buffer under the drawing lock to reduce parsing peaks.
    lock_draw(); free(s_next_fb); s_next_fb = NULL; unlock_draw();
    int64_t open_started = esp_timer_get_time();
    esp_err_t err = book_open(path);
    int64_t source_ms = (esp_timer_get_time() - open_started) / 1000;
    ensure_prep();
    if (err != ESP_OK) {
        pending_progress_t* pending = pending_find(path);
        if (pending && !pending->dirty) pending_discard(path);
        copy_text(s_message, sizeof(s_message), book_error_message(err));
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        return false;
    }
    copy_text(s_path, sizeof(s_path), path);
    publish_open_path(path);
    s_file_size = (uint32_t)st.st_size;
    book_progress_t p = {0};
    bool resume = pending_restore(path, s_file_size, &p) || book_progress_load(path, s_file_size, &p);
    s_px = resume ? p.px : app_settings_book_px();
    if (s_px < BOOK_PX_MIN || s_px > BOOK_PX_MAX) s_px = 48;
    size_t chapter = resume && p.chapter < book_chapter_count() ? p.chapter : 0;
    if (!load_chapter(ctx, chapter, resume && p.chapter == chapter ? p.byte_off : 0, false)) {
        free_book();
        return false;
    }
    s_view = READING;
    s_toolbar = s_clear_confirm = s_batch_confirm = false;
    s_turns = s_unsaved = 0;
    s_size_settle_ms = 0;
    pending_mark_latest(s_path);
    save_progress();
    // 打开成功才算看过：书库和番茄书架上的“+N 章”消失。/ Only a successful open counts as seen and clears the badge.
    if (tomato_file_of(s_path)) tomato_shelf_mark_seen(tomato_file_of(s_path));
    ESP_LOGI(TAG, "opened kind=%d chapters=%u pages=%u px=%d source=%lld chapter=%lld ms", book_kind(), (unsigned)book_chapter_count(), (unsigned)book_layout_page_count(), s_px,
             (long long)source_ms, (long long)((esp_timer_get_time() - open_started) / 1000 - source_ms));
    return true;
}
static app_redraw_t resume_choice(app_ctx_t* ctx, bool confirmed) {
    if (!s_resume_path[0]) return APP_REDRAW_NONE;
    char path[BOOK_STORE_PATH_MAX];
    copy_text(path, sizeof(path), s_resume_path);
    s_resume_path[0] = 0;
    if (confirmed) open_book(ctx, path);
    return APP_REDRAW_PAGE;
}
/* ---- 书架数据 / Shelf data ---- */
static bool shelf_matches(const shelf_entry_t* item) {
    // 来源：0 全部、1 本地、2 番茄。/ Source: 0 all, 1 local, 2 Tomato.
    return item->search_match && (s_filter == 0 || (s_filter == 1 && !item->tomato) || (s_filter == 2 && item->tomato));
}
static int compare_books(const void* a, const void* b) {
    const shelf_entry_t* x = a;
    const shelf_entry_t* y = b;
    if (shelf_matches(x) != shelf_matches(y)) return shelf_matches(x) ? -1 : 1;
    if (s_recent_sort && x->recent != y->recent) return x->recent > y->recent ? -1 : 1;
    int name = strcasecmp(x->name, y->name);
    return name ? name : strcmp(x->path, y->path);
}
static void sort_shelf(app_ctx_t* ctx) {
    if (s_count > 1) qsort(s_shelf, s_count, sizeof(*s_shelf), compare_books);
    s_visible_count = 0;
    while (s_visible_count < s_count && shelf_matches(&s_shelf[s_visible_count])) ++s_visible_count;
    ctx->leaf = 0;
}
static bool shelf_reserve(void) {
    if ((size_t)s_count < s_shelf_capacity) return true;
    if (s_count == INT_MAX) return false;
    size_t cap = s_shelf_capacity ? s_shelf_capacity * 2 : 32;
    if (cap > INT_MAX || cap > SIZE_MAX / sizeof(*s_shelf)) return false;
    shelf_entry_t* entries = heap_caps_realloc(s_shelf, cap * sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) return false;
    s_shelf = entries;
    s_shelf_capacity = cap;
    return true;
}

// 在删除文件前预留记录，避免删除成功后才遇到内存不足。
// Reserve before unlink so allocation failure never loses a completed deletion's retry.
static delete_retry_t* delete_retry_reserve(const shelf_entry_t* entry) {
    delete_retry_t* existing = delete_retry_find(entry->path);
    if (existing) return existing;
    delete_retry_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->entry = *entry;
    p->next = s_delete_retries;
    s_delete_retries = p;
    return p;
}
static void delete_retry_discard(const char* path) {
    delete_retry_t** p = &s_delete_retries;
    while (*p) {
        if (!strcmp((*p)->entry.path, path)) {
            delete_retry_t* old = *p;
            *p = old->next;
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}
// 标出番茄下载的书：书名、作者、新章节数来自 tomato/shelf.json。
// Mark Tomato books: title, author and new chapters come from tomato/shelf.json.
static void annotate_tomato(void) {
    tomato_book_t* t = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0;
    if (t) tomato_shelf_load(t, TOMATO_SHELF_MAX, &n);
    const size_t dir_len = strlen(TOMATO_BOOKS_DIR);
    for (int i = 0; i < s_count; ++i) {
        shelf_entry_t* e = &s_shelf[i];
        e->tomato = false;
        e->new_chapters = e->fresh_chapters = 0;
        e->title[0] = e->author[0] = e->book_id[0] = 0;
        if (e->is_flash || strncmp(e->path, TOMATO_BOOKS_DIR, dir_len) || e->path[dir_len] != '/') continue;
        const char* base = e->path + dir_len + 1;
        for (int j = 0; j < n; ++j) {
            if (strcmp(t[j].file, base)) continue;
            e->tomato = true;
            copy_text(e->title, sizeof(e->title), t[j].title);
            copy_text(e->author, sizeof(e->author), t[j].author);
            copy_text(e->book_id, sizeof(e->book_id), t[j].book_id);
            e->new_chapters = (uint16_t)tomato_book_pending(&t[j]);
            e->fresh_chapters = (uint16_t)tomato_book_fresh(&t[j]);
            break;
        }
    }
    heap_caps_free(t);
}
// 番茄更新替换书文件时中途拔卡，可能只剩“书名.txt.old”：原文件不在就改回来，书不会从书库消失。
// A card pulled while a Tomato update replaced a book may leave only "name.txt.old": rename it back when the book
// itself is missing, so it never vanishes from the shelf.
static void recover_old_books(const char* root) {
    char names[4][96];
    int n = 0;
    DIR* dir = opendir(root);
    if (!dir) return;
    struct dirent* ent;
    while (n < 4 && (ent = readdir(dir))) {
        const size_t len = strlen(ent->d_name);
        if (len > 8 && len < sizeof(names[0]) && !strcasecmp(ent->d_name + len - 4, ".old")) {
            memcpy(names[n], ent->d_name, len - 3);
            names[n][len - 4] = 0;
            const char* ext = strrchr(names[n], '.');
            if (ext && (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".epub"))) n++;
        }
    }
    closedir(dir);
    for (int k = 0; k < n; k++) {
        char base[200], old[208];
        if (snprintf(base, sizeof(base), "%.100s/%s", root, names[k]) >= (int)sizeof(base)) continue;
        snprintf(old, sizeof(old), "%s.old", base);
        struct stat st;
        if (stat(base, &st) != 0 && rename(old, base) == 0) ESP_LOGW(TAG, "restored %s", base);
    }
}

// 按进度记录重算“继续阅读”卡片指向的书；清进度后也要重算，卡片不能停在已清的书上。
// Recompute the continue-card book from saved progress; clearing progress must also redo it so the card never points at a cleared book.
static void refresh_continue(void) {
    book_progress_recent_t last;
    s_continue[0] = 0;
    if (book_progress_recent(&last, 1) == 1) copy_text(s_continue, sizeof(s_continue), last.path);
}
static void scan_shelf(app_ctx_t* ctx) {
    refresh_continue();
    s_count = 0;
    s_visible_count = 0;
    s_message[0] = 0;
    s_shelf_warning[0] = 0;
    book_store_root_t roots[2];
    int n = 0;
    esp_err_t root_err = book_store_roots(roots, &n);
    // 没插卡不算读取错误。/ A missing card is not a read error.
    bool truncated = false, skipped = false;
    bool unreadable = (root_err != ESP_OK && root_err != ESP_ERR_NOT_FOUND) || book_store_roots_degraded();
    s_storage[0] = 0;
    for (int i = 0; i < n; ++i) {
        char capacity[64];
        snprintf(capacity, sizeof(capacity), "%s%s %.1f MB", i ? " · " : "", roots[i].is_flash ? "内置余" : "TF余", book_store_free_bytes(&roots[i]) / 1048576.0);
        strncat(s_storage, capacity, sizeof(s_storage) - strlen(s_storage) - 1);
        recover_old_books(roots[i].path);
        DIR* dir = opendir(roots[i].path);
        if (!dir) { unreadable = true; continue; }
        struct dirent* ent;
        for (;;) {
            errno = 0;
            ent = readdir(dir);
            if (!ent) { if (errno) unreadable = true; break; }
            const char* ext = strrchr(ent->d_name, '.');
            if (!ext || (strcasecmp(ext, ".txt") && strcasecmp(ext, ".epub"))) continue;
            shelf_entry_t candidate = {0};
            shelf_entry_t* item = &candidate;
            int len = snprintf(item->path, sizeof(item->path), "%s/%s", roots[i].path, ent->d_name);
            if (len < 0 || (size_t)len >= sizeof(item->path) || strlen(ent->d_name) >= sizeof(item->name)) { skipped = true; continue; }
            struct stat st;
            if (stat(item->path, &st) != 0) { unreadable = true; continue; }
            if (!S_ISREG(st.st_mode)) continue;
            if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) { skipped = true; continue; }
            if (!shelf_reserve()) { truncated = true; break; }
            copy_text(item->name, sizeof(item->name), ent->d_name);
            item->search_match = read_pico_search_match(item->name, s_query);
            item->size = st.st_size;
            item->is_flash = roots[i].is_flash;
            book_progress_t p;
            item->has_progress = book_progress_load(item->path, item->size, &p);
            item->pct = item->has_progress ? p.pct : 0;
            item->recent = item->has_progress ? p.last_open_s : 0;
            s_shelf[s_count++] = candidate;
        }
        closedir(dir);
    }
    // 同路径重新出现也先完成旧清理，避免新阅读进度被后续重试擦掉。
    // Finish old cleanup even if the path reappears, before new reading can create progress.
    for (delete_retry_t* p = s_delete_retries; p; p = p->next) {
        int i = 0;
        while (i < s_count && strcmp(s_shelf[i].path, p->entry.path)) ++i;
        if (i == s_count) {
            if (!shelf_reserve()) { truncated = true; continue; }
            ++s_count;
        }
        s_shelf[i] = p->entry;
        s_shelf[i].selected = false;
        s_shelf[i].search_match = read_pico_search_match(p->entry.name, s_query);
    }
    annotate_tomato();
    sort_shelf(ctx);
    if (!n) copy_text(s_storage, sizeof(s_storage), root_err == ESP_ERR_NOT_FOUND ? "没有 TF 卡" : "存储不可用，请检查 TF 卡");
    if (truncated) snprintf(s_shelf_warning, sizeof(s_shelf_warning), "内存不足，仅列 %d 本；释放后重扫", s_count);
    else if (unreadable) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分目录或文件不可读，请检查后重扫");
    else if (skipped) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分文件过大或名称过长，未列入");
    else if (s_pending_invalidated) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "图书已更新，旧待保存进度已作废");
    if (!s_count && !unreadable && !truncated)
        copy_text(s_message, sizeof(s_message), n ? "暂无图书，请传入 TXT 或 EPUB" : "书都放在 TF 卡上，请插入 TF 卡");
    ESP_LOGI(TAG, "shelf books=%d roots=%d", s_count, n);
}

static void refresh_capacity(void) {
    book_store_root_t roots[2];
    int count = 0;
    s_storage[0] = 0;
    book_store_roots(roots, &count);
    for (int i = 0; i < count; ++i) {
        char line[64];
        snprintf(line, sizeof(line), "%s%s %.1f MB", i ? " · " : "", roots[i].is_flash ? "内置余" : "TF余", book_store_free_bytes(&roots[i]) / 1048576.0);
        strncat(s_storage, line, sizeof(s_storage) - strlen(s_storage) - 1);
    }
}
static void manage_apply(app_ctx_t* ctx) {
    if (!strcmp(s_path, s_managed.path)) free_book();
    esp_err_t err;
    if (s_delete_confirm && !s_file_removed) {
        delete_retry_t* retry = delete_retry_reserve(&s_managed);
        if (!retry) {
            copy_text(s_manage_message, sizeof(s_manage_message), "内存不足，无法保留删除重试记录");
            s_clear_confirm = false;
            return;
        }
        bool removed = false;
        err = book_store_delete(s_managed.path, &removed);
        if (removed) {
            retry->entry.removed = true;
            s_file_removed = true;
            if (tomato_file_of(s_managed.path)) tomato_shelf_remove_file(tomato_file_of(s_managed.path));
            // 封面缓存跟着书一起删，TF 卡不越用越大。/ Delete the cover cache with the book so the card does not keep growing.
            book_cover_forget(s_managed.path);
            pending_discard(s_managed.path);
            book_store_notify_changed();
            s_store_revision = book_store_revision();
            for (int i = 0; i < s_count; ++i) if (!strcmp(s_shelf[i].path, s_managed.path)) s_shelf[i].removed = true;
            refresh_capacity();
        } else delete_retry_discard(s_managed.path);
    } else {
        pending_discard(s_managed.path);
        err = book_progress_forget(s_managed.path);
    }
    s_clear_confirm = false;
    if (err != ESP_OK) {
        copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ?
                  "文件已删除，进度清理失败，请重试" : s_delete_confirm ?
                  "文件删除失败，请检查存储后重试" : "进度清理失败，请重新尝试");
        return;
    }
    delete_retry_discard(s_managed.path);
    s_view = SHELF;
    int write = 0, leaf = ctx->leaf;
    for (int i = 0; i < s_count; ++i) {
        if (!strcmp(s_shelf[i].path, s_managed.path)) {
            if (s_file_removed) continue;
            s_shelf[i].has_progress = false; s_shelf[i].pct = 0; s_shelf[i].recent = 0;
        }
        s_shelf[write++] = s_shelf[i];
    }
    s_count = write;
    refresh_continue();
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
}

/* ---- 输入与生命周期 / Input and lifecycle ---- */
static void sensor_set(app_ctx_t* ctx, bool on) {
    memset(&s_shake, 0, sizeof(s_shake));
    if (!ctx->sensor_ready) return;
    if (on) {
        if (!s_sensor_saved) {
            s_sensor_config = *sc7a20h_get_config(ctx->acc);
            s_sensor_saved = true;
        }
        sc7a20h_sensor_config_t config = s_sensor_config;
        config.odr = SC7A20H_ODR_100;
        config.fs = SC7A20H_FS_2G;
        s_sensor_on = sc7a20h_apply_config(ctx->acc, &config) == ESP_OK &&
            sc7a20h_activity_config(ctx->acc, BOOK_SHAKE_THS_MG, BOOK_SHAKE_DUR) == ESP_OK;
        if (!s_sensor_on) {
            sc7a20h_aoi_cfg_t off = {0};
            sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
            sc7a20h_apply_config(ctx->acc, &s_sensor_config);
            s_sensor_saved = false;
            read_pico_sensor_sleep(ctx->acc);
        }
    } else {
        sc7a20h_aoi_cfg_t off = {0};
        sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
        if (s_sensor_saved) sc7a20h_apply_config(ctx->acc, &s_sensor_config);
        s_sensor_saved = false;
        read_pico_sensor_sleep(ctx->acc);
        s_sensor_on = false;
    }
}
// 工具条上的“上一章 / 下一章”：跳到那一章开头，收起工具条，保存进度。/ The toolbar's previous / next chapter: jump to
// that chapter's start, close the toolbar and save progress.
static app_redraw_t jump_chapter(app_ctx_t* ctx, int dir) {
    if (!s_text || !chapter_jump_ok(dir)) return APP_REDRAW_NONE;
    save_progress();
    if (!load_chapter(ctx, dir < 0 ? s_chapter - 1 : s_chapter + 1, 0, false)) {
        if (s_view == READING) { s_view = s_text ? TOC : SHELF; ctx->leaf = s_text ? toc_leaf() : 0; }
        return APP_REDRAW_PAGE;
    }
    s_toolbar = false;
    s_typeset = false;
    s_auto_ms = ctx->now_ms;
    invalidate_prep();
    save_progress();
    ESP_LOGI(TAG, "jump chapter=%u", (unsigned)s_chapter);
    return APP_REDRAW_PAGE;
}
static app_redraw_t turn_page(app_ctx_t* ctx, int dir) {
    if (!s_text || s_clear_confirm) return APP_REDRAW_NONE;
    if (dir > 0 && s_page + 1 >= book_layout_page_count() && !book_layout_complete()) {
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_extend(2);
        unlock_draw();
        if (!ok) {
            free_book(); s_view = SHELF; ctx->leaf = 0;
            copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
            return APP_REDRAW_PAGE;
        }
    }
    bool changed = true;
    if (dir > 0 && s_page + 1 < book_layout_page_count()) ++s_page;
    else if (dir < 0 && s_page) --s_page;
    else if (dir > 0 && s_chapter + 1 < book_chapter_count()) { save_progress(); changed = load_chapter(ctx, s_chapter + 1, 0, false); }
    else if (dir < 0 && s_chapter) { save_progress(); changed = load_chapter(ctx, s_chapter - 1, 0, true); }
    else return APP_REDRAW_NONE;
    if (!changed) { s_view = s_text ? TOC : SHELF; ctx->leaf = s_text ? toc_leaf() : 0; return APP_REDRAW_PAGE; }
    s_toolbar = false;
    s_last_turn_ms = ctx->now_ms;
    s_auto_ms = ctx->now_ms;   // 不管怎么翻的，自动翻页都从这页重新计时 / Any turn restarts the auto countdown
#if APP_GC16_EVERY > 0
    s_full = (++s_turns % APP_GC16_EVERY) == 0;
#else
    s_full = false;
#endif
    if (s_unsaved < 8) ++s_unsaved;
    if (s_unsaved >= 8 && !s_save_failed) save_progress();
    ESP_LOGI(TAG, "turn chapter=%u page=%u/%u pct=%u", (unsigned)s_chapter, (unsigned)s_page + 1, (unsigned)book_layout_page_count(), percent(s_page));
    return paint_reading(ctx, MODE_GL16);
}
static app_redraw_t resize_text(app_ctx_t* ctx, int dir) {
    int next = s_px + dir * BOOK_PX_STEP;
    if (next < BOOK_PX_MIN || next > BOOK_PX_MAX) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    save_progress();
    lock_draw();
    invalidate_prep();
    int old = s_px;
    if (!book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), next)) {
        bool restored = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), old);
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            copy_text(s_message, sizeof(s_message), "排版失败，请重新扫描并打开图书");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    s_px = next;
    s_page = book_layout_page_for_offset(off);
    unlock_draw();
    app_settings_set_book_px(s_px);
    save_progress();
    s_size_settle_ms = ctx->now_ms + BOOK_SIZE_SETTLE_MS;
    return paint_reading(ctx, MODE_DU);
}
// 换排版样式：按当前位置重排；失败就退回原样式。/ Change the style: re-paginate at the current position, or roll back.
static app_redraw_t restyle(app_ctx_t* ctx, int row, int opt) {
    app_book_style_t next = s_style;
    if (row == 0) next.line = (uint8_t)opt;
    else if (row == 1) next.para = (uint8_t)opt;
    else if (row == 2) next.margin = (uint8_t)opt;
    else if (row == 3) next.indent = opt != 0;
    else if (row == 4) next.underline = opt != 0;
    else next.keep_blank = opt != 0;
    if (!memcmp(&next, &s_style, sizeof(next))) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    save_progress();
    lock_draw();
    invalidate_prep();
    const app_book_style_t old = s_style;
    s_style = next;
    apply_style();
    if (!book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px)) {
        s_style = old;
        apply_style();
        bool restored = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            copy_text(s_message, sizeof(s_message), "排版失败，请重新扫描并打开图书");
        }
        return APP_REDRAW_PAGE;
    }
    s_page = book_layout_page_for_offset(off);
    unlock_draw();
    app_settings_set_book_style(&s_style);
    save_progress();
    (void)ctx;
    return APP_REDRAW_PAGE;
}
// Treader：字体页不在菜单里了，直接引用。/ Treader: the font page left the menu, so reference it directly.
extern const app_desc_t app_font_pick;
static void goto_font(app_ctx_t* ctx) {
    save_progress();
    ctx->request_app = &app_font_pick;
}
static app_redraw_t manage_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (manage_check_shown() && ui_rect_hit(manage_check_rect(), x, y)) {
        // 查完的结果显示在书库状态行。/ The outcome shows on the library's status line.
        if (tom_running()) return APP_REDRAW_NONE;
        char id[TOMATO_ID_LEN];
        copy_text(id, sizeof(id), s_managed.book_id);
        s_view = SHELF;
        return tomato_action(id);
    }
    int count = s_clear_confirm || s_file_removed ? 2 : 3;
    for (int i = 0; i < count; ++i) if (ui_rect_hit(manage_rect(i, count), x, y)) {
        if (s_clear_confirm) { if (!i) s_clear_confirm = false; else manage_apply(ctx); }
        else if (!i) s_view = SHELF;
        else if (s_file_removed) manage_apply(ctx);
        else { s_clear_confirm = true; s_delete_confirm = i == 2; s_manage_message[0] = 0; }
        break;
    }
    return APP_REDRAW_PAGE;
}
// 成功项退出选择，已删文件的失败项仅重试元数据。/ Deselect successes; removed-file failures retry metadata only.
static void batch_apply(app_ctx_t* ctx) {
    unsigned done = 0, failed = 0;
    int write = 0;
    for (int i = 0; i < s_count; ++i) {
        shelf_entry_t item = s_shelf[i];
        bool discard = false;
        if (item.selected) {
            if (!strcmp(s_path, item.path)) free_book();
            esp_err_t err;
            if (s_batch_delete && !item.removed) {
                delete_retry_t* retry = delete_retry_reserve(&item);
                if (!retry) { ++failed; s_shelf[write++] = item; continue; }
                bool removed = false;
                err = book_store_delete(item.path, &removed);
                if (removed) {
                    retry->entry.removed = true;
                    item.removed = true; pending_discard(item.path);
                    if (tomato_file_of(item.path)) tomato_shelf_remove_file(tomato_file_of(item.path));
                    book_cover_forget(item.path);
                    book_store_notify_changed(); s_store_revision = book_store_revision();
                } else delete_retry_discard(item.path);
            } else { pending_discard(item.path); err = book_progress_forget(item.path); }
            if (err == ESP_OK) {
                delete_retry_discard(item.path);
                ++done; item.selected = false; item.has_progress = false; item.pct = 0; item.recent = 0;
                discard = item.removed;
            } else ++failed;
        }
        if (!discard) s_shelf[write++] = item;
    }
    s_count = write;
    refresh_capacity();
    if (done) refresh_continue();
    int leaf = ctx->leaf;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
    s_batch_confirm = false;
    snprintf(s_batch_message, sizeof(s_batch_message), "成功 %u 本，失败 %u 本%s", done, failed, failed ? "；所选可重试" : "");
}
static app_redraw_t batch_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_batch_confirm) {
        if (ui_rect_hit(ui_row_rect(0, 2, DIALOG_BTN_Y, UI_BTN_H), x, y)) s_batch_confirm = false;
        else if (ui_rect_hit(ui_row_rect(1, 2, DIALOG_BTN_Y, UI_BTN_H), x, y)) batch_apply(ctx);
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; i < 5; ++i) if (ui_rect_hit(batch_rect(i), x, y)) {
        if (!i) select_page(ctx->leaf);
        else if (i == 1) clear_selection();
        else if (i == 2) { clear_selection(); s_batch_message[0] = 0; scan_shelf(ctx); }
        else if (selected_count()) { s_batch_delete = i == 3; s_batch_confirm = true; }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
// 番茄检查、下载更新、只查一本（book_id 非空）；在跑就取消。后台在跑浏览任务先让它停，停不了就说原因。
// Tomato check, update download, or a single-book check (book_id set); cancels when running. A browsing job in the
// background is asked to stop first; otherwise say why it cannot start.
static app_redraw_t tomato_action(const char* book_id) {
    if (tom_running()) {
        tomato_sync_cancel();
        s_tom_cancelling = true;
        copy_text(s_tom_note, sizeof(s_tom_note), "正在取消…");
        return APP_REDRAW_PAGE;
    }
    s_tom_confirm_cancel = s_tom_cancelling = false;
    const tomato_preempt_t room = tomato_sync_busy() ? tomato_sync_preempt() : TOMATO_PREEMPT_FREE;
    if (room != TOMATO_PREEMPT_FREE) {
        copy_text(s_tom_note, sizeof(s_tom_note), room == TOMATO_PREEMPT_STOPPING ? "正在停下后台加载，请再点一次"
                                                                                  : "后台正在下载或同步，完成后再试");
        return APP_REDRAW_PAGE;
    }
    const tom_op_t op = book_id ? TOM_ONE : tomato_pending() > 0 ? TOM_UPDATE : TOM_CHECK;
    const esp_err_t err = op == TOM_ONE ? tomato_sync_start_check_one(book_id)
                        : op == TOM_UPDATE ? tomato_sync_start_update() : tomato_sync_start_check();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tomato op %d: %s", op, esp_err_to_name(err));
        copy_text(s_tom_note, sizeof(s_tom_note), "后台正在忙，稍后再试");
        return APP_REDRAW_PAGE;
    }
    s_tom_op = op;
    s_tom_seq = tomato_sync_seq();
    s_tom_rev = 0;
    s_tom_note[0] = 0;
    return APP_REDRAW_PAGE;
}
static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_resume_path[0]) {
        for (int i = 0; i < 2; ++i) if (ui_rect_hit(ui_row_rect(i, 2, DIALOG_BTN_Y, UI_BTN_H), x, y)) {
            return resume_choice(ctx, i != 0);
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == MANAGE) return manage_action(ctx, x, y);
    if (s_view == SEARCH) {
        if (y < TR_S(180) && x < 96) { search_finish(ctx, false); return APP_REDRAW_PAGE; }
        tr_kbd_result_t res = tr_kbd_touch(&s_search_kbd, x, y);
        if (res == TR_KBD_SUBMIT) { search_finish(ctx, true); return APP_REDRAW_PAGE; }
        if (res != TR_KBD_CHANGED) return APP_REDRAW_NONE;
        render(ctx, ctx->fb);
        s_area = (EpdRect){0, TR_S(70), UI_LOCK_WIDTH, UI_LOCK_HEIGHT - TR_S(70)};
        s_mode = MODE_DU;
        return APP_REDRAW_AREA;
    }
    if (s_view == BULK) { app_redraw_t result = batch_action(ctx, x, y); if (result != APP_REDRAW_NONE) return result; }
    if (s_view == READING) {
        if (s_image_open) return close_image();
        // 字体提示只在工具条收起时可点；工具条开着时顶栏（‹ 书库、书名）盖住了它。
        // The font hint is tappable only with the toolbar closed; an open toolbar's top bar ("‹ 书库", title) covers it.
        if (!s_toolbar && ttf_font_is_builtin() && y < TR_S(140)) { goto_font(ctx); return APP_REDRAW_NONE; }
        if (s_toolbar && s_typeset) {
            if (y >= TYPESET_Y) {
                if (ui_rect_hit(typeset_done_rect(), x, y)) { s_typeset = false; return APP_REDRAW_PAGE; }
                for (int row = 0; row < TYPESET_ROWS; ++row)
                    for (int opt = 0; opt < typeset_choices(row); ++opt)
                        if (ui_rect_hit(typeset_rect(row, opt), x, y)) return restyle(ctx, row, opt);
                return APP_REDRAW_NONE;
            }
            // 点正文区：收起面板和工具条，看效果。/ Tapping the page closes the panel and toolbar to show the result.
            if (y > TR_S(110)) { s_typeset = false; s_toolbar = false; invalidate_prep(); return APP_REDRAW_PAGE; }
        }
        if (s_toolbar) {
            for (int i = 0; i < BOOK_TOOL_COUNT; ++i) if (ui_rect_hit(tool_rect(i), x, y)) {
                invalidate_prep();
                if (i == 0) { save_progress(); s_view = TOC; ctx->leaf = toc_leaf(); }
                else if (i == 1 || i == 2) return resize_text(ctx, i == 1 ? -1 : 1);
                else if (i == 3) { save_progress(); s_toolbar = false; ctx->request_menu = true; return APP_REDRAW_NONE; }
                else if (i == 4) { s_typeset = true; return APP_REDRAW_PAGE; }
                else if (i == 7 || i == 8) {
                    s_auto_level = auto_turn_step(s_auto_level, i == 7 ? -1 : 1);
                    app_settings_set_auto_turn(false, (uint8_t)s_auto_level);
                    return APP_REDRAW_PAGE;
                }
                else if (i == 9 || i == 10) return jump_chapter(ctx, i == 9 ? -1 : 1);
                else { save_progress(); free_book(); s_view = SHELF; scan_shelf(ctx); }
                return APP_REDRAW_PAGE;
            }
            EpdRect track = track_rect();
            if (ui_rect_hit(track, x, y)) {
                save_progress();
                uint32_t target = (uint64_t)(x - track.x) * book_total_bytes() / (track.width - 1);
                size_t ch = 0;
                while (ch + 1 < book_chapter_count() && book_chapter_byte_offset(ch + 1) <= target) ++ch;
                if (load_chapter(ctx, ch, 0, false)) {
                    uint32_t start = book_chapter_byte_offset(ch), end = chapter_end();
                    size_t off = end > start ? (uint64_t)(target - start) * s_text_len / (end - start) : 0;
                    // load_chapter 只排到第 0 页附近；持绘制锁续排到覆盖目标偏移，再定页，否则会落回章首并覆盖进度。
                    // load_chapter only lays out the first pages; extend under the draw lock until the target offset is
                    // covered, then pick the page, or the jump lands at the chapter start and overwrites the progress.
                    lock_draw();
                    invalidate_prep();
                    bool ok = true;
                    while (ok && !book_layout_complete() && book_layout_page_start_offset(book_layout_page_count()) <= off)
                        ok = book_layout_extend(2);
                    if (ok) s_page = book_layout_page_for_offset(off);
                    unlock_draw();
                    if (!ok) {
                        free_book(); s_view = SHELF; ctx->leaf = 0;
                        copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
                        return APP_REDRAW_PAGE;
                    }
                    save_progress();
                }
                return APP_REDRAW_PAGE;
            }
            // 点在工具条面板的空白处什么也不做，不要翻页。/ Taps on empty parts of the toolbar panel do nothing.
            if (y >= TOOL_PANEL_Y && y < UI_BAR_TOP) return APP_REDRAW_NONE;
        }
        if (!s_toolbar) {
            lock_draw();
            size_t block = book_layout_image_at(s_page, body_rect(), x, y, NULL);
            unlock_draw();
            if (block != SIZE_MAX) return open_image(ctx, block);
        }
        const int zone = tap_zone(x, y);
        if (zone) return turn_page(ctx, zone);
        s_toolbar = !s_toolbar;
            s_typeset = false;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_view == SHELF || s_view == BULK) {
        // 更新中：点一下胶囊变“再点取消”，再点才取消；点别处复原。/ While running: one tap on the pill asks for
        // confirmation and a second cancels; tapping elsewhere resets it.
        const bool confirm = s_tom_confirm_cancel;
        s_tom_confirm_cancel = false;
        if (tomato_hint_shown() && ui_rect_hit(tomato_hint_rect(), x, y)) {
            if (!tom_running()) return tomato_action(NULL);
            if (s_tom_cancelling) return APP_REDRAW_NONE;
            if (!confirm) { s_tom_confirm_cancel = true; return APP_REDRAW_PAGE; }
            return tomato_action(NULL);
        }
        if (confirm) return APP_REDRAW_PAGE;
        if (ui_rect_hit(chip_rect(0), x, y)) {
            s_filter = (s_filter + 1) % 3; clear_selection(); s_batch_message[0] = 0;
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(chip_rect(1), x, y)) {
            s_recent_sort = !s_recent_sort;
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(chip_rect(2), x, y)) {
            s_message[0] = 0;
            s_view = s_view == BULK ? SHELF : BULK;
            s_batch_confirm = false;
            return APP_REDRAW_PAGE;
        }
        if (s_query[0] && ui_rect_hit(chip_rect(3), x, y)) {
            s_query[0] = 0;
            refresh_search_matches();
            clear_selection();
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(search_icon_rect(), x, y)) { search_begin(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit(view_icon_rect(), x, y)) {
            // 换列表 / 网格，停在原来这页第一本所在的页。/ Switch list / grid and stay on the page holding the old
            // page's first book.
            const int first = ctx->leaf * BOOK_ROWS;
            app_settings_set_shelf_grid(!app_settings_shelf_grid());
            ctx->leaf = first / BOOK_ROWS;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(import_icon_rect(), x, y)) {
            // 先到“设置 › 传书”选 Wi-Fi 还是 USB。/ Go to Settings > Transfer to pick Wi-Fi or USB.
            app_tr_settings_request_transfer();
            ctx->request_app = tr_nav_tab_app(TR_TAB_SETTINGS);
            return APP_REDRAW_NONE;
        }
        if (s_view == SHELF && s_save_failed && ui_rect_hit(save_retry_rect(), x, y)) {
            retry_progress();
            return APP_REDRAW_PAGE;
        }
        if (s_tom_cfg && s_view == SHELF && ui_rect_hit(store_rect(), x, y)) {
            ctx->request_app = &app_tr_store;
            return APP_REDRAW_NONE;
        }
        if (card_shown() && ui_rect_hit(card_rect(), x, y)) {
            open_book(ctx, s_shelf[continue_index()].path);
            return APP_REDRAW_PAGE;
        }
        if (s_view == SHELF && tr_nav_touch(ctx, x, y, TR_TAB_LIBRARY)) return APP_REDRAW_NONE;
    }
    if (s_view == TOC && toc_jump_shown() && ui_rect_hit(toc_jump_rect(), x, y)) {
        ctx->leaf = ctx->leaf + 1 < leaves() ? leaves() - 1 : 0;
        return APP_REDRAW_PAGE;
    }
    int nav = nav_hit(x, y);
    if (nav == 0 || nav == 2) {
        int next = ctx->leaf + (nav == 0 ? -1 : 1);
        if (next < 0 || next >= leaves()) return APP_REDRAW_NONE;
        ctx->leaf = next;
        return APP_REDRAW_PAGE;
    }
    if (nav == 1) {
        s_message[0] = 0;
        if (s_view == TOC && s_text) { s_view = READING; s_toolbar = false; }
        else { s_view = s_view == BULK ? SHELF : BULK; s_batch_confirm = false; }
        return APP_REDRAW_PAGE;
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return APP_REDRAW_NONE;
    int rows = s_view == TOC ? BOOK_TOC_ROWS : BOOK_ROWS;
    for (int row = 0; row < rows; ++row) if (ui_rect_hit(row_rect(row, s_view == TOC), x, y)) {
        int i = ctx->leaf * rows + row;
        if (s_view == BULK && i < s_visible_count) toggle_selection(i);
        else if (s_view == SHELF && i < s_visible_count) {
            if (s_shelf[i].removed) {
                s_managed = s_shelf[i]; s_view = MANAGE; s_file_removed = s_delete_confirm = true; s_clear_confirm = false;
                copy_text(s_manage_message, sizeof(s_manage_message), "文件已删除，进度清理失败，请重试");
            } else open_book(ctx, s_shelf[i].path);
        }
        else if (s_view == TOC && i < book_chapter_count()) {
            save_progress();
            if (load_chapter(ctx, i, 0, false)) { s_view = READING; s_toolbar = false; save_progress(); }
        }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
// 番茄更新只在末尾追加章节：把阅读进度记录迁到新文件大小。/ A Tomato update only appends chapters: move the progress
// record to the new file size.
static void tomato_file_hook(const char* path, uint32_t old_size, uint32_t new_size) {
    if (old_size && old_size != new_size) book_progress_resize(path, old_size, new_size);
}
static void on_enter(app_ctx_t* ctx) {
    s_resume_path[0] = 0;
    ensure_prep();
    s_view = SHELF;
    ctx->leaf = 0;
    s_count = 0;
    s_px = app_settings_book_px();
    s_auto_level = app_settings_auto_turn(false);
    s_auto_ms = ctx->now_ms;
    s_auto_done = false;
    // 排版样式每次进书库读一次，排版前生效。/ Load the layout style on entry so it applies before any pagination.
    s_style = app_settings_book_style();
    s_typeset = false;
    apply_style();
    s_scan_pending = true;
    // 续读弹窗不再出现：书架顶部的“继续阅读”卡片代替它。/ No resume dialog: the continue card replaces it.
    s_resume_pending = false;
    // 传书页已停止并 join HTTP，安全注销且只丢弃对应路径的旧进度。
    // Transfer has stopped and joined HTTP; safely unregister only invalidated paths.
    pending_drop_invalidated();
    s_store_revision = book_store_revision();
    s_toolbar = s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_du_count = 0;
    s_full = false;
    s_size_settle_ms = 0;
    s_poll_ms = 0;
    book_cover_drop_pending();
    copy_text(s_storage, sizeof(s_storage), "正在检测存储…");
    copy_text(s_message, sizeof(s_message), "正在扫描图书…");
    read_pico_sd_start_probe();
    s_shake_enabled = app_settings_book_shake();
    if (s_shake_enabled) sensor_set(ctx, true);
    // 番茄并进书库：服务器设了才显示“书城”和检查更新；更新换书文件时把进度迁到新大小，正在读的书不换。
    // Tomato in the library: "书城" and the update check show only with a server set; an update moves progress to
    // the new file size and skips the book being read.
    tomato_config_t cfg;
    s_tom_cfg = tomato_config_load(&cfg) == ESP_OK;
    memset(&cfg, 0, sizeof(cfg));
    tomato_sync_set_file_hook(tomato_file_hook);
    tomato_sync_set_open_check(app_book_path_is_open);
    if (!tom_running() && !(s_tom_op != TOM_NONE && !tomato_sync_busy() && tomato_sync_seq() == s_tom_seq)) {
        s_tom_op = TOM_NONE;
        s_tom_note[0] = 0;
    }
}
static void book_on_exit(app_ctx_t* ctx) {
    auto_turn_set_active(false);
    s_resume_path[0] = 0;
    if (s_view == SEARCH) { memset(&s_search_kbd, 0, sizeof(s_search_kbd)); s_view = SHELF; }
    s_pressed_control = -1;
    save_progress();
    sensor_set(ctx, false);
    // 拿着排版锁再释放书和删预排版任务：任务这时不可能画到一半（它不是在等通知就是在等这把锁）。
    // Free the book and delete the prep task while holding the draw lock: the task cannot be mid-draw then (it waits
    // either for a notification or for this lock).
    lock_draw();
    free_book();
    if (s_prep_task) { vTaskDelete(s_prep_task); s_prep_task = NULL; }
    unlock_draw();
    free(s_shelf);
    s_shelf = NULL;
    s_shelf_capacity = 0;
    s_count = s_visible_count = 0;
    free(s_next_fb);
    s_next_fb = NULL;
    if (s_prep_done) { vSemaphoreDelete(s_prep_done); s_prep_done = NULL; }
    if (s_draw_lock) { vSemaphoreDelete(s_draw_lock); s_draw_lock = NULL; }
}
// 每次锁屏前（含浅睡）存一次进度；不画屏、不停传感器。没打开书时 save_progress 直接返回。
// Save progress before every lock, light sleep included; no drawing, no sensor shutdown. save_progress is a no-op with no book open.
static void book_on_lock(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_text) return;
    lock_draw();
    save_progress();
    unlock_draw();
}
// 先停止使用旧卡句柄与字体预渲染，主循环随后切换内置字体。
// Stop old-card handles and font preparation before the loop switches to the builtin font.
static void book_on_media_lost(app_ctx_t* ctx) {
    s_resume_path[0] = 0;
    lock_draw();
    save_progress();
    free_book();
    unlock_draw();
    s_view = SHELF;
    s_count = s_visible_count = 0;
    ctx->leaf = 0;
    s_toolbar = s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_scan_pending = true;
    s_resume_pending = false;
    s_du_count = 0;
    s_size_settle_ms = 0;
    copy_text(s_storage, sizeof(s_storage), "TF 卡已移除");
    copy_text(s_message, sizeof(s_message), "已停止阅读，正在检查内置图书");
}
// 控件编号仅用于保持按下与抬起命中同一个目标。/ IDs pair a press with release on the same control.
static int control_at(app_ctx_t* ctx, uint16_t x, uint16_t y, EpdRect* rect) {
    if (s_resume_path[0]) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, DIALOG_BTN_Y, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 700 + i;
        }
        return -1;
    }
    if (s_view == SEARCH) {
        // 整个搜索页算一个控件：按下不画按压态，抬起交给键盘。/ The search page is one control: no press paint; release goes to the keyboard.
        *rect = (EpdRect){0, 0, UI_LOCK_WIDTH, UI_LOCK_HEIGHT};
        return 499;
    }
    if (s_view == BULK && s_batch_confirm) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, DIALOG_BTN_Y, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 600 + i;
        }
        return -1;
    }
    if (s_view == BULK) for (int i = 0; i < 5; ++i) { *rect = batch_rect(i); if (ui_rect_hit(*rect, x, y)) return 610 + i; }
    if (s_view == MANAGE) {
        *rect = manage_check_rect();
        if (manage_check_shown() && ui_rect_hit(*rect, x, y)) return 410;
        int count = s_clear_confirm || s_file_removed ? 2 : 3;
        for (int i = 0; i < count; ++i) {
            *rect = manage_rect(i, count);
            if (ui_rect_hit(*rect, x, y)) return s_clear_confirm ? 300 + i : s_file_removed && i == 1 ? 403 : 400 + i;
        }
        return -1;
    }
    if (s_clear_confirm) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, TR_S(610), UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 300 + i;
        }
        return -1;
    }
    if (s_view == READING) {
        if (s_toolbar && s_typeset && y >= TYPESET_Y) {
            *rect = typeset_done_rect();
            if (ui_rect_hit(*rect, x, y)) return 840;
            for (int row = 0; row < TYPESET_ROWS; ++row)
                for (int opt = 0; opt < typeset_choices(row); ++opt) {
                    *rect = typeset_rect(row, opt);
                    if (ui_rect_hit(*rect, x, y)) return 800 + row * 4 + opt;
                }
            return -1;
        }
        if (s_toolbar && !s_typeset) for (int i = 0; i < BOOK_TOOL_COUNT; ++i) {
            if ((i == 9 || i == 10) && !chapter_jump_ok(i == 9 ? -1 : 1)) continue;
            *rect = tool_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 200 + i;
        }
        if (!s_toolbar) {
            lock_draw();
            size_t block = book_layout_image_at(s_page, body_rect(), x, y, rect);
            unlock_draw();
            if (block != SIZE_MAX) return 1000 + (int)block;
        }
        return -1;
    }
    if (s_view == TOC) {
        *rect = toc_jump_rect();
        if (toc_jump_shown() && ui_rect_hit(*rect, x, y)) return CONTROL_TOC_JUMP;
    }
    for (int i = 0; i < 3; ++i) {
        *rect = nav_rect(i);
        if (rect->width && ui_rect_hit(*rect, x, y)) return 100 + i;
    }
    if (s_view == SHELF || s_view == BULK) {
        for (int i = 0; i < 4; ++i) {
            if (i == 3 && !s_query[0]) break;
            *rect = chip_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 110 + i;
        }
        *rect = search_icon_rect();
        if (ui_rect_hit(*rect, x, y)) return 114;
        *rect = view_icon_rect();
        if (ui_rect_hit(*rect, x, y)) return 118;
        *rect = import_icon_rect();
        if (ui_rect_hit(*rect, x, y)) return 115;
        *rect = save_retry_rect();
        if (s_view == SHELF && s_save_failed && ui_rect_hit(*rect, x, y)) return 116;
        *rect = tomato_hint_rect();
        if (tomato_hint_shown() && ui_rect_hit(*rect, x, y)) return CONTROL_TOMATO_HINT;
        *rect = store_rect();
        if (s_tom_cfg && s_view == SHELF && ui_rect_hit(*rect, x, y)) return CONTROL_STORE;
        *rect = card_rect();
        if (card_shown() && ui_rect_hit(*rect, x, y)) {
            // 点击判断用卡片本身；刷新区域放大 8 像素，包住画在外圈的按下描边。
            // Hit-test the card itself; widen the refresh area by 8 px to cover the pressed outline drawn outside it.
            *rect = ui_inset_rect(card_rect(), -TR_S(8));

            return CONTROL_CARD;
        }
        if (s_view == SHELF) {
            int tab = tr_tab_hit(x, y);
            if (tab >= 0) { *rect = tr_tab_area(); return 120 + tab; }
        }
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return -1;
    int rows = s_view == TOC ? BOOK_TOC_ROWS : BOOK_ROWS;
    int count = s_view == TOC ? (int)book_chapter_count() : s_visible_count;
    for (int row = 0; row < rows && ctx->leaf * rows + row < count; ++row) {
        *rect = row_rect(row, s_view == TOC);
        if (ui_rect_hit(*rect, x, y)) {
            // 网格的按下描边画在格子外圈，刷新区域放大包住它。/ The grid's pressed outline sits outside the cell; widen
            // the refresh area to cover it.
            if (shelf_grid()) *rect = ui_inset_rect(*rect, -TR_S(8));
            return row;
        }
    }
    return -1;
}
static app_redraw_t paint_control(app_ctx_t* ctx, EpdRect rect) {
    render(ctx, ctx->fb);
    s_area = rect;
    s_mode = MODE_DU;
    return APP_REDRAW_AREA;
}
static app_redraw_t gesture_event(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    // 碰屏幕就重新计时。/ Any touch restarts the auto-turn countdown.
    s_auto_ms = ctx->now_ms;
    s_auto_done = false;
    if (s_image_open) {
        s_pressed_control = -1;
        return ev->type == UI_GESTURE_TAP ? close_image() : APP_REDRAW_NONE;
    }
    EpdRect start_rect = {0}, end_rect = {0};
    int start = control_at(ctx, ev->x0, ev->y0, &start_rect);
    int end = control_at(ctx, ev->x, ev->y, &end_rect);
    if (ev->type == UI_GESTURE_PRESS) {
        if (start >= 1000 || start == 499) { s_pressed_control = -1; return APP_REDRAW_NONE; }
        s_pressed_control = start;
        return start >= 0 ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
    }
    bool decorated = s_pressed_control >= 0;
    s_pressed_control = -1;
    if (ev->type == UI_GESTURE_LONG_PRESS && start == end) {
        if (s_view == SHELF && start >= 0 && start < BOOK_ROWS && !s_clear_confirm) {
            s_managed = s_shelf[ctx->leaf * BOOK_ROWS + start];
            s_view = MANAGE;
            s_clear_confirm = false;
            s_delete_confirm = s_file_removed = s_managed.removed;
            copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ? "文件已删除，进度清理失败，请重试" : "");
            return APP_REDRAW_PAGE;
        }
        if (s_view == READING && !s_toolbar && !s_clear_confirm && ui_rect_hit(body_rect(), ev->x0, ev->y0)) {
            save_progress();
            s_view = TOC;
            ctx->leaf = toc_leaf();
            return APP_REDRAW_PAGE;
        }
    }
    if (ev->type == UI_GESTURE_TAP && !s_scan_pending) {
        if (start >= 0 && start == end) {
            app_redraw_t result = action_at(ctx, ev->x0, ev->y0);
            // 整页类控件（搜索页、标签、图标）没动作就什么也不刷。/ Page-wide controls with no action refresh nothing.
            if (result == APP_REDRAW_NONE && (start == 499 || (start >= 114 && start < 130))) return APP_REDRAW_NONE;
            return result != APP_REDRAW_NONE ? result : paint_control(ctx, start_rect);
        }
        if (start < 0 && end < 0 && s_view == READING && !s_clear_confirm)
            return action_at(ctx, ev->x0, ev->y0);
    }
    if (!s_clear_confirm && !s_scan_pending && !s_resume_path[0]) {
        if (s_view == READING && !s_toolbar && ui_rect_hit(body_rect(), ev->x0, ev->y0)) {
            if (ev->type == UI_GESTURE_SWIPE_L) return turn_page(ctx, 1);
            if (ev->type == UI_GESTURE_SWIPE_R) return turn_page(ctx, -1);
        } else if ((s_view == SHELF || s_view == TOC || (s_view == BULK && !s_batch_confirm)) &&
                   (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
            int next = ctx->leaf + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
            if (next >= 0 && next < leaves()) { ctx->leaf = next; return APP_REDRAW_PAGE; }
        }
    }
    return decorated ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
}
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    s_auto_ms = ctx->now_ms;
    s_auto_done = false;
    s_pressed_control = -1;
    if (s_image_open && (key == UI_KEY_1 || key == UI_KEY_2 || key == UI_KEY_3)) return close_image();
    if (s_resume_path[0]) {
        if (key == UI_KEY_1 || key == UI_KEY_3) {
            return resume_choice(ctx, key == UI_KEY_3);
        }
        if (key == UI_KEY_2) s_resume_path[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (s_scan_pending || s_clear_confirm) return APP_REDRAW_NONE;
    if (s_view == MANAGE) {
        if (key == UI_KEY_1) { s_view = SHELF; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_view == SEARCH) { if (key == UI_KEY_1) { search_finish(ctx, false); return APP_REDRAW_PAGE; } return APP_REDRAW_NONE; }
    if (s_batch_confirm) { if (key == UI_KEY_1) { s_batch_confirm = false; return APP_REDRAW_PAGE; } return APP_REDRAW_NONE; }
    if (key == UI_KEY_2) {
        if (s_view == BULK) s_view = SHELF;
        else if (s_view == READING) {
            s_toolbar = !s_toolbar;
            s_typeset = false;
            invalidate_prep();
        } else if (s_view == TOC && s_text) {
            s_view = READING;
            s_toolbar = false;
        } else {
            // 书库就是主页：中键（home）回书架第一页，已在第一页就整屏刷新。阅读时它开关工具条。
            // The library is home: the home key returns to page one, or refreshes fully when already there.
            if (ctx->leaf != 0) { ctx->leaf = 0; return APP_REDRAW_PAGE; }
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_PAGE;
    }
    if (key != UI_KEY_1 && key != UI_KEY_3) return APP_REDRAW_NONE;
    int dir = key == UI_KEY_1 ? -1 : 1;
    if (s_view == READING) return turn_page(ctx, dir);
    int next = ctx->leaf + dir;
    if (next < 0 || next >= leaves()) return APP_REDRAW_NONE;
    ctx->leaf = next;
    return APP_REDRAW_PAGE;
}
static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (ctx->consumed) return APP_REDRAW_NONE;
    // 定时自动翻页：工具条、插图、确认框开着时暂停，翻到书尾就停，碰一下屏幕再继续。
    // Timed auto turn: paused by the toolbar, images and dialogs; stops at the end until the next touch.
    const bool auto_on = s_view == READING && s_text && !s_toolbar && !s_image_open && !s_clear_confirm &&
                         s_auto_level > 0 && !s_auto_done;
    // 刚开始、或隔了很久才又跑到这里（锁屏、菜单）：从现在重新计时，不立刻翻。
    // Just started, or a long gap since the last tick (lock screen, menu): restart the countdown instead of turning.
    if (auto_on && (!s_auto_was_on || ctx->now_ms - s_auto_tick > 1500)) s_auto_ms = ctx->now_ms;
    s_auto_was_on = auto_on;
    s_auto_tick = ctx->now_ms;
    auto_turn_set_active(auto_on);
    if (auto_on && !(ctx->touch && ctx->touch->touched) && ctx->now_ms - s_auto_ms >= auto_turn_ms(s_auto_level)) {
        s_auto_ms = ctx->now_ms;
        const app_redraw_t r = turn_page(ctx, 1);
        if (r == APP_REDRAW_NONE) { s_auto_done = true; auto_turn_set_active(false); }
        return r;
    }
    if (s_save_failed && ctx->now_ms - s_save_retry_ms >= 15000) {
        s_save_retry_ms = ctx->now_ms;
        bool failed = s_save_failed;
        retry_progress();
        if (failed != s_save_failed) { invalidate_prep(); return APP_REDRAW_PAGE; }
    }
    if (s_scan_pending && ctx->now_ms - s_poll_ms >= 500) {
        s_poll_ms = ctx->now_ms;
        read_pico_sd_info_t info = {0};
        if (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_scan_pending = false;
        scan_shelf(ctx);
        if (s_open_request[0]) {
            char path[BOOK_STORE_PATH_MAX];
            copy_text(path, sizeof(path), s_open_request);
            s_open_request[0] = 0;
            s_resume_pending = false;
            if (open_book(ctx, path)) return APP_REDRAW_FULL;
        }
        if (s_resume_pending) {
            s_resume_pending = false;
            char path[BOOK_STORE_PATH_MAX];
            if (book_progress_last_path(path, sizeof(path))) {
                for (int i = 0; i < s_count; ++i) if (!s_shelf[i].removed && !strcmp(s_shelf[i].path, path)) {
                    copy_text(s_resume_path, sizeof(s_resume_path), path);
                    copy_text(s_resume_name, sizeof(s_resume_name), s_shelf[i].name);
                    break;
                }
            }
        }
        return APP_REDRAW_PAGE;
    }
    // 本页发起的番茄任务：结束了取结果、重扫书架；还在跑就只局刷状态行右边。/ This page's Tomato job: when it ends
    // take the outcome and rescan; while it runs, DU only the right of the status line.
    if (s_tom_op != TOM_NONE && !s_scan_pending && ctx->now_ms - s_tom_poll_ms >= 400 &&
        (s_view == SHELF || s_view == BULK)) {
        s_tom_poll_ms = ctx->now_ms;
        tomato_status_t st;
        tomato_sync_get_status(&st);
        if (!tom_running()) {
            if (tomato_sync_seq() == s_tom_seq && (st.phase == TOMATO_DONE || st.phase == TOMATO_FAILED))
                copy_text(s_tom_note, sizeof(s_tom_note), st.message);
            else s_tom_note[0] = 0;
            s_tom_op = TOM_NONE;
            s_tom_confirm_cancel = s_tom_cancelling = false;
            const int leaf = ctx->leaf;
            scan_shelf(ctx);
            ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
            return APP_REDRAW_PAGE;
        }
        // 下载中字节在涨但 revision 不变，隔一会儿也刷一次。/ While downloading the bytes grow without a revision bump,
        // so refresh every little while too.
        const bool grew = st.phase == TOMATO_DOWNLOADING && st.bytes != s_tom_bytes &&
                          ctx->now_ms - s_tom_draw_ms >= TOM_BYTES_REDRAW_MS;
        if (s_view == SHELF && (st.revision != s_tom_rev || grew) && tomato_hint_shown() &&
            !(ctx->touch && ctx->touch->touched)) {
            s_tom_rev = st.revision;
            s_tom_bytes = st.bytes;
            s_tom_draw_ms = ctx->now_ms;
            render(ctx, ctx->fb);
            s_area = tomato_busy_rect();
            s_mode = MODE_DU;
            return APP_REDRAW_AREA;
        }
    }
    if (s_du_count && (s_du_count >= UI_SETTLE_DU_MAX ||
        ctx->now_ms - s_du_ms >= UI_SETTLE_IDLE_MS)) {
        s_area = s_du_area;
        s_mode = MODE_GL16;
        ESP_LOGI(TAG, "settle du=%u", s_du_count);
        return APP_REDRAW_AREA;
    }
    if (s_text && strcmp(s_font_path, ttf_font_path())) {
        size_t off = book_layout_page_start_offset(s_page);
        save_progress();
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
        if (ok) s_page = book_layout_page_for_offset(off);
        copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
        unlock_draw();
        if (!ok) { free_book(); s_view = SHELF; copy_text(s_message, sizeof(s_message), "字体重排失败，请重新打开图书"); }
        return APP_REDRAW_PAGE;
    }
    if (s_size_settle_ms && ctx->now_ms >= s_size_settle_ms) {
        s_size_settle_ms = 0;
        if (s_view == READING && !s_image_open) return paint_reading(ctx, MODE_GL16);
    }
    if (s_view == READING && !s_image_open && s_text && !book_layout_complete() &&
        !s_toolbar && !(ctx->touch && ctx->touch->touched)) {
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_extend(2);
        unlock_draw();
        if (!ok) {
            free_book(); s_view = SHELF; ctx->leaf = 0;
            copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
            return APP_REDRAW_PAGE;
        }
        // 页数完成后随下次交互更新，避免阅读中重复刷屏。
        // Display the final page count on the next interaction, avoiding unsolicited refreshes.
    }
    // 书架上缺的封面一次生成一张，全部生成完整页重画一次。/ Build missing shelf covers one per tick; redraw once when done.
    if ((s_view == SHELF || s_view == BULK) && !s_toolbar && !(ctx->touch && ctx->touch->touched) && book_cover_tick())
        return APP_REDRAW_PAGE;
    if (s_shake_enabled && s_sensor_on && ctx->now_ms - s_sensor_ms >= 40) {
        s_sensor_ms = ctx->now_ms;
        if (!sc7a20h_powered(ctx->acc)) sensor_set(ctx, true);
        sc7a20h_events_t ev;
        if (sc7a20h_read_events(ctx->acc, &ev) == ESP_OK) {
            bool suppressed = s_view != READING || s_image_open || s_toolbar || s_clear_confirm ||
                              (ctx->touch && ctx->touch->touched) || ctx->now_ms - s_last_turn_ms < 800;
            if (book_shake_feed(&s_shake, ev.aoi2_src & 0x40, suppressed, ctx->now_ms)) return turn_page(ctx, 1);
        }
    }
    return APP_REDRAW_NONE;
}
static EpdRect area_hint(app_ctx_t* ctx) { (void)ctx; return s_area; }

const app_desc_t app_book = {
    .title = "书库", .detail = "TF 卡上的 TXT 与 EPUB", .enter_full = true, .owns_keys = true, .tab_nav = true,
    .render = render, .present = present, .on_enter = on_enter, .on_exit = book_on_exit,
    .on_media_lost = book_on_media_lost, .on_lock = book_on_lock,
    .on_gesture = gesture_event, .on_key = on_key,
    .on_tick = on_tick, .area_hint = area_hint,
};
