/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证器会话，见 tr_auth.h。钥匙和账号表放内部 RAM，一把互斥锁保护它们的建立和清除；离开计时器到点时在
 * 定时器任务里上锁。
 *
 * English: the authenticator session, see tr_auth.h. The key and table sit in internal RAM behind one mutex that
 * guards creating and clearing them; the away timer locks from the timer task.
 */

#include "tr_auth.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "bootloader_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "read_pico_sd.h"

#define TAG "tr_auth"
#define NS "totp"
#define KEY_DEV "dev"
#define KEY_FAILS "fails"
#define KEY_AUTO "auto"
#define KEY_PIN "pin"           // 1 = 这份验证器用 4 位 PIN（没有这个键是旧的字母数字口令）/ 1 = this vault uses a 4-digit PIN (no key means the old alphanumeric passphrase)
#define AWAY_US (5LL * 60 * 1000000)

static SemaphoreHandle_t s_mu;
static StaticSemaphore_t s_mu_buf;
static totp_key_t* s_key;       // 解锁时非空 / Non-NULL while unlocked
static totp_table_t* s_table;
static int s_fails = -1;        // -1 = 还没读 NVS / Not read from NVS yet
static int64_t s_until_us;      // 到这个时刻前不能再输 / No tries before this moment
static int s_auto = -1;
static int s_pin = -1;          // KEY_PIN 的缓存，-1 = 还没读 / Cache of KEY_PIN, -1 = not read yet
static esp_timer_handle_t s_away;

static void take(void) {
    if (!s_mu) s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
    xSemaphoreTake(s_mu, portMAX_DELAY);
}
static void give(void) { xSemaphoreGive(s_mu); }

/* ---- NVS ---- */

static bool dev_load(uint8_t dev[TOTP_DEV_SECRET_LEN]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = TOTP_DEV_SECRET_LEN;
    const bool ok = nvs_get_blob(h, KEY_DEV, dev, &n) == ESP_OK && n == TOTP_DEV_SECRET_LEN;
    nvs_close(h);
    if (!ok) totp_wipe(dev, TOTP_DEV_SECRET_LEN);
    return ok;
}

static bool dev_exists(void) {
    uint8_t dev[TOTP_DEV_SECRET_LEN];
    const bool ok = dev_load(dev);
    totp_wipe(dev, sizeof(dev));
    return ok;
}

