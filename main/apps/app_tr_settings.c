/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 设置：首页七个大分类（Wi-Fi、显示与阅读、底部标签、传书、内容来源、同步、电源与设备），点进去是二级页；
 * Wi-Fi 一行打开传书页的选网界面（只管保存的那一个网络，不开传书服务）。
 * 二级页里：字体、壁纸、睡眠、传书、TF 卡、概览等跳到各自页面；底部标签开关、番茄与 WebDAV 的地址在本页用键盘
 * 编辑；内容来源再分番茄、小红书、微博、X 和验证器，小红书、微博和 X 共用一个服务器页（地址、令牌、测试），还能
 * 打开“手机网页填写”，用传书的网页服务收地址和令牌；番茄小说页也有“手机网页填写”，收地址和设备密码；验证器那一行
 * 打开验证器页自己画的设置；“关于 Treader”在本页显示。
 *
 * English: Settings. The home lists seven categories (Wi-Fi, display and reading, tab bar, transfer, content
 * sources, sync, power and device); each opens a second-level page. The Wi-Fi row opens the transfer page's network
 * picker (managing the one saved network, without the transfer service). There: font, wallpaper, sleep, transfer, TF card and
 * overview jump to their own pages; tab switches and the Tomato and WebDAV addresses are edited here with the keyboard;
 * content sources split into Tomato, Xiaohongshu, Weibo, X and the authenticator; Xiaohongshu, Weibo and X share one
 * server page (address, token, test) that can also serve a phone web form (through the transfer web service) for the
 * address and token, and the Tomato page has the same web form for its address and device password; the authenticator row opens the settings view drawn by the authenticator page; "About Treader"
 * shows here.
 *
 * 冻结：书库和设置两个功能不能隐藏，书库固定在底部第一格；网页填写只收当前服务器页的地址和令牌，
 * 离开本页或关掉开关就停；令牌从不显示原值。
 * Frozen: the library and settings features cannot be hidden and the library stays in the first bar cell; the web
 * form takes only the current server page's address and token and stops when the page is left or the switch is
 * turned off; the token is never shown.
 * 修订（2026-11-21，用户要求）：“底部标签”从“开关显示（最多 7 个）”改成“挑哪几个放底部、排顺序、隐藏”；底部最后一格
 * 固定是“全部”，没放在底部的功能在“全部”页里。原因：写死个数不方便，以后加功能也更麻烦。
 * Revision (2026-11-21, user request): "Tab bar" changed from show/hide switches (at most 7) to picking which features
 * sit in the bar, their order, and hiding; the last cell is always "All" and unpinned features live on the All page.
 * Reason: a fixed count was inconvenient and would get worse as features are added.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "app_loop.h"
#include "book_progress.h"
#include "tr_pin.h"
#include "book_source.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "tomato.h"
#include "tr_auth.h"
#include "tr_kbd.h"
#include "tr_lock.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"

#define POLL_MS 500
// 长度都随显示大小放大（TR_S）；表里的 y 是正常档的值，用的时候再换算。/ Lengths scale with the display size
// (TR_S); y values in the tables are normal-size values, scaled where used.
#define CAT_TOP TR_S(170)
#define CAT_H TR_S(136)
#define ROW_H TR_S(72)
#define BACK_RECT (EpdRect){0, TR_S(70), 320, TR_S(90)}

extern const app_desc_t app_font_pick;
extern const app_desc_t app_sleep;
extern const app_desc_t app_transfer;
void app_transfer_open_wifi(void);
extern const app_desc_t app_usb;
extern const app_desc_t app_home;
extern const app_desc_t app_selftest;
extern const app_desc_t app_tr_wall;
extern const app_desc_t app_tr_card;
extern const app_desc_t app_tr_update;
extern const app_desc_t app_tr_auth;
void app_tr_auth_request_settings(void);

extern const char server_form_start[] asm("_binary_server_form_html_start");
extern const char server_form_end[] asm("_binary_server_form_html_end");

typedef enum {
    P_HOME, P_DISPLAY, P_TABS, P_TRANSFER, P_SOURCES, P_SRC_TOMATO, P_SRC_SERVER, P_SRC_HUPU, P_SYNC, P_POWER, P_ABOUT,
    P_TIME, P_TZ, P_TSET, P_TURN
} page_t;

// 自建服务器页：小红书、微博、X 和漫画（WEBTOON）共用，按 s_srv 选。/ Self-hosted server page shared by
// Xiaohongshu, Weibo, X and comics (WEBTOON), picked by s_srv.
typedef enum { S_RED, S_WB, S_X, S_WT, S_COUNT } srv_t;

static const struct {
    const char* name;      // 页面和提示里的称呼 / Name in titles and hints
    const char* panel;     // 服务器面板叫什么 / What the server panel is called
    const char* what;      // 这台服务器给哪个功能用 / The feature this server feeds
    const char* example;   // 地址示例 / Address example
    tr_icon_t icon;
    esp_err_t (*load)(tomato_srv_config_t* out);
    esp_err_t (*save)(const tomato_srv_config_t* cfg);
    uint32_t (*revision)(void);
    int (*list)(void);                 // 列表条数；漫画没有 / List size; comics have none
    esp_err_t (*list_set)(int n);
    const char* unit;                  // 条数的量词 / Counter word for the list size
} SERVERS[S_COUNT] = {
    [S_RED] = {"小红书", "rednote", "", "http://192.168.1.20:8091", TR_ICON_XHS, tomato_red_config_load,
               tomato_red_config_save, tomato_red_config_revision, tomato_red_list_limit, tomato_red_list_limit_set,
               "篇"},
    [S_WB] = {"微博", "weibo", "", "http://192.168.1.20:8092", TR_ICON_WEIBO, tomato_wb_config_load,
              tomato_wb_config_save, tomato_wb_config_revision, tomato_wb_list_limit, tomato_wb_list_limit_set, "条"},
    [S_X] = {"X", "x", "", "http://192.168.1.20:8093", TR_ICON_X, tomato_x_config_load, tomato_x_config_save,
             tomato_x_config_revision, tomato_x_list_limit, tomato_x_list_limit_set, "条"},
    [S_WT] = {"漫画", "webtoon", "WEBTOON 发现与追更", "http://192.168.1.20:8095", TR_ICON_COMIC, tomato_wt_config_load,
              tomato_wt_config_save, tomato_wt_config_revision, NULL, NULL, ""},
};

// 首页分类。/ Home categories.
typedef enum { C_NET, C_DISPLAY, C_TABS, C_WIFI, C_SOURCES, C_SYNC, C_POWER, C_COUNT } cat_t;

// 二级页的行：y 是行顶；section 非空时在上方画分组名。/ Sub-page rows: y is the row top; section draws a group name.
typedef struct {
    int id;
    const char* section;
    const char* label;
    int y;
} row_t;

enum {
    R_SIZE, R_FONT, R_WALL, R_TURN, R_FTURN, R_DEPTH,
    R_SRV, R_STORE, R_WEB, R_URL, R_TOKEN, R_TEST, R_LIST, R_WT_LANG, R_WT_SIMP, R_WT_MAX,
    R_DAV, R_DAV_SYNC,
    R_SLEEP, R_IDLE, R_PIN, R_TIME, R_CARD, R_ABOUT,
    R_TAUTO, R_TZONE, R_TSET,
    R_HIMG, R_HPAGES,
};

static const row_t DISPLAY_ROWS[] = {
    {R_SIZE, NULL, "显示大小", 190},
    {R_FONT, NULL, "字体", 262},
    {R_WALL, NULL, "锁屏壁纸", 334},
    {R_TURN, NULL, "翻页区域", 406},
    {R_FTURN, NULL, "翻页刷新", 478},
    {R_DEPTH, NULL, "文字深浅", 550},
};
// 看书时的翻页区域：名称和一句说明，顺序同 app_turn_zone_t。/ Reader tap zones: name and one-line note, in
// app_turn_zone_t order.
static const char* const TURN_NAMES[APP_TURN_ZONES] = {"左右", "中下翻页", "点哪都翻"};
static const char* const TURN_NOTES[APP_TURN_ZONES] = {
    "点左边上一页，右边下一页，中间开工具条",
    "点上面开工具条；下面左边一条上一页，其余下一页",
    "点上面开工具条，其余都是下一页；上一页用右滑或左键",
};

static const row_t TOMATO_ROWS[] = {
    {R_WEB, NULL, "手机网页填写", 190},
    {R_SRV, NULL, "番茄服务器", 274},
    {R_STORE, NULL, "书城榜单", 346},
};
static const row_t SERVER_ROWS[] = {
    {R_WEB, NULL, "手机网页填写", 190},
    {R_URL, NULL, "服务器地址", 274},
    {R_TOKEN, NULL, "设备令牌", 346},
    {R_TEST, NULL, "测试连接", 418},
    {R_LIST, NULL, "每次拉取", 490},
};
// 漫画服务器页：没有列表条数，换成语言版本、简体显示和每次最多下载几话。/ The comic server page: no list size;
// language version, simplified display and episodes per check instead.
static const row_t WT_ROWS[] = {
    {R_WEB, NULL, "手机网页填写", 190},
    {R_URL, NULL, "服务器地址", 274},
    {R_TOKEN, NULL, "设备令牌", 346},
    {R_TEST, NULL, "测试连接", 418},
    {R_WT_LANG, NULL, "语言版本", 490},
    {R_WT_SIMP, NULL, "简体显示", 562},
    {R_WT_MAX, NULL, "每次最多下载", 634},
};
#define SRV_ROWS (s_srv == S_WT ? WT_ROWS : SERVER_ROWS)
#define SRV_ROW_COUNT (s_srv == S_WT ? COUNT(WT_ROWS) : COUNT(SERVER_ROWS))
static const row_t HUPU_ROWS[] = {
    {R_HIMG, NULL, "显示图片", 190},
    {R_HPAGES, NULL, "关注每个专区", 262},
};
static const row_t SYNC_ROWS[] = {
    {R_WEB, NULL, "手机网页填写", 190},
    {R_DAV, NULL, "WebDAV", 274},
    {R_DAV_SYNC, NULL, "立即同步", 346},
};
static const row_t POWER_ROWS[] = {
    {R_SLEEP, NULL, "睡眠与锁屏", 190},
    {R_IDLE, NULL, "自动锁屏", 262},
    {R_PIN, NULL, "锁屏密码", 334},
    {R_TIME, NULL, "时间与时区", 406},
    {R_CARD, NULL, "TF 卡", 478},
    {R_ABOUT, NULL, "关于 Treader", 550},
};
static const row_t TIME_ROWS[] = {
    {R_TAUTO, NULL, "自动校时", 190},
    {R_TZONE, NULL, "时区", 262},
    {R_TSET, NULL, "手动设置时间", 334},
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define WEB_ROW_H TR_S(84)
#define WEB_BOX_Y TR_S(284)
// 网页填写的二维码不随显示大小放大（和图片一样），扫码不受影响。/ The web-form QR code keeps its size (like
// images); scanning is unaffected.
#define WEB_QR 220
// 内容来源页：番茄、各个自建服务器、虎扑，最后是验证器。/ Content sources page: Tomato, each self-hosted server,
// Hupu, then the authenticator.
#define SOURCES (3 + S_COUNT)
#define SRC_HUPU (1 + S_COUNT)
#define SRC_AUTH (2 + S_COUNT)

// 底部标签页：每个标签一行。/ Tab page: one row per tab.
#define TAB_TOP TR_S(210)   // “底部标签”页第一段的标题 / First section heading on the tab-bar page
#define TAB_H TR_S(70)      // 每行高度 / Row height
#define TAB_SEC_H TR_S(40)  // 段标题高度 / Section heading height
#define TAB_SEC_GAP TR_S(22)
#define TAB_BTN_H TR_S(52)

static page_t s_page;
static int s_open_source = -1;  // 别的页要求直接打开某个来源（tr_tab_t）/ Another page asked for a source (tr_tab_t)
static bool s_open_transfer;  // 书库的传书按钮要求打开“传书”/ The library's transfer button asked for "Transfer"

// 键盘编辑：番茄（地址→密码）、WebDAV（地址→用户名→密码）、榜单、自建服务器地址和令牌。
// Keyboard edits: Tomato (URL, password), WebDAV (URL, user, password), rank site, self-hosted server URL and token.
typedef enum {
    E_NONE = 0, E_SRV_URL, E_SRV_PASS, E_DAV_URL, E_DAV_USER, E_DAV_PASS, E_STORE_URL, E_HOST_URL, E_HOST_TOKEN
} edit_t;

static const struct { const char* title; const char* hint; const char* go; } EDIT[] = {
    [E_NONE] = {"", "", ""},
    [E_SRV_URL] = {"番茄服务器地址", "例如 http://192.168.1.10:18423", "下一步"},
    [E_SRV_PASS] = {"番茄服务器密码", "服务器没有设置密码就留空", "保存"},
    [E_DAV_URL] = {"WebDAV 目录地址", "例如 https://dav.jianguoyun.com/dav/treader", "下一步"},
    [E_DAV_USER] = {"WebDAV 用户名", "坚果云填登录邮箱", "下一步"},
    [E_DAV_PASS] = {"WebDAV 密码", "坚果云填“应用密码”，不是登录密码", "保存"},
    [E_STORE_URL] = {"书城榜单地址", "留空使用默认榜单；填 FanqieRankTracker 网站地址", "保存"},
    // 标题前面加服务器名，地址提示按服务器拼，见 draw_edit。/ The title gets the server name in front and the address
    // hint is built per server, see draw_edit.
    [E_HOST_URL] = {"服务器地址", "", "保存"},
    [E_HOST_TOKEN] = {"设备令牌", "令牌很长，建议用“手机网页填写”；留空表示不改", "保存"},
};

static edit_t s_edit;
static char s_wifi[33];
static tomato_config_t s_cfg;
static tomato_webdav_config_t s_dav;
static tomato_srv_config_t s_host[S_COUNT];
static bool s_has_cfg, s_has_dav, s_has_host[S_COUNT];
static srv_t s_srv;
static char s_message[112];
EXT_RAM_BSS_ATTR static tr_kbd_t s_kbd;
static char s_step1[160], s_step2[64];
static bool s_syncing;
static int64_t s_poll_ms;
static uint32_t s_seen_revision;

// 自建服务器：测试连接的结果和网页填写的状态。/ Self-hosted servers: test results and web-form state.
static bool s_testing;
static srv_t s_test_srv;
static char s_test_result[S_COUNT][96];
static bool s_web_on;
static srv_t s_web_srv;
static uint32_t s_web_revision;
// 网页填写的是番茄（地址和设备密码）而不是 SERVERS 里的服务器。/ The web form serves Tomato (address and device
// password) rather than a server from SERVERS.
static bool s_web_tomato;
static bool s_web_dav;   // 网页填的是 WebDAV（同步页）/ The web form fills WebDAV (the sync page)
// 番茄配置没有修改计数：网页每保存一次在 HTTP 任务里加一，主任务看它变没变。/ Tomato config has no revision: the
// HTTP task bumps this on every web save and the main task watches it.
static volatile uint32_t s_web_tomato_rev;
static volatile uint32_t s_web_dav_rev;
static read_pico_transfer_status_t s_web_status;
static char s_web_note[96];

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

// 本机文件不小于记录时的大小才写回（番茄更新只追加章节）；章节和位置要放得下（文件和备份时一样）。
// Import only if the local file is no smaller (updates only append) and can hold the chapter and offset (same file).
static tomato_import_result_t import_progress(const tomato_progress_t* in) {
    struct stat st;
    if (!in->size || stat(in->path, &st) != 0 || (uint32_t)st.st_size < in->size) return TOMATO_IMPORT_MISSING;
    if (!book_position_fits(in->path, in->chapter, in->off)) return TOMATO_IMPORT_MISMATCH;
    book_progress_t p = {
        .file_size = (uint32_t)st.st_size,
        .chapter = in->chapter,
        .byte_off = in->off,
        .px = in->px >= 36 && in->px <= 72 && (in->px - 36) % 4 == 0 ? in->px : 40,
        .pct = in->pct > 100 ? 100 : in->pct,
    };
    // 导入不改最近阅读顺序。/ Importing must not reorder "recently read".
    return book_progress_import(in->path, &p) == ESP_OK ? TOMATO_IMPORT_OK : TOMATO_IMPORT_MISSING;
}

/* ---- 数据 / Data ---- */

static void load(void) {
    bool configured = false;
    if (read_pico_transfer_get_saved_wifi(s_wifi, &configured) != ESP_OK || !configured) s_wifi[0] = 0;
    s_has_cfg = tomato_config_load(&s_cfg) == ESP_OK;
    // 内置服务器的地址和密码不留在页面里，只记“在用内置”。/ Keep no copy of the built-in URL or password, only the flag.
    if (s_cfg.builtin) {
        memset(s_cfg.url, 0, sizeof(s_cfg.url));
        memset(s_cfg.password, 0, sizeof(s_cfg.password));
    }
    s_has_dav = tomato_webdav_config_load(&s_dav) == ESP_OK;
    for (int i = 0; i < S_COUNT; i++) s_has_host[i] = SERVERS[i].load(&s_host[i]) == ESP_OK;
}

// 去掉 http:// 只显示主机。/ Drop the scheme and show the host only.
static const char* host_of(const char* url) {
    const char* p = strstr(url, "://");
    return p ? p + 3 : url;
}

static void font_name(char* out, size_t cap) {
    snprintf(out, cap, "%s", ttf_font_is_builtin() ? "内置字体" : ttf_font_display_name());
}

static void wall_name(char* out, size_t cap) {
    char path[288];
    const char* base = tr_lock_wallpaper_get(path, sizeof(path)) ? strrchr(path, '/') : NULL;
    snprintf(out, cap, "%.100s", base ? base + 1 : "书卡样式");
}

static void card_value(char* out, size_t cap) {
    read_pico_sd_info_t info = {0};
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_OK && info.mounted) snprintf(out, cap, "可用 %.1f GB", info.free_bytes / 1073741824.0);
    else snprintf(out, cap, !info.present ? "没有插卡" : "需要处理");
}

