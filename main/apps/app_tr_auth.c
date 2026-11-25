/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：“验证器”功能页。按 RFC 6238 离线显示两步验证码；口令在屏幕键盘上输；账号用设备热点上的网页导入（设备
 * 屏幕上确认后才写入）；长按删除；从“设置 › 内容来源 › 验证器”进来时画这一功能自己的设置（改口令、自动上锁、
 * 清空）。会话、加密文件和 NVS 在 tr_auth。
 *
 * English: the Authenticator feature page. Shows RFC 6238 codes offline; the passphrase is typed on the on-screen
 * keyboard; accounts are imported through a web page on the device hotspot (written only after confirming on the
 * device); a long press deletes; opened from Settings > Content sources > Authenticator it draws this feature's own
 * settings (change passphrase, auto-lock, clear). Session, encrypted file and NVS live in tr_auth.
 *
 * 冻结：验证码区只用黑白（会被 DU 局部刷新）；密钥不出设备，网页只拿到名称；导入和删除要在设备上确认；导入只开
 * 设备热点，不走已有 Wi-Fi；锁屏时由 sleep.c 上锁。
 * Frozen: the code area is black and white only (DU refreshes it); secrets never leave the device and the web page
 * gets names only; imports and deletes need confirmation on the device; import uses the device hotspot only, never
 * an existing Wi-Fi; sleep.c locks on every lock.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "app.h"
#include "app_loop.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "read_pico_transfer.h"
#include "tomato.h"
#include "tr_auth.h"
#include "tr_kbd.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"

extern const char auth_form_start[] asm("_binary_auth_form_html_start");
extern const char auth_form_end[] asm("_binary_auth_form_html_end");
extern const app_desc_t app_tr_auth;
extern const app_desc_t app_tr_settings;
void app_tr_settings_request_source(int tab);

/* ---- 排版 / Layout ---- */

#define BACK_RECT (EpdRect){0, TR_S(70), 320, TR_S(90)}
#define LIST_TOP TR_S(170)
#define BANNER_H TR_S(64)
#define ROW_H TR_S(150)
#define PAGER_Y (TR_TABS_Y - TR_S(84))
#define CODE_PX TR_S(64)
#define BAR_W TR_S(120)
#define BAR_H TR_S(12)
#define BTN_W TR_S(72)        // 标题栏右边的图标按钮 / Icon buttons right of the title
#define SET_ROW_H TR_S(72)
#define SET_TOP TR_S(190)
#define WARN_S 10             // 估计误差超过就提醒校时 / Ask for a sync above this estimated error
#define DOUBT_S 25            // 超过就在码旁边标“可能不准” / Mark codes "may be off" above this
#define NEAR_S 5              // 剩几秒时显示下一个码 / Show the next code in the last seconds

typedef enum { V_MAIN, V_PASS, V_DETAIL, V_IMPORT, V_SET } view_t;
typedef enum { PM_UNLOCK, PM_SET1, PM_SET2, PM_OLD, PM_NEW1, PM_NEW2 } pass_mode_t;
typedef enum { J_NONE, J_UNLOCK, J_SETUP, J_CHANGE } job_t;
typedef enum { D_NONE, D_DELETE, D_WIPE1, D_WIPE2, D_WEB } dialog_t;

static view_t s_view;
static view_t s_pass_back;       // 口令页返回到哪 / Where the passphrase view returns to
static pass_mode_t s_pass;
static job_t s_job;
static dialog_t s_dialog;
EXT_RAM_BSS_ATTR static tr_kbd_t s_kbd;
static char s_first[TR_AUTH_PASS_MAX + 1];   // 第一遍或旧口令 / First entry or the old passphrase
static char s_second[TR_AUTH_PASS_MAX + 1];  // 新口令第一遍 / First entry of the new passphrase
static char s_msg[120];
static int s_page;
static int s_detail = -1;        // 单独显示的账号下标 / Index shown on its own
static int s_delete = -1;        // 要删的账号下标 / Index to delete
static bool s_from_settings;     // 从设置进来 / Opened from Settings
static bool s_open_settings;     // 下次进页打开设置视图 / Open the settings view on next entry
static int64_t s_shown_slot = -1;   // 上次画的 5 秒格 / The 5-second slot last drawn
static bool s_syncing;           // 正在校时 / Time sync running

/* ---- 时间 / Time ---- */

static int64_t now_s(void) { return (int64_t)time(NULL); }
static bool time_ok(void) { return tr_time_valid(); }

/* ---- 导入（网页）/ Import (web) ---- */

typedef enum { W_NONE, W_PENDING, W_DONE, W_CANCELLED, W_FAILED } web_state_t;
typedef enum { WO_NONE, WO_IMPORT, WO_DELETE } web_op_t;

static SemaphoreHandle_t s_web_mu;
static StaticSemaphore_t s_web_mu_buf;
static bool s_web_on;
static char s_ap_pass[16];
static read_pico_transfer_status_t s_web_status;
// 网页送来、等设备确认的请求（HTTP 任务写，主循环读）。/ A request from the page awaiting device confirmation
// (written by the HTTP task, read by the main loop).
static web_op_t s_web_op;
static web_state_t s_web_state;
static int s_web_count;
static char s_web_note[96];
static totp_account_t* s_stage;   // 等确认的导入账号 / Accounts awaiting import confirmation
static int s_stage_n;
static uint32_t s_stage_del;      // 等确认删除的编号 / Id awaiting delete confirmation
static bool s_web_dirty;          // 网页改了账号表，要重画 / The page changed the table; redraw

static void web_lock(void) {
    if (!s_web_mu) s_web_mu = xSemaphoreCreateMutexStatic(&s_web_mu_buf);
    xSemaphoreTake(s_web_mu, portMAX_DELAY);
}
static void web_unlock(void) { xSemaphoreGive(s_web_mu); }

static void stage_free(void) {
    if (s_stage) {
        totp_wipe(s_stage, sizeof(*s_stage) * TOTP_MAX_ACCOUNTS);
        heap_caps_free(s_stage);
    }
    s_stage = NULL;
    s_stage_n = 0;
}

static void reply(char* out, size_t cap, const char* fmt, const char* text) { snprintf(out, cap, fmt, text); }

// 把网页上的一个账号转成结构；不合格返回原因。/ Turn one account from the page into a struct; returns why not.
static const char* parse_account(const cJSON* j, totp_account_t* a) {
    memset(a, 0, sizeof(*a));
    const cJSON* issuer = cJSON_GetObjectItemCaseSensitive(j, "issuer");
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(j, "name");
    const cJSON* secret = cJSON_GetObjectItemCaseSensitive(j, "secret");
    const cJSON* algo = cJSON_GetObjectItemCaseSensitive(j, "algo");
    const cJSON* digits = cJSON_GetObjectItemCaseSensitive(j, "digits");
    const cJSON* period = cJSON_GetObjectItemCaseSensitive(j, "period");
    if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) > TOTP_TEXT_MAX) return "名称不对";
    if (cJSON_IsString(issuer) && strlen(issuer->valuestring) > TOTP_TEXT_MAX) return "发行方太长";
    snprintf(a->name, sizeof(a->name), "%s", name->valuestring);
    snprintf(a->issuer, sizeof(a->issuer), "%s", cJSON_IsString(issuer) ? issuer->valuestring : "");
    size_t n = 0;
    if (!cJSON_IsString(secret) || !totp_base32_decode(secret->valuestring, a->secret, sizeof(a->secret), &n))
        return "密钥不对";
    a->secret_len = (uint8_t)n;
    const char* al = cJSON_IsString(algo) ? algo->valuestring : "SHA1";
    a->algo = !strcmp(al, "SHA256") ? TOTP_SHA256 : !strcmp(al, "SHA512") ? TOTP_SHA512 : !strcmp(al, "SHA1") ? TOTP_SHA1 : 9;
    a->digits = cJSON_IsNumber(digits) ? (uint8_t)digits->valueint : 6;
    a->period = cJSON_IsNumber(period) ? (uint8_t)period->valueint : 30;
    return totp_account_valid(a) ? NULL : "算法、位数或周期不支持";
}

