/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：验证器页的渲染测试（各种状态、口令键盘、导入、设置、对话框）。/ English: Render bench for the
 * Authenticator page (every state, the passphrase keyboard, import, settings, dialogs).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_auth.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

#ifndef HR_AUTH_PAGE
#define HR_AUTH_PAGE "auth"
#endif
const char* hr_page = HR_AUTH_PAGE;

/* ---- 替身 / Stand-ins ---- */

static tr_auth_state_t s_state = TR_AUTH_UNSET;
static totp_table_t s_tab;
static int s_err_s = 2, s_wait;
static bool s_time = true;

tr_auth_state_t tr_auth_state(void) { return s_state; }
static bool s_pin_mode;   // 这份验证器用 PIN（数字键盘）还是旧口令（屏幕键盘）/ PIN (keypad) or old passphrase (keyboard)
bool tr_auth_pin_mode(void) { return s_pin_mode; }
totp_table_t* tr_auth_table(void) { return s_state == TR_AUTH_OPEN ? &s_tab : NULL; }
int tr_auth_count(void) { return s_state == TR_AUTH_OPEN ? s_tab.count : -1; }
int tr_auth_wait_s(void) { return s_wait; }
int tr_auth_fails(void) { return 0; }
tr_auth_auto_t tr_auth_auto_lock(void) { return TR_AUTH_LOCK_ON_SLEEP; }
void totp_wipe(void* p, size_t n) { memset(p, 0, n); }
bool tr_time_valid(void) { return s_time; }
int tr_time_error_s(void) { return s_err_s; }
bool tomato_sync_busy(void) { return false; }
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    epd_fill_rect(area, 0x00, fb);
    epd_fill_rect((EpdRect){area.x + 12, area.y + 12, area.width - 24, area.height - 24}, 0xFF, fb);
}

// 验证码用编号凑一个固定的数，排版看得出来就行。/ A fixed code from the id; enough to judge the layout.
totp_err_t totp_code(const totp_account_t* a, int64_t t, uint32_t* out) {
    *out = (uint32_t)((a->id * 271828u + (uint32_t)(t / a->period) * 7919u) % (a->digits == 8 ? 100000000u : 1000000u));
    return TOTP_OK;
}
void totp_format(uint32_t code, int digits, char* out, size_t cap) {
    if (digits == 8) snprintf(out, cap, "%04u %04u", code / 10000, code % 10000);
    else snprintf(out, cap, "%03u %03u", code / 1000 % 1000, code % 1000);
}
int totp_remaining(const totp_account_t* a, int64_t t) { return a->period - (int)(t % a->period); }
int totp_table_find(const totp_table_t* t, uint32_t id) {
    for (int i = 0; i < t->count; i++)
        if (t->items[i].id == id) return i;
    return -1;
}

static void add(const char* issuer, const char* name, int digits) {
    totp_account_t* a = &s_tab.items[s_tab.count++];
    memset(a, 0, sizeof(*a));
    a->id = (uint32_t)s_tab.count;
    snprintf(a->issuer, sizeof(a->issuer), "%s", issuer);
    snprintf(a->name, sizeof(a->name), "%s", name);
    a->digits = (uint8_t)digits;
    a->period = 30;
    a->secret_len = 10;
}

/* ---- 场景 / Scenes ---- */

static app_ctx_t s_ctx;