static void row_value(int id, char* out, size_t cap) {
    char tmp[120];
    switch (id) {
    case R_SIZE: snprintf(out, cap, "%s ›", app_settings_display_label(app_settings_display_size())); break;
    case R_FONT: font_name(tmp, sizeof(tmp)); snprintf(out, cap, "%s ›", tmp); break;
    case R_WALL: wall_name(tmp, sizeof(tmp)); snprintf(out, cap, "%s ›", tmp); break;
    case R_TURN: snprintf(out, cap, "%s ›", TURN_NAMES[app_settings_turn_zone() % APP_TURN_ZONES]); break;
    case R_FTURN: snprintf(out, cap, "%s ›", app_settings_fast_turn() ? "快速" : "正常"); break;
    case R_DEPTH: {
        static const char* const NAMES[TR_TEXT_DEPTHS] = {"标准", "深", "更深"};
        snprintf(out, cap, "%s ›", NAMES[app_settings_text_depth() % TR_TEXT_DEPTHS]);
        break;
    }
    case R_SRV: snprintf(out, cap, "%s ›", !s_has_cfg ? "未设置" : s_cfg.builtin ? "内置服务器" : host_of(s_cfg.url)); break;
    case R_STORE: {
        char url[160];
        snprintf(out, cap, "%.150s ›", tomato_store_url_load(url, sizeof(url)) == ESP_OK ? host_of(url) : "默认榜单");
        break;
    }
    case R_URL: snprintf(out, cap, "%.150s ›", s_has_host[s_srv] ? host_of(s_host[s_srv].url) : "未设置"); break;
    case R_TOKEN: snprintf(out, cap, "%s ›", s_host[s_srv].token[0] ? "已填写" : "未填写"); break;
    case R_TEST:
        if (s_testing && s_test_srv == s_srv) snprintf(out, cap, "测试中…");
        else if (s_test_result[s_srv][0]) snprintf(out, cap, "%.90s ›", s_test_result[s_srv]);
        else snprintf(out, cap, s_has_host[s_srv] ? "点一下测试 ›" : "先填地址");
        break;
    case R_LIST: snprintf(out, cap, "%d %s ›", SERVERS[s_srv].list(), SERVERS[s_srv].unit); break;
    case R_WT_LANG: snprintf(out, cap, "%s ›", tomato_wt_lang_name(tomato_wt_lang())); break;
    case R_WT_SIMP: snprintf(out, cap, "%s ›", tomato_wt_simp() ? "开" : "关"); break;
    case R_WT_MAX: snprintf(out, cap, "%d 话 ›", tomato_wt_max()); break;
    case R_HPAGES: snprintf(out, cap, "%d 页 ›", app_settings_hupu_pages()); break;
    case R_DAV: snprintf(out, cap, "%.150s ›", s_has_dav ? host_of(s_dav.url) : "未设置"); break;
    case R_DAV_SYNC: {
        tomato_status_t st;
        tomato_sync_get_status(&st);
        if (s_syncing) snprintf(out, cap, "%s…", st.phase == TOMATO_CONNECTING ? "连接 Wi-Fi" : "同步中");
        else snprintf(out, cap, s_has_dav ? "进度和番茄书架 ›" : "先设置 WebDAV");
        break;
    }
    case R_SLEEP: snprintf(out, cap, "›"); break;
    case R_IDLE: snprintf(out, cap, "%s ›", app_settings_lock_idle_label(app_settings_lock_idle())); break;
    case R_PIN: snprintf(out, cap, "%s ›", app_settings_pin_enabled() ? "已开启" : "关闭"); break;
    case R_TIME: {
        char clock[24] = "未设置";
        const time_t now = time(NULL);
        if (tr_time_valid()) {
            struct tm tm;
            localtime_r(&now, &tm);
            snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100);
        }
        snprintf(out, cap, "%s · %s ›", clock, app_settings_tz_offset(app_settings_tz()));
        break;
    }
    case R_TZONE:
        snprintf(out, cap, "%s %s ›", app_settings_tz_name(app_settings_tz()), app_settings_tz_offset(app_settings_tz()));
        break;
    case R_TSET: {
        if (app_settings_time_auto()) { snprintf(out, cap, "关掉自动校时后可调"); break; }
        if (!tr_time_valid()) { snprintf(out, cap, "未设置 ›"); break; }
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d ›", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100,
                 tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100);
        break;
    }
    case R_CARD: card_value(tmp, sizeof(tmp)); snprintf(out, cap, "%s ›", tmp); break;
    case R_ABOUT: snprintf(out, cap, "版本、开源致谢 ›"); break;
    default: out[0] = 0; break;
    }
}

/* ---- 网页填写 / Web form ---- */

// HTTP 任务里调用：GET 返回地址和“有没有令牌”，POST 保存。令牌从不返回。
// Runs on the HTTP task: GET returns the address and whether a token exists; POST saves. The token never goes out.
static int web_form_tomato(const char* body, size_t len, char* reply, size_t cap) {
    tomato_config_t cur;
    const bool has = tomato_config_load(&cur) == ESP_OK && !cur.builtin;
    if (!body) {
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", "番茄小说");
        cJSON_AddStringToObject(j, "example", "http://192.168.1.10:18423");
        cJSON_AddStringToObject(j, "hint", "地址和设备密码在番茄服务器面板 → 设备与外网，复制后粘贴到这里。");
        cJSON_AddStringToObject(j, "secret", "设备密码");
        cJSON_AddStringToObject(j, "secret_ph", "粘贴设备密码");
        cJSON_AddStringToObject(j, "secret_hint", "面板上的设备密码；服务器没设密码就留空。");
        cJSON_AddStringToObject(j, "secret_kept", "已经填过密码，不显示原值；留空表示不改。");
        cJSON_AddStringToObject(j, "saved", "已保存。回到设备上就能用番茄小说和书城了。");
        cJSON_AddBoolToObject(j, "optional", true);
        // 内置服务器不回显。/ Never echo the built-in server.
        cJSON_AddStringToObject(j, "url", has ? cur.url : "");
        cJSON_AddBoolToObject(j, "has_token", has && cur.password[0] != 0);
        memset(&cur, 0, sizeof(cur));
        char* text = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        snprintf(reply, cap, "%s", text ? text : "{}");
        cJSON_free(text);
        return 200;
    }
    cJSON* j = cJSON_ParseWithLength(body, len);
    const cJSON* url = cJSON_GetObjectItemCaseSensitive(j, "url");
    const cJSON* pass = cJSON_GetObjectItemCaseSensitive(j, "token");
    int status = 400;
    if (!cJSON_IsString(url) || (strncmp(url->valuestring, "http://", 7) && strncmp(url->valuestring, "https://", 8)) ||
        strlen(url->valuestring) >= sizeof(cur.url) - 1) {
        snprintf(reply, cap, "{\"error\":\"地址要以 http:// 或 https:// 开头\"}");
    } else if (cJSON_IsString(pass) && strlen(pass->valuestring) >= sizeof(cur.password) - 1) {
        snprintf(reply, cap, "{\"error\":\"密码太长\"}");
    } else {
        // 密码留空：有自填的旧密码就沿用，没有就是不设密码。/ A blank password keeps an earlier self-hosted one, or
        // means no password.
        tomato_config_t next = {0};
        snprintf(next.url, sizeof(next.url), "%s", url->valuestring);
        snprintf(next.password, sizeof(next.password), "%s",
                 cJSON_IsString(pass) && pass->valuestring[0] ? pass->valuestring : has ? cur.password : "");
        const esp_err_t err = tomato_config_save(&next);
        memset(&next, 0, sizeof(next));
        status = err == ESP_OK ? 200 : 500;
        snprintf(reply, cap, err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"保存失败\"}");
        if (err == ESP_OK) s_web_tomato_rev++;
    }
    if (cJSON_IsString(pass)) memset(pass->valuestring, 0, strlen(pass->valuestring));
    cJSON_Delete(j);
    memset(&cur, 0, sizeof(cur));
    return status;
}

// WebDAV：地址、用户名、密码三项。GET 返回地址和用户名、“有没有密码”；POST 保存。密码从不返回，留空表示不改。
// WebDAV: address, user name and password. GET returns the address, the user name and whether a password exists; POST
// saves. The password never goes out, and a blank one keeps the old.
static int web_form_dav(const char* body, size_t len, char* reply, size_t cap) {
    tomato_webdav_config_t cur = {0};
    const bool has = tomato_webdav_config_load(&cur) == ESP_OK;
    if (!body) {
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", "WebDAV");
        cJSON_AddStringToObject(j, "example", "https://dav.jianguoyun.com/dav/treader");
        cJSON_AddStringToObject(j, "hint", "WebDAV 用来同步阅读进度和番茄书架。地址是同步文件夹的完整地址，要事先建好。");
        cJSON_AddStringToObject(j, "url_label", "WebDAV 目录地址");
        cJSON_AddStringToObject(j, "url_hint", "坚果云是 https://dav.jianguoyun.com/dav/文件夹名");
        cJSON_AddStringToObject(j, "user_label", "用户名");
        cJSON_AddStringToObject(j, "user_ph", "坚果云填登录邮箱");
        cJSON_AddStringToObject(j, "user", has ? cur.user : "");
        cJSON_AddStringToObject(j, "secret", "密码");
        cJSON_AddStringToObject(j, "secret_ph", "粘贴密码");
        cJSON_AddStringToObject(j, "secret_hint", "坚果云填“应用密码”，不是登录密码。服务器不要密码就留空。");
        cJSON_AddStringToObject(j, "secret_kept", "已经填过密码，不显示原值；留空表示不改。");
        cJSON_AddStringToObject(j, "saved", "已保存。回到设备上点“立即同步”试试。");
        cJSON_AddBoolToObject(j, "optional", true);
        cJSON_AddStringToObject(j, "url", has ? cur.url : "");
        cJSON_AddBoolToObject(j, "has_token", has && cur.password[0] != 0);
        memset(&cur, 0, sizeof(cur));
        char* text = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        snprintf(reply, cap, "%s", text ? text : "{}");
        cJSON_free(text);
        return 200;
    }
    cJSON* j = cJSON_ParseWithLength(body, len);
    const cJSON* url = cJSON_GetObjectItemCaseSensitive(j, "url");
    const cJSON* user = cJSON_GetObjectItemCaseSensitive(j, "user");
    const cJSON* pass = cJSON_GetObjectItemCaseSensitive(j, "token");
    int status = 400;
    if (!cJSON_IsString(url) || (strncmp(url->valuestring, "http://", 7) && strncmp(url->valuestring, "https://", 8)) ||
        strlen(url->valuestring) >= sizeof(cur.url) - 1) {
        snprintf(reply, cap, "{\"error\":\"地址要以 http:// 或 https:// 开头\"}");
    } else if (cJSON_IsString(user) && strlen(user->valuestring) >= sizeof(cur.user) - 1) {
        snprintf(reply, cap, "{\"error\":\"用户名太长\"}");
    } else if (cJSON_IsString(pass) && strlen(pass->valuestring) >= sizeof(cur.password) - 1) {
        snprintf(reply, cap, "{\"error\":\"密码太长\"}");
    } else {
        tomato_webdav_config_t next = {0};
        snprintf(next.url, sizeof(next.url), "%s", url->valuestring);
        snprintf(next.user, sizeof(next.user), "%s", cJSON_IsString(user) ? user->valuestring : "");
        // 密码留空：沿用旧的。/ A blank password keeps the old one.
        snprintf(next.password, sizeof(next.password), "%s",
                 cJSON_IsString(pass) && pass->valuestring[0] ? pass->valuestring : has ? cur.password : "");
        const esp_err_t err = tomato_webdav_config_save(&next);
        memset(&next, 0, sizeof(next));
        status = err == ESP_OK ? 200 : 500;
        snprintf(reply, cap, err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"保存失败\"}");
        if (err == ESP_OK) s_web_dav_rev++;
    }
    if (cJSON_IsString(pass)) memset(pass->valuestring, 0, strlen(pass->valuestring));
    cJSON_Delete(j);
    memset(&cur, 0, sizeof(cur));
    return status;
}