// 清掉 JSON 里的密钥文字再释放。/ Wipe secret strings in the JSON before freeing it.
static void wipe_secrets(cJSON* list) {
    const cJSON* it;
    cJSON_ArrayForEach(it, list) {
        const cJSON* s = cJSON_GetObjectItemCaseSensitive(it, "secret");
        if (cJSON_IsString(s)) totp_wipe(s->valuestring, strlen(s->valuestring));
    }
}

static int web_get(char* out, size_t cap) {
    cJSON* j = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(j, "accounts");
    web_lock();
    const totp_table_t* t = tr_auth_table();
    for (int i = 0; t && i < t->count; i++) {
        cJSON* a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "id", t->items[i].id);
        cJSON_AddStringToObject(a, "issuer", t->items[i].issuer);
        cJSON_AddStringToObject(a, "name", t->items[i].name);
        cJSON_AddItemToArray(list, a);
    }
    cJSON_AddNumberToObject(j, "max", TOTP_MAX_ACCOUNTS);
    cJSON* last = cJSON_AddObjectToObject(j, "last");
    static const char* const ST[] = {"none", "pending", "done", "cancelled", "failed"};
    cJSON_AddStringToObject(last, "op", s_web_op == WO_DELETE ? "delete" : s_web_op == WO_IMPORT ? "import" : "none");
    cJSON_AddStringToObject(last, "state", ST[s_web_state]);
    cJSON_AddNumberToObject(last, "count", s_web_count);
    cJSON_AddStringToObject(last, "msg", s_web_note);
    web_unlock();
    char* text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    int status = 200;
    if (!text || strlen(text) >= cap) {
        snprintf(out, cap, "{\"error\":\"列表太长\"}");
        status = 500;
    } else {
        snprintf(out, cap, "%s", text);
    }
    cJSON_free(text);
    return status;
}

// HTTP 任务里调用。/ Runs on the HTTP task.
static int web_form(const char* body, size_t len, char* out, size_t cap) {
    if (!body) return web_get(out, cap);
    cJSON* j = cJSON_ParseWithLength(body, len);
    const cJSON* op = cJSON_GetObjectItemCaseSensitive(j, "op");
    int status = 400;
    const char* err = NULL;
    char why[96];
    web_lock();
    totp_table_t* t = tr_auth_table();
    if (!cJSON_IsString(op)) {
        err = "请求不对";
    } else if (!t) {
        err = "验证器已上锁，请在设备上解锁后再导入";
    } else if (s_web_state == W_PENDING) {
        status = 409;
        err = "设备上还有一个请求等待确认";
    } else if (!strcmp(op->valuestring, "import")) {
        cJSON* list = cJSON_GetObjectItemCaseSensitive(j, "accounts");
        const int n = cJSON_GetArraySize(list);
        if (!cJSON_IsArray(list) || n < 1) err = "没有要导入的账号";
        else if (t->count + n > TOTP_MAX_ACCOUNTS) err = "超过 100 个账号的上限";
        else if (!(s_stage = heap_caps_calloc(TOTP_MAX_ACCOUNTS, sizeof(*s_stage), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) &&
                 !(s_stage = heap_caps_calloc(TOTP_MAX_ACCOUNTS, sizeof(*s_stage), MALLOC_CAP_8BIT)))
            err = "内存不足";
        else {
            int i = 0;
            const cJSON* it;
            cJSON_ArrayForEach(it, list) {
                const char* bad = parse_account(it, &s_stage[i]);
                if (bad) {
                    snprintf(why, sizeof(why), "第 %d 个账号%s", i + 1, bad);
                    err = why;
                    break;
                }
                i++;
            }
            if (err) stage_free();
            else {
                s_stage_n = i;
                s_web_op = WO_IMPORT;
                s_web_state = W_PENDING;
                s_web_count = i;
                s_web_note[0] = 0;
                status = 200;
            }
        }
        wipe_secrets(list);
    } else {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(j, "id");
        const int i = cJSON_IsNumber(id) ? totp_table_find(t, (uint32_t)id->valuedouble) : -1;
        if (i < 0) {
            err = "设备上没有这个账号";
        } else if (!strcmp(op->valuestring, "delete")) {
            s_stage_del = t->items[i].id;
            s_web_op = WO_DELETE;
            s_web_state = W_PENDING;
            s_web_count = 1;
            s_web_note[0] = 0;
            status = 200;
        } else if (!strcmp(op->valuestring, "rename")) {
            const cJSON* issuer = cJSON_GetObjectItemCaseSensitive(j, "issuer");
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(j, "name");
            if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) > TOTP_TEXT_MAX ||
                (cJSON_IsString(issuer) && strlen(issuer->valuestring) > TOTP_TEXT_MAX)) {
                err = "名称不对";
            } else {
                snprintf(t->items[i].name, sizeof(t->items[i].name), "%s", name->valuestring);
                snprintf(t->items[i].issuer, sizeof(t->items[i].issuer), "%s",
                         cJSON_IsString(issuer) ? issuer->valuestring : "");
                status = tr_auth_save() == TOTP_OK ? 200 : 500;
                if (status != 200) err = "保存失败";
                s_web_dirty = true;
            }
        } else if (!strcmp(op->valuestring, "move")) {
            const cJSON* to = cJSON_GetObjectItemCaseSensitive(j, "to");
            if (!cJSON_IsNumber(to)) {
                err = "请求不对";
            } else {
                totp_table_move(t, i, to->valueint);
                status = tr_auth_save() == TOTP_OK ? 200 : 500;
                if (status != 200) err = "保存失败";
                s_web_dirty = true;
            }
        } else {
            err = "请求不对";
        }
    }
    web_unlock();
    cJSON_Delete(j);
    if (err) reply(out, cap, "{\"error\":\"%s\"}", err);
    else snprintf(out, cap, "{\"ok\":true}");
    return err ? (status == 200 ? 400 : status) : 200;
}

static void web_stop(void) {
    if (!s_web_on) return;
    read_pico_transfer_stop();
    ui_wifi_qr_clear();
    s_web_on = false;
    app_loop_hold_awake(false);
    web_lock();
    if (s_web_state == W_PENDING) s_web_state = W_CANCELLED;
    stage_free();
    web_unlock();
    if (s_dialog == D_WEB) s_dialog = D_NONE;
    totp_wipe(s_ap_pass, sizeof(s_ap_pass));
}