static void put_u8(const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t get_u8(const char* key, uint8_t def) {
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = def;
    nvs_close(h);
    return v;
}

// 第 5 次起要等：30 秒、1 分、2 分……最长 15 分钟。/ From the fifth miss: 30 s, 1 min, 2 min... up to 15 min.
static int wait_for(int fails) {
    if (fails <= TR_AUTH_FREE_TRIES) return 0;
    int s = 30;
    for (int i = TR_AUTH_FREE_TRIES + 1; i < fails && s < 900; i++) s *= 2;
    return s > 900 ? 900 : s;
}

static void load_fails(void) {
    if (s_fails >= 0) return;
    s_fails = get_u8(KEY_FAILS, 0);
    // 重启不清零：开机时等待从头算。/ A restart does not reset it: the wait starts over at boot.
    s_until_us = esp_timer_get_time() + (int64_t)wait_for(s_fails) * 1000000;
}

static void set_fails(int n) {
    s_fails = n > 255 ? 255 : n;
    put_u8(KEY_FAILS, (uint8_t)s_fails);
    s_until_us = esp_timer_get_time() + (int64_t)wait_for(s_fails) * 1000000;
}

/* ---- 状态 / State ---- */

static bool card_ok(void) {
    read_pico_sd_info_t info;
    return read_pico_sd_get_info(&info) == ESP_OK && info.present && info.mounted;
}

tr_auth_state_t tr_auth_state(void) {
    if (!card_ok()) return TR_AUTH_NOCARD;
    const bool file = totp_vault_exists(TR_AUTH_PATH), dev = dev_exists();
    if (!file && !dev) return TR_AUTH_UNSET;
    if (file && !dev) return TR_AUTH_FOREIGN;
    if (!file) return TR_AUTH_MISSING;
    return s_key ? TR_AUTH_OPEN : TR_AUTH_LOCKED;
}

bool tr_auth_pass_ok(const char* pass, const char** why) {
    const size_t n = pass ? strlen(pass) : 0;
    bool digits = n == TR_AUTH_PIN_LEN;
    for (size_t i = 0; digits && i < n; i++) digits = pass[i] >= '0' && pass[i] <= '9';
    const char* r = digits ? NULL : "PIN 要正好 4 位数字";
    if (why) *why = r;
    return r == NULL;
}

bool tr_auth_pin_mode(void) {
    if (s_pin < 0) s_pin = get_u8(KEY_PIN, 0) == 1;
    return s_pin == 1;
}

// 设置或改口令成功后记下这份验证器用 PIN。/ After a successful setup or change, note that this vault uses a PIN.
static void mark_pin(void) {
    put_u8(KEY_PIN, 1);
    s_pin = 1;
}

int tr_auth_wait_s(void) {
    load_fails();
    const int64_t left = s_until_us - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

int tr_auth_fails(void) {
    load_fails();
    return s_fails;
}

/* ---- 会话 / Session ---- */

// 拿到钥匙和账号表后装进会话（转交所有权）。/ Install a key and table into the session (takes ownership).
static void install(totp_key_t* k, totp_table_t* t) {
    take();
    if (s_key) { totp_wipe(s_key, sizeof(*s_key)); heap_caps_free(s_key); }
    if (s_table) { totp_wipe(s_table, sizeof(*s_table)); heap_caps_free(s_table); }
    s_key = k;
    s_table = t;
    give();
}

static bool alloc_pair(totp_key_t** k, totp_table_t** t) {
    // 优先内部 RAM；不够时退到 PSRAM（同样只在通电时存在）。/ Prefer internal RAM; fall back to PSRAM, which
    // likewise holds data only while powered.
    *k = heap_caps_calloc(1, sizeof(**k), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *t = heap_caps_calloc(1, sizeof(**t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!*t) *t = heap_caps_calloc(1, sizeof(**t), MALLOC_CAP_8BIT);
    if (*k && *t) return true;
    heap_caps_free(*k);
    heap_caps_free(*t);
    *k = NULL;
    *t = NULL;
    return false;
}

static void free_pair(totp_key_t* k, totp_table_t* t) {
    if (k) { totp_wipe(k, sizeof(*k)); heap_caps_free(k); }
    if (t) { totp_wipe(t, sizeof(*t)); heap_caps_free(t); }
}

static void make_dirs(void) {
    mkdir("/sdcard/treader", 0755);
    mkdir("/sdcard/treader/totp", 0755);
}

totp_err_t tr_auth_setup(const char* pass) {
    if (!tr_auth_pass_ok(pass, NULL)) return TOTP_ERR_ARG;
    if (tr_auth_state() != TR_AUTH_UNSET) return TOTP_ERR_ARG;
    uint8_t dev[TOTP_DEV_SECRET_LEN];
    // 打开硬件熵源再取随机数（没开 Wi-Fi 时默认不是真随机）。/ Enable the hardware entropy source first (without
    // Wi-Fi the default is not truly random).
    bootloader_random_enable();
    const bool rnd = totp_random(dev, sizeof(dev));
    bootloader_random_disable();
    if (!rnd) return TOTP_ERR_CRYPTO;
    totp_key_t* k;
    totp_table_t* t;
    if (!alloc_pair(&k, &t)) {
        totp_wipe(dev, sizeof(dev));
        return TOTP_ERR_NOMEM;
    }
    // 4 位 PIN 的迭代次数固定，解锁约 1 秒（设备上算一次 1 万次约 0.9 秒）。/ A fixed count for the 4-digit PIN, about
    // 1 s to unlock (10,000 rounds take about 0.9 s on the device).
    totp_err_t err = totp_key_new(pass, dev, TOTP_PIN_ITERS, k);
    if (err == TOTP_OK) {
        t->next_id = 1;
        make_dirs();
        err = totp_vault_save(TR_AUTH_PATH, k, t);
    }
    if (err == TOTP_OK) {
        nvs_handle_t h;
        esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_set_blob(h, KEY_DEV, dev, sizeof(dev));
            if (e == ESP_OK) e = nvs_commit(h);
            nvs_close(h);
        }
        if (e != ESP_OK) {
            totp_vault_remove(TR_AUTH_PATH);
            err = TOTP_ERR_IO;
        }
    }
    totp_wipe(dev, sizeof(dev));
    if (err != TOTP_OK) {
        free_pair(k, t);
        return err;
    }
    set_fails(0);
    mark_pin();
    install(k, t);
    return TOTP_OK;
}

totp_err_t tr_auth_unlock(const char* pass) {
    if (tr_auth_wait_s() > 0) return TOTP_ERR_ARG;
    uint8_t dev[TOTP_DEV_SECRET_LEN];
    if (!dev_load(dev)) return TOTP_ERR_NOENT;
    totp_key_t* k;
    totp_table_t* t;
    if (!alloc_pair(&k, &t)) {
        totp_wipe(dev, sizeof(dev));
        return TOTP_ERR_NOMEM;
    }
    const totp_err_t err = totp_vault_unlock(TR_AUTH_PATH, pass, dev, k, t);
    totp_wipe(dev, sizeof(dev));
    if (err != TOTP_OK) {
        free_pair(k, t);
        if (err == TOTP_ERR_AUTH) set_fails(tr_auth_fails() + 1);
        return err;
    }
    if (tr_auth_fails()) set_fails(0);
    install(k, t);
    return TOTP_OK;
}

totp_err_t tr_auth_change(const char* old_pass, const char* new_pass) {
    if (!tr_auth_pass_ok(new_pass, NULL)) return TOTP_ERR_ARG;
    if (tr_auth_wait_s() > 0) return TOTP_ERR_ARG;
    uint8_t dev[TOTP_DEV_SECRET_LEN];
    if (!dev_load(dev)) return TOTP_ERR_NOENT;
    totp_key_t* k;
    totp_table_t* t;
    if (!alloc_pair(&k, &t)) {
        totp_wipe(dev, sizeof(dev));
        return TOTP_ERR_NOMEM;
    }
    const bool was_open = s_key != NULL;
    totp_err_t err = totp_vault_unlock(TR_AUTH_PATH, old_pass, dev, k, t);
    if (err == TOTP_ERR_AUTH) set_fails(tr_auth_fails() + 1);
    if (err == TOTP_OK) {
        if (tr_auth_fails()) set_fails(0);
        err = totp_key_new(new_pass, dev, TOTP_PIN_ITERS, k);
    }
    if (err == TOTP_OK) err = totp_vault_save(TR_AUTH_PATH, k, t);
    if (err == TOTP_OK) {
        totp_vault_drop_backup(TR_AUTH_PATH);
        mark_pin();
    }
    totp_wipe(dev, sizeof(dev));
    // 改之前开着就保持开着（换成新钥匙），否则保持上锁。/ Stay unlocked with the new key if it was open, else locked.
    if (err == TOTP_OK && was_open) install(k, t);
    else free_pair(k, t);
    return err;
}

void tr_auth_lock(void) {
    take();
    const bool had = s_key != NULL;
    free_pair(s_key, s_table);
    s_key = NULL;
    s_table = NULL;
    give();
    if (had) ESP_LOGI(TAG, "locked");
}

void tr_auth_wipe(void) {
    tr_auth_lock();
    totp_vault_remove(TR_AUTH_PATH);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_DEV);
        nvs_erase_key(h, KEY_FAILS);
        nvs_erase_key(h, KEY_PIN);
        nvs_commit(h);
        nvs_close(h);
    }
    s_fails = 0;
    s_until_us = 0;
    s_pin = 0;
}

totp_table_t* tr_auth_table(void) { return s_table; }

totp_err_t tr_auth_save(void) {
    if (!s_key || !s_table) return TOTP_ERR_ARG;
    make_dirs();
    return totp_vault_save(TR_AUTH_PATH, s_key, s_table);
}

int tr_auth_count(void) {
    take();
    const int n = s_table ? s_table->count : -1;
    give();
    return n;
}

/* ---- 自动上锁 / Auto-lock ---- */

tr_auth_auto_t tr_auth_auto_lock(void) {
    if (s_auto < 0) s_auto = get_u8(KEY_AUTO, TR_AUTH_LOCK_ON_SLEEP) ? TR_AUTH_LOCK_AWAY : TR_AUTH_LOCK_ON_SLEEP;
    return (tr_auth_auto_t)s_auto;
}

void tr_auth_set_auto_lock(tr_auth_auto_t mode) {
    s_auto = mode == TR_AUTH_LOCK_AWAY ? TR_AUTH_LOCK_AWAY : TR_AUTH_LOCK_ON_SLEEP;
    put_u8(KEY_AUTO, (uint8_t)s_auto);
}

static void away_cb(void* arg) {
    (void)arg;
    tr_auth_lock();
}

void tr_auth_enter(void) {
    if (s_away) esp_timer_stop(s_away);
}

void tr_auth_leave(void) {
    if (!s_key || tr_auth_auto_lock() != TR_AUTH_LOCK_AWAY) return;
    if (!s_away) {
        const esp_timer_create_args_t args = {.callback = away_cb, .name = "auth_away"};
        if (esp_timer_create(&args, &s_away) != ESP_OK) return;
    }
    esp_timer_stop(s_away);
    esp_timer_start_once(s_away, AWAY_US);
}