static int web_form(const char* body, size_t len, char* reply, size_t cap) {
    if (s_web_dav) return web_form_dav(body, len, reply, cap);
    if (s_web_tomato) return web_form_tomato(body, len, reply, cap);
    tomato_srv_config_t cur;
    const bool has = SERVERS[s_web_srv].load(&cur) == ESP_OK;
    if (!body) {
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", SERVERS[s_web_srv].name);
        cJSON_AddStringToObject(j, "panel", SERVERS[s_web_srv].panel);
        cJSON_AddStringToObject(j, "example", SERVERS[s_web_srv].example);
        cJSON_AddStringToObject(j, "url", has ? cur.url : "");
        cJSON_AddBoolToObject(j, "has_token", cur.token[0] != 0);
        memset(&cur, 0, sizeof(cur));
        char* text = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        snprintf(reply, cap, "%s", text ? text : "{}");
        cJSON_free(text);
        return 200;
    }
    cJSON* j = cJSON_ParseWithLength(body, len);
    const cJSON* url = cJSON_GetObjectItemCaseSensitive(j, "url");
    const cJSON* token = cJSON_GetObjectItemCaseSensitive(j, "token");
    int status = 400;
    if (!cJSON_IsString(url) || (strncmp(url->valuestring, "http://", 7) && strncmp(url->valuestring, "https://", 8)) ||
        strlen(url->valuestring) >= sizeof(cur.url) - 1) {
        snprintf(reply, cap, "{\"error\":\"地址要以 http:// 或 https:// 开头\"}");
    } else if (cJSON_IsString(token) && strlen(token->valuestring) >= sizeof(cur.token) - 1) {
        snprintf(reply, cap, "{\"error\":\"令牌太长\"}");
    } else if (!(cJSON_IsString(token) && token->valuestring[0]) && !cur.token[0]) {
        snprintf(reply, cap, "{\"error\":\"请粘贴设备令牌\"}");
    } else {
        tomato_srv_config_t next = {0};
        snprintf(next.url, sizeof(next.url), "%s", url->valuestring);
        snprintf(next.token, sizeof(next.token), "%s",
                 cJSON_IsString(token) && token->valuestring[0] ? token->valuestring : cur.token);
        const esp_err_t err = SERVERS[s_web_srv].save(&next);
        memset(&next, 0, sizeof(next));
        status = err == ESP_OK ? 200 : 500;
        snprintf(reply, cap, err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"保存失败\"}");
    }
    if (cJSON_IsString(token)) memset(token->valuestring, 0, strlen(token->valuestring));
    cJSON_Delete(j);
    memset(&cur, 0, sizeof(cur));
    return status;
}

// 前台优先：能用就返回真；否则在 s_message 写原因。/ Foreground first: true when the worker is free; otherwise
// s_message says why.
static bool make_room(void) {
    const tomato_preempt_t r = tomato_sync_preempt();
    if (r == TOMATO_PREEMPT_FREE) return true;
    snprintf(s_message, sizeof(s_message), r == TOMATO_PREEMPT_STOPPING ? "正在停下后台加载，请再点一次"
                                                                         : "后台正在下载或同步，完成后再试");
    return false;
}

static void web_stop(void) {
    if (!s_web_on) return;
    read_pico_transfer_stop();
    ui_wifi_qr_clear();
    s_web_on = false;
    app_loop_hold_awake(false);
}

// 当前网页填写对应的那份设置的修订号。/ The revision of the settings the web form is filling.
static uint32_t web_rev(void) {
    return s_web_dav ? s_web_dav_rev : s_web_tomato ? s_web_tomato_rev : SERVERS[s_web_srv].revision();
}

static void web_start(void) {
    if (!make_room()) return;
    bool configured = false;
    char ssid[33];
    read_pico_transfer_get_saved_wifi(ssid, &configured);
    const read_pico_transfer_cfg_t cfg = {
        .mode = configured ? READ_PICO_TRANSFER_MODE_STA : READ_PICO_TRANSFER_MODE_AP,
        .form_html = server_form_start,
        .form_html_len = (size_t)(server_form_end - server_form_start - 1),
        .form_cb = web_form,
    };
    s_web_srv = s_srv;   // 回调在 HTTP 任务里读它，开着期间不变 / Read by the HTTP task; fixed while serving
    s_web_tomato = s_page == P_SRC_TOMATO;
    s_web_dav = s_page == P_SYNC;
    // 后台任务做完还留着的 Wi-Fi 先断开，热点才开得起来。/ Drop Wi-Fi lingering after a background job so the hotspot can start.
    tomato_net_idle_off();
    const esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) {
        snprintf(s_message, sizeof(s_message), "打不开网页服务（%s）", esp_err_to_name(err));
        return;
    }
    s_web_on = true;
    s_web_revision = web_rev();
    s_web_note[0] = 0;
    memset(&s_web_status, 0, sizeof(s_web_status));
    read_pico_transfer_get_status(&s_web_status);
    app_loop_hold_awake(true);
}

static void web_prepare_qr(void) {
    if (!s_web_status.network_ready) return;
    if (s_web_status.mode == READ_PICO_TRANSFER_MODE_AP)
        ui_wifi_qr_prepare(s_web_status.ssid, READ_PICO_TRANSFER_PASSWORD);
    else
        ui_wifi_qr_prepare_url(s_web_status.url);
}

static void start_test(srv_t which) {
    if (!s_has_host[which] || s_web_on || tomato_sync_preempt() != TOMATO_PREEMPT_FREE) return;
    esp_err_t err;
    if (which == S_RED) {
        const tomato_red_req_t req = {.job = TOMATO_RED_TEST};
        err = tomato_sync_start_red(&req);
    } else if (which == S_WB) {
        const tomato_wb_req_t req = {.job = TOMATO_WB_TEST};
        err = tomato_sync_start_wb(&req);
    } else if (which == S_X) {
        const tomato_x_req_t req = {.job = TOMATO_X_TEST};
        err = tomato_sync_start_x(&req);
    } else {
        const tomato_wt_req_t req = {.job = TOMATO_WT_TEST};
        err = tomato_sync_start_wt(&req);
    }
    if (err == ESP_OK) {
        s_testing = true;
        s_test_srv = which;
        s_test_result[which][0] = 0;
    }
}

/* ---- 绘制 / Drawing ---- */

static void draw_back_title(uint8_t* fb, const char* title) {
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), title, EPD_DRAW_ALIGN_LEFT, true);
}

// 提示放在标题右边；标题随显示大小变宽，提示的宽度跟着让。/ The message sits right of the title; as the title
// grows with the display size, the message width yields.
static void draw_message(uint8_t* fb) {
    if (s_message[0])
        tr_message(fb, TR_W - TR_MARGIN, TR_S(104), TR_S(92), TR_W - TR_MARGIN - TR_S(284), TR_S(26), s_message,
                   EPD_DRAW_ALIGN_RIGHT, false);
}

static void draw_cat(uint8_t* fb, int y, int h, tr_icon_t icon, const char* name, const char* summary, bool last);

// 首页分类行高：放不下时压低行高并省掉版本号（关于页里有）。/ Home category row height: when the rows do not
// fit, rows get shorter and the version line is dropped (About shows it).
static int home_cat_h(void) {
    if (CAT_TOP + C_COUNT * CAT_H <= TR_TABS_Y - TR_S(84)) return CAT_H;
    return (TR_TABS_Y - TR_S(8) - CAT_TOP) / C_COUNT;
}

// 内容来源页行高：放不下就压低。/ Content-source row height, shortened when the rows do not fit.
static int source_h(void) {
    if (CAT_TOP + SOURCES * CAT_H <= TR_TABS_Y - TR_S(8)) return CAT_H;
    return (TR_TABS_Y - TR_S(8) - CAT_TOP) / SOURCES;
}

// 验证器的状态一句话。/ The authenticator state in a few words.
static const char* auth_state_text(void) {
    switch (tr_auth_state()) {
    case TR_AUTH_UNSET: return "未设置";
    case TR_AUTH_LOCKED: return "已上锁";
    case TR_AUTH_OPEN: return "已解锁";
    case TR_AUTH_NOCARD: return "没有 TF 卡";
    default: return "需要处理";
    }
}

static void cat_summary(cat_t c, char* out, size_t cap) {
    char a[120], b[120];
    switch (c) {
    case C_DISPLAY:
        font_name(a, sizeof(a));
        wall_name(b, sizeof(b));
        snprintf(out, cap, "显示大小 %s · %.60s · 锁屏壁纸 %.50s",
                 app_settings_display_label(app_settings_display_size()), a, b);
        break;
    case C_TABS: {
        tr_tab_t bar[TR_TAB_MAX];
        const int n = tr_tab_bar_tabs(bar);
        int len = snprintf(out, cap, "底部：");
        for (int i = 0; i < n && len < (int)cap; i++)
            len += snprintf(out + len, cap - (size_t)len, "%s%s", i ? "、" : "", tr_tab_label(bar[i]));
        break;
    }
    case C_NET:
        if (s_wifi[0]) snprintf(out, cap, "已保存：%s · 只能保存一个", s_wifi);
        else snprintf(out, cap, "未保存网络 · 只能保存一个");
        break;
    case C_WIFI: snprintf(out, cap, "Wi-Fi 传书 · USB 传书"); break;
    case C_SOURCES:
        snprintf(out, cap, "番茄%s · 小红书%s · 微博%s · X%s · 漫画%s · 验证器%s",
                 !s_has_cfg ? "未设置" : s_cfg.builtin ? "内置" : "已设置", s_has_host[S_RED] ? "已设置" : "未设置",
                 s_has_host[S_WB] ? "已设置" : "未设置", s_has_host[S_X] ? "已设置" : "未设置",
                 s_has_host[S_WT] ? "已设置" : "未设置", auth_state_text());
        break;
    case C_SYNC: snprintf(out, cap, "%.100s", s_has_dav ? host_of(s_dav.url) : "WebDAV 未设置"); break;
    case C_POWER:
        card_value(a, sizeof(a));
        snprintf(out, cap, "自动锁屏 %s%s · TF 卡%s", app_settings_lock_idle_label(app_settings_lock_idle()),
                 app_settings_pin_enabled() ? " · 有密码" : "", a);
        break;
    default: out[0] = 0; break;
    }
}

static void draw_home(uint8_t* fb) {
    static const char* const NAMES[C_COUNT] = {"Wi-Fi", "显示与阅读", "底部标签", "传书", "内容来源", "同步", "电源与设备"};
    static const tr_icon_t ICONS[C_COUNT] = {TR_ICON_WIFI, TR_ICON_FONT, TR_ICON_TABS, TR_ICON_IMPORT, TR_ICON_SOURCE,
                                             TR_ICON_SYNC, TR_ICON_POWER};
    tr_title(fb, "设置");
    draw_message(fb);
    const int h = home_cat_h();
    for (int i = 0; i < C_COUNT; i++) {
        char sum[200];
        cat_summary((cat_t)i, sum, sizeof(sum));
        draw_cat(fb, CAT_TOP + i * h, h, ICONS[i], NAMES[i], sum, i + 1 == C_COUNT);
    }
    if (h < CAT_H) return;
    const esp_app_desc_t* app = esp_app_get_description();
    char ver[48];
    snprintf(ver, sizeof(ver), "Treader %.20s", app->version);
    tr_text_dim(fb, TR_W / 2, TR_TABS_Y - TR_S(64), TR_S(24), ver, EPD_DRAW_ALIGN_CENTER);
}

// 一行大分类（首页和传书页共用）。h 比标准行矮时内容整体上移居中。/ One large category row (home and the
// transfer page share it). When h is below the standard height the content moves up to stay centred.
static void draw_cat(uint8_t* fb, int y, int h, tr_icon_t icon, const char* name, const char* summary, bool last) {
    // Material 3 列表项：图标放在浅灰圆底上，摘要和箭头用深灰（这几页整页刷新）。/ Material 3 list item: the icon
    // on a light gray circle, summary and chevron in dark gray (these pages refresh as whole pages).
    const int top = y + (h - CAT_H) / 2;
    const EpdRect box = {TR_MARGIN, top + TR_S(30), TR_S(76), TR_S(76)};
    ui_fill_round_rect(fb, box, TR_S(38), TR_GRAY_SURFACE);
    tr_icon_on(fb, icon, box.x + TR_S(38), box.y + TR_S(38), UI_GRAY_BLACK, TR_GRAY_SURFACE);
    const int tx = TR_MARGIN + TR_S(104);
    tr_text(fb, tx, top + TR_S(24), TR_S(34), name, EPD_DRAW_ALIGN_LEFT, true);
    char sum[200];
    snprintf(sum, sizeof(sum), "%s", summary);
    tr_fit(sum, TR_S(26), TR_W - TR_MARGIN - tx - TR_S(40));
    tr_text_dim(fb, tx, top + TR_S(78), TR_S(26), sum, EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(fb, TR_W - TR_MARGIN, top + TR_S(46), TR_S(34), "›", EPD_DRAW_ALIGN_RIGHT);
    if (!last) tr_hline(fb, y + h - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static void draw_transfer(uint8_t* fb) {
    draw_back_title(fb, "传书");
    draw_message(fb);
    char wifi[80];
    snprintf(wifi, sizeof(wifi), "手机或电脑浏览器上传%s%s", s_wifi[0] ? " · " : "", s_wifi);
    draw_cat(fb, CAT_TOP, CAT_H, TR_ICON_WIFI, "Wi-Fi 传书", wifi, false);
    draw_cat(fb, CAT_TOP + CAT_H, CAT_H, TR_ICON_USB, "USB 传书",
             tomato_sync_busy() && !tomato_sync_preemptible() ? "后台正在下载或同步，完成后再用" : "连电脑当 U 盘，拷大文件更方便", true);
    tr_wrap(fb, TR_MARGIN, CAT_TOP + 2 * CAT_H + TR_S(28), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36),
            "Wi-Fi 传书一边传一边能用设备，只收书、字体和漫画；USB 传书整张卡都能管理，但传输期间设备暂停，结束后重启恢复。", 4);
}

// Material 3 开关：开是黑轨道加白圆钮；关是深灰描边轨道加小一圈的深灰圆钮。深灰在 DU 下按黑处理。
// Material 3 switch: on is a black track with a white knob; off is an outlined dark gray track with a smaller dark
// gray knob. Dark gray reads as black under DU.
static void draw_switch(uint8_t* fb, int right, int y, bool on) {
    const int w = TR_S(100), h = TR_S(54), rad = h / 2;
    const EpdRect r = {right - w, y, w, h};
    if (on) {
        ui_fill_round_rect(fb, r, rad, UI_GRAY_BLACK);
        epd_fill_circle(r.x + r.width - rad, y + rad, TR_S(19), UI_GRAY_WHITE, fb);
    } else {
        ui_draw_round_rect(fb, r, rad, TR_GRAY_OUTLINE);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), rad - 1, TR_GRAY_OUTLINE);
        epd_fill_circle(r.x + rad, y + rad, TR_S(13), TR_GRAY_OUTLINE, fb);
    }
}