// 每次随机一个热点口令（去掉容易看错的字母）。/ A random hotspot passphrase each time (no easily confused letters).
static void make_ap_pass(void) {
    static const char CH[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t r[12];
    if (!totp_random(r, sizeof(r))) for (size_t i = 0; i < sizeof(r); i++) r[i] = (uint8_t)esp_random();
    for (size_t i = 0; i < sizeof(r); i++) s_ap_pass[i] = CH[r[i] % (sizeof(CH) - 1)];
    s_ap_pass[sizeof(r)] = 0;
}

static void web_start(void) {
    const tomato_preempt_t p = tomato_sync_preempt();
    if (p != TOMATO_PREEMPT_FREE) {
        snprintf(s_msg, sizeof(s_msg), p == TOMATO_PREEMPT_STOPPING ? "正在停下后台加载，请再点一次"
                                                                     : "后台正在下载或同步，完成后再导入");
        return;
    }
    make_ap_pass();
    const read_pico_transfer_cfg_t cfg = {
        .mode = READ_PICO_TRANSFER_MODE_AP,
        .form_html = auth_form_start,
        .form_html_len = (size_t)(auth_form_end - auth_form_start - 1),
        .form_cb = web_form,
        .form_body_max = 40 * 1024,
        .form_reply_max = 24 * 1024,
        .ap_password = s_ap_pass,
        .ap_max_conn = 1,
    };
    web_lock();
    s_web_op = WO_NONE;
    s_web_state = W_NONE;
    s_web_count = 0;
    s_web_note[0] = 0;
    web_unlock();
    // 后台任务做完还留着的 Wi-Fi 先断开，热点才开得起来。/ Drop Wi-Fi lingering after a background job so the hotspot can start.
    tomato_net_idle_off();
    const esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "打不开设备热点（%s）", esp_err_to_name(err));
        return;
    }
    s_web_on = true;
    memset(&s_web_status, 0, sizeof(s_web_status));
    read_pico_transfer_get_status(&s_web_status);
    if (s_web_status.network_ready) ui_wifi_qr_prepare(s_web_status.ssid, s_ap_pass);
    app_loop_hold_awake(true);
}

// 设备上确认或取消网页的请求。/ Confirm or cancel the page's request on the device.
static void web_decide(bool yes) {
    web_lock();
    totp_table_t* t = tr_auth_table();
    if (s_web_state != W_PENDING || !t) {
        web_unlock();
        return;
    }
    if (!yes) {
        s_web_state = W_CANCELLED;
        snprintf(s_web_note, sizeof(s_web_note), "已在设备上取消");
    } else if (s_web_op == WO_IMPORT) {
        int added = 0, dup = 0;
        for (int i = 0; i < s_stage_n; i++) {
            bool d = false;
            if (totp_table_add(t, &s_stage[i], &d)) added++;
            else if (d) dup++;
        }
        const bool ok = !added || tr_auth_save() == TOTP_OK;
        s_web_state = ok ? W_DONE : W_FAILED;
        s_web_count = added;
        if (!ok) snprintf(s_web_note, sizeof(s_web_note), "写入 TF 卡失败");
        else if (dup) snprintf(s_web_note, sizeof(s_web_note), "已导入 %d 个，%d 个已经有了", added, dup);
        else snprintf(s_web_note, sizeof(s_web_note), "已导入 %d 个账号", added);
    } else {
        const int i = totp_table_find(t, s_stage_del);
        if (i >= 0) totp_table_remove(t, i);
        const bool ok = i < 0 || tr_auth_save() == TOTP_OK;
        s_web_state = ok ? W_DONE : W_FAILED;
        snprintf(s_web_note, sizeof(s_web_note), ok ? "已删除" : "写入 TF 卡失败");
    }
    snprintf(s_msg, sizeof(s_msg), "%s", s_web_note);
    stage_free();
    web_unlock();
    s_dialog = D_NONE;
}

/* ---- 绘制工具 / Drawing helpers ---- */

static void draw_back_title(uint8_t* fb, const char* title) {
    tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), title, EPD_DRAW_ALIGN_LEFT, true);
}

static void label_of(const totp_account_t* a, char* out, size_t cap) {
    if (a->issuer[0]) snprintf(out, cap, "%s · %s", a->issuer, a->name);
    else snprintf(out, cap, "%s", a->name);
}

// 倒计时条：黑框里按剩余时间填黑（只用黑白）。/ Countdown bar: black fill for the time left inside a black frame
// (black and white only).
static void draw_bar(uint8_t* fb, int x, int y, int w, int h, int left, int period) {
    epd_draw_rect((EpdRect){x, y, w, h}, UI_GRAY_BLACK, fb);
    const int fill = (w - 4) * left / period;
    if (fill > 0) epd_fill_rect((EpdRect){x + 2, y + 2, fill, h - 4}, UI_GRAY_BLACK, fb);
}

// 对话框：标题、说明和两个按钮（右边是确认）。/ Dialog: a title, text and two buttons (confirm on the right).
static EpdRect dialog_rect(void) { return (EpdRect){TR_MARGIN, TR_S(330), TR_W - 2 * TR_MARGIN, TR_S(470)}; }
static EpdRect dialog_btn(bool ok) {
    const EpdRect d = dialog_rect();
    const int w = (d.width - TR_S(96)) / 2;
    return (EpdRect){ok ? d.x + d.width - TR_S(36) - w : d.x + TR_S(36), d.y + d.height - TR_S(104), w, TR_S(72)};
}

static void draw_dialog(uint8_t* fb, const char* title, const char* text, const char* ok) {
    const EpdRect d = dialog_rect();
    epd_fill_rect(d, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, d, TR_S(24), UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(d, 1), TR_S(24) - 1, UI_GRAY_BLACK);
    tr_text(fb, d.x + TR_S(36), d.y + TR_S(36), TR_S(36), title, EPD_DRAW_ALIGN_LEFT, true);
    tr_wrap(fb, d.x + TR_S(36), d.y + TR_S(100), d.width - TR_S(72), TR_S(28), TR_S(42), text, 6);
    tr_pill(fb, dialog_btn(false), "取消", false, TR_S(30));
    tr_pill(fb, dialog_btn(true), ok, true, TR_S(30));
}

/* ---- 列表 / List ---- */

static bool banner_shown(void) {
    if (!time_ok()) return false;
    return s_syncing || tr_time_error_s() > WARN_S;
}
static int list_top(void) { return LIST_TOP + (banner_shown() ? BANNER_H + TR_S(8) : 0); }
static int per_page(void) {
    const int n = (PAGER_Y - TR_S(10) - list_top()) / ROW_H;
    return n < 1 ? 1 : n;
}
static int pages(void) {
    const totp_table_t* t = tr_auth_table();
    const int n = t ? t->count : 0;
    return n ? (n + per_page() - 1) / per_page() : 1;
}
static EpdRect icon_btn(int k) { return (EpdRect){TR_W - TR_MARGIN - (3 - k) * BTN_W, TR_S(80), BTN_W, BTN_W}; }

