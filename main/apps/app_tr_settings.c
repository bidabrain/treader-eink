/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 设置：首页六个大分类（显示与阅读、底部标签、Wi-Fi 与传书、内容来源、同步、电源与设备），点进去是二级页。
 * 二级页里：字体、壁纸、睡眠、传书、TF 卡、概览等跳到各自页面；底部标签开关、番茄与 WebDAV 的地址在本页用键盘
 * 编辑；内容来源再分番茄、小红书、微博三页，小红书和微博共用一个服务器页（地址、令牌、测试），还能打开“手机网页
 * 填写”，用传书的网页服务收地址和令牌；“关于 Treader”在本页显示。
 *
 * English: Settings. The home lists six categories (display and reading, tab bar, Wi-Fi and transfer, content sources,
 * sync, power and device); each opens a second-level page. There: font, wallpaper, sleep, transfer, TF card and
 * overview jump to their own pages; tab switches and the Tomato and WebDAV addresses are edited here with the keyboard;
 * content sources split into Tomato, Xiaohongshu and Weibo pages, the last two sharing one server page (address,
 * token, test) that can also serve a phone web form (through the transfer web service) for the address and token;
 * "About Treader" shows here.
 *
 * 冻结：书库和设置两个标签不能隐藏，最多同时显示 TR_TABS_SHOWN_MAX 个；网页填写只收当前服务器页的地址和令牌，
 * 离开本页或关掉开关就停；令牌从不显示原值。
 * Frozen: the library and settings tabs cannot be hidden and at most TR_TABS_SHOWN_MAX tabs show; the web form takes
 * only the current server page's address and token and stops when the page is left or the switch is turned off; the
 * token is never shown.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
#define CAT_TOP 170
#define CAT_H 136
#define ROW_H 72
#define BACK_RECT (EpdRect){0, 70, 320, 90}

extern const app_desc_t app_font_pick;
extern const app_desc_t app_sleep;
extern const app_desc_t app_transfer;
extern const app_desc_t app_usb;
extern const app_desc_t app_home;
extern const app_desc_t app_selftest;
extern const app_desc_t app_tr_wall;
extern const app_desc_t app_tr_card;
extern const app_desc_t app_tr_update;

extern const char server_form_start[] asm("_binary_server_form_html_start");
extern const char server_form_end[] asm("_binary_server_form_html_end");

typedef enum {
    P_HOME, P_DISPLAY, P_TABS, P_TRANSFER, P_SOURCES, P_SRC_TOMATO, P_SRC_SERVER, P_SYNC, P_POWER, P_ABOUT
} page_t;

// 自建服务器页：小红书、微博和 X 共用，按 s_srv 选。/ Self-hosted server page shared by Xiaohongshu, Weibo and X,
// picked by s_srv.
typedef enum { S_RED, S_WB, S_X, S_COUNT } srv_t;

static const struct {
    const char* name;      // 页面和提示里的称呼 / Name in titles and hints
    const char* panel;     // 服务器面板叫什么 / What the server panel is called
    const char* example;   // 地址示例 / Address example
    tr_icon_t icon;
    esp_err_t (*load)(tomato_srv_config_t* out);
    esp_err_t (*save)(const tomato_srv_config_t* cfg);
    uint32_t (*revision)(void);
    int (*list)(void);                 // 列表条数 / List size
    esp_err_t (*list_set)(int n);
    const char* unit;                  // 条数的量词 / Counter word for the list size
} SERVERS[S_COUNT] = {
    [S_RED] = {"小红书", "rednote", "http://192.168.1.20:8091", TR_ICON_XHS, tomato_red_config_load,
               tomato_red_config_save, tomato_red_config_revision, tomato_red_list_limit, tomato_red_list_limit_set,
               "篇"},
    [S_WB] = {"微博", "weibo", "http://192.168.1.20:8092", TR_ICON_WEIBO, tomato_wb_config_load,
              tomato_wb_config_save, tomato_wb_config_revision, tomato_wb_list_limit, tomato_wb_list_limit_set, "条"},
    [S_X] = {"X", "x", "http://192.168.1.20:8093", TR_ICON_X, tomato_x_config_load, tomato_x_config_save,
             tomato_x_config_revision, tomato_x_list_limit, tomato_x_list_limit_set, "条"},
};