static int row_h(const row_t* r) { return r->id == R_WEB ? WEB_ROW_H : ROW_H; }

static void draw_rows(uint8_t* fb, const row_t* rows, int n) {
    for (int i = 0; i < n; i++) {
        const row_t* r = &rows[i];
        const int y = TR_S(r->y), h = row_h(r);
        if (r->section) tr_text_dim(fb, TR_MARGIN, y - TR_S(36), TR_S(26), r->section, EPD_DRAW_ALIGN_LEFT);
        tr_text(fb, TR_MARGIN, y + (h - TR_S(32)) / 2 - TR_S(2), TR_S(32), r->label, EPD_DRAW_ALIGN_LEFT,
                r->id == R_WEB && s_web_on);
        if (r->id == R_WEB) {
            draw_switch(fb, TR_W - TR_MARGIN, y + (h - TR_S(54)) / 2, s_web_on);
        } else if (r->id == R_TAUTO) {
            draw_switch(fb, TR_W - TR_MARGIN, y + (h - TR_S(54)) / 2, app_settings_time_auto());
        } else if (r->id == R_HIMG) {
            draw_switch(fb, TR_W - TR_MARGIN, y + (h - TR_S(54)) / 2, app_settings_hupu_images());
        } else {
            // 右边的值让开左边的名字（正常档至少留 220）。/ The value keeps clear of the label (220 at least).
            char value[160];
            row_value(r->id, value, sizeof(value));
            int label = ttf_text_width_px(TR_S(32), r->label) + TR_S(28);
            if (label < 220) label = 220;
            tr_fit(value, TR_S(28), TR_W - 2 * TR_MARGIN - label);
            tr_text_dim(fb, TR_W - TR_MARGIN, y + (h - TR_S(28)) / 2, TR_S(28), value, EPD_DRAW_ALIGN_RIGHT);
        }
        const bool last = i + 1 == n || rows[i + 1].section;
        if (!last && !(r->id == R_WEB && s_web_on)) tr_hline(fb, y + h - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
}

// 网页填写框：二维码左边，说明和网址在右边；说明折行变多时网址和下半部分往下让，框跟着变高。
// Web-form box: QR on the left, instructions and address on the right; when the instructions wrap to more lines the
// address and the lower half move down and the box grows with them.
static void draw_web_box(uint8_t* fb) {
    const int bx = TR_MARGIN, by = WEB_BOX_Y, bw = TR_W - 2 * TR_MARGIN;
    const int pad = TR_S(32);
    // 正常档照旧最多 3 / 4 行；放大后列变窄，给到 6 行。/ Normal size keeps 3 / 4 lines; larger sizes get 6.
    const int more = ui_scale_pct == 100 ? 0 : 6;
    int sep = by + TR_S(270);
    char line[160];
    if (!s_web_status.network_ready) {
        tr_wrap(fb, bx + pad, by + TR_S(40), bw - 2 * pad, TR_S(28), TR_S(42),
                s_web_status.state == READ_PICO_TRANSFER_ERROR ? "连不上 Wi-Fi。检查设置 › Wi-Fi 里保存的网络，或关掉开关再试。"
                                                               : "正在打开网页服务…", more ? more : 3);
    } else {
        const EpdRect qr = {bx + TR_S(28), by + TR_S(28), WEB_QR, WEB_QR};
        ui_wifi_qr_draw(fb, qr);
        const int tx = qr.x + qr.width + TR_S(20), tw = bx + bw - pad - tx, ty = by + TR_S(36);
        const bool sta = s_web_status.mode == READ_PICO_TRANSFER_MODE_STA;
        if (sta) snprintf(line, sizeof(line), "手机连上同一个 Wi-Fi（%s），扫码或在浏览器打开：", s_web_status.wifi_ssid);
        else snprintf(line, sizeof(line), "扫码连上设备热点 %s（口令 %s），再在浏览器打开：", s_web_status.ssid,
                      READ_PICO_TRANSFER_PASSWORD);
        const int n = tr_wrap(fb, tx, ty, tw, TR_S(26), TR_S(38), line, more ? more : sta ? 3 : 4);
        int uy = by + TR_S(sta ? 168 : 196);
        if (uy < ty + n * TR_S(38) + TR_S(8)) uy = ty + n * TR_S(38) + TR_S(8);
        // 网址是内容：放不下就把字号缩到放得下。/ The address is content: shrink its size until it fits.
        const char* host = host_of(sta ? s_web_status.url : READ_PICO_TRANSFER_URL);
        int px = TR_S(32);
        while (px > 24 && ttf_text_width_px(px, host) > tw) px -= 2;
        tr_text(fb, tx, uy, px, host, EPD_DRAW_ALIGN_LEFT, true);
        if (sep < uy + TR_S(56)) sep = uy + TR_S(56);
        if (sep < qr.y + qr.height + TR_S(22)) sep = qr.y + qr.height + TR_S(22);
    }
    tr_hline(fb, sep, bx + pad, bw - 2 * pad, true);
    tr_text(fb, bx + pad, sep + TR_S(20), TR_S(28), s_web_note[0] ? s_web_note : "等待网页提交…", EPD_DRAW_ALIGN_LEFT,
            false);
    const int n = tr_wrap(fb, bx + pad, sep + TR_S(72), bw - 2 * pad, TR_S(24), TR_S(36),
                          s_web_status.mode == READ_PICO_TRANSFER_MODE_AP
                              ? "设备没有保存 Wi-Fi，开的是自己的热点；提交后要连回家里网络才能测试。关掉开关或离开这一页就停止。"
                              : s_web_tomato || s_web_dav ? "提交后自动保存。关掉开关或离开这一页就停止，网页随之失效。"
                                             : "提交后自动保存并测试连接。关掉开关或离开这一页就停止，网页随之失效。",
                          more ? more : 3);
    // 框高：正常档固定 480；放大后包住实际内容。/ Box height: 480 at normal size; larger sizes wrap the content.
    int bh = TR_S(480);
    if (more) bh = sep + TR_S(72) + n * TR_S(36) + TR_S(30) - by;
    const EpdRect b = {bx, by, bw, bh};
    ui_draw_round_rect(fb, b, TR_S(24), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(b, 1), TR_S(24) - 1, TR_GRAY_OUTLINE);
}

// “底部标签”页的行：书库和放在底部的（按顺序）、只在“全部”里的、隐藏的三段。绘制和点击都用这一份排版。
// Rows on the tab-bar page in three sections: the library and pinned features (in order), those only on the All
// page, and hidden ones. Drawing and taps share this one layout.
typedef enum { TB_PINNED, TB_REST, TB_HIDDEN } tb_sec_t;
typedef enum { TB_UP, TB_DOWN, TB_OUT, TB_IN, TB_HIDE, TB_SHOW, TB_BTN_COUNT } tb_btn_t;
typedef struct {
    tr_tab_t tab;
    tb_sec_t sec;
    int index;   // 在常用里的位置（书库是 -1）/ Position among the pins (-1 for the library)
    int y;
} tb_row_t;

static uint8_t s_tb_pins[TR_TAB_MAX];
static int s_tb_pin_n;
static int s_tb_h, s_tb_btn_h;   // 这次排版的行高和按钮高 / Row and button height of the current layout

// 常用最多几个（底部减去书库和“全部”两格）。/ Most pins (the bar minus the library and "All").
static int tb_capacity(void) { return tr_tab_slots() - 2; }

// 段标题：三段，加上放大后“这一档在‘全部’里”的小标题（正常档写在行里）。/ Section headings: the three
// sections plus, at larger sizes, a "this size: on All" sub-heading (normal size writes it inside the row).
enum { TB_OVER = 3, TB_HEADS };

static int tb_layout(tb_row_t rows[TR_TAB_MAX], int sec_y[TB_HEADS]) {
    s_tb_pin_n = app_settings_tab_pins(s_tb_pins);
    const uint16_t hidden = app_settings_hidden_tabs() & (uint16_t)~TR_TABS_RETIRED;
    const int cap = tb_capacity();
    // 放大后一行放不下名字、说明和三个按钮，超出的常用改放到小标题下面。/ At larger sizes a row cannot hold the
    // name, the note and three buttons, so pins beyond the bar go under a sub-heading.
    const bool over_head = ui_scale_pct != 100 && s_tb_pin_n > cap;
    int count = 1 + s_tb_pin_n;
    for (int k = 1; k < TR_TAB_MAX; k++) {
        const int t = TR_TAB_ORDER[k];   // 按显示顺序，设置最后 / Display order, settings last
        if (TR_TABS_RETIRED & (1u << t)) continue;
        bool pinned = false;
        for (int i = 0; i < s_tb_pin_n; i++) pinned |= s_tb_pins[i] == t;
        count += !pinned;
    }
    const int heads = 2 + (hidden ? 1 : 0) + (over_head ? 1 : 0);
    // 行放不下标签栏上面时，收紧段距再均分行高。/ When the rows do not fit above the tab bar, tighten the section
    // spacing and share out the row height.
    int y = TAB_TOP, sec = TAB_SEC_H, gap = TAB_SEC_GAP;
    s_tb_h = TAB_H;
    const int limit = TR_TABS_Y - TR_S(8);
    if (y + count * s_tb_h + heads * sec + 2 * gap > limit) {
        y = TR_S(158) + TR_S(26) + TR_S(14);
        sec = TR_S(26) + TR_S(12);
        gap = TR_S(8);
        s_tb_h = (limit - y - heads * sec - 2 * gap) / count;
        if (s_tb_h > TAB_H) s_tb_h = TAB_H;
    }
    s_tb_btn_h = s_tb_h - TR_S(10) < TAB_BTN_H ? s_tb_h - TR_S(10) : TAB_BTN_H;
    int n = 0;
    sec_y[TB_PINNED] = y;
    sec_y[TB_OVER] = -1;
    y += sec;
    rows[n++] = (tb_row_t){TR_TAB_LIBRARY, TB_PINNED, -1, y};
    y += s_tb_h;
    for (int i = 0; i < s_tb_pin_n; i++) {
        if (over_head && i == cap) {
            sec_y[TB_OVER] = y;
            y += sec;
        }
        rows[n++] = (tb_row_t){(tr_tab_t)s_tb_pins[i], TB_PINNED, i, y};
        y += s_tb_h;
    }
    y += gap;
    sec_y[TB_REST] = y;
    y += sec;
    for (int k = 1; k < TR_TAB_MAX; k++) {
        const int t = TR_TAB_ORDER[k];   // 按显示顺序，设置最后 / Display order, settings last
        if (TR_TABS_RETIRED & (1u << t)) continue;
        bool pinned = false;
        for (int i = 0; i < s_tb_pin_n; i++) pinned |= s_tb_pins[i] == t;
        if (pinned || (hidden & (1u << t))) continue;
        rows[n++] = (tb_row_t){(tr_tab_t)t, TB_REST, -1, y};
        y += s_tb_h;
    }
    y += gap;
    sec_y[TB_HIDDEN] = hidden ? y : -1;
    if (hidden) y += sec;
    for (int k = 1; k < TR_TAB_MAX; k++) {
        const int t = TR_TAB_ORDER[k];   // 按显示顺序，设置最后 / Display order, settings last
        if (TR_TABS_RETIRED & (1u << t)) continue;
        if (!(hidden & (1u << t))) continue;
        rows[n++] = (tb_row_t){(tr_tab_t)t, TB_HIDDEN, -1, y};
        y += s_tb_h;
    }
    return n;
}

static int tb_btn_w(const char* label) { return ttf_text_width_px(TR_S(26), label) + TR_S(32); }

// 这一行某个按钮的位置；这一行没有这个按钮返回 false。/ Where a row's button sits; false if the row lacks it.
static bool tb_btn_rect(const tb_row_t* r, tb_btn_t b, EpdRect* out) {
    const int bh = s_tb_btn_h, top = r->y + (s_tb_h - bh) / 2;
    int right = TR_W - TR_MARGIN;
    if (r->sec == TB_PINNED) {
        if (r->index < 0) return false;
        const int out_w = tb_btn_w("移出");
        if (b == TB_OUT) { *out = (EpdRect){right - out_w, top, out_w, bh}; return true; }
        right -= out_w + TR_S(14);
        if (b == TB_DOWN) { *out = (EpdRect){right - bh, top, bh, bh}; return true; }
        right -= bh + TR_S(10);
        if (b == TB_UP) { *out = (EpdRect){right - bh, top, bh, bh}; return true; }
        return false;
    }
    if (r->sec == TB_REST) {
        const int in_w = tb_btn_w("放到底部");
        if (b == TB_IN) { *out = (EpdRect){right - in_w, top, in_w, bh}; return true; }
        right -= in_w + TR_S(14);
        const int hide_w = tb_btn_w("隐藏");
        if (b == TB_HIDE && !(TR_TABS_FIXED & (1u << r->tab))) {
            *out = (EpdRect){right - hide_w, top, hide_w, bh};
            return true;
        }
        return false;
    }
    const int show_w = tb_btn_w("显示");
    if (b == TB_SHOW) { *out = (EpdRect){right - show_w, top, show_w, bh}; return true; }
    return false;
}

// 这一行的按钮能不能按（到头的箭头、底部满了的“放到底部”是灰的）。/ Whether a row's button is enabled (arrows at
// the ends and "Pin" with a full bar are gray).
static bool tb_btn_enabled(const tb_row_t* r, tb_btn_t b) {
    if (b == TB_UP) return r->index > 0;
    if (b == TB_DOWN) return r->index >= 0 && r->index + 1 < s_tb_pin_n;
    if (b == TB_IN) return s_tb_pin_n < tb_capacity();
    return true;
}

static void tb_draw_btn(uint8_t* fb, EpdRect r, tb_btn_t b, bool enabled) {
    // 能按：深灰描边；不能按：浅灰底、深灰字。这一页整页刷新。/ Enabled: dark outline; disabled: light gray fill
    // with dark gray text. This page refreshes whole pages.
    if (enabled) {
        ui_draw_round_rect(fb, r, r.height / 2, TR_GRAY_OUTLINE);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), r.height / 2 - 1, TR_GRAY_OUTLINE);
    } else {
        ui_fill_round_rect(fb, r, r.height / 2, TR_GRAY_SURFACE);
    }
    const uint8_t ink = enabled ? UI_GRAY_BLACK : TR_GRAY_OUTLINE, bg = enabled ? UI_GRAY_WHITE : TR_GRAY_SURFACE;
    if (b == TB_UP || b == TB_DOWN) {
        const int cx = r.x + r.width / 2, cy = r.y + r.height / 2, a = TR_S(10), dy = b == TB_UP ? -a / 2 : a / 2;
        for (int d = -1; d <= 1; d++) {
            epd_draw_line(cx - a + d, cy - dy, cx + d, cy + dy, ink, fb);
            epd_draw_line(cx + d, cy + dy, cx + a + d, cy - dy, ink, fb);
        }
        return;
    }
    static const char* const LABELS[TB_BTN_COUNT] = {"", "", "移出", "放到底部", "隐藏", "显示"};
    tr_text_on(fb, r.x + r.width / 2, r.y + (r.height - TR_S(26)) / 2 - TR_S(2), TR_S(26), LABELS[b],
               EPD_DRAW_ALIGN_CENTER, false, ink, bg);
}