static void draw_banner(uint8_t* fb) {
    const EpdRect b = {TR_MARGIN, LIST_TOP, TR_W - 2 * TR_MARGIN, BANNER_H};
    ui_draw_round_rect(fb, b, TR_S(16), UI_GRAY_BLACK);
    char text[96];
    if (s_syncing) snprintf(text, sizeof(text), "正在连 Wi-Fi 校时…");
    else snprintf(text, sizeof(text), "时间可能差约 %d 秒，点这里连 Wi-Fi 校时", tr_time_error_s());
    tr_fit(text, TR_S(26), b.width - TR_S(40));
    tr_text(fb, b.x + TR_S(20), b.y + (BANNER_H - TR_S(26)) / 2 - TR_S(2), TR_S(26), text, EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_row(uint8_t* fb, const totp_account_t* a, int y, int64_t now) {
    char label[160], code[16];
    label_of(a, label, sizeof(label));
    tr_fit(label, TR_S(26), TR_W - 2 * TR_MARGIN);
    tr_text(fb, TR_MARGIN, y + TR_S(16), TR_S(26), label, EPD_DRAW_ALIGN_LEFT, false);
    uint32_t c = 0;
    if (totp_code(a, now, &c) == TOTP_OK) totp_format(c, a->digits, code, sizeof(code));
    else snprintf(code, sizeof(code), "—");
    tr_text(fb, TR_MARGIN, y + TR_S(56), CODE_PX, code, EPD_DRAW_ALIGN_LEFT, true);
    const int left = totp_remaining(a, now);
    const int bx = TR_W - TR_MARGIN - BAR_W;
    draw_bar(fb, bx, y + TR_S(64), BAR_W, BAR_H, left, a->period);
    char small[48];
    if (tr_time_error_s() > DOUBT_S) {
        snprintf(small, sizeof(small), "可能不准");
    } else if (left <= NEAR_S && totp_code(a, now + a->period, &c) == TOTP_OK) {
        char next[16];
        totp_format(c, a->digits, next, sizeof(next));
        snprintf(small, sizeof(small), "下一个 %s", next);
    } else {
        small[0] = 0;
    }
    if (small[0]) tr_text(fb, TR_W - TR_MARGIN, y + TR_S(92), TR_S(24), small, EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, y + ROW_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static void draw_list(uint8_t* fb) {
    const totp_table_t* t = tr_auth_table();
    const int64_t now = now_s();
    if (banner_shown()) draw_banner(fb);
    if (!t || !t->count) {
        tr_wrap(fb, TR_MARGIN, list_top() + TR_S(30), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "还没有账号。点右上角“导入”，手机连上设备热点后，在网页里粘贴链接或选 Google Authenticator 导出二维码的照片。",
                5);
        return;
    }
    const int per = per_page(), first = s_page * per;
    for (int i = first; i < t->count && i < first + per; i++) draw_row(fb, &t->items[i], list_top() + (i - first) * ROW_H, now);
    if (pages() > 1) tr_pager(fb, PAGER_Y, s_page, pages());
}

static void draw_header(uint8_t* fb) {
    tr_title(fb, "验证器");
    static const tr_icon_t ICONS[3] = {TR_ICON_IMPORT, TR_ICON_LOCK, TR_ICON_SETTINGS};
    for (int k = 0; k < 3; k++) {
        const EpdRect r = icon_btn(k);
        tr_icon(fb, ICONS[k], r.x + r.width / 2, r.y + r.height / 2, false);
    }
}

static void draw_detail(uint8_t* fb) {
    const totp_table_t* t = tr_auth_table();
    if (!t || s_detail < 0 || s_detail >= t->count) return;
    const totp_account_t* a = &t->items[s_detail];
    const int64_t now = now_s();
    draw_back_title(fb, a->issuer[0] ? a->issuer : a->name);
    int y = TR_S(220);
    if (a->issuer[0]) {
        char name[TOTP_TEXT_MAX + 1];
        snprintf(name, sizeof(name), "%s", a->name);
        tr_fit(name, TR_S(30), TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_W / 2, y, TR_S(30), name, EPD_DRAW_ALIGN_CENTER, false);
    }
    y += TR_S(90);
    char code[16];
    uint32_t c = 0;
    if (totp_code(a, now, &c) == TOTP_OK) totp_format(c, a->digits, code, sizeof(code));
    else snprintf(code, sizeof(code), "—");
    int px = TR_S(120);
    while (px > 40 && ttf_text_width_px(px, code) > TR_W - 2 * TR_MARGIN) px -= 4;
    tr_text(fb, TR_W / 2, y, px, code, EPD_DRAW_ALIGN_CENTER, true);
    y += px + TR_S(50);
    const int left = totp_remaining(a, now), w = TR_W - 2 * TR_MARGIN - TR_S(120);
    draw_bar(fb, TR_MARGIN + TR_S(60), y, w, TR_S(18), left, a->period);
    char line[64];
    snprintf(line, sizeof(line), "还剩 %d 秒", left);
    tr_text(fb, TR_W / 2, y + TR_S(40), TR_S(28), line, EPD_DRAW_ALIGN_CENTER, false);
    if (totp_code(a, now + a->period, &c) == TOTP_OK) {
        char next[16];
        totp_format(c, a->digits, next, sizeof(next));
        snprintf(line, sizeof(line), "下一个 %s", next);
        tr_text(fb, TR_W / 2, y + TR_S(96), TR_S(30), line, EPD_DRAW_ALIGN_CENTER, false);
    }
    if (tr_time_error_s() > DOUBT_S)
        tr_text(fb, TR_W / 2, y + TR_S(160), TR_S(26), "时间可能不准，验证码可能用不了", EPD_DRAW_ALIGN_CENTER, false);
}

/* ---- 其他视图 / Other views ---- */

static const char* pass_title(void) {
    switch (s_pass) {
    case PM_UNLOCK: return "输入验证器口令";
    case PM_SET1: return "设置验证器口令";
    case PM_SET2: return "再输一次口令";
    case PM_OLD: return "输入原来的口令";
    case PM_NEW1: return "输入新口令";
    default: return "再输一次新口令";
    }
}

static void draw_pass(uint8_t* fb) {
    if (s_pass == PM_UNLOCK && !s_from_settings) tr_title(fb, "验证器");
    else draw_back_title(fb, "验证器");
    tr_text(fb, TR_MARGIN, TR_S(176), TR_S(32), pass_title(), EPD_DRAW_ALIGN_LEFT, true);
    const int fy = TR_S(230), fh = TR_S(84);
    const EpdRect f = {TR_MARGIN, fy, TR_W - 2 * TR_MARGIN, fh};
    ui_draw_round_rect(fb, f, TR_S(14), TR_GRAY_OUTLINE);
    ui_draw_round_rect(fb, ui_inset_rect(f, 1), TR_S(14) - 1, TR_GRAY_OUTLINE);
    // 只露最后一个字符。/ Only the last character shows.
    char shown[TR_KBD_TEXT_MAX];
    const size_t n = strlen(s_kbd.text);
    size_t k = 0;
    for (size_t i = 0; i + 1 < n && k < sizeof(shown) - 2; i++) shown[k++] = '*';
    if (n && k < sizeof(shown) - 1) shown[k++] = s_kbd.text[n - 1];
    shown[k] = 0;
    const int px = TR_S(32);
    const char* tail = tr_tail_fit(shown, px, f.width - TR_S(48));
    tr_text(fb, f.x + TR_S(20), f.y + TR_S(22), px, tail, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){f.x + TR_S(22) + ttf_text_width_px(px, tail), f.y + TR_S(18), TR_S(3), TR_S(46)},
                  UI_GRAY_BLACK, fb);
    char hint[200];
    const int wait = tr_auth_wait_s();
    if (s_job != J_NONE) {
        snprintf(hint, sizeof(hint), "%s", s_job == J_UNLOCK ? "正在解锁…" : s_job == J_SETUP ? "正在设置…" : "正在修改…");
    } else if (s_msg[0]) {
        snprintf(hint, sizeof(hint), "%s", s_msg);
    } else if (wait > 0 && (s_pass == PM_UNLOCK || s_pass == PM_OLD)) {
        snprintf(hint, sizeof(hint), "输错太多次，请等 %d 秒再试", wait);
    } else if (s_pass == PM_SET1 || s_pass == PM_NEW1) {
        snprintf(hint, sizeof(hint), "至少 8 位，字母和数字都要有。忘了口令只能清空验证器，再从手机重新导入。");
    } else if (s_pass == PM_UNLOCK) {
        snprintf(hint, sizeof(hint), "口令只在这台设备上输入，和开机密码无关。");
    } else {
        hint[0] = 0;
    }
    tr_wrap(fb, TR_MARGIN, fy + fh + TR_S(18), TR_W - 2 * TR_MARGIN, TR_S(26), TR_S(36), hint, 3);
    tr_kbd_draw(fb, &s_kbd);
}

static void draw_import(uint8_t* fb) {
    draw_back_title(fb, "导入账号");
    const int bx = TR_MARGIN, by = TR_S(170), bw = TR_W - 2 * TR_MARGIN;
    const int pad = TR_S(28);
    int y = by + pad;
    if (!s_web_on) {
        tr_wrap(fb, bx, y, bw, TR_S(28), TR_S(42), s_msg[0] ? s_msg : "设备热点没有打开。", 4);
        return;
    }
    const EpdRect qr = {bx, y, 220, 220};
    ui_wifi_qr_draw(fb, qr);
    const int tx = qr.x + qr.width + TR_S(24), tw = bx + bw - tx;
    char line[160];
    snprintf(line, sizeof(line), "手机扫码连上设备热点 %s，口令 %s", s_web_status.ssid, s_ap_pass);
    const int n = tr_wrap(fb, tx, y, tw, TR_S(26), TR_S(38), line, 5);
    int uy = y + n * TR_S(38) + TR_S(12);
    tr_text_dim(fb, tx, uy, TR_S(26), "再在浏览器打开", EPD_DRAW_ALIGN_LEFT);
    uy += TR_S(40);
    int px = TR_S(32);
    while (px > 24 && ttf_text_width_px(px, "192.168.4.1") > tw) px -= 2;
    tr_text(fb, tx, uy, px, "192.168.4.1", EPD_DRAW_ALIGN_LEFT, true);
    y = (qr.y + qr.height > uy + px ? qr.y + qr.height : uy + px) + TR_S(36);
    tr_hline(fb, y, bx, bw, true);
    y += TR_S(24);
    web_lock();
    char note[120];
    if (s_web_note[0]) snprintf(note, sizeof(note), "%s", s_web_note);
    else snprintf(note, sizeof(note), "%s", s_web_status.sta_count ? "手机已连上，等网页提交…" : "等手机连上…");
    web_unlock();
    tr_text(fb, bx, y, TR_S(28), note, EPD_DRAW_ALIGN_LEFT, true);
    y += TR_S(60);
    tr_wrap(fb, bx, y, bw, TR_S(24), TR_S(36),
            "网页里可以粘贴 otpauth 链接、选 Google Authenticator 导出二维码的照片，或手动填密钥。网页发来的账号要在这里点“导入”才会写入；密钥不会离开设备。离开这一页热点就关闭。",
            6);
}

static void draw_settings(uint8_t* fb) {
    draw_back_title(fb, "验证器");
    if (s_msg[0])
        tr_message(fb, TR_W - TR_MARGIN, TR_S(104), TR_S(92), TR_W - TR_MARGIN - TR_S(284), TR_S(26), s_msg,
                   EPD_DRAW_ALIGN_RIGHT, false);
    const tr_auth_state_t st = tr_auth_state();
    char state[80];
    const int n = tr_auth_count();
    if (n >= 0) snprintf(state, sizeof(state), "已解锁 · %d 个账号", n);
    else snprintf(state, sizeof(state), "%s", st == TR_AUTH_UNSET ? "还没设置口令" : st == TR_AUTH_LOCKED ? "已上锁" :
                                              st == TR_AUTH_NOCARD ? "没有 TF 卡" : "需要清空后重新设置");
    tr_text_dim(fb, TR_MARGIN, TR_S(150), TR_S(26), state, EPD_DRAW_ALIGN_LEFT);
    static const char* const LABELS[3] = {"修改口令", "自动上锁", "清空验证器"};
    for (int i = 0; i < 3; i++) {
        const int y = SET_TOP + i * SET_ROW_H;
        tr_text(fb, TR_MARGIN, y + (SET_ROW_H - TR_S(32)) / 2 - TR_S(2), TR_S(32), LABELS[i], EPD_DRAW_ALIGN_LEFT, false);
        const char* v = i == 0 ? (st == TR_AUTH_LOCKED || st == TR_AUTH_OPEN ? "›" : "未设置")
                      : i == 1 ? (tr_auth_auto_lock() == TR_AUTH_LOCK_AWAY ? "离开 5 分钟后" : "锁屏或重启时")
                               : "›";
        tr_text_dim(fb, TR_W - TR_MARGIN, y + (SET_ROW_H - TR_S(28)) / 2, TR_S(28), v, EPD_DRAW_ALIGN_RIGHT);
        if (i < 2) tr_hline(fb, y + SET_ROW_H - 2, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
    tr_wrap(fb, TR_MARGIN, SET_TOP + 3 * SET_ROW_H + TR_S(30), TR_W - 2 * TR_MARGIN, TR_S(24), TR_S(36),
            "验证器口令只管这一个功能，和开机密码分开。账号加密存在 TF 卡上，解锁要口令和这台设备一起，换卡或换设备都解不开。忘了口令没有找回办法，只能清空验证器，再从手机 Google Authenticator 重新导入。自动上锁点一下切换。",
            8);
}

// 没解锁时的说明和按钮。/ Text and button while not unlocked.
static const char* main_text(tr_auth_state_t st, const char** btn) {
    *btn = NULL;
    switch (st) {
    case TR_AUTH_NOCARD: return "验证器的账号加密存在 TF 卡上。请插入 TF 卡。";
    case TR_AUTH_UNSET:
        *btn = "设置口令";
        return "在这台设备上离线显示两步验证码，和手机 Google Authenticator 算出的一样，不需要服务器。\n"
               "先设一个验证器口令（至少 8 位，字母和数字都有），再用设备热点从手机导入账号。手机上的 Google "
               "Authenticator 请一直保留作备份。";
    case TR_AUTH_LOCKED:
        *btn = "解锁";
        return "验证器已上锁。";
    case TR_AUTH_FOREIGN:
        *btn = "清空并重新设置";
        return "TF 卡上的验证器不是在这台设备上设置的（或者设备设置被重置过），解不开。可以清空后重新设置，再从手机重新导入。";
    case TR_AUTH_MISSING:
        *btn = "清空并重新设置";
        return "TF 卡上没有验证器文件。换回原来的卡就能继续用；或者清空后重新设置，再从手机重新导入。";
    default: return "";
    }
}

// 按钮放在说明下面：按说明折成几行算。/ The button sits below the text, by its wrapped line count.
static EpdRect main_btn_rect(tr_auth_state_t st) {
    const char* btn;
    const char* p = main_text(st, &btn);
    int lines = 0;
    while (*p && lines < 10) {
        if (*p == '\n') { p++; continue; }
        p += tr_line_bytes(p, TR_S(30), TR_W - 2 * TR_MARGIN);
        lines++;
    }
    return (EpdRect){TR_MARGIN, LIST_TOP + TR_S(60) + lines * TR_S(46), TR_S(300), TR_S(72)};
}

static void draw_main(uint8_t* fb) {
    const tr_auth_state_t st = tr_auth_state();
    if (st == TR_AUTH_OPEN) {
        draw_header(fb);
        if (s_msg[0]) {
            // 宽度按标题实际宽度让开（英文版标题更长），至少留一小段。/ Leave room for the title's real width (longer in
            // English), keeping at least a short span.
            int mw = TR_W - 2 * TR_MARGIN - 3 * BTN_W - ttf_text_width_px(TR_S(52), "验证器") - TR_S(44);
            if (mw < TR_S(80)) mw = TR_S(80);
            tr_message(fb, TR_W - TR_MARGIN - 3 * BTN_W, TR_S(104), TR_S(92), mw, TR_S(24), s_msg, EPD_DRAW_ALIGN_RIGHT,
                       false);
        }
        if (!time_ok()) {
            tr_wrap(fb, TR_MARGIN, LIST_TOP + TR_S(20), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                    s_syncing ? "正在连 Wi-Fi 校时…" : "设备时间还没校准，验证码会算错。点下面的按钮连 Wi-Fi 校时，或在“设置 › 电源与设备 › 时间与时区”里手动设置。",
                    5);
            if (!s_syncing) tr_pill(fb, (EpdRect){TR_MARGIN, TR_S(440), TR_S(280), TR_S(72)}, "连 Wi-Fi 校时", true, TR_S(30));
            return;
        }
        draw_list(fb);
        return;
    }
    tr_title(fb, "验证器");
    const char* btn = NULL;
    const char* text = main_text(st, &btn);
    const EpdRect b = main_btn_rect(st);
    tr_wrap(fb, TR_MARGIN, LIST_TOP + TR_S(20), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46), text, 10);
    if (btn) tr_pill(fb, b, btn, true, TR_S(30));
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    epd_fill_rect((EpdRect){0, 0, TR_W, TR_H}, UI_GRAY_WHITE, fb);
    tr_status_bar(fb);
    // 被锁上后（锁屏、离开太久）只能看口令页或设置。/ Once locked (lock screen, away too long) only the passphrase
    // view or settings remain.
    if ((s_view == V_DETAIL || s_view == V_IMPORT) && tr_auth_state() != TR_AUTH_OPEN) s_view = V_MAIN;
    switch (s_view) {
    case V_PASS: draw_pass(fb); return;
    case V_DETAIL: draw_detail(fb); break;
    case V_IMPORT: draw_import(fb); break;
    case V_SET: draw_settings(fb); break;
    default: draw_main(fb); break;
    }
    if (s_dialog == D_DELETE) {
        const totp_table_t* t = tr_auth_table();
        char text[220], label[160];
        if (t && s_delete >= 0 && s_delete < t->count) label_of(&t->items[s_delete], label, sizeof(label));
        else label[0] = 0;
        snprintf(text, sizeof(text), "删除“%s”？手机上的 Google Authenticator 不受影响。", label);
        draw_dialog(fb, "删除账号", text, "删除");
    } else if (s_dialog == D_WIPE1) {
        draw_dialog(fb, "清空验证器？",
                    "会删除 TF 卡上的加密文件和设备里配套的随机数，所有账号都没了。之后要重新设置口令，再从手机重新导入。", "清空");
    } else if (s_dialog == D_WIPE2) {
        draw_dialog(fb, "再确认一次", "清空以后无法恢复。确定清空验证器？", "确认清空");
    } else if (s_dialog == D_WEB) {
        char text[400];
        web_lock();
        if (s_web_op == WO_IMPORT) {
            int len = snprintf(text, sizeof(text), "网页发来 %d 个账号：", s_stage_n);
            for (int i = 0; i < s_stage_n && i < 4 && len < (int)sizeof(text); i++) {
                char label[160];
                label_of(&s_stage[i], label, sizeof(label));
                len += snprintf(text + len, sizeof(text) - (size_t)len, "%s%s", i ? "、" : "", label);
            }
            if (s_stage_n > 4 && len < (int)sizeof(text)) snprintf(text + len, sizeof(text) - (size_t)len, " 等");
        } else {
            const totp_table_t* t = tr_auth_table();
            const int i = t ? totp_table_find(t, s_stage_del) : -1;
            char label[160] = "";
            if (i >= 0) label_of(&t->items[i], label, sizeof(label));
            snprintf(text, sizeof(text), "网页要删除“%s”。手机上的 Google Authenticator 不受影响。", label);
        }
        const bool imp = s_web_op == WO_IMPORT;
        web_unlock();
        draw_dialog(fb, imp ? "导入账号？" : "删除账号？", text, imp ? "导入" : "删除");
    }
    if (s_view == V_SET) tr_tab_bar(fb, TR_TAB_SETTINGS);
    else if (s_view == V_MAIN) tr_tab_bar(fb, TR_TAB_AUTH);
}

/* ---- 口令流程 / Passphrase flow ---- */

static void open_pass(pass_mode_t m, view_t back) {
    s_pass = m;
    s_pass_back = back;
    s_view = V_PASS;
    s_msg[0] = 0;
    tr_kbd_open(&s_kbd, "", false, m == PM_SET1 || m == PM_OLD || m == PM_NEW1 ? "下一步" : m == PM_UNLOCK ? "解锁" : "确定");
}

static void clear_entries(void) {
    totp_wipe(s_first, sizeof(s_first));
    totp_wipe(s_second, sizeof(s_second));
    totp_wipe(&s_kbd, sizeof(s_kbd));
}

static app_redraw_t pass_back(void) {
    clear_entries();
    s_msg[0] = 0;
    s_view = s_pass_back;
    return APP_REDRAW_PAGE;
}

static app_redraw_t pass_submit(void) {
    const char* why = NULL;
    s_msg[0] = 0;
    if (strlen(s_kbd.text) > TR_AUTH_PASS_MAX) {
        snprintf(s_msg, sizeof(s_msg), "口令最多 64 位");
        return APP_REDRAW_PAGE;
    }
    switch (s_pass) {
    case PM_UNLOCK:
        if (tr_auth_wait_s() > 0) return APP_REDRAW_PAGE;
        if (!s_kbd.text[0]) return APP_REDRAW_NONE;
        snprintf(s_first, sizeof(s_first), "%.64s", s_kbd.text);
        s_job = J_UNLOCK;
        return APP_REDRAW_PAGE;
    case PM_SET1:
    case PM_NEW1:
        if (!tr_auth_pass_ok(s_kbd.text, &why)) {
            snprintf(s_msg, sizeof(s_msg), "%s", why);
            return APP_REDRAW_PAGE;
        }
        snprintf(s_pass == PM_SET1 ? s_first : s_second, sizeof(s_first), "%.64s", s_kbd.text);
        {
            const view_t back = s_pass_back;
            open_pass(s_pass == PM_SET1 ? PM_SET2 : PM_NEW2, back);
        }
        return APP_REDRAW_PAGE;
    case PM_SET2:
    case PM_NEW2: {
        const char* want = s_pass == PM_SET2 ? s_first : s_second;
        if (strcmp(want, s_kbd.text)) {
            const view_t back = s_pass_back;
            const pass_mode_t again = s_pass == PM_SET2 ? PM_SET1 : PM_NEW1;
            totp_wipe(s_pass == PM_SET2 ? s_first : s_second, sizeof(s_first));
            open_pass(again, back);
            snprintf(s_msg, sizeof(s_msg), "两次输入不一样，请重新输入");
            return APP_REDRAW_PAGE;
        }
        s_job = s_pass == PM_SET2 ? J_SETUP : J_CHANGE;
        return APP_REDRAW_PAGE;
    }
    case PM_OLD:
        if (tr_auth_wait_s() > 0) return APP_REDRAW_PAGE;
        if (!s_kbd.text[0]) return APP_REDRAW_NONE;
        snprintf(s_first, sizeof(s_first), "%.64s", s_kbd.text);
        {
            const view_t back = s_pass_back;
            open_pass(PM_NEW1, back);
        }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static const char* err_text(totp_err_t e) {
    switch (e) {
    case TOTP_ERR_AUTH: return "口令不对";
    case TOTP_ERR_NOENT: return "TF 卡上没有验证器文件";
    case TOTP_ERR_FORMAT: return "验证器文件损坏";
    case TOTP_ERR_NOMEM: return "内存不足";
    case TOTP_ERR_IO: return "读写 TF 卡失败";
    default: return "出错了，请重试";
    }
}

// 口令运算要 1 秒左右，放到下一轮做，先把“正在…”画出来。/ Key derivation takes about a second, so it runs on the
// next tick after "Working..." is on screen.
static app_redraw_t run_job(void) {
    const job_t job = s_job;
    s_job = J_NONE;
    totp_err_t e;
    if (job == J_UNLOCK) {
        e = tr_auth_unlock(s_first);
        if (e == TOTP_OK) {
            clear_entries();
            s_view = s_from_settings ? V_SET : V_MAIN;
            s_page = 0;
            return APP_REDRAW_PAGE;
        }
        const pass_mode_t m = s_pass;
        const view_t back = s_pass_back;
        open_pass(m, back);
        if (e == TOTP_ERR_AUTH) {
            const int left = TR_AUTH_FREE_TRIES - tr_auth_fails();
            if (left > 0) snprintf(s_msg, sizeof(s_msg), "口令不对，还能再试 %d 次", left);
            else snprintf(s_msg, sizeof(s_msg), "口令不对，请等 %d 秒再试", tr_auth_wait_s());
        } else {
            snprintf(s_msg, sizeof(s_msg), "%s", err_text(e));
        }
        totp_wipe(s_first, sizeof(s_first));
        return APP_REDRAW_PAGE;
    }
    if (job == J_SETUP) {
        e = tr_auth_setup(s_first);
        clear_entries();
        if (e == TOTP_OK) {
            s_view = V_MAIN;
            snprintf(s_msg, sizeof(s_msg), "口令已设置");
        } else {
            open_pass(PM_SET1, s_pass_back);
            snprintf(s_msg, sizeof(s_msg), "设置失败：%s", err_text(e));
        }
        return APP_REDRAW_PAGE;
    }
    e = tr_auth_change(s_first, s_second);
    clear_entries();
    if (e == TOTP_OK) {
        s_view = s_pass_back;
        snprintf(s_msg, sizeof(s_msg), "口令已修改");
    } else {
        open_pass(PM_OLD, s_pass_back);
        if (e == TOTP_ERR_AUTH) {
            const int left = TR_AUTH_FREE_TRIES - tr_auth_fails();
            if (left > 0) snprintf(s_msg, sizeof(s_msg), "原来的口令不对，还能再试 %d 次", left);
            else snprintf(s_msg, sizeof(s_msg), "原来的口令不对，请等 %d 秒再试", tr_auth_wait_s());
        } else {
            snprintf(s_msg, sizeof(s_msg), "修改失败：%s", err_text(e));
        }
    }
    return APP_REDRAW_PAGE;
}

/* ---- 触摸 / Touch ---- */

static app_redraw_t start_time_sync(void) {
    if (s_syncing) return APP_REDRAW_NONE;
    const tomato_preempt_t p = tomato_sync_preempt();
    if (p != TOMATO_PREEMPT_FREE) {
        snprintf(s_msg, sizeof(s_msg), p == TOMATO_PREEMPT_STOPPING ? "正在停下后台加载，请再点一次" : "后台正在联网，完成后再校时");
        return APP_REDRAW_PAGE;
    }
    if (tomato_sync_start_time() != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "现在不能校时，请稍后再试");
        return APP_REDRAW_PAGE;
    }
    s_syncing = true;
    s_msg[0] = 0;
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_settings_app(app_ctx_t* ctx) {
    app_tr_settings_request_source(TR_TAB_AUTH);
    ctx->request_app = &app_tr_settings;
    return APP_REDRAW_NONE;
}

static app_redraw_t tap_dialog(uint16_t x, uint16_t y) {
    const bool ok = ui_rect_hit(dialog_btn(true), x, y), no = ui_rect_hit(dialog_btn(false), x, y);
    if (!ok && !no) return APP_REDRAW_NONE;
    const dialog_t d = s_dialog;
    s_dialog = D_NONE;
    if (d == D_WEB) {
        web_decide(ok);
        return APP_REDRAW_PAGE;
    }
    if (!ok) return APP_REDRAW_PAGE;
    if (d == D_DELETE) {
        totp_table_t* t = tr_auth_table();
        if (t && s_delete >= 0 && s_delete < t->count) {
            web_lock();
            totp_table_remove(t, s_delete);
            const totp_err_t e = tr_auth_save();
            web_unlock();
            snprintf(s_msg, sizeof(s_msg), e == TOTP_OK ? "已删除" : "删除没能写进 TF 卡");
            if (s_page >= pages()) s_page = pages() - 1;
        }
        s_delete = -1;
        if (s_view == V_DETAIL) s_view = V_MAIN;
    } else if (d == D_WIPE1) {
        s_dialog = D_WIPE2;
    } else if (d == D_WIPE2) {
        tr_auth_wipe();
        snprintf(s_msg, sizeof(s_msg), "验证器已清空");
    }
    return APP_REDRAW_PAGE;
}

static int row_at(uint16_t y) {
    const totp_table_t* t = tr_auth_table();
    if (!t || y < list_top()) return -1;
    const int k = (y - list_top()) / ROW_H;
    if (k >= per_page()) return -1;
    const int i = s_page * per_page() + k;
    return i < t->count ? i : -1;
}

static app_redraw_t tap_main(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, TR_TAB_AUTH)) return APP_REDRAW_NONE;
    const tr_auth_state_t st = tr_auth_state();
    if (st != TR_AUTH_OPEN) {
        if (!ui_rect_hit(main_btn_rect(st), x, y)) return APP_REDRAW_NONE;
        if (st == TR_AUTH_LOCKED) {
            open_pass(PM_UNLOCK, V_MAIN);
            return APP_REDRAW_PAGE;
        }
        if (st == TR_AUTH_UNSET) {
            open_pass(PM_SET1, V_MAIN);
            return APP_REDRAW_PAGE;
        }
        if (st == TR_AUTH_FOREIGN || st == TR_AUTH_MISSING) {
            s_dialog = D_WIPE1;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    for (int k = 0; k < 3; k++) {
        if (!ui_rect_hit(icon_btn(k), x, y)) continue;
        s_msg[0] = 0;
        if (k == 0) {
            s_view = V_IMPORT;
            web_start();
            return APP_REDRAW_FULL;
        }
        if (k == 1) {
            tr_auth_lock();
            open_pass(PM_UNLOCK, V_MAIN);
            return APP_REDRAW_PAGE;
        }
        return open_settings_app(ctx);
    }
    if (!time_ok()) {
        if (ui_rect_hit((EpdRect){TR_MARGIN, TR_S(440), TR_S(280), TR_S(72)}, x, y)) return start_time_sync();
        return APP_REDRAW_NONE;
    }
    if (banner_shown() && y >= LIST_TOP && y < LIST_TOP + BANNER_H) return start_time_sync();
    if (pages() > 1) {
        const int turn = tr_pager_hit(PAGER_Y, x, y);
        if (turn) {
            s_page = (s_page + turn + pages()) % pages();
            return APP_REDRAW_PAGE;
        }
    }
    const int i = row_at(y);
    if (i < 0) return APP_REDRAW_NONE;
    s_detail = i;
    s_view = V_DETAIL;
    return APP_REDRAW_PAGE;
}

static app_redraw_t tap_settings(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    const int tab = tr_tab_hit(x, y);
    if (tab == TR_TAB_SETTINGS) return open_settings_app(ctx);
    if (tr_nav_touch(ctx, x, y, TR_TAB_SETTINGS)) return APP_REDRAW_NONE;
    if (ui_rect_hit(BACK_RECT, x, y)) return open_settings_app(ctx);
    if (y < SET_TOP || y >= SET_TOP + 3 * SET_ROW_H) return APP_REDRAW_NONE;
    const tr_auth_state_t st = tr_auth_state();
    s_msg[0] = 0;
    switch ((y - SET_TOP) / SET_ROW_H) {
    case 0:
        if (st != TR_AUTH_LOCKED && st != TR_AUTH_OPEN) {
            snprintf(s_msg, sizeof(s_msg), "还没设置口令");
            return APP_REDRAW_PAGE;
        }
        open_pass(PM_OLD, V_SET);
        return APP_REDRAW_PAGE;
    case 1:
        tr_auth_set_auto_lock(tr_auth_auto_lock() == TR_AUTH_LOCK_AWAY ? TR_AUTH_LOCK_ON_SLEEP : TR_AUTH_LOCK_AWAY);
        return APP_REDRAW_PAGE;
    default:
        if (st == TR_AUTH_UNSET || st == TR_AUTH_NOCARD) {
            snprintf(s_msg, sizeof(s_msg), st == TR_AUTH_NOCARD ? "没有 TF 卡" : "验证器是空的");
            return APP_REDRAW_PAGE;
        }
        s_dialog = D_WIPE1;
        return APP_REDRAW_PAGE;
    }
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_job != J_NONE) return APP_REDRAW_NONE;
    if (s_view == V_PASS) {
        if (ev->type != UI_GESTURE_PRESS) return APP_REDRAW_NONE;
        const bool back_ok = !(s_pass == PM_UNLOCK && !s_from_settings);
        if (back_ok && ui_rect_hit(BACK_RECT, ev->x0, ev->y0)) return pass_back();
        const tr_kbd_result_t r = tr_kbd_touch(&s_kbd, ev->x0, ev->y0);
        if (r == TR_KBD_CHANGED) {
            s_msg[0] = 0;
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return r == TR_KBD_SUBMIT ? pass_submit() : APP_REDRAW_NONE;
    }
    if (s_dialog != D_NONE) return ev->type == UI_GESTURE_TAP ? tap_dialog(ev->x0, ev->y0) : APP_REDRAW_NONE;
    if (ev->type == UI_GESTURE_LONG_PRESS && s_view == V_MAIN) {
        const int i = row_at(ev->y0);
        if (i < 0 || tr_auth_state() != TR_AUTH_OPEN) return APP_REDRAW_NONE;
        s_delete = i;
        s_dialog = D_DELETE;
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_LONG_PRESS && s_view == V_DETAIL) {
        s_delete = s_detail;
        s_dialog = D_DELETE;
        return APP_REDRAW_PAGE;
    }
    if (s_view == V_MAIN && pages() > 1 && (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R)) {
        s_page = (s_page + (ev->type == UI_GESTURE_SWIPE_L ? 1 : -1) + pages()) % pages();
        return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const uint16_t x = ev->x0, y = ev->y0;
    switch (s_view) {
    case V_DETAIL:
        if (ui_rect_hit(BACK_RECT, x, y)) {
            s_view = V_MAIN;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case V_IMPORT:
        if (ui_rect_hit(BACK_RECT, x, y)) {
            web_stop();
            s_view = V_MAIN;
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_NONE;
    case V_SET: return tap_settings(ctx, x, y);
    default: return tap_main(ctx, x, y);
    }
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1 || s_job != J_NONE) return APP_REDRAW_NONE;
    if (s_dialog != D_NONE) {
        if (s_dialog == D_WEB) web_decide(false);
        s_dialog = D_NONE;
        return APP_REDRAW_PAGE;
    }
    switch (s_view) {
    case V_PASS: return s_pass == PM_UNLOCK && !s_from_settings ? APP_REDRAW_NONE : pass_back();
    case V_DETAIL: s_view = V_MAIN; return APP_REDRAW_PAGE;
    case V_IMPORT: web_stop(); s_view = V_MAIN; return APP_REDRAW_FULL;
    case V_SET: return open_settings_app(ctx);
    default:
        if (pages() > 1) {
            s_page = (s_page + 1) % pages();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
}

/* ---- 节拍 / Tick ---- */

static app_redraw_t tick_web(void) {
    read_pico_transfer_service_poll();
    read_pico_transfer_status_t next;
    read_pico_transfer_get_status(&next);
    bool changed = next.sta_count != s_web_status.sta_count || next.network_ready != s_web_status.network_ready;
    if (next.network_ready && !s_web_status.network_ready) ui_wifi_qr_prepare(next.ssid, s_ap_pass);
    s_web_status = next;
    web_lock();
    const bool pending = s_web_state == W_PENDING;
    web_unlock();
    if (pending && s_dialog != D_WEB) {
        s_dialog = D_WEB;
        changed = true;
    }
    return changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    (void)ctx;
    if (s_job != J_NONE) return run_job();
    if (s_syncing && !tomato_sync_busy()) {
        tomato_status_t st;
        tomato_sync_get_status(&st);
        s_syncing = false;
        snprintf(s_msg, sizeof(s_msg), "%s", st.phase == TOMATO_DONE ? "已校时" : st.message[0] ? st.message : "校时失败");
        return APP_REDRAW_PAGE;
    }
    if (s_view == V_IMPORT && s_web_on) return tick_web();
    if (s_web_dirty) {
        s_web_dirty = false;
        return APP_REDRAW_PAGE;
    }
    // 显示验证码时每 5 秒更新倒计时（DU），换码时整页刷新清残影。/ While codes show, update the countdown every
    // 5 s by DU, and refresh the whole page when codes change to clear ghosting.
    const bool codes = (s_view == V_MAIN || s_view == V_DETAIL) && s_dialog == D_NONE && tr_auth_table() &&
                       time_ok();
    if (s_view == V_PASS && (s_pass == PM_UNLOCK || s_pass == PM_OLD) && !s_msg[0]) {
        // 等待时间每 5 秒刷一下提示。/ Refresh the wait hint every 5 s.
        const int64_t slot = tr_auth_wait_s() > 0 ? now_s() / 5 : -2;
        if (slot != s_shown_slot) {
            const bool was = s_shown_slot >= 0 || slot >= 0;
            s_shown_slot = slot;
            if (was) return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (!codes) {
        s_shown_slot = -1;
        return APP_REDRAW_NONE;
    }
    const int64_t now = now_s(), slot = now / 5;
    if (slot == s_shown_slot) return APP_REDRAW_NONE;
    const bool first = s_shown_slot < 0;
    s_shown_slot = slot;
    if (first) return APP_REDRAW_NONE;
    // 30 秒周期的换码时刻落在 5 秒格的开头。/ A 30 s period boundary starts a 5 s slot.
    if (now % 30 < 5) return APP_REDRAW_PAGE;
    render(ctx, ctx->fb);
    return APP_REDRAW_AREA;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    if (s_view == V_PASS) return (EpdRect){0, TR_S(160), TR_W, TR_H - TR_S(160)};
    if (s_view == V_DETAIL) return (EpdRect){0, TR_S(200), TR_W, TR_TABS_Y - TR_S(200)};
    return (EpdRect){0, LIST_TOP, TR_W, PAGER_Y - LIST_TOP};
}

/* ---- 生命周期 / Lifecycle ---- */

void app_tr_auth_request_settings(void) { s_open_settings = true; }

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    tr_auth_enter();
    s_from_settings = s_open_settings;
    s_open_settings = false;
    s_dialog = D_NONE;
    s_job = J_NONE;
    s_msg[0] = 0;
    s_shown_slot = -1;
    s_detail = -1;
    if (s_page >= pages()) s_page = 0;
    if (s_from_settings) s_view = V_SET;
    else if (tr_auth_state() == TR_AUTH_LOCKED) open_pass(PM_UNLOCK, V_MAIN);
    else s_view = V_MAIN;
}

static void leave(void) {
    web_stop();
    clear_entries();
    s_dialog = D_NONE;
    s_job = J_NONE;
    s_view = V_MAIN;
}

static void page_exit(app_ctx_t* ctx) {
    (void)ctx;
    leave();
    tr_auth_leave();
}

static void on_lock(app_ctx_t* ctx) {
    (void)ctx;
    // sleep.c 会上锁；这里先收起热点和输入，醒来回到口令页。/ sleep.c locks; here the hotspot and entries are put
    // away, and waking shows the passphrase view.
    const bool settings = s_view == V_SET;
    leave();
    if (settings) s_view = V_SET;
    else if (tr_auth_state() != TR_AUTH_UNSET) open_pass(PM_UNLOCK, V_MAIN);
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    leave();
    tr_auth_lock();
}

const app_desc_t app_tr_auth = {
    .title = "验证器",
    .detail = "离线两步验证码",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_exit,
    .on_lock = on_lock,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