// 首页分类。/ Home categories.
typedef enum { C_DISPLAY, C_TABS, C_WIFI, C_SOURCES, C_SYNC, C_POWER, C_COUNT } cat_t;

// 二级页的行：y 是行顶；section 非空时在上方画分组名。/ Sub-page rows: y is the row top; section draws a group name.
typedef struct {
    int id;
    const char* section;
    const char* label;
    int y;
} row_t;

enum {
    R_FONT, R_WALL,
    R_SRV, R_STORE, R_WEB, R_URL, R_TOKEN, R_TEST, R_LIST,
    R_DAV, R_DAV_SYNC,
    R_SLEEP, R_IDLE, R_PIN, R_CARD, R_ABOUT,
};

static const row_t DISPLAY_ROWS[] = {
    {R_FONT, NULL, "字体", 190},
    {R_WALL, NULL, "锁屏壁纸", 262},
};
static const row_t TOMATO_ROWS[] = {
    {R_SRV, NULL, "番茄服务器", 190},
    {R_STORE, NULL, "书城榜单", 262},
};
static const row_t SERVER_ROWS[] = {
    {R_WEB, NULL, "手机网页填写", 190},
    {R_URL, NULL, "服务器地址", 274},
    {R_TOKEN, NULL, "设备令牌", 346},
    {R_TEST, NULL, "测试连接", 418},
    {R_LIST, NULL, "列表条数", 490},
};
static const row_t SYNC_ROWS[] = {
    {R_DAV, NULL, "WebDAV", 190},
    {R_DAV_SYNC, NULL, "立即同步", 262},
};
static const row_t POWER_ROWS[] = {
    {R_SLEEP, NULL, "睡眠与锁屏", 190},
    {R_IDLE, NULL, "自动锁屏", 262},
    {R_PIN, NULL, "锁屏密码", 334},
    {R_CARD, NULL, "TF 卡", 406},
    {R_ABOUT, NULL, "关于 Treader", 478},
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define WEB_ROW_H 84
#define WEB_BOX (EpdRect){TR_MARGIN, 284, TR_W - 2 * TR_MARGIN, 480}
// 内容来源页：番茄加各个自建服务器。/ Content sources page: Tomato plus each self-hosted server.
#define SOURCES (1 + S_COUNT)

// 底部标签页：每个标签一行。/ Tab page: one row per tab.
#define TAB_TOP 270
#define TAB_H 90

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
    case R_FONT: font_name(tmp, sizeof(tmp)); snprintf(out, cap, "%s ›", tmp); break;
    case R_WALL: wall_name(tmp, sizeof(tmp)); snprintf(out, cap, "%s ›", tmp); break;
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
    case R_CARD: card_value(tmp, sizeof(tmp)); snprintf(out, cap, "%s ›", tmp); break;
    case R_ABOUT: snprintf(out, cap, "版本、开源致谢 ›"); break;
    default: out[0] = 0; break;
    }
}

/* ---- 网页填写 / Web form ---- */

// HTTP 任务里调用：GET 返回地址和“有没有令牌”，POST 保存。令牌从不返回。
// Runs on the HTTP task: GET returns the address and whether a token exists; POST saves. The token never goes out.
static int web_form(const char* body, size_t len, char* reply, size_t cap) {
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
    const esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) {
        snprintf(s_message, sizeof(s_message), "打不开网页服务（%s）", esp_err_to_name(err));
        return;
    }
    s_web_on = true;
    s_web_revision = SERVERS[s_web_srv].revision();
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
    } else {
        const tomato_x_req_t req = {.job = TOMATO_X_TEST};
        err = tomato_sync_start_x(&req);
    }
    if (err == ESP_OK) {
        s_testing = true;
        s_test_srv = which;
        s_test_result[which][0] = 0;
    }
}

/* ---- 绘制 / Drawing ---- */

static void draw_back_title(uint8_t* fb, const char* title) {
    tr_text(fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 96, 100, 36, title, EPD_DRAW_ALIGN_LEFT, true);
}

static void draw_message(uint8_t* fb) {
    if (s_message[0]) tr_message(fb, TR_W - TR_MARGIN, 104, 92, 360, 26, s_message, EPD_DRAW_ALIGN_RIGHT, false);
}