static void shot(const char* name, bool bar) {
    if (!bar) hr_no_bar_check();
    // 账号名是内容，放不下截断是预期的。/ Account names are content; truncating them is expected.
    if (s_tab.count) hr_allow_fit();
    render(&s_ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    s_ctx.fb = hr_fb;
    memset(&s_tab, 0, sizeof(s_tab));
    s_dialog = D_NONE;
    s_msg[0] = 0;
    s_from_settings = false;

    s_state = TR_AUTH_UNSET;
    s_view = V_MAIN;
    shot("unset", true);
    s_state = TR_AUTH_NOCARD;
    shot("nocard", true);
    s_state = TR_AUTH_FOREIGN;
    shot("foreign", true);
    s_state = TR_AUTH_MISSING;
    shot("missing", true);
    s_dialog = D_WIPE1;
    shot("wipe1", true);
    s_dialog = D_WIPE2;
    shot("wipe2", true);
    s_dialog = D_NONE;

    s_state = TR_AUTH_LOCKED;
    open_pass(PM_UNLOCK, V_MAIN);
    snprintf(s_kbd.text, sizeof(s_kbd.text), "abcd1234");
    shot("unlock", false);
    snprintf(s_msg, sizeof(s_msg), "口令不对，还能再试 2 次");
    shot("unlock_wrong", false);
    s_msg[0] = 0;
    s_wait = 120;
    shot("unlock_wait", false);
    s_wait = 0;
    s_job = J_UNLOCK;
    shot("unlock_busy", false);
    s_job = J_NONE;
    s_state = TR_AUTH_UNSET;
    open_pass(PM_SET1, V_MAIN);
    snprintf(s_kbd.text, sizeof(s_kbd.text), "abc");
    shot("set1", false);
    snprintf(s_msg, sizeof(s_msg), "PIN 要正好 4 位数字");
    shot("set1_bad", false);

    s_state = TR_AUTH_OPEN;
    s_view = V_MAIN;
    s_msg[0] = 0;
    shot("empty", true);
    add("GitHub", "someone@example.com", 6);
    add("Google", "someone.longname.account@gmail.example.com", 6);
    add("一个名字特别特别特别长的发行方名称", "账号名称也写得很长很长很长很长", 8);
    add("", "没有发行方的账号", 6);
    add("Microsoft", "someone@outlook.example", 6);
    add("Cloudflare", "ops", 6);
    add("Dropbox", "family", 8);
    s_page = 0;
    shot("list", true);
    s_page = pages() - 1;
    shot("list_last", true);
    s_page = 0;
    snprintf(s_msg, sizeof(s_msg), "已导入 7 个账号");
    shot("list_msg", true);
    s_msg[0] = 0;
    s_err_s = 40;
    shot("list_drift", true);
    s_syncing = true;
    shot("list_syncing", true);
    s_syncing = false;
    s_err_s = 2;
    s_time = false;
    shot("no_time", true);
    s_time = true;
    s_delete = 2;
    s_dialog = D_DELETE;
    shot("delete", true);
    s_dialog = D_NONE;

    s_view = V_DETAIL;
    s_detail = 0;
    shot("detail", false);
    s_detail = 2;
    shot("detail_long", false);

    s_view = V_IMPORT;
    s_web_on = true;
    snprintf(s_ap_pass, sizeof(s_ap_pass), "k7pq2mxa9tvr");
    snprintf(s_web_status.ssid, sizeof(s_web_status.ssid), "ReadPico-3A7F");
    s_web_status.network_ready = true;
    shot("import", false);
    s_web_status.sta_count = 1;
    snprintf(s_web_note, sizeof(s_web_note), "已导入 3 个账号");
    shot("import_done", false);
    static totp_account_t stage[TOTP_MAX_ACCOUNTS];
    s_stage = stage;
    s_stage_n = 6;
    for (int i = 0; i < 6; i++) stage[i] = s_tab.items[i];
    s_web_op = WO_IMPORT;
    s_web_state = W_PENDING;
    s_dialog = D_WEB;
    shot("import_confirm", false);
    s_web_op = WO_DELETE;
    s_stage_del = 2;
    shot("web_delete", false);
    s_dialog = D_NONE;
    s_web_state = W_NONE;
    s_stage = NULL;
    s_web_on = false;
    s_web_note[0] = 0;

    s_view = V_SET;
    s_from_settings = true;
    shot("settings", true);
    s_state = TR_AUTH_LOCKED;
    snprintf(s_msg, sizeof(s_msg), "PIN 已修改");
    shot("settings_locked", true);
    s_pin_mode = true;
    shot("settings_locked_pin", true);
    s_pin_mode = false;
    s_msg[0] = 0;
    open_pass(PM_OLD, V_SET);
    shot("old", false);
    open_pass(PM_NEW2, V_SET);
    snprintf(s_msg, sizeof(s_msg), "两次输入不一样，请重新输入");
    shot("new2", false);

    // PIN 版：数字键盘整屏自己画，这一页只写提示和“输入 PIN”按钮。/ The PIN version: the keypad draws the whole screen
    // itself, this page only shows the hint and the "Enter PIN" button.
    s_pin_mode = true;
    s_msg[0] = 0;
    s_state = TR_AUTH_LOCKED;
    open_pass(PM_UNLOCK, V_MAIN);
    s_pad_pending = false;
    shot("pin_unlock", false);
    snprintf(s_msg, sizeof(s_msg), "PIN 不对，还能再试 2 次");
    shot("pin_unlock_wrong", false);
    s_msg[0] = 0;
    s_wait = 120;
    shot("pin_unlock_wait", false);
    s_wait = 0;
    s_job = J_UNLOCK;
    shot("pin_unlock_busy", false);
    s_job = J_NONE;
    s_state = TR_AUTH_UNSET;
    open_pass(PM_SET1, V_MAIN);
    s_pad_pending = false;
    shot("pin_set1", false);
    open_pass(PM_OLD, V_SET);
    s_pad_pending = false;
    shot("pin_old", false);
    s_pin_mode = false;
}
