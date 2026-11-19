/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 读写。打开失败就用关机默认值，不擦除整个分区。
 *
 * NVS load/store. A failed open keeps the power-off default; the
 * partition is not erased.
 */

#include "settings.h"
#include "tr_ui.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "settings"
#define NVS_NS "read_pico"
#define NVS_KEY_SLEEP "sleep"
#define NVS_KEY_FONT "font"
#define NVS_KEY_WAKE "lwake"
#define NVS_KEY_BOOT "lboot"
#define NVS_KEY_PICKUP "pickup"
#define NVS_KEY_BOOK_PX "bk_px"
#define NVS_KEY_BOOK_SHAKE "bk_shake"
#define NVS_KEY_BOOK_STYLE "bk_style"
#define NVS_KEY_HIDE_HUPU "hide_hupu"   // 旧版，只读来迁移 / Old key, read only to migrate
#define NVS_KEY_TABS_OFF "tabs_off"
#define NVS_KEY_TABS_X "tabs_x"     // X 标签已按默认关闭迁移过 / X tab already migrated to hidden
#define NVS_KEY_BOOK_AUTO "bk_auto"
#define NVS_KEY_COMIC_AUTO "cm_auto"
#define NVS_KEY_LOCK_IDLE "lock_idle"
#define NVS_KEY_PIN "pin_hash"      // 8 字节盐 + 32 字节 SHA-256 / 8-byte salt + 32-byte SHA-256
#define PIN_SALT_LEN 8
#define PIN_HASH_LEN 32
#define FONT_PATH_MAX 160

static app_sleep_mode_t s_sleep = APP_SLEEP_OFF;
static char s_font[FONT_PATH_MAX];
static uint8_t s_last_wake;
static uint8_t s_last_boot;
static bool s_pickup_wake;
static uint8_t s_book_px = 48;
static bool s_book_shake;
// 默认：标准行距、小段距、中页边距、按原文缩进、无下划线（和以前的排版一样）。
// Defaults: standard line spacing, small paragraph gap, medium margins, indent as written, no rules (the old layout).
static uint8_t s_book_style = 0x15;
// X 标签默认关闭。/ The X tab starts hidden.
static uint8_t s_tabs_off = 1u << TR_TAB_X;
static uint8_t s_book_auto, s_comic_auto;
static uint8_t s_lock_idle = 1;
static bool s_pin_on;
static uint8_t s_pin[PIN_SALT_LEN + PIN_HASH_LEN];

static void nvs_put_u8(const char* key, uint8_t value);

static app_book_style_t unpack_style(uint8_t v) {
    app_book_style_t s = {.line = v & 3, .para = (v >> 2) & 3, .margin = (v >> 4) & 3,
                          .indent = (v >> 6) & 1, .underline = (v >> 7) & 1};
    if (s.line > 2) s.line = 1;
    if (s.para > 2) s.para = 1;
    if (s.margin > 2) s.margin = 1;
    return s;
}

static uint8_t pack_style(const app_book_style_t* s) {
    return (uint8_t)((s->line & 3) | (s->para & 3) << 2 | (s->margin & 3) << 4 | (s->indent ? 1 : 0) << 6 |
                     (s->underline ? 1 : 0) << 7);
}

static uint8_t valid_book_px(uint8_t px) {
    return px >= 36 && px <= 72 && (px - 36) % 4 == 0 ? px : 48;
}