static void draw_tabs(uint8_t* fb) {
    draw_back_title(fb, "底部标签");
    draw_message(fb);
    char line[96];
    snprintf(line, sizeof(line), "底部放 %d 个，最后一格固定是“全部”", tr_tab_slots() - 1);
    tr_text_dim(fb, TR_MARGIN, TR_S(158), TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
    tb_row_t rows[TR_TAB_MAX];
    int sec_y[TB_HEADS];
    const int n = tb_layout(rows, sec_y);
    const int h = s_tb_h, px = TR_S(26);
    tr_text_dim(fb, TR_MARGIN, sec_y[TB_PINNED], px, "放在底部（按顺序）", EPD_DRAW_ALIGN_LEFT);
    tr_text_dim(fb, TR_MARGIN, sec_y[TB_REST], px, "只在“全部”里", EPD_DRAW_ALIGN_LEFT);
    if (sec_y[TB_HIDDEN] >= 0)
        tr_text_dim(fb, TR_MARGIN, sec_y[TB_HIDDEN], px, "已隐藏（“全部”里也不显示）", EPD_DRAW_ALIGN_LEFT);
    if (sec_y[TB_OVER] >= 0) tr_text_dim(fb, TR_MARGIN, sec_y[TB_OVER], px, "这一档在“全部”里", EPD_DRAW_ALIGN_LEFT);
    for (int i = 0; i < n; i++) {
        const tb_row_t* r = &rows[i];
        const bool pinned = r->sec == TB_PINNED;
        int x = TR_MARGIN;
        if (pinned) {
            snprintf(line, sizeof(line), "%d", r->index + 2);
            tr_text_dim(fb, x + TR_S(6), r->y + (h - TR_S(28)) / 2 - TR_S(2), TR_S(28), line, EPD_DRAW_ALIGN_LEFT);
            x += TR_S(40);
        }
        tr_icon(fb, tr_tab_icon(r->tab), x + TR_S(20), r->y + h / 2, false);
        const int ly = r->y + (h - TR_S(32)) / 2 - TR_S(2);
        if (r->sec == TB_HIDDEN)
            tr_text_dim(fb, x + TR_S(60), ly, TR_S(32), tr_tab_label(r->tab), EPD_DRAW_ALIGN_LEFT);
        else tr_text(fb, x + TR_S(60), ly, TR_S(32), tr_tab_label(r->tab), EPD_DRAW_ALIGN_LEFT, false);
        if (r->sec == TB_PINNED && r->index < 0) {
            tr_text_dim(fb, TR_W - TR_MARGIN, r->y + (h - px) / 2, px, "固定第一格（主页）", EPD_DRAW_ALIGN_RIGHT);
        } else if (r->sec == TB_PINNED && r->index >= tb_capacity() && sec_y[TB_OVER] < 0) {
            // 这一档底部放不下：先留在“全部”里。/ No room in the bar at this size: on the All page for now.
            tr_text_dim(fb, x + TR_S(60) + ttf_text_width_px(TR_S(32), tr_tab_label(r->tab)) + TR_S(16),
                        r->y + (h - TR_S(24)) / 2, TR_S(24), "这一档在“全部”里", EPD_DRAW_ALIGN_LEFT);
        }
        for (int b = 0; b < TB_BTN_COUNT; b++) {
            EpdRect br;
            if (tb_btn_rect(r, (tb_btn_t)b, &br)) tb_draw_btn(fb, br, (tb_btn_t)b, tb_btn_enabled(r, (tb_btn_t)b));
        }
        if (i + 1 < n && rows[i + 1].sec == r->sec && rows[i + 1].y == r->y + h)
            tr_hline(fb, r->y + h - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
}

static void draw_sources(uint8_t* fb) {
    draw_back_title(fb, "内容来源");
    draw_message(fb);
    char sum[200];
    snprintf(sum, sizeof(sum), "%.150s · 书城榜单", !s_has_cfg ? "番茄服务器未设置" : s_cfg.builtin ? "内置服务器" :
             host_of(s_cfg.url));
    const int h = source_h();
    draw_cat(fb, CAT_TOP, h, TR_ICON_TOMATO, "番茄小说", sum, false);
    for (int i = 0; i < S_COUNT; i++) {
        if (SERVERS[i].what[0])
            snprintf(sum, sizeof(sum), "%s · %.150s", SERVERS[i].what, s_has_host[i] ? host_of(s_host[i].url) : "未设置");
        else
            snprintf(sum, sizeof(sum), "%s 服务器 · %.150s", SERVERS[i].panel,
                     s_has_host[i] ? host_of(s_host[i].url) : "未设置");
        draw_cat(fb, CAT_TOP + (1 + i) * h, h, SERVERS[i].icon, SERVERS[i].name, sum, false);
    }
    draw_cat(fb, CAT_TOP + SRC_HUPU * h, h, TR_ICON_BALL, "虎扑",
             app_settings_hupu_images() ? "显示图片" : "只看文字（图片已关）", false);
    const int n = tr_auth_count();
    if (n >= 0) snprintf(sum, sizeof(sum), "%d 个账号 · PIN 与自动上锁", n);
    else snprintf(sum, sizeof(sum), "%s · PIN 与自动上锁", auth_state_text());
    draw_cat(fb, CAT_TOP + SRC_AUTH * h, h, TR_ICON_KEY, "验证器", sum, true);
}

/* ---- 虎扑 / Hupu ---- */

// 打开“显示图片”前的提醒。/ The warning before turning "show images" on.
static bool s_hupu_ask;

static EpdRect ask_rect(void) { return (EpdRect){TR_MARGIN, TR_S(330), TR_W - 2 * TR_MARGIN, TR_S(470)}; }
static EpdRect ask_btn(bool ok) {
    const EpdRect d = ask_rect();
    const int w = (d.width - TR_S(96)) / 2;
    return (EpdRect){ok ? d.x + d.width - TR_S(36) - w : d.x + TR_S(36), d.y + d.height - TR_S(104), w, TR_S(72)};
}

static void draw_hupu(uint8_t* fb) {
    draw_back_title(fb, "虎扑");
    draw_message(fb);
    draw_rows(fb, HUPU_ROWS, COUNT(HUPU_ROWS));
    // 确认框盖在说明上，开着时不画说明。/ The confirm dialog covers the notes, so skip them while it is up.
    if (s_hupu_ask) goto ask;
    const int lines = tr_wrap(fb, TR_MARGIN, TR_S(372), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36),
            "默认只看文字：帖子里的图写成“[图片]”，列表不带缩略图，不下载也不解码图片。打开后才加载图片，设备负担会大很多。", 4);
    // 第二段接在第一段后面，空一行。/ The second note follows the first after a blank line.
    tr_wrap(fb, TR_MARGIN, TR_S(372) + (lines + 1) * TR_S(36), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36),
            "关注每个专区：刷新关注时每个专区取几页，1、2、3 轮换。页数越多列表越长，刷新也越慢。", 3);
    return;
ask:;
    const EpdRect d = ask_rect();
    epd_fill_rect(d, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, d, TR_S(24), UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(d, 1), TR_S(24) - 1, UI_GRAY_BLACK);
    tr_text(fb, d.x + TR_S(36), d.y + TR_S(36), TR_S(36), "打开虎扑图片？", EPD_DRAW_ALIGN_LEFT, true);
    tr_wrap(fb, d.x + TR_S(36), d.y + TR_S(100), d.width - TR_S(72), TR_S(28), TR_S(42),
            "加载图片会大大增加设备的负担（下载、解码都占内存和时间），可能让设备变慢甚至卡死。卡住时长按电源键重启，再到这里关掉。", 6);
    tr_pill(fb, ask_btn(false), "取消", false, TR_S(30));
    tr_pill(fb, ask_btn(true), "打开", true, TR_S(30));
}

static app_redraw_t touch_hupu_ask(uint16_t x, uint16_t y) {
    const bool ok = ui_rect_hit(ask_btn(true), x, y);
    if (!ok && !ui_rect_hit(ask_btn(false), x, y)) return APP_REDRAW_NONE;
    s_hupu_ask = false;
    if (ok) {
        app_settings_set_hupu_images(true);
        tomato_hupu_set_images(true);
        snprintf(s_message, sizeof(s_message), "已打开虎扑图片");
    }
    return APP_REDRAW_PAGE;
}

static void draw_server(uint8_t* fb) {
    draw_back_title(fb, SERVERS[s_srv].name);
    draw_message(fb);
    if (s_web_on) {
        draw_rows(fb, SERVER_ROWS, 1);
        draw_web_box(fb);
        return;
    }
    draw_rows(fb, SRV_ROWS, SRV_ROW_COUNT);
    char hint[200];
    snprintf(hint, sizeof(hint), "地址和令牌在 %s 面板的“设备与外网”页。令牌很长，建议打开“手机网页填写”，在手机上粘贴。",
             SERVERS[s_srv].panel);
    // 漫画多两行设置，说明往下挪两行。/ Comics have two more rows; the notes move down two rows.
    const int top = s_srv == S_WT ? 726 : 582;
    // 漫画页行多：放大后标签栏上面剩几行就写几行，放不下的说明不写。/ The comic page has more rows: scaled up, write
    // only the note lines that fit above the tab bar and skip the rest.
    const int room = s_srv == S_WT ? (TR_TABS_Y - TR_S(8) - TR_S(top)) / TR_S(36) : 3;
    if (room <= 0) return;
    const int n = tr_wrap(fb, TR_MARGIN, TR_S(top), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36), hint, room < 3 ? room : 3);
    // 第二段跟在第一段后面（放大后第一段可能多折一行）。/ The second note follows the first (which may wrap to an
    // extra line at larger sizes).
    int y = TR_S(top + 118);
    if (y < TR_S(top) + n * TR_S(36) + TR_S(10)) y = TR_S(top) + n * TR_S(36) + TR_S(10);
    const int room2 = s_srv == S_WT ? (TR_TABS_Y - TR_S(8) - y) / TR_S(36) : 3;
    if (room2 <= 0) return;
    tr_wrap(fb, TR_MARGIN, y, TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36), s_srv == S_WT ?
            "语言版本、简体显示：点一下切换，下次刷新发现页时生效。每次最多下载：一次“检查更新”最多下几话，5、10、20、50 轮换。" :
            "每次拉取：20、30、50 轮换，下次刷新生效。关注列表会保留以前缓存的条目（最多 100 条），新拉的排前面；缩略图先下前两页，翻到再补。",
            room2 < 3 ? room2 : 3);
}

static void draw_edit(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    char title[48];
    snprintf(title, sizeof(title), "%s%s", s_edit == E_HOST_URL || s_edit == E_HOST_TOKEN ? SERVERS[s_srv].name : "",
             EDIT[s_edit].title);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), title, EPD_DRAW_ALIGN_LEFT, true);
    // 键盘随显示大小往上长；两行提示放不下时，输入框变矮、提示行距收紧，整体贴着键盘往上排。
    // The keyboard grows upward with the display size; when two hint lines no longer fit, the field gets shorter and
    // the hint tighter, stacked up from the keyboard.
    int fy = TR_S(176), fh = TR_S(84), hy = TR_S(286), lh = TR_S(36);
    const int room = TR_KBD_TOP - TR_S(6);
    if (hy + 2 * lh > room) {
        lh = TR_S(26) + TR_S(6);
        hy = room - 2 * lh;
        fh = TR_S(76);
        fy = hy - TR_S(16) - fh;
    }
    const int inset = (TR_S(84) - fh) / 2;   // 矮框里的字和光标往上提 / Text and caret move up in a shorter field
    // 输入框保持白底（打字时按 DU 局部刷新），框线用深灰。/ The field stays white (typing refreshes by DU); its
    // frame is dark gray.
    EpdRect f = {TR_MARGIN, fy, TR_W - 2 * TR_MARGIN, fh};
    ui_draw_round_rect(fb, f, TR_S(14), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(f, 1), TR_S(14) - 1, TR_GRAY_OUTLINE);
    bool secret = s_edit == E_SRV_PASS || s_edit == E_DAV_PASS || s_edit == E_HOST_TOKEN;
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
    const int px = TR_S(32);
    const char* tail = tr_tail_fit(shown, px, f.width - TR_S(48));
    tr_text(fb, f.x + TR_S(20), f.y + TR_S(22) - inset, px, tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + TR_S(22) + ttf_text_width_px(px, tail), f.y + TR_S(18) - inset, TR_S(3), TR_S(46)},
                  UI_GRAY_BLACK, fb);
    const char* hint = EDIT[s_edit].hint;
    char host_hint[160];
    if (s_edit == E_HOST_URL) {
        snprintf(host_hint, sizeof(host_hint), "%s 面板“设备与外网”里的地址，如 %s", SERVERS[s_srv].panel,
                 SERVERS[s_srv].example);
        hint = host_hint;
    }
    if (s_edit == E_SRV_URL && tomato_config_has_builtin()) hint = "留空使用内置服务器；自建的填 http://地址:端口";
    if (s_message[0]) hint = s_message;   // 输入不对时的提示 / Why the input was refused
    tr_wrap(fb, TR_MARGIN, hy, TR_W - 2 * TR_MARGIN, TR_S(26), lh, hint, 2);
    tr_kbd_draw(fb, &s_kbd);
}

/* ---- 关于 / About ---- */

// 正常档一页放下；放大后分两页：上页版本和设备入口，下页开源致谢，底部翻页。
// One page at the normal size; larger sizes split into two pages (version and device entries, then credits) with a
// pager at the bottom.
static int s_about_page;
static bool about_paged(void) { return TR_S(1186) > TR_H; }
static int about_device_y(void) { return about_paged() ? TR_S(558) : TR_S(1030); }
static int about_test_y(void) { return about_device_y() + TR_S(76); }
#define ABOUT_PAGER_Y (TR_H - TR_S(110))