static void draw_cat(uint8_t* fb, int y, tr_icon_t icon, const char* name, const char* summary, bool last);

static void cat_summary(cat_t c, char* out, size_t cap) {
    char a[120], b[120];
    switch (c) {
    case C_DISPLAY:
        font_name(a, sizeof(a));
        wall_name(b, sizeof(b));
        snprintf(out, cap, "%.60s · 锁屏壁纸 %.50s", a, b);
        break;
    case C_TABS: {
        int n = 0, len = 0;
        len = snprintf(out, cap, "显示 %d 个：", tr_tab_count());
        for (int t = 0; t < TR_TAB_MAX && len < (int)cap; t++) {
            if (!tr_tab_shown((tr_tab_t)t)) continue;
            len += snprintf(out + len, cap - (size_t)len, "%s%s", n++ ? "、" : "", tr_tab_label((tr_tab_t)t));
        }
        break;
    }
    case C_WIFI: snprintf(out, cap, "Wi-Fi 传书%s%s · USB 传书", s_wifi[0] ? "：" : "", s_wifi); break;
    case C_SOURCES:
        snprintf(out, cap, "番茄%s · 小红书%s · 微博%s · X%s", !s_has_cfg ? "未设置" : s_cfg.builtin ? "内置" : "已设置",
                 s_has_host[S_RED] ? "已设置" : "未设置", s_has_host[S_WB] ? "已设置" : "未设置",
                 s_has_host[S_X] ? "已设置" : "未设置");
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
    static const char* const NAMES[C_COUNT] = {"显示与阅读", "底部标签", "传书", "内容来源", "同步", "电源与设备"};
    static const tr_icon_t ICONS[C_COUNT] = {TR_ICON_FONT, TR_ICON_TABS, TR_ICON_WIFI, TR_ICON_SOURCE, TR_ICON_SYNC,
                                             TR_ICON_POWER};
    tr_title(fb, "设置");
    draw_message(fb);
    for (int i = 0; i < C_COUNT; i++) {
        char sum[200];
        cat_summary((cat_t)i, sum, sizeof(sum));
        draw_cat(fb, CAT_TOP + i * CAT_H, ICONS[i], NAMES[i], sum, i + 1 == C_COUNT);
    }
    const esp_app_desc_t* app = esp_app_get_description();
    char ver[48];
    snprintf(ver, sizeof(ver), "Treader %.20s", app->version);
    tr_text(fb, TR_W / 2, 1030, 24, ver, EPD_DRAW_ALIGN_CENTER, false);
}

// 一行大分类（首页和传书页共用）。/ One large category row (home and the transfer page share it).
static void draw_cat(uint8_t* fb, int y, tr_icon_t icon, const char* name, const char* summary, bool last) {
    const EpdRect box = {TR_MARGIN, y + 30, 76, 76};
    ui_draw_round_rect(fb, box, 18, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(box, 1), 17, UI_GRAY_BLACK);
    tr_icon(fb, icon, box.x + 38, box.y + 38, false);
    tr_text(fb, TR_MARGIN + 104, y + 24, 34, name, EPD_DRAW_ALIGN_LEFT, true);
    char sum[200];
    snprintf(sum, sizeof(sum), "%s", summary);
    tr_fit(sum, 26, TR_W - 2 * TR_MARGIN - 104 - 40);
    tr_text(fb, TR_MARGIN + 104, y + 78, 26, sum, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, y + 46, 34, "›", EPD_DRAW_ALIGN_RIGHT, false);
    if (!last) tr_hline(fb, y + CAT_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static void draw_transfer(uint8_t* fb) {
    draw_back_title(fb, "传书");
    draw_message(fb);
    char wifi[80];
    snprintf(wifi, sizeof(wifi), "手机或电脑浏览器上传%s%s", s_wifi[0] ? " · " : "", s_wifi);
    draw_cat(fb, CAT_TOP, TR_ICON_WIFI, "Wi-Fi 传书", wifi, false);
    draw_cat(fb, CAT_TOP + CAT_H, TR_ICON_USB, "USB 传书",
             tomato_sync_busy() && !tomato_sync_preemptible() ? "后台正在下载或同步，完成后再用" : "连电脑当 U 盘，拷大文件更方便", true);
    tr_wrap(fb, TR_MARGIN, CAT_TOP + 2 * CAT_H + 28, TR_W - 2 * TR_MARGIN, 24, 36,
            "Wi-Fi 传书一边传一边能用设备，只收书、字体和漫画；USB 传书整张卡都能管理，但传输期间设备暂停，结束后重启恢复。", 3);
}

static void draw_switch(uint8_t* fb, int right, int y, bool on) {
    const EpdRect r = {right - 100, y, 100, 54};
    if (on) ui_fill_round_rect(fb, r, 27, UI_GRAY_BLACK);
    else {
        ui_draw_round_rect(fb, r, 27, UI_GRAY_BLACK);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), 26, UI_GRAY_BLACK);
        ui_draw_round_rect(fb, ui_inset_rect(r, 2), 25, UI_GRAY_BLACK);
    }
    const int kx = on ? r.x + r.width - 27 : r.x + 27;
    epd_fill_circle(kx, y + 27, 18, UI_GRAY_WHITE, fb);
    if (!on) for (int d = 0; d < 3; d++) epd_draw_circle(kx, y + 27, 18 - d, UI_GRAY_BLACK, fb);
}

// 灰色的关着开关（显示满了，不能再开）。/ A gray off switch (the bar is full, it cannot turn on).
static void draw_switch_gray(uint8_t* fb, int right, int y) {
    const EpdRect r = {right - 100, y, 100, 54};
    ui_draw_round_rect(fb, r, 27, TR_GRAY_LINE);
    ui_draw_round_rect(fb, ui_inset_rect(r, 1), 26, TR_GRAY_LINE);
    epd_draw_circle(r.x + 27, y + 27, 18, TR_GRAY_LINE, fb);
    epd_draw_circle(r.x + 27, y + 27, 17, TR_GRAY_LINE, fb);
}

static void draw_rows(uint8_t* fb, const row_t* rows, int n) {
    for (int i = 0; i < n; i++) {
        const row_t* r = &rows[i];
        if (r->section) tr_text(fb, TR_MARGIN, r->y - 36, 26, r->section, EPD_DRAW_ALIGN_LEFT, false);
        const int h = r->id == R_WEB ? WEB_ROW_H : ROW_H;
        tr_text(fb, TR_MARGIN, r->y + (h - 32) / 2 - 2, 32, r->label, EPD_DRAW_ALIGN_LEFT, r->id == R_WEB && s_web_on);
        if (r->id == R_WEB) {
            draw_switch(fb, TR_W - TR_MARGIN, r->y + 15, s_web_on);
        } else {
            char value[160];
            row_value(r->id, value, sizeof(value));
            tr_fit(value, 28, TR_W - 2 * TR_MARGIN - 220);
            tr_text(fb, TR_W - TR_MARGIN, r->y + (h - 28) / 2, 28, value, EPD_DRAW_ALIGN_RIGHT, false);
        }
        const bool last = i + 1 == n || rows[i + 1].section;
        if (!last && !(r->id == R_WEB && s_web_on)) tr_hline(fb, r->y + h - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
}

static void draw_web_box(uint8_t* fb) {
    const EpdRect b = WEB_BOX;
    ui_draw_round_rect(fb, b, 18, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(b, 1), 17, UI_GRAY_BLACK);
    const int tx = b.x + 268, tw = b.x + b.width - 32 - tx;
    char line[160];
    if (!s_web_status.network_ready) {
        tr_wrap(fb, b.x + 32, b.y + 40, b.width - 64, 28, 42,
                s_web_status.state == READ_PICO_TRANSFER_ERROR ? "连不上 Wi-Fi。检查“Wi-Fi 与传书”里保存的网络，或关掉开关再试。"
                                                               : "正在打开网页服务…", 3);
    } else {
        ui_wifi_qr_draw(fb, (EpdRect){b.x + 28, b.y + 28, 220, 220});
        if (s_web_status.mode == READ_PICO_TRANSFER_MODE_STA) {
            snprintf(line, sizeof(line), "手机连上同一个 Wi-Fi（%s），扫码或在浏览器打开：", s_web_status.wifi_ssid);
            tr_wrap(fb, tx, b.y + 36, tw, 26, 38, line, 3);
            tr_text(fb, tx, b.y + 168, 32, host_of(s_web_status.url), EPD_DRAW_ALIGN_LEFT, true);
        } else {
            snprintf(line, sizeof(line), "扫码连上设备热点 %s（口令 %s），再在浏览器打开：", s_web_status.ssid,
                     READ_PICO_TRANSFER_PASSWORD);
            tr_wrap(fb, tx, b.y + 36, tw, 26, 38, line, 4);
            tr_text(fb, tx, b.y + 196, 32, host_of(READ_PICO_TRANSFER_URL), EPD_DRAW_ALIGN_LEFT, true);
        }
    }
    tr_hline(fb, b.y + 270, b.x + 32, b.width - 64, true);
    tr_text(fb, b.x + 32, b.y + 290, 28, s_web_note[0] ? s_web_note : "等待网页提交…", EPD_DRAW_ALIGN_LEFT, false);
    tr_wrap(fb, b.x + 32, b.y + 342, b.width - 64, 24, 36,
            s_web_status.mode == READ_PICO_TRANSFER_MODE_AP
                ? "设备没有保存 Wi-Fi，开的是自己的热点；提交后要连回家里网络才能测试。关掉开关或离开这一页就停止。"
                : "提交后自动保存并测试连接。关掉开关或离开这一页就停止，网页随之失效。", 3);
}

static void draw_tabs(uint8_t* fb) {
    const bool full = tr_tab_count() >= TR_TABS_SHOWN_MAX;
    draw_back_title(fb, "底部标签");
    draw_message(fb);
    tr_wrap(fb, TR_MARGIN, 176, TR_W - 2 * TR_MARGIN, 26, 38,
            "关掉的标签不出现在底部和菜单里，功能入口关闭；已有的内容和设置都保留，打开就回来。", 2);
    for (int t = 0; t < TR_TAB_MAX; t++) {
        const int y = TAB_TOP + t * TAB_H;
        tr_icon(fb, tr_tab_icon((tr_tab_t)t), TR_MARGIN + 20, y + TAB_H / 2, false);
        tr_text(fb, TR_MARGIN + 62, y + (TAB_H - 32) / 2 - 2, 32, tr_tab_label((tr_tab_t)t), EPD_DRAW_ALIGN_LEFT, false);
        if (TR_TABS_FIXED & (1u << t))
            tr_text(fb, TR_W - TR_MARGIN, y + (TAB_H - 28) / 2, 28, "固定", EPD_DRAW_ALIGN_RIGHT, false);
        else if (!tr_tab_shown((tr_tab_t)t) && full)
            // 已经显示满了：关着的开关画成灰的，点了不开。/ Already full: off switches are gray and do not turn on.
            draw_switch_gray(fb, TR_W - TR_MARGIN, y + (TAB_H - 54) / 2);
        else
            draw_switch(fb, TR_W - TR_MARGIN, y + (TAB_H - 54) / 2, tr_tab_shown((tr_tab_t)t));
        if (t + 1 < TR_TAB_MAX) tr_hline(fb, y + TAB_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_text(fb, TR_MARGIN, TAB_TOP + TR_TAB_MAX * TAB_H + 30, 26, "下面的标签栏就是效果，开关一下整屏刷新一次。",
            EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_sources(uint8_t* fb) {
    draw_back_title(fb, "内容来源");
    draw_message(fb);
    char sum[200];
    snprintf(sum, sizeof(sum), "%.150s · 书城榜单", !s_has_cfg ? "番茄服务器未设置" : s_cfg.builtin ? "内置服务器" :
             host_of(s_cfg.url));
    draw_cat(fb, CAT_TOP, TR_ICON_TOMATO, "番茄小说", sum, false);
    for (int i = 0; i < S_COUNT; i++) {
        snprintf(sum, sizeof(sum), "%s 服务器 · %.150s", SERVERS[i].panel,
                 s_has_host[i] ? host_of(s_host[i].url) : "未设置");
        draw_cat(fb, CAT_TOP + (1 + i) * CAT_H, SERVERS[i].icon, SERVERS[i].name, sum, i + 1 == S_COUNT);
    }
}

static void draw_server(uint8_t* fb) {
    draw_back_title(fb, SERVERS[s_srv].name);
    draw_message(fb);
    if (s_web_on) {
        draw_rows(fb, SERVER_ROWS, 1);
        draw_web_box(fb);
        return;
    }
    draw_rows(fb, SERVER_ROWS, COUNT(SERVER_ROWS));
    char hint[200];
    snprintf(hint, sizeof(hint), "地址和令牌在 %s 面板的“设备与外网”页。令牌很长，建议打开“手机网页填写”，在手机上粘贴。",
             SERVERS[s_srv].panel);
    tr_wrap(fb, TR_MARGIN, 582, TR_W - 2 * TR_MARGIN, 24, 36, hint, 3);
    tr_wrap(fb, TR_MARGIN, 700, TR_W - 2 * TR_MARGIN, 24, 36,
            "列表条数：100、200、300 轮换，下次刷新列表时生效。条数越多，TF 卡上存的缩略图越多，第一次刷新也越慢。", 3);
}

static void draw_edit(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    char title[48];
    snprintf(title, sizeof(title), "%s%s", s_edit == E_HOST_URL || s_edit == E_HOST_TOKEN ? SERVERS[s_srv].name : "",
             EDIT[s_edit].title);
    tr_text(fb, 96, 100, 36, title, EPD_DRAW_ALIGN_LEFT, true);
    EpdRect f = {TR_MARGIN, 176, TR_W - 2 * TR_MARGIN, 84};
    ui_draw_round_rect(fb, f, 14, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(f, 1), 13, UI_GRAY_BLACK);
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
    const char* tail = tr_tail_fit(shown, 32, f.width - 48);
    tr_text(fb, f.x + 20, f.y + 22, 32, tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + 22 + ttf_text_width_px(32, tail), f.y + 18, 3, 46}, UI_GRAY_BLACK, fb);
    const char* hint = EDIT[s_edit].hint;
    char host_hint[160];
    if (s_edit == E_HOST_URL) {
        snprintf(host_hint, sizeof(host_hint), "%s 面板“设备与外网”里的地址，如 %s", SERVERS[s_srv].panel,
                 SERVERS[s_srv].example);
        hint = host_hint;
    }
    if (s_edit == E_SRV_URL && tomato_config_has_builtin()) hint = "留空使用内置服务器；自建的填 http://地址:端口";
    if (s_message[0]) hint = s_message;   // 输入不对时的提示 / Why the input was refused
    tr_wrap(fb, TR_MARGIN, 286, TR_W - 2 * TR_MARGIN, 26, 36, hint, 2);
    tr_kbd_draw(fb, &s_kbd);
}

/* ---- 关于 / About ---- */

#define ABOUT_DEVICE_Y 1030
#define ABOUT_TEST_Y 1106

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
    tr_text(fb, TR_MARGIN, y, 30, label, EPD_DRAW_ALIGN_LEFT, false);
    char v[96];
    snprintf(v, sizeof(v), "%s", value);
    tr_fit(v, 28, TR_W - 2 * TR_MARGIN - 160);
    tr_text(fb, TR_W - TR_MARGIN, y + 2, 28, v, EPD_DRAW_ALIGN_RIGHT, false);
}

static void draw_about(uint8_t* fb) {
    const esp_app_desc_t* app = esp_app_get_description();
    char line[64];
    draw_back_title(fb, "关于 Treader");
    tr_text(fb, TR_MARGIN, 184, 60, "Treader", EPD_DRAW_ALIGN_LEFT, true);
    // 一行放不下（约 730 px），折成两行。/ Too wide for one line (~730 px); wrap to two.
    tr_wrap(fb, TR_MARGIN, 256, TR_W - 2 * TR_MARGIN, 28, 34, "小纸 Pico 阅读固件 · 书库、番茄小说、WebDAV 同步", 2);
    snprintf(line, sizeof(line), "%.20s · TF 卡升级 ›", app->version);
    about_row(fb, 336, "版本", line);
    tr_hline(fb, 390, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    snprintf(line, sizeof(line), "%s %s UTC", app->date, app->time);
    about_row(fb, 410, "构建时间", line);
    tr_hline(fb, 464, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    about_row(fb, 484, "项目主页", "github.com/bidabrain/treader-eink");
    tr_text(fb, TR_MARGIN, 584, 26, "开源致谢", EPD_DRAW_ALIGN_LEFT, false);
    int y = 628;
    for (size_t i = 0; i < sizeof(CREDITS) / sizeof(CREDITS[0]); i++, y += 42) {
        char credit[96];
        snprintf(credit, sizeof(credit), "%s", CREDITS[i]);
        tr_fit(credit, 28, TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_MARGIN, y, 28, credit, EPD_DRAW_ALIGN_LEFT, false);
    }
    tr_wrap(fb, TR_MARGIN, y + 16, TR_W - 2 * TR_MARGIN, 26, 38,
            "非官方固件，与 MindReset、番茄小说没有隶属关系。各组件按各自许可使用，详见项目仓库。", 2);
    tr_hline(fb, ABOUT_DEVICE_Y - 20, TR_MARGIN, TR_W - 2 * TR_MARGIN, false);
    tr_text(fb, TR_MARGIN, ABOUT_DEVICE_Y + 10, 32, "设备信息", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, ABOUT_DEVICE_Y + 14, 28, "电量、存储、芯片 ›", EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, ABOUT_TEST_Y - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    tr_text(fb, TR_MARGIN, ABOUT_TEST_Y + 10, 32, "设备自检", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_W - TR_MARGIN, ABOUT_TEST_Y + 14, 28, "屏幕、触摸、电源 ›", EPD_DRAW_ALIGN_RIGHT, false);
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
        draw_rows(fb, TOMATO_ROWS, COUNT(TOMATO_ROWS));
        break;
    case P_SRC_SERVER: draw_server(fb); break;
    case P_SYNC: draw_back_title(fb, "同步"); draw_message(fb); draw_rows(fb, SYNC_ROWS, COUNT(SYNC_ROWS)); break;
    case P_POWER:
        draw_back_title(fb, "电源与设备");
        draw_message(fb);
        draw_rows(fb, POWER_ROWS, COUNT(POWER_ROWS));
        break;
    case P_ABOUT: draw_about(fb); return;
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
    if (y < 160 && x < 120) { s_edit = E_NONE; memset(&s_kbd, 0, sizeof(s_kbd)); return APP_REDRAW_PAGE; }
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
    if (s_page == P_SRC_SERVER) web_stop();
    s_page = s_page == P_ABOUT ? P_POWER : s_page == P_SRC_TOMATO || s_page == P_SRC_SERVER ? P_SOURCES : P_HOME;
    return APP_REDRAW_PAGE;
}

static app_redraw_t touch_sources(uint16_t y) {
    if (y < CAT_TOP || y >= CAT_TOP + SOURCES * CAT_H) return APP_REDRAW_NONE;
    const int i = (y - CAT_TOP) / CAT_H;
    if (i == 0) return open_page(P_SRC_TOMATO);
    s_srv = (srv_t)(i - 1);
    return open_page(P_SRC_SERVER);
}

static app_redraw_t touch_home(app_ctx_t* ctx, uint16_t y) {
    if (y < CAT_TOP || y >= CAT_TOP + C_COUNT * CAT_H) return APP_REDRAW_NONE;
    switch ((cat_t)((y - CAT_TOP) / CAT_H)) {
    case C_DISPLAY: return open_page(P_DISPLAY);
    case C_TABS: return open_page(P_TABS);
    case C_WIFI: return open_page(P_TRANSFER);
    case C_SOURCES: return open_page(P_SOURCES);
    case C_SYNC: return open_page(P_SYNC);
    case C_POWER: return open_page(P_POWER);
    default: return APP_REDRAW_NONE;
    }
}

static int row_hit(const row_t* rows, int n, uint16_t y) {
    for (int i = 0; i < n; i++) {
        const int h = rows[i].id == R_WEB ? WEB_ROW_H : ROW_H;
        if (y >= rows[i].y && y < rows[i].y + h) return rows[i].id;
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

static app_redraw_t touch_tabs(uint16_t y) {
    if (y < TAB_TOP || y >= TAB_TOP + TR_TAB_MAX * TAB_H) return APP_REDRAW_NONE;
    const int t = (y - TAB_TOP) / TAB_H;
    if (TR_TABS_FIXED & (1u << t)) return APP_REDRAW_NONE;
    if (!tr_tab_shown((tr_tab_t)t) && tr_tab_count() >= TR_TABS_SHOWN_MAX) {
        snprintf(s_message, sizeof(s_message), "最多显示 %d 个标签，先关掉一个", TR_TABS_SHOWN_MAX);
        return APP_REDRAW_PAGE;
    }
    s_message[0] = 0;
    const uint8_t mask = app_settings_hidden_tabs() ^ (uint8_t)(1u << t);
    app_settings_set_hidden_tabs(mask);
    tr_tabs_set_hidden(mask);
    // 标签栏整条变了，整屏刷新。/ The whole tab bar changes, so refresh the full screen.
    return APP_REDRAW_FULL;
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
    case R_SLEEP: ctx->request_app = &app_sleep; return APP_REDRAW_NONE;
    case R_IDLE:
        app_settings_set_lock_idle((uint8_t)((app_settings_lock_idle() + 1) % APP_LOCK_IDLE_LEVELS));
        return APP_REDRAW_PAGE;
    case R_PIN: return touch_pin(ctx);
    case R_CARD: ctx->request_app = &app_tr_card; return APP_REDRAW_NONE;
    case R_ABOUT: return open_page(P_ABOUT);
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
    case R_LIST: {
        const int n = SERVERS[s_srv].list();
        SERVERS[s_srv].list_set(n >= TOMATO_LIST_LIMIT_MAX ? TOMATO_LIST_LIMIT_MIN : n + 100);
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
        if (y >= 320 && y < 392) { ctx->request_app = &app_tr_update; return APP_REDRAW_NONE; }
        if (y >= ABOUT_DEVICE_Y - 20 && y < ABOUT_TEST_Y) ctx->request_app = &app_home;
        else if (y >= ABOUT_TEST_Y && y < ABOUT_TEST_Y + 80) ctx->request_app = &app_selftest;
        return APP_REDRAW_NONE;
    }
    if (tr_nav_touch(ctx, x, y, TR_TAB_SETTINGS)) return APP_REDRAW_NONE;
    if (s_page == P_HOME) return touch_home(ctx, y);
    if (ui_rect_hit(BACK_RECT, x, y)) return go_back();
    int id = -1;
    switch (s_page) {
    case P_TABS: return touch_tabs(y);
    case P_SOURCES: return touch_sources(y);
    case P_SRC_TOMATO: id = row_hit(TOMATO_ROWS, COUNT(TOMATO_ROWS), y); break;
    case P_SRC_SERVER: id = row_hit(SERVER_ROWS, s_web_on ? 1 : COUNT(SERVER_ROWS), y); break;
    case P_TRANSFER: return touch_transfer(ctx, y);
    case P_DISPLAY: id = row_hit(DISPLAY_ROWS, COUNT(DISPLAY_ROWS), y); break;
    case P_SYNC: id = row_hit(SYNC_ROWS, COUNT(SYNC_ROWS), y); break;
    case P_POWER: id = row_hit(POWER_ROWS, COUNT(POWER_ROWS), y); break;
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
    } else if (s_open_source == TR_TAB_XHS || s_open_source == TR_TAB_WEIBO || s_open_source == TR_TAB_X) {
        s_srv = s_open_source == TR_TAB_XHS ? S_RED : s_open_source == TR_TAB_WEIBO ? S_WB : S_X;
        s_page = P_SRC_SERVER;
    } else if (s_open_source >= 0) {
        s_page = P_SOURCES;
    }
    s_open_source = -1;
    s_open_transfer = false;
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
    if (s_edit == E_NONE && s_page == P_SYNC) return (EpdRect){0, SYNC_ROWS[1].y, TR_W, ROW_H};
    if (s_edit == E_NONE && s_page == P_SRC_SERVER) return (EpdRect){0, SERVER_ROWS[3].y, TR_W, ROW_H};
    return (EpdRect){0, 160, TR_W, TR_H - 160};
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
    if (SERVERS[s_web_srv].revision() != s_web_revision) {
        if (!read_pico_transfer_try_stop_if_idle()) return APP_REDRAW_NONE;
        s_web_on = false;
        ui_wifi_qr_clear();
        app_loop_hold_awake(false);
        load();
        snprintf(s_message, sizeof(s_message), "已收到网页填写的设置");
        start_test(s_web_srv);
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
        return s_page == P_SRC_SERVER && s_edit == E_NONE ? r : APP_REDRAW_NONE;
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