void app_settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init %s, use power off", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t raw = APP_SLEEP_OFF;
    if (nvs_get_u8(h, NVS_KEY_SLEEP, &raw) == ESP_OK && raw <= APP_SLEEP_OFF) {
        s_sleep = (app_sleep_mode_t)raw;
    }
    size_t font_len = sizeof(s_font);
    if (nvs_get_str(h, NVS_KEY_FONT, s_font, &font_len) != ESP_OK) {
        s_font[0] = '\0';
    }
    uint8_t wake = 0;
    if (nvs_get_u8(h, NVS_KEY_WAKE, &wake) == ESP_OK) s_last_wake = wake;
    uint8_t boot = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOT, &boot) == ESP_OK) s_last_boot = boot;
    uint8_t pickup = 0;
    if (nvs_get_u8(h, NVS_KEY_PICKUP, &pickup) == ESP_OK) s_pickup_wake = pickup != 0;
    uint8_t book_px = 48, book_shake = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_PX, &book_px) == ESP_OK) s_book_px = valid_book_px(book_px);
    if (nvs_get_u8(h, NVS_KEY_BOOK_SHAKE, &book_shake) == ESP_OK) s_book_shake = book_shake != 0;
    uint8_t book_style = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_STYLE, &book_style) == ESP_OK) s_book_style = book_style;
    uint8_t tabs_off = 0, hide_hupu = 0;
    if (nvs_get_u8(h, NVS_KEY_TABS_OFF, &tabs_off) == ESP_OK) s_tabs_off = tabs_off;
    else if (nvs_get_u8(h, NVS_KEY_HIDE_HUPU, &hide_hupu) == ESP_OK && hide_hupu) s_tabs_off = 1u << TR_TAB_HUPU;
    // 加 X 标签之前存的掩码没有它的位：第一次读到时把 X 关上，免得升级后多出一个标签。/ Masks saved before the X tab
    // lack its bit: hide X the first time so an upgrade does not add a tab.
    uint8_t tabs_x = 0;
    const bool migrate_x = nvs_get_u8(h, NVS_KEY_TABS_X, &tabs_x) != ESP_OK;
    if (migrate_x) s_tabs_off |= 1u << TR_TAB_X;
    s_tabs_off &= (uint8_t)~TR_TABS_FIXED;
    uint8_t book_auto = 0, comic_auto = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_AUTO, &book_auto) == ESP_OK) s_book_auto = book_auto;
    if (nvs_get_u8(h, NVS_KEY_COMIC_AUTO, &comic_auto) == ESP_OK) s_comic_auto = comic_auto;
    uint8_t lock_idle = 0;
    if (nvs_get_u8(h, NVS_KEY_LOCK_IDLE, &lock_idle) == ESP_OK && lock_idle < APP_LOCK_IDLE_LEVELS) {
        s_lock_idle = lock_idle;
    }
    size_t pin_len = sizeof(s_pin);
    s_pin_on = nvs_get_blob(h, NVS_KEY_PIN, s_pin, &pin_len) == ESP_OK && pin_len == sizeof(s_pin);
    nvs_close(h);
    if (migrate_x) {
        nvs_put_u8(NVS_KEY_TABS_OFF, s_tabs_off);
        nvs_put_u8(NVS_KEY_TABS_X, 1);
    }
    ESP_LOGI(
        TAG, "sleep mode %s, font %s",
        app_sleep_mode_name(s_sleep),
        s_font[0] != '\0' ? s_font : "(builtin)"
    );
}

app_sleep_mode_t app_settings_sleep_mode(void) {
    return s_sleep;
}

const char* app_sleep_mode_name(app_sleep_mode_t mode) {
    switch (mode) {
        case APP_SLEEP_LIGHT: return "light";
        case APP_SLEEP_DEEP: return "deep";
        case APP_SLEEP_OFF: return "off";
        default: return "?";
    }
}