static const char* const CREDITS[] = {
    "MindReset Read Pico 示例固件 · Apache-2.0",
    "epdiy 墨水屏驱动 · LGPL-3.0",
    "XH View OS 拼音引擎 · Apache-2.0",
    "jieba、pypinyin 词库数据 · MIT",
    "寒蝉端黑体 · SIL OFL 1.1",
    "stb_truetype、stb_image、TJpgDec、cJSON",
    "Tomato-Novel-Downloader 下载服务",
};

static void about_row(uint8_t* fb, int y, const char* label, const char* value) {
    tr_text(fb, TR_MARGIN, y, TR_S(30), label, EPD_DRAW_ALIGN_LEFT, false);
    char v[96];
    snprintf(v, sizeof(v), "%s", value);
    tr_fit(v, TR_S(28), TR_W - 2 * TR_MARGIN - TR_S(160));
    tr_text_dim(fb, TR_W - TR_MARGIN, y + TR_S(2), TR_S(28), v, EPD_DRAW_ALIGN_RIGHT);
}

static void draw_credits(uint8_t* fb, int y) {
    tr_text_dim(fb, TR_MARGIN, y, TR_S(26), "开源致谢", EPD_DRAW_ALIGN_LEFT);
    y += TR_S(44);
    // 放大后一条放不下就折成两行，不截断。/ At larger sizes a credit that does not fit wraps instead of being cut.
    for (size_t i = 0; i < sizeof(CREDITS) / sizeof(CREDITS[0]); i++) {
        const int n = tr_wrap(fb, TR_MARGIN, y, TR_W - 2 * TR_MARGIN, TR_S(28), TR_S(36), CREDITS[i], 2);
        y += TR_S(42) + (n - 1) * TR_S(36);
    }
    tr_wrap(fb, TR_MARGIN, y + TR_S(16), TR_W - 2 * TR_MARGIN, TR_S(26), TR_S(38),
            "非官方固件，与 MindReset、番茄小说没有隶属关系。各组件按各自许可使用，详见项目仓库。", 3);
}