void app_settings_set_sleep_mode(app_sleep_mode_t mode) {
    if (mode > APP_SLEEP_OFF) mode = APP_SLEEP_OFF;
    s_sleep = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SLEEP, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

const char* app_settings_font_path(void) {
    return s_font;
}

static void nvs_put_u8(const char* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_last_wake(void) {
    return s_last_wake;
}

void app_settings_set_last_wake(uint8_t src) {
    if (src == s_last_wake) return;
    s_last_wake = src;
    nvs_put_u8(NVS_KEY_WAKE, src);
}

uint8_t app_settings_last_boot(void) {
    return s_last_boot;
}

void app_settings_set_last_boot(uint8_t reason) {
    if (reason == 0 || reason == s_last_boot) return;
    s_last_boot = reason;
    nvs_put_u8(NVS_KEY_BOOT, reason);
}

bool app_settings_pickup_wake(void) {
    return s_pickup_wake;
}

void app_settings_set_pickup_wake(bool on) {
    if (s_pickup_wake == on) return;
    s_pickup_wake = on;
    nvs_put_u8(NVS_KEY_PICKUP, on ? 1 : 0);
}

void app_settings_set_font_path(const char* path) {
    if (path == NULL) path = "";
    strlcpy(s_font, path, sizeof(s_font));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_FONT, s_font);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_px(void) {
    return s_book_px;
}

void app_settings_set_book_px(uint8_t px) {
    px = valid_book_px(px);
    if (s_book_px == px) return;
    s_book_px = px;
    nvs_put_u8(NVS_KEY_BOOK_PX, px);
}

bool app_settings_book_shake(void) {
    return s_book_shake;
}

app_book_style_t app_settings_book_style(void) {
    return unpack_style(s_book_style);
}

void app_settings_set_book_style(const app_book_style_t* style) {
    const app_book_style_t clean = unpack_style(pack_style(style));
    const uint8_t v = pack_style(&clean);
    if (v == s_book_style) return;
    s_book_style = v;
    nvs_put_u8(NVS_KEY_BOOK_STYLE, v);
}

uint8_t app_settings_hidden_tabs(void) {
    return s_tabs_off;
}

void app_settings_set_hidden_tabs(uint8_t mask) {
    mask &= (uint8_t)~TR_TABS_FIXED;
    if (s_tabs_off == mask) return;
    s_tabs_off = mask;
    nvs_put_u8(NVS_KEY_TABS_OFF, mask);
    // 存过就说明掩码已经包含 X 的位，以后开机不再迁移（否则新设备打开 X 后重启又被关掉）。/ A saved mask already
    // covers the X bit; never migrate it again (otherwise a new device that turned X on would lose it on reboot).
    nvs_put_u8(NVS_KEY_TABS_X, 1);
}

uint8_t app_settings_auto_turn(bool comic) {
    const uint8_t v = comic ? s_comic_auto : s_book_auto;
    return v > 10 ? 10 : v;   // 坏值按最长一档 / Clamp a corrupt value to the longest level
}

void app_settings_set_auto_turn(bool comic, uint8_t level) {
    uint8_t* v = comic ? &s_comic_auto : &s_book_auto;
    if (*v == level) return;
    *v = level;
    nvs_put_u8(comic ? NVS_KEY_COMIC_AUTO : NVS_KEY_BOOK_AUTO, level);
}

void app_settings_set_book_shake(bool on) {
    if (s_book_shake == on) return;
    s_book_shake = on;
    nvs_put_u8(NVS_KEY_BOOK_SHAKE, on ? 1 : 0);
}

/* ---- 自动锁屏 / Idle lock ---- */

static const uint8_t LOCK_IDLE_MIN[APP_LOCK_IDLE_LEVELS] = {1, 2, 5, 10, 0};
static const char* const LOCK_IDLE_LABEL[APP_LOCK_IDLE_LEVELS] = {"1 分钟", "2 分钟", "5 分钟", "10 分钟", "从不"};

uint8_t app_settings_lock_idle(void) {
    return s_lock_idle;
}

void app_settings_set_lock_idle(uint8_t level) {
    if (level >= APP_LOCK_IDLE_LEVELS || level == s_lock_idle) return;
    s_lock_idle = level;
    nvs_put_u8(NVS_KEY_LOCK_IDLE, level);
}

int64_t app_settings_lock_idle_ms(void) {
    return (int64_t)LOCK_IDLE_MIN[s_lock_idle] * 60 * 1000;
}

const char* app_settings_lock_idle_label(uint8_t level) {
    return level < APP_LOCK_IDLE_LEVELS ? LOCK_IDLE_LABEL[level] : "";
}

/* ---- 锁屏密码 / Lock PIN ---- */

static bool pin_valid(const char* pin) {
    if (!pin || strlen(pin) != 4) return false;
    for (int i = 0; i < 4; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

static bool pin_hash(const uint8_t* salt, const char* pin, uint8_t out[PIN_HASH_LEN]) {
    uint8_t buf[PIN_SALT_LEN + 4];
    memcpy(buf, salt, PIN_SALT_LEN);
    memcpy(buf + PIN_SALT_LEN, pin, 4);
    // IDF 6 的 mbedtls 走 PSA 接口；初始化可以重复调用。/ IDF 6's mbedtls uses the PSA API; init may be repeated.
    size_t len = 0;
    const bool ok = psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_compute(PSA_ALG_SHA_256, buf, sizeof(buf), out, PIN_HASH_LEN, &len) == PSA_SUCCESS &&
        len == PIN_HASH_LEN;
    memset(buf, 0, sizeof(buf));
    return ok;
}

bool app_settings_pin_enabled(void) {
    return s_pin_on;
}

bool app_settings_pin_check(const char* pin) {
    if (!s_pin_on || !pin_valid(pin)) return false;
    uint8_t h[PIN_HASH_LEN];
    if (!pin_hash(s_pin, pin, h)) return false;
    uint8_t diff = 0;
    for (int i = 0; i < PIN_HASH_LEN; i++) diff |= h[i] ^ s_pin[PIN_SALT_LEN + i];
    return diff == 0;
}

esp_err_t app_settings_pin_set(const char* pin) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (!pin || !pin[0]) {
        err = nvs_erase_key(h, NVS_KEY_PIN);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err == ESP_OK) { s_pin_on = false; memset(s_pin, 0, sizeof(s_pin)); }
        return err;
    }
    if (!pin_valid(pin)) { nvs_close(h); return ESP_ERR_INVALID_ARG; }
    uint8_t rec[PIN_SALT_LEN + PIN_HASH_LEN];
    esp_fill_random(rec, PIN_SALT_LEN);
    if (!pin_hash(rec, pin, rec + PIN_SALT_LEN)) { nvs_close(h); return ESP_FAIL; }
    err = nvs_set_blob(h, NVS_KEY_PIN, rec, sizeof(rec));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) { memcpy(s_pin, rec, sizeof(rec)); s_pin_on = true; }
    return err;
}