static void draw_about(uint8_t* fb) {
    const esp_app_desc_t* app = esp_app_get_description();
    char line[64];
    draw_back_title(fb, "关于 Treader");
    const bool paged = about_paged();
    if (paged) tr_pager(fb, ABOUT_PAGER_Y, s_about_page, 2);
    if (paged && s_about_page == 1) {
        draw_credits(fb, TR_S(184));
        return;
    }
    tr_text(fb, TR_MARGIN, TR_S(184), TR_S(60), "Treader", EPD_DRAW_ALIGN_LEFT, true);
    // 一行放不下（约 730 px），折成两行。/ Too wide for one line (~730 px); wrap to two.
    tr_wrap(fb, TR_MARGIN, TR_S(256), TR_W - 2 * TR_MARGIN, TR_S(28), TR_S(34),
            "小纸 Pico 阅读固件 · 书库、番茄小说、WebDAV 同步", 2);
    snprintf(line, sizeof(line), "%.20s · TF 卡升级 ›", app->version);
    about_row(fb, TR_S(336), "版本", line);
    tr_hline(fb, TR_S(390), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    snprintf(line, sizeof(line), "%s %s UTC", app->date, app->time);
    about_row(fb, TR_S(410), "构建时间", line);
    tr_hline(fb, TR_S(464), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    about_row(fb, TR_S(484), "项目主页", "github.com/bidabrain/treader-eink");
    if (!paged) draw_credits(fb, TR_S(584));
    const int dev = about_device_y(), test = about_test_y();
    tr_hline(fb, dev - TR_S(20), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    tr_text(fb, TR_MARGIN, dev + TR_S(10), TR_S(32), "设备信息", EPD_DRAW_ALIGN_LEFT, false);
    tr_text_dim(fb, TR_W - TR_MARGIN, dev + TR_S(14), TR_S(28), "电量、存储、芯片 ›", EPD_DRAW_ALIGN_RIGHT);
    tr_hline(fb, test - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    tr_text(fb, TR_MARGIN, test + TR_S(10), TR_S(32), "设备自检", EPD_DRAW_ALIGN_LEFT, false);
    tr_text_dim(fb, TR_W - TR_MARGIN, test + TR_S(14), TR_S(28), "屏幕、触摸、电源 ›", EPD_DRAW_ALIGN_RIGHT);
}

/* ---- 时间与时区 / Time and zone ---- */

// 手动设置时间：年、月、日、时、分五栏，每栏上下各一个按钮。放大后栏宽不超过一行能放下的宽度，栏距最少 8。
// Manual time: five columns (year, month, day, hour, minute), each with a button above and below. At larger sizes a
// column is never wider than one row allows, with gaps of at least 8.
#define TSET_COLS 5
#define TSET_COL_W tset_col_w()
#define TSET_GAP ((TR_W - 2 * TR_MARGIN - TSET_COLS * TSET_COL_W) / (TSET_COLS - 1))
#define TSET_LABEL_Y TR_S(200)
#define TSET_UP_Y TR_S(248)
#define TSET_BTN_H TR_S(88)
#define TSET_VALUE_Y TR_S(360)
#define TSET_DOWN_Y TR_S(444)
#define TSET_SAVE (EpdRect){TR_MARGIN, TR_S(600), TR_W - 2 * TR_MARGIN, TR_S(88)}
// 时区列表：行高随显示大小放大，放不下时分页。/ Zone list: row height scales; pages when it does not fit.
#define TZ_TOP TR_S(170)
#define TZ_ROW_H TR_S(60)
#define TZ_PAGER_Y (TR_TABS_Y - TR_S(84))

static int s_tset[TSET_COLS];   // 年、月、日、时、分 / Year, month, day, hour, minute
static int s_tz_page;

static int tset_col_w(void) {
    const int fit = (TR_W - 2 * TR_MARGIN - (TSET_COLS - 1) * TR_S(8)) / TSET_COLS;
    return TR_S(108) < fit ? TR_S(108) : fit;
}

// 一页几个时区：全放得下就一页，否则留出翻页的位置。/ Zones per page: all on one page when they fit, otherwise
// leave room for the pager.
static int tz_per_page(void) {
    const int n = app_settings_tz_count();
    if (TZ_TOP + n * TZ_ROW_H <= TR_TABS_Y - TR_S(16)) return n;
    return (TZ_PAGER_Y - TR_S(20) - TZ_TOP) / TZ_ROW_H;
}

static int tz_pages(void) { return (app_settings_tz_count() + tz_per_page() - 1) / tz_per_page(); }

static int days_in_month(int year, int month) {
    static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : DAYS[(month - 1) % 12];
}

static EpdRect tset_col(int k, int y, int h) {
    return (EpdRect){TR_MARGIN + k * (TSET_COL_W + TSET_GAP), y, TSET_COL_W, h};
}

static void draw_time_page(uint8_t* fb) {
    draw_back_title(fb, "时间与时区");
    draw_message(fb);
    draw_rows(fb, TIME_ROWS, COUNT(TIME_ROWS));
    tr_wrap(fb, TR_MARGIN, TR_S(440), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36),
            "自动校时开着时，设备每次联网（番茄、小红书、传书等）都会顺便对一次时，按上面的时区显示。关掉后只用手动设置的时间。"
            "时间存在电源芯片的时钟里，断电也继续走。", 5);
}

// 翻页区域：每个选项左边一块小屏示意，浅灰上一页、深灰下一页、白色开工具条。
// Turn zones: each option has a small screen sketch on the left; light gray is back, dark gray forward, white the
// toolbar.
#define TURN_TOP TR_S(170)
#define TURN_OPT_H TR_S(206)
#define TURN_SKETCH_H TR_S(170)
#define TURN_SKETCH_W (TURN_SKETCH_H * 684 / 1216)
#define TURN_BACK_INK 0xD0
#define TURN_FWD_INK 0x60

static void turn_zone_fill(uint8_t* fb, EpdRect r, int dir) {
    if (r.width <= 0 || r.height <= 0) return;
    if (dir) epd_fill_rect(r, dir < 0 ? TURN_BACK_INK : TURN_FWD_INK, fb);
    const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    if (dir < 0) tr_text(fb, cx, cy - TR_S(14), TR_S(28), "‹", EPD_DRAW_ALIGN_CENTER, true);
    else if (dir > 0) tr_text_inv(fb, cx, cy - TR_S(14), TR_S(28), "›", EPD_DRAW_ALIGN_CENTER, true);
    else for (int k = -1; k <= 1; k++)   // 菜单三道横线 / Three menu bars
        epd_fill_rect((EpdRect){cx - TR_S(10), cy + k * TR_S(7) - 1, TR_S(20), 3}, UI_GRAY_BLACK, fb);
}

static void draw_turn_sketch(uint8_t* fb, int x, int y, int mode) {
    const int w = TURN_SKETCH_W, h = TURN_SKETCH_H, side = w * 3 / 10;
    if (mode == APP_TURN_SIDES) {
        turn_zone_fill(fb, (EpdRect){x, y, side, h}, -1);
        turn_zone_fill(fb, (EpdRect){x + side, y, w - 2 * side, h}, 0);
        turn_zone_fill(fb, (EpdRect){x + w - side, y, side, h}, 1);
    } else {
        const int top = h / 4;
        turn_zone_fill(fb, (EpdRect){x, y, w, top}, 0);
        const int left = mode == APP_TURN_LOWER ? side : 0;
        turn_zone_fill(fb, (EpdRect){x, y + top, left, h - top}, -1);
        turn_zone_fill(fb, (EpdRect){x + left, y + top, w - left, h - top}, 1);
        tr_hline(fb, y + top, x, w, true);
    }
    epd_draw_rect((EpdRect){x, y, w, h}, UI_GRAY_BLACK, fb);
    epd_draw_rect((EpdRect){x + 1, y + 1, w - 2, h - 2}, UI_GRAY_BLACK, fb);
}

static void draw_turn_page(uint8_t* fb) {
    draw_back_title(fb, "翻页区域");
    const int cur = app_settings_turn_zone();
    const int tx = TR_MARGIN + TURN_SKETCH_W + TR_S(28), tw = TR_W - TR_MARGIN - tx;
    for (int i = 0; i < APP_TURN_ZONES; i++) {
        const int y = TURN_TOP + i * TURN_OPT_H, sy = y + (TURN_OPT_H - TURN_SKETCH_H) / 2;
        draw_turn_sketch(fb, TR_MARGIN, sy, i);
        tr_text(fb, tx, sy + TR_S(6), TR_S(32), TURN_NAMES[i], EPD_DRAW_ALIGN_LEFT, i == cur);
        if (i == cur) tr_text(fb, TR_W - TR_MARGIN, sy + TR_S(10), TR_S(26), "当前", EPD_DRAW_ALIGN_RIGHT, true);
        tr_wrap(fb, tx, sy + TR_S(56), tw, TR_S(24), TR_S(34), TURN_NOTES[i], 3);
        if (i + 1 < APP_TURN_ZONES) tr_hline(fb, y + TURN_OPT_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
}

static void draw_tz_page(uint8_t* fb) {
    draw_back_title(fb, "时区");
    const int cur = app_settings_tz(), per = tz_per_page(), n = app_settings_tz_count();
    const int h = TZ_ROW_H;
    for (int k = 0; k < per; k++) {
        const int i = s_tz_page * per + k;
        if (i >= n) break;
        const int y = TZ_TOP + k * h;
        tr_text(fb, TR_MARGIN, y + (h - TR_S(30)) / 2 - TR_S(2), TR_S(30), app_settings_tz_name((uint8_t)i),
                EPD_DRAW_ALIGN_LEFT, i == cur);
        char right[32];
        snprintf(right, sizeof(right), "%s%s", app_settings_tz_offset((uint8_t)i), i == cur ? " · 当前" : "");
        if (i == cur) tr_text(fb, TR_W - TR_MARGIN, y + (h - TR_S(26)) / 2, TR_S(26), right, EPD_DRAW_ALIGN_RIGHT, true);
        else tr_text_dim(fb, TR_W - TR_MARGIN, y + (h - TR_S(26)) / 2, TR_S(26), right, EPD_DRAW_ALIGN_RIGHT);
        if (i + 1 < n && k + 1 < per) tr_hline(fb, y + h - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_pager(fb, TZ_PAGER_Y, s_tz_page, tz_pages());
}

static void draw_tset_page(uint8_t* fb) {
    draw_back_title(fb, "手动设置时间");
    static const char* const LABELS[TSET_COLS] = {"年", "月", "日", "时", "分"};
    for (int k = 0; k < TSET_COLS; k++) {
        const EpdRect c = tset_col(k, 0, 0);
        tr_text_dim(fb, c.x + TSET_COL_W / 2, TSET_LABEL_Y, TR_S(26), LABELS[k], EPD_DRAW_ALIGN_CENTER);
        tr_pill(fb, tset_col(k, TSET_UP_Y, TSET_BTN_H), "+", false, TR_S(40));
        char v[8];
        snprintf(v, sizeof(v), k == 0 ? "%04d" : "%02d", s_tset[k] % 10000);
        // 年份四位：栏变窄时字号收到放得下。/ The four-digit year shrinks to fit a narrower column.
        int px = TR_S(k == 0 ? 40 : 48);
        while (px > 32 && ttf_text_width_px(px, v) > TSET_COL_W - TR_S(4)) px -= 2;
        tr_text(fb, c.x + TSET_COL_W / 2, TSET_VALUE_Y + (TR_S(k == 0 ? 40 : 48) - px) / 2, px, v,
                EPD_DRAW_ALIGN_CENTER, true);
        tr_pill(fb, tset_col(k, TSET_DOWN_Y, TSET_BTN_H), "−", false, TR_S(40));
    }
    tr_pill(fb, TSET_SAVE, "保存", true, TR_S(32));
    char hint[96];
    snprintf(hint, sizeof(hint), "按%s %s的本地时间填写，保存后立刻生效。", app_settings_tz_name(app_settings_tz()),
             app_settings_tz_offset(app_settings_tz()));
    tr_wrap(fb, TR_MARGIN, TR_S(720), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36), hint, 2);
}

static void tset_begin(void) {
    struct tm tm = {.tm_year = 2026 - 1900, .tm_mon = 0, .tm_mday = 1};
    if (tr_time_valid()) {
        const time_t now = time(NULL);
        localtime_r(&now, &tm);
    }
    s_tset[0] = tm.tm_year + 1900;
    s_tset[1] = tm.tm_mon + 1;
    s_tset[2] = tm.tm_mday;
    s_tset[3] = tm.tm_hour;
    s_tset[4] = tm.tm_min;
}

static app_redraw_t touch_tset(uint16_t x, uint16_t y) {
    if (ui_rect_hit(TSET_SAVE, x, y)) {
        const esp_err_t err = tr_time_set_local(s_tset[0], s_tset[1], s_tset[2], s_tset[3], s_tset[4]);
        s_page = P_TIME;
        snprintf(s_message, sizeof(s_message), "%s", err == ESP_OK ? "时间已设置" : "这个时间不对，没有保存");
        return APP_REDRAW_PAGE;
    }
    for (int k = 0; k < TSET_COLS; k++) {
        const int dir = ui_rect_hit(tset_col(k, TSET_UP_Y, TSET_BTN_H), x, y) ? 1 :
                        ui_rect_hit(tset_col(k, TSET_DOWN_Y, TSET_BTN_H), x, y) ? -1 : 0;
        if (!dir) continue;
        int* v = &s_tset[k];
        switch (k) {
        case 0: *v += dir; if (*v < 2025) *v = 2099; if (*v > 2099) *v = 2025; break;
        case 1: *v = (*v - 1 + dir + 12) % 12 + 1; break;
        case 2: { const int n = days_in_month(s_tset[0], s_tset[1]); *v = (*v - 1 + dir + n) % n + 1; break; }
        case 3: *v = (*v + dir + 24) % 24; break;
        default: *v = (*v + dir + 60) % 60; break;
        }
        // 换了年或月，日不能超过这个月的天数。/ After a year or month change, keep the day inside the month.
        const int n = days_in_month(s_tset[0], s_tset[1]);
        if (s_tset[2] > n) s_tset[2] = n;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    tr_status_bar(fb);
    if (s_edit != E_NONE) { draw_edit(fb); return; }
    switch (s_page) {
    case P_HOME: draw_home(fb); break;
    case P_DISPLAY: draw_back_title(fb, "显示与阅读"); draw_rows(fb, DISPLAY_ROWS, COUNT(DISPLAY_ROWS)); break;
    case P_TABS: draw_tabs(fb); break;
    case P_TRANSFER: draw_transfer(fb); break;
    case P_SOURCES: draw_sources(fb); break;
    case P_SRC_TOMATO:
        draw_back_title(fb, "番茄小说");
        draw_message(fb);
        if (s_web_on) {
            draw_rows(fb, TOMATO_ROWS, 1);
            draw_web_box(fb);
            break;
        }
        draw_rows(fb, TOMATO_ROWS, COUNT(TOMATO_ROWS));
        break;
    case P_SRC_SERVER: draw_server(fb); break;
    case P_SRC_HUPU: draw_hupu(fb); break;
    case P_SYNC:
        draw_back_title(fb, "同步");
        draw_message(fb);
        if (s_web_on) {
            draw_rows(fb, SYNC_ROWS, 1);
            draw_web_box(fb);
            break;
        }
        draw_rows(fb, SYNC_ROWS, COUNT(SYNC_ROWS));
        tr_wrap(fb, TR_MARGIN, TR_S(454), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36),
                "WebDAV 的地址、用户名和密码很长，建议打开“手机网页填写”，在手机上粘贴。坚果云的密码要用“应用密码”。", 3);
        break;
    case P_POWER:
        draw_back_title(fb, "电源与设备");
        draw_message(fb);
        draw_rows(fb, POWER_ROWS, COUNT(POWER_ROWS));
        break;
    case P_ABOUT: draw_about(fb); return;
    case P_TIME: draw_time_page(fb); break;
    case P_TZ: draw_tz_page(fb); break;
    case P_TSET: draw_tset_page(fb); break;
    case P_TURN: draw_turn_page(fb); break;
    }
    tr_tab_bar(fb, TR_TAB_SETTINGS);
}

/* ---- 编辑 / Editing ---- */

static void begin_edit(edit_t step, const char* initial) {
    s_edit = step;
    tr_kbd_open(&s_kbd, initial, false, EDIT[step].go);
}

static app_redraw_t finish_edit(void) {
    esp_err_t err;
    if (s_edit == E_STORE_URL) {
        // 只剩协议头也算没填，恢复默认。/ A bare scheme counts as empty and restores the default.
        const bool empty = !s_kbd.text[0] || !strcmp(s_kbd.text, "http://") || !strcmp(s_kbd.text, "https://");
        err = tomato_store_url_save(empty ? "" : s_kbd.text);
    } else if (s_edit == E_SRV_PASS) {
        tomato_config_t next = {0};
        snprintf(next.url, sizeof(next.url), "%.127s", s_step1);
        snprintf(next.password, sizeof(next.password), "%.63s", s_kbd.text);
        err = tomato_config_save(&next);
        memset(&next, 0, sizeof(next));
    } else if (s_edit == E_HOST_URL || s_edit == E_HOST_TOKEN) {
        tomato_srv_config_t next;
        SERVERS[s_srv].load(&next);
        if (s_edit == E_HOST_URL) {
            if (s_kbd.text[0] && strncmp(s_kbd.text, "http://", 7) && strncmp(s_kbd.text, "https://", 8)) {
                memset(&next, 0, sizeof(next));
                snprintf(s_message, sizeof(s_message), "地址要以 http:// 或 https:// 开头");
                return APP_REDRAW_PAGE;   // 留在键盘里改 / Stay in the keyboard to fix it
            }
            snprintf(next.url, sizeof(next.url), "%.150s", s_kbd.text);
            if (!strcmp(next.url, "http://") || !strcmp(next.url, "https://")) next.url[0] = 0;
        } else if (s_kbd.text[0]) {
            snprintf(next.token, sizeof(next.token), "%.90s", s_kbd.text);
        }
        err = SERVERS[s_srv].save(&next);
        memset(&next, 0, sizeof(next));
        s_test_result[s_srv][0] = 0;
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
    snprintf(s_message, sizeof(s_message), err == ESP_OK ? "已保存" : "保存失败");
    load();
    s_edit = E_NONE;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_edit(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < TR_S(160) && x < TR_S(120)) { s_edit = E_NONE; memset(&s_kbd, 0, sizeof(s_kbd)); return APP_REDRAW_PAGE; }
    tr_kbd_result_t res = tr_kbd_touch(&s_kbd, x, y);
    if (res == TR_KBD_CHANGED) {
        render(ctx, ctx->fb);
        return APP_REDRAW_AREA;
    }
    if (res != TR_KBD_SUBMIT) return APP_REDRAW_NONE;
    switch (s_edit) {
    case E_SRV_URL:
        snprintf(s_step1, sizeof(s_step1), "%.159s", s_kbd.text);
        // 只剩协议头也算没填；没填就清掉自定义服务器，回到内置默认，不用再问密码。
        // A bare scheme counts as empty; empty clears the custom server and returns to the built-in default.
        if (!strcmp(s_step1, "http://") || !strcmp(s_step1, "https://")) s_step1[0] = 0;
        if (!s_step1[0]) {
            s_edit = E_SRV_PASS;
            s_kbd.text[0] = 0;
            return finish_edit();
        }
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

/* ---- 触摸 / Touch ---- */

static app_redraw_t open_page(page_t p) {
    s_page = p;
    s_message[0] = 0;
    return APP_REDRAW_PAGE;
}

static app_redraw_t go_back(void) {
    s_message[0] = 0;
    s_hupu_ask = false;
    if (s_page == P_SRC_SERVER || s_page == P_SRC_TOMATO || s_page == P_SYNC) web_stop();
    s_page = s_page == P_ABOUT || s_page == P_TIME ? P_POWER : s_page == P_TZ || s_page == P_TSET ? P_TIME :
             s_page == P_TURN ? P_DISPLAY :
             s_page == P_SRC_TOMATO || s_page == P_SRC_SERVER || s_page == P_SRC_HUPU ? P_SOURCES : P_HOME;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_sources(app_ctx_t* ctx, uint16_t y) {
    const int h = source_h();
    if (y < CAT_TOP || y >= CAT_TOP + SOURCES * h) return APP_REDRAW_NONE;
    const int i = (y - CAT_TOP) / h;
    if (i == 0) return open_page(P_SRC_TOMATO);
    if (i == SRC_HUPU) return open_page(P_SRC_HUPU);
    if (i == SRC_AUTH) {
        // 验证器的设置由它自己的页面画，看起来仍在设置里。/ The authenticator draws its own settings, still under
        // Settings.
        app_tr_auth_request_settings();
        ctx->request_app = &app_tr_auth;
        return APP_REDRAW_NONE;
    }
    s_srv = (srv_t)(i - 1);
    return open_page(P_SRC_SERVER);
}

static app_redraw_t touch_home(app_ctx_t* ctx, uint16_t y) {
    const int h = home_cat_h();
    if (y < CAT_TOP || y >= CAT_TOP + C_COUNT * h) return APP_REDRAW_NONE;
    switch ((cat_t)((y - CAT_TOP) / h)) {
    case C_DISPLAY: return open_page(P_DISPLAY);
    case C_TABS: return open_page(P_TABS);
    case C_NET:
        // 后台要是在联网就先请它让出 Wi-Fi。/ Ask a background job to give up Wi-Fi first.
        if (!make_room()) return APP_REDRAW_PAGE;
        app_transfer_open_wifi();
        ctx->request_app = &app_transfer;
        return APP_REDRAW_NONE;
    case C_WIFI: return open_page(P_TRANSFER);
    case C_SOURCES: return open_page(P_SOURCES);
    case C_SYNC: return open_page(P_SYNC);
    case C_POWER: return open_page(P_POWER);
    default: return APP_REDRAW_NONE;
    }
}

static int row_hit(const row_t* rows, int n, uint16_t y) {
    for (int i = 0; i < n; i++) {
        const int top = TR_S(rows[i].y);
        if (y >= top && y < top + row_h(&rows[i])) return rows[i].id;
    }
    return -1;
}

static app_redraw_t touch_transfer(app_ctx_t* ctx, uint16_t y) {
    if (y < CAT_TOP || y >= CAT_TOP + 2 * CAT_H) return APP_REDRAW_NONE;
    // USB 传书页自己在“开始”时才让后台让路；只是进去看看不取消任何东西。/ The USB page makes room only on
    // "Start"; merely opening it cancels nothing.
    if (y >= CAT_TOP + CAT_H) { ctx->request_app = &app_usb; return APP_REDRAW_NONE; }
    if (!make_room()) return APP_REDRAW_PAGE;
    ctx->request_app = &app_transfer;
    return APP_REDRAW_NONE;
}

// 保存常用和隐藏，交给标签栏，整页重画（标签栏也跟着变）。/ Save pins and hidden features, hand them to the bar,
// and redraw the whole page (the bar changes with it).
static app_redraw_t tb_apply(uint16_t hidden) {
    app_settings_set_tab_pins(s_tb_pins, s_tb_pin_n);
    app_settings_set_hidden_tabs(hidden);
    tr_tabs_configure(app_settings_hidden_tabs(), s_tb_pins, s_tb_pin_n);
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_tabs(uint16_t x, uint16_t y) {
    tb_row_t rows[TR_TAB_MAX];
    int sec_y[TB_HEADS];
    const int n = tb_layout(rows, sec_y);
    uint16_t hidden = app_settings_hidden_tabs();
    for (int i = 0; i < n; i++) {
        const tb_row_t* r = &rows[i];
        for (int b = 0; b < TB_BTN_COUNT; b++) {
            EpdRect br;
            if (!tb_btn_rect(r, (tb_btn_t)b, &br) || !ui_rect_hit(br, x, y)) continue;
            if (!tb_btn_enabled(r, (tb_btn_t)b)) {
                if (b == TB_IN) {
                    snprintf(s_message, sizeof(s_message), "底部满了，先移出一个");
                    return APP_REDRAW_PAGE;
                }
                return APP_REDRAW_NONE;
            }
            s_message[0] = 0;
            const int k = r->index;
            switch ((tb_btn_t)b) {
            case TB_UP:
            case TB_DOWN: {
                const int j = b == TB_UP ? k - 1 : k + 1;
                const uint8_t t = s_tb_pins[k];
                s_tb_pins[k] = s_tb_pins[j];
                s_tb_pins[j] = t;
                break;
            }
            case TB_OUT:
                for (int m = k; m + 1 < s_tb_pin_n; m++) s_tb_pins[m] = s_tb_pins[m + 1];
                s_tb_pin_n--;
                break;
            case TB_IN: s_tb_pins[s_tb_pin_n++] = (uint8_t)r->tab; break;
            case TB_HIDE: hidden |= (uint16_t)(1u << r->tab); break;
            case TB_SHOW: hidden &= (uint16_t)~(1u << r->tab); break;
            default: return APP_REDRAW_NONE;
            }
            return tb_apply(hidden);
        }
    }
    return APP_REDRAW_NONE;
}

// 锁屏密码：关着就输两遍设新密码；开着就先输当前密码再关掉。键盘自己占满屏幕，回来整屏重画。
// Lock PIN: when off, enter a new PIN twice; when on, enter the current PIN to turn it off. The keypad takes the whole
// screen, so the page redraws fully afterwards.
static app_redraw_t touch_pin(app_ctx_t* ctx) {
    char a[5], b[5];
    if (app_settings_pin_enabled()) {
        if (tr_pin_pad(ctx->hl, ctx->tp, "关闭锁屏密码", "输入当前密码", true, 60000, 0, a) != TR_PIN_ENTERED) {
            return APP_REDRAW_FULL;
        }
        const bool ok = app_settings_pin_check(a);
        memset(a, 0, sizeof(a));
        if (!ok) snprintf(s_message, sizeof(s_message), "密码不对，没有关闭");
        else if (app_settings_pin_set(NULL) == ESP_OK) snprintf(s_message, sizeof(s_message), "已关闭锁屏密码");
        else snprintf(s_message, sizeof(s_message), "关闭失败，再试一次");
        return APP_REDRAW_FULL;
    }
    if (tr_pin_pad(ctx->hl, ctx->tp, "设置锁屏密码", "输入 4 位数字", true, 60000, 0, a) != TR_PIN_ENTERED) {
        return APP_REDRAW_FULL;
    }
    const tr_pin_result_t r = tr_pin_pad(ctx->hl, ctx->tp, "设置锁屏密码", "再输一次确认", true, 60000, 0, b);
    if (r == TR_PIN_ENTERED && !strcmp(a, b)) {
        snprintf(s_message, sizeof(s_message), "%s", app_settings_pin_set(a) == ESP_OK ?
                 "已开启：开机和解锁都要输密码" : "保存失败，再试一次");
    } else if (r == TR_PIN_ENTERED) {
        snprintf(s_message, sizeof(s_message), "两次不一样，没有设置");
    }
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    return APP_REDRAW_FULL;
}

static app_redraw_t touch_row(app_ctx_t* ctx, int id) {
    s_message[0] = 0;
    switch (id) {
    case R_FONT: ctx->request_app = &app_font_pick; return APP_REDRAW_NONE;
    case R_WALL: ctx->request_app = &app_tr_wall; return APP_REDRAW_NONE;
    case R_TURN: return open_page(P_TURN);
    case R_FTURN: app_settings_set_fast_turn(!app_settings_fast_turn()); return APP_REDRAW_PAGE;
    case R_DEPTH:
        // 标准 → 深 → 更深 → 标准，马上整屏重画。/ Standard → dark → darker → standard, redrawn full-screen at once.
        app_settings_set_text_depth((uint8_t)((app_settings_text_depth() + 1) % TR_TEXT_DEPTHS));
        tr_text_depth_apply(app_settings_text_depth());
        return APP_REDRAW_FULL;
    case R_SLEEP: ctx->request_app = &app_sleep; return APP_REDRAW_NONE;
    case R_SIZE:
        // 换档后整页排版都变了，整屏重画。/ The whole layout changes with the size; redraw the full screen.
        app_settings_set_display_size((uint8_t)((app_settings_display_size() + 1) % APP_DISPLAY_SIZES));
        ui_set_scale(app_settings_display_pct());
        return APP_REDRAW_FULL;
    case R_IDLE:
        app_settings_set_lock_idle((uint8_t)((app_settings_lock_idle() + 1) % APP_LOCK_IDLE_LEVELS));
        return APP_REDRAW_PAGE;
    case R_PIN: return touch_pin(ctx);
    case R_TIME: return open_page(P_TIME);
    case R_TAUTO:
        app_settings_set_time_auto(!app_settings_time_auto());
        tr_time_apply_settings();
        snprintf(s_message, sizeof(s_message), "%s", app_settings_time_auto() ? "下次联网时自动对时" : "已关闭自动校时");
        return APP_REDRAW_PAGE;
    case R_HPAGES: {
        // 1 → 2 → 3 → 1，下次刷新关注时生效。/ 1 -> 2 -> 3 -> 1, used from the next following refresh.
        const uint8_t n = app_settings_hupu_pages();
        app_settings_set_hupu_pages(n >= APP_HUPU_PAGES_MAX ? 1 : n + 1);
        return APP_REDRAW_PAGE;
    }
    case R_HIMG:
        // 关：马上生效；开：先提醒。/ Off takes effect at once; on asks first.
        if (!app_settings_hupu_images()) { s_hupu_ask = true; return APP_REDRAW_PAGE; }
        app_settings_set_hupu_images(false);
        tomato_hupu_set_images(false);
        snprintf(s_message, sizeof(s_message), "已关闭虎扑图片");
        return APP_REDRAW_PAGE;
    case R_TZONE:
        // 从当前时区所在的那一页开始。/ Start on the page holding the current zone.
        s_tz_page = app_settings_tz() / tz_per_page();
        return open_page(P_TZ);
    case R_TSET:
        if (app_settings_time_auto()) {
            snprintf(s_message, sizeof(s_message), "先关掉自动校时");
            return APP_REDRAW_PAGE;
        }
        tset_begin();
        return open_page(P_TSET);
    case R_CARD: ctx->request_app = &app_tr_card; return APP_REDRAW_NONE;
    case R_ABOUT: s_about_page = 0; return open_page(P_ABOUT);
    case R_STORE: {
        char url[160];
        begin_edit(E_STORE_URL, tomato_store_url_load(url, sizeof(url)) == ESP_OK ? url : "");
        return APP_REDRAW_PAGE;
    }
    case R_SRV:
        // 地址只用英文和符号。/ Addresses take English letters and symbols only.
        // 内置服务器不回显；有内置时默认留空。/ Never echo the built-in server; start empty when one exists.
        begin_edit(E_SRV_URL, s_has_cfg && !s_cfg.builtin ? s_cfg.url : tomato_config_has_builtin() ? "" : "http://");
        return APP_REDRAW_PAGE;
    case R_WEB:
        if (s_web_on) web_stop();
        else web_start();
        if (s_web_on) web_prepare_qr();
        return APP_REDRAW_PAGE;
    case R_URL: begin_edit(E_HOST_URL, s_has_host[s_srv] ? s_host[s_srv].url : "http://"); return APP_REDRAW_PAGE;
    case R_TOKEN: begin_edit(E_HOST_TOKEN, ""); return APP_REDRAW_PAGE;
    case R_WT_LANG: tomato_wt_lang_set((tomato_wt_lang() + 1) % TOMATO_WT_LANGS); return APP_REDRAW_PAGE;
    case R_WT_SIMP: tomato_wt_simp_set(!tomato_wt_simp()); return APP_REDRAW_PAGE;
    case R_WT_MAX: {
        const int n = tomato_wt_max();
        tomato_wt_max_set(n == 5 ? 10 : n == 10 ? 20 : n == 20 ? 50 : 5);
        return APP_REDRAW_PAGE;
    }
    case R_LIST: {
        if (!SERVERS[s_srv].list) return APP_REDRAW_NONE;
        const int n = SERVERS[s_srv].list();
        // 20 → 30 → 50 → 20。/ 20 -> 30 -> 50 -> 20.
        SERVERS[s_srv].list_set(n == TOMATO_LIST_LIMIT_MIN   ? TOMATO_LIST_LIMIT_MID
                                : n == TOMATO_LIST_LIMIT_MID ? TOMATO_LIST_LIMIT_MAX
                                                             : TOMATO_LIST_LIMIT_MIN);
        return APP_REDRAW_PAGE;
    }
    case R_TEST:
        if (!make_room()) return APP_REDRAW_PAGE;
        start_test(s_srv);
        return APP_REDRAW_PAGE;
    case R_DAV: begin_edit(E_DAV_URL, s_has_dav ? s_dav.url : "https://"); return APP_REDRAW_PAGE;
    case R_DAV_SYNC:
        if (!s_has_dav) return APP_REDRAW_NONE;
        if (!make_room()) return APP_REDRAW_PAGE;
        tomato_sync_set_progress_io(export_progress, import_progress);
        if (tomato_sync_start_webdav() == ESP_OK) s_syncing = true;
        return APP_REDRAW_PAGE;
    default: return APP_REDRAW_NONE;
    }
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    const uint16_t x = touch->x, y = touch->y;
    if (s_edit != E_NONE) return touch_edit(ctx, x, y);
    if (s_page == P_ABOUT) {
        if (ui_rect_hit(BACK_RECT, x, y)) return go_back();
        if (about_paged()) {
            const int turn = tr_pager_hit(ABOUT_PAGER_Y, x, y);
            if (turn) { s_about_page = (s_about_page + 1) % 2; return APP_REDRAW_PAGE; }
            if (s_about_page) return APP_REDRAW_NONE;
        }
        const int dev = about_device_y(), test = about_test_y();
        if (y >= TR_S(320) && y < TR_S(392)) { ctx->request_app = &app_tr_update; return APP_REDRAW_NONE; }
        if (y >= dev - TR_S(20) && y < test) ctx->request_app = &app_home;
        else if (y >= test && y < test + TR_S(80)) ctx->request_app = &app_selftest;
        return APP_REDRAW_NONE;
    }
    if (tr_nav_touch(ctx, x, y, TR_TAB_SETTINGS)) return APP_REDRAW_NONE;
    if (s_page == P_HOME) return touch_home(ctx, y);
    if (ui_rect_hit(BACK_RECT, x, y)) return go_back();
    int id = -1;
    switch (s_page) {
    case P_TABS: return touch_tabs(x, y);
    case P_SOURCES: return touch_sources(ctx, y);
    case P_SRC_TOMATO: id = row_hit(TOMATO_ROWS, s_web_on ? 1 : COUNT(TOMATO_ROWS), y); break;
    case P_SRC_SERVER: id = row_hit(SRV_ROWS, s_web_on ? 1 : SRV_ROW_COUNT, y); break;
    case P_SRC_HUPU:
        if (s_hupu_ask) return touch_hupu_ask(x, y);
        id = row_hit(HUPU_ROWS, COUNT(HUPU_ROWS), y);
        break;
    case P_TRANSFER: return touch_transfer(ctx, y);
    case P_DISPLAY: id = row_hit(DISPLAY_ROWS, COUNT(DISPLAY_ROWS), y); break;
    case P_SYNC: id = row_hit(SYNC_ROWS, s_web_on ? 1 : COUNT(SYNC_ROWS), y); break;
    case P_POWER: id = row_hit(POWER_ROWS, COUNT(POWER_ROWS), y); break;
    case P_TIME: id = row_hit(TIME_ROWS, COUNT(TIME_ROWS), y); break;
    case P_TSET: return touch_tset(x, y);
    case P_TURN: {
        if (y < TURN_TOP || y >= TURN_TOP + APP_TURN_ZONES * TURN_OPT_H) return APP_REDRAW_NONE;
        const uint8_t i = (uint8_t)((y - TURN_TOP) / TURN_OPT_H);
        if (i == app_settings_turn_zone()) return APP_REDRAW_NONE;
        app_settings_set_turn_zone(i);
        return APP_REDRAW_PAGE;
    }
    case P_TZ: {
        const int turn = tr_pager_hit(TZ_PAGER_Y, x, y);
        if (turn && tz_pages() > 1) {
            s_tz_page = (s_tz_page + turn + tz_pages()) % tz_pages();
            return APP_REDRAW_PAGE;
        }
        if (y < TZ_TOP) return APP_REDRAW_NONE;
        const int k = (y - TZ_TOP) / TZ_ROW_H;
        if (k >= tz_per_page()) return APP_REDRAW_NONE;
        const int i = s_tz_page * tz_per_page() + k;
        if (i >= app_settings_tz_count()) return APP_REDRAW_NONE;
        app_settings_set_tz((uint8_t)i);
        tr_time_apply_settings();
        s_page = P_TIME;
        snprintf(s_message, sizeof(s_message), "时区已改为%s", app_settings_tz_name((uint8_t)i));
        return APP_REDRAW_PAGE;
    }
    default: break;
    }
    return id >= 0 ? touch_row(ctx, id) : APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    if (s_edit != E_NONE) { s_edit = E_NONE; memset(&s_kbd, 0, sizeof(s_kbd)); return APP_REDRAW_PAGE; }
    return s_page == P_HOME ? APP_REDRAW_NONE : go_back();
}

/* ---- 生命周期 / Lifecycle ---- */

void app_tr_settings_request_source(int tab) { s_open_source = tab; }
void app_tr_settings_request_transfer(void) { s_open_transfer = true; }

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_edit = E_NONE;
    s_page = s_open_transfer ? P_TRANSFER : P_HOME;
    if (s_open_source == TR_TAB_TOMATO) {
        s_page = P_SRC_TOMATO;
    } else if (s_open_source == TR_TAB_XHS || s_open_source == TR_TAB_WEIBO || s_open_source == TR_TAB_X ||
               s_open_source == TR_TAB_COMIC) {
        s_srv = s_open_source == TR_TAB_XHS ? S_RED : s_open_source == TR_TAB_WEIBO ? S_WB :
                s_open_source == TR_TAB_X ? S_X : S_WT;
        s_page = P_SRC_SERVER;
    } else if (s_open_source == TR_TAB_HUPU) {
        s_page = P_SRC_HUPU;
    } else if (s_open_source >= 0) {
        s_page = P_SOURCES;
    }
    s_open_source = -1;
    s_open_transfer = false;
    s_hupu_ask = false;
    s_message[0] = 0;
    // 离开期间同步可能已结束。/ The sync may have finished while away.
    if (s_syncing && !tomato_sync_busy()) s_syncing = false;
    if (s_testing && !tomato_sync_busy()) s_testing = false;
    load();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    web_stop();
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_dav, 0, sizeof(s_dav));
    memset(s_host, 0, sizeof(s_host));
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_edit = E_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    if (s_edit == E_NONE && s_page == P_SYNC) return (EpdRect){0, TR_S(SYNC_ROWS[2].y), TR_W, ROW_H};
    if (s_edit == E_NONE && s_page == P_SRC_SERVER) return (EpdRect){0, TR_S(SERVER_ROWS[3].y), TR_W, ROW_H};
    return (EpdRect){0, TR_S(160), TR_W, TR_H - TR_S(160)};
}

// 网页填写：等联网、等提交；提交后停服并测试连接。/ Web form: wait for the network and a submission; then stop
// serving and test the connection.
static app_redraw_t tick_web(void) {
    read_pico_transfer_service_poll();
    read_pico_transfer_status_t next;
    read_pico_transfer_get_status(&next);
    const bool net_changed = next.network_ready != s_web_status.network_ready || strcmp(next.url, s_web_status.url) ||
                             next.state != s_web_status.state;
    s_web_status = next;
    if (web_rev() != s_web_revision) {
        if (!read_pico_transfer_try_stop_if_idle()) return APP_REDRAW_NONE;
        s_web_on = false;
        ui_wifi_qr_clear();
        app_loop_hold_awake(false);
        load();
        snprintf(s_message, sizeof(s_message), "已收到网页填写的设置");
        // 番茄和 WebDAV 没有测试连接这一行，存好就行。/ Tomato and WebDAV have no connection-test row; saving is enough.
        if (!s_web_tomato && !s_web_dav) start_test(s_web_srv);
        return APP_REDRAW_PAGE;
    }
    if (net_changed) {
        web_prepare_qr();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

// 同步或测试进行中只刷新那一行；结束后整页显示结果。/ While syncing or testing, refresh only that row; when done,
// show the result with a page refresh.
static app_redraw_t on_tick(app_ctx_t* ctx) {
    tr_time_poll();
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    if (s_web_on) {
        const app_redraw_t r = tick_web();
        return (s_page == P_SRC_SERVER || s_page == P_SRC_TOMATO || s_page == P_SYNC) && s_edit == E_NONE ? r : APP_REDRAW_NONE;
    }
    if (!s_syncing && !s_testing) return APP_REDRAW_NONE;
    tomato_status_t st;
    tomato_sync_get_status(&st);
    if (!tomato_sync_busy()) {
        if (s_syncing) snprintf(s_message, sizeof(s_message), "%s", st.message);
        if (s_testing)
            snprintf(s_test_result[s_test_srv], sizeof(s_test_result[0]), "%s", st.message[0] ? st.message : "失败");
        s_syncing = s_testing = false;
        load();
        return s_edit == E_NONE ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (st.revision == s_seen_revision || s_edit != E_NONE || s_page == P_ABOUT) return APP_REDRAW_NONE;
    s_seen_revision = st.revision;
    if (!(s_syncing && s_page == P_SYNC)) return APP_REDRAW_NONE;
    render(ctx, ctx->fb);
    return APP_REDRAW_AREA;
}

const app_desc_t app_tr_settings = {
    .title = "设置",
    .detail = "显示、标签、Wi-Fi、内容来源、同步与设备",
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
