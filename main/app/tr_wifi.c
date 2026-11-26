/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：保存的多个 Wi-Fi，见 tr_wifi.h。卡上的读写和钥匙由一把互斥锁串起来（界面和番茄联网任务都会调）；
 * 列表放堆上，用完擦除。
 *
 * English: several saved Wi-Fi networks, see tr_wifi.h. Card I/O and the key sit behind one mutex because both the UI
 * and the Tomato networking task call in; the list lives on the heap and is wiped after use.
 */

#include "tr_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "read_pico_wifi_vault.h"
#include "tomato.h"

#define TAG "tr_wifi"
#define NS "rp_wifi"
#define KEY_CARD "card_key"
// 用当前网络连上时，卡上的时间旧过这么久才重写，免得每次联网都写卡。/ When joining with the current network, rewrite the
// card only if its time is older than this, so not every connection writes the card.
#define TOUCH_S (6 * 3600)

static SemaphoreHandle_t s_mu;
static StaticSemaphore_t s_mu_buf;
static portMUX_TYPE s_name_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_name[33];    // 当前网络名，各任务都可能改 / Current network name, changed from any task
static char s_shown[33];   // 交给状态栏的副本 / The copy handed to the status bar

static void lock(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

static uint32_t now_or_zero(void) {
    return tomato_time_valid() ? (uint32_t)time(NULL) : 0;
}

bool tr_wifi_card_ok(void) {
    read_pico_sd_info_t info;
    return read_pico_sd_get_info(&info) == ESP_OK && info.present && info.mounted;
}

/* ---- 钥匙和文件 / Key and file ---- */

// 取文件钥匙；create 时没有就新建设备钥匙存进 NVS。/ Get the file key; with create, make and store a device key when
// there is none.
static bool key_get(uint8_t key[WIFI_VAULT_KEY_LEN], bool create) {
    uint8_t dev[WIFI_VAULT_KEY_LEN];
    nvs_handle_t h;
    size_t n = sizeof(dev);
    bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, KEY_CARD, dev, &n) == ESP_OK && n == sizeof(dev);
        nvs_close(h);
    }
    if (!ok && create) {
        // 射频没开时先打开硬件熵源（不能和 Wi-Fi 同时开；Wi-Fi 开着时随机数本来就是真随机）。/ Enable the hardware
        // entropy source while the radio is off (it must not run alongside Wi-Fi, which already makes the RNG truly
        // random).
        wifi_mode_t mode;
        const bool radio = esp_wifi_get_mode(&mode) == ESP_OK;
        if (!radio) bootloader_random_enable();
        const bool rnd = wifi_vault_random(dev, sizeof(dev));
        if (!radio) bootloader_random_disable();
        if (rnd && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            ok = nvs_set_blob(h, KEY_CARD, dev, sizeof(dev)) == ESP_OK && nvs_commit(h) == ESP_OK;
            nvs_close(h);
        }
        ESP_LOGI(TAG, "card key created=%d", ok);
    }
    if (ok) ok = wifi_vault_derive(dev, key) == WIFI_VAULT_OK;
    wifi_vault_wipe(dev, sizeof(dev));
    return ok;
}

// 读卡上的列表。读不开（没钥匙、钥匙不对、坏文件）当空列表、返回 OK；没卡或读写失败返回错误，这时不能覆盖。
// Read the card list. Unreadable (no key, wrong key, corrupt) counts as empty and returns OK; no card or an I/O failure
// returns an error, and then the file must not be overwritten.
static esp_err_t vault_read(wifi_vault_t* v) {
    memset(v, 0, sizeof(*v));
    if (!tr_wifi_card_ok()) return ESP_ERR_NOT_FOUND;
    uint8_t key[WIFI_VAULT_KEY_LEN];
    if (!key_get(key, false)) return ESP_OK;
    const wifi_vault_err_t err = wifi_vault_load(TR_WIFI_PATH, key, v);
    wifi_vault_wipe(key, sizeof(key));
    if (err != WIFI_VAULT_OK && err != WIFI_VAULT_ERR_NOENT) ESP_LOGW(TAG, "card list unreadable: %d", (int)err);
    return err == WIFI_VAULT_ERR_IO ? ESP_FAIL : ESP_OK;
}

static esp_err_t vault_write(const wifi_vault_t* v) {
    if (!tr_wifi_card_ok()) return ESP_ERR_NOT_FOUND;
    uint8_t key[WIFI_VAULT_KEY_LEN];
    if (!key_get(key, true)) return ESP_FAIL;
    mkdir("/sdcard/treader", 0755);
    mkdir("/sdcard/treader/wifi", 0755);
    const wifi_vault_err_t err = wifi_vault_save(TR_WIFI_PATH, key, v);
    wifi_vault_wipe(key, sizeof(key));
    ESP_LOGI(TAG, "card list saved count=%d result=%d", v->count, (int)err);
    return err == WIFI_VAULT_OK ? ESP_OK : ESP_FAIL;
}

static wifi_vault_t* vault_new(void) { return calloc(1, sizeof(wifi_vault_t)); }

static void vault_free(wifi_vault_t* v) {
    if (!v) return;
    wifi_vault_wipe(v, sizeof(*v));
    free(v);
}

// 把一个网络写进卡（锁已拿着）；only_if_stale 时内容没变、时间也不旧就不写。/ Put one network on the card (lock held);
// with only_if_stale, skip when nothing changed and the time is recent.
static esp_err_t card_put_locked(const char* ssid, const char* pass, bool only_if_stale) {
    wifi_vault_t* v = vault_new();
    if (!v) return ESP_ERR_NO_MEM;
    esp_err_t err = vault_read(v);
    if (err == ESP_OK) {
        const uint32_t now = now_or_zero();
        const int i = wifi_vault_find(v, ssid);
        const bool same = i >= 0 && !strcmp(v->items[i].password, pass);
        const bool stale = i < 0 || (now && now - v->items[i].last_ok > TOUCH_S);
        if (!only_if_stale || !same || stale)
            err = wifi_vault_put(v, ssid, pass, now) ? vault_write(v) : ESP_ERR_INVALID_ARG;
    }
    vault_free(v);
    return err;
}

/* ---- 当前网络名 / Current name ---- */

void tr_wifi_refresh_name(void) {
    char ssid[33];
    bool configured = false;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    // 传书组件的凭据存储可能正被别的任务短暂占着。/ The transfer credential store may be briefly held by another task.
    for (int i = 0; i < 5 && err == ESP_ERR_INVALID_STATE; ++i) {
        err = read_pico_transfer_get_current_wifi(ssid, &configured, NULL);
        if (err == ESP_ERR_INVALID_STATE) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) return;
    portENTER_CRITICAL(&s_name_lock);
    snprintf(s_name, sizeof(s_name), "%s", configured ? ssid : "");
    portEXIT_CRITICAL(&s_name_lock);
}

const char* tr_wifi_current_name(void) {
    portENTER_CRITICAL(&s_name_lock);
    memcpy(s_shown, s_name, sizeof(s_shown));
    portEXIT_CRITICAL(&s_name_lock);
    return s_shown;
}

/* ---- 番茄联网回调 / Tomato networking hooks ---- */

static int hook_candidates(const char* failed, tomato_wifi_cred_t* out, int max) {
    wifi_vault_t* v = vault_new();
    read_pico_transfer_network_t* nets = calloc(READ_PICO_TRANSFER_SCAN_MAX, sizeof(*nets));
    int n = 0;
    if (!v || !nets) goto done;
    lock();
    vault_read(v);
    // 临时网络连不上时，NVS 里那个也是候选（没卡时它不在列表里）。/ When the temporary network fails, the NVS one is a
    // candidate too (without a card it is not on the list).
    char ssid[33], pass[65];
    if (read_pico_transfer_load_saved_wifi(ssid, pass) == ESP_OK && wifi_vault_find(v, ssid) < 0)
        wifi_vault_put(v, ssid, pass, 0);
    wifi_vault_wipe(pass, sizeof(pass));
    unlock();
    wifi_vault_remove(v, failed);
    if (!v->count) goto done;
    size_t found = 0;
    if (read_pico_transfer_scan_wifi(nets, &found) != ESP_OK) goto done;
    // 扫描结果已按信号从强到弱。/ Scan results are already strongest first.
    for (size_t i = 0; i < found && n < max; ++i) {
        const int j = wifi_vault_find(v, nets[i].ssid);
        if (j < 0 || !nets[i].supported) continue;
        memcpy(out[n].ssid, v->items[j].ssid, sizeof(out[n].ssid));
        memcpy(out[n].password, v->items[j].password, sizeof(out[n].password));
        ++n;
    }
    ESP_LOGI(TAG, "fallback candidates=%d saved=%d scanned=%u", n, v->count, (unsigned)found);
done:
    vault_free(v);
    free(nets);
    return n;
}

static void hook_joined(const char* ssid, const char* pass, bool fallback) {
    lock();
    if (fallback) {
        // 换到的网络记成当前网络。/ The network switched to becomes the current one.
        if (read_pico_transfer_set_session_wifi(ssid, pass) == ESP_OK) read_pico_transfer_commit_session_wifi();
        ESP_LOGI(TAG, "switched to another saved network");
    }
    card_put_locked(ssid, pass, !fallback);
    unlock();
    tr_wifi_refresh_name();
}

static const tomato_wifi_hooks_t HOOKS = {.candidates = hook_candidates, .joined = hook_joined};

void tr_wifi_init(void) {
    if (!s_mu) s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
    tomato_net_set_wifi_hooks(&HOOKS);
    tr_wifi_refresh_name();
}

/* ---- 界面用 / For the UI ---- */

int tr_wifi_list(tr_wifi_item_t* out, int max) {
    if (!out || max <= 0) return 0;
    wifi_vault_t* v = vault_new();
    if (!v) return 0;
    char current[33], stored[33];
    bool has_current = false, has_stored = false;
    lock();
    vault_read(v);
    unlock();
    read_pico_transfer_get_current_wifi(current, &has_current, NULL);
    read_pico_transfer_get_saved_wifi(stored, &has_stored);
    wifi_vault_sort(v);
    int n = 0;
    if (has_current) {
        const int i = wifi_vault_find(v, current);
        out[n] = (tr_wifi_item_t){.last_ok = i >= 0 ? v->items[i].last_ok : 0, .current = true, .on_card = i >= 0,
                                  .saved = i >= 0 || (has_stored && !strcmp(stored, current))};
        memcpy(out[n].ssid, current, sizeof(out[n].ssid));
        ++n;
    }
    for (int i = 0; i < v->count && n < max; ++i) {
        if (has_current && !strcmp(v->items[i].ssid, current)) continue;
        out[n] = (tr_wifi_item_t){.last_ok = v->items[i].last_ok, .on_card = true, .saved = true};
        memcpy(out[n].ssid, v->items[i].ssid, sizeof(out[n].ssid));
        ++n;
    }
    // 临时网络在用时，NVS 里那个不在卡上也要列出来。/ While a temporary network is in use, list the NVS one even when it
    // is not on the card.
    if (has_stored && n < max && (!has_current || strcmp(stored, current)) && wifi_vault_find(v, stored) < 0) {
        out[n] = (tr_wifi_item_t){.saved = true};
        memcpy(out[n].ssid, stored, sizeof(out[n].ssid));
        ++n;
    }
    vault_free(v);
    return n;
}

bool tr_wifi_take(const char* ssid, char pass[65]) {
    pass[0] = 0;
    if (!ssid || !ssid[0]) return false;
    char name[33];
    if (read_pico_transfer_load_current_wifi(name, pass, NULL) == ESP_OK && !strcmp(name, ssid)) return true;
    if (read_pico_transfer_load_saved_wifi(name, pass) == ESP_OK && !strcmp(name, ssid)) return true;
    wifi_vault_wipe(pass, 65);
    wifi_vault_t* v = vault_new();
    if (!v) return false;
    lock();
    vault_read(v);
    unlock();
    const int i = wifi_vault_find(v, ssid);
    if (i >= 0) memcpy(pass, v->items[i].password, 65);
    vault_free(v);
    return i >= 0;
}

esp_err_t tr_wifi_commit(const char* ssid, const char* pass) {
    lock();
    esp_err_t err = read_pico_transfer_set_session_wifi(ssid, pass);
    if (err == ESP_OK) err = read_pico_transfer_commit_session_wifi();
    // 卡写不上（没卡）也算保存成功：NVS 里已经有了。/ A failed card write (no card) still counts: NVS has it.
    if (err == ESP_OK) card_put_locked(ssid, pass, false);
    unlock();
    tr_wifi_refresh_name();
    ESP_LOGI(TAG, "commit result=%s", esp_err_to_name(err));
    return err;
}

esp_err_t tr_wifi_use_once(const char* ssid, const char* pass) {
    const esp_err_t err = read_pico_transfer_set_session_wifi(ssid, pass);
    tr_wifi_refresh_name();
    return err;
}

esp_err_t tr_wifi_forget(const char* ssid) {
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_vault_t* v = vault_new();
    if (!v) return ESP_ERR_NO_MEM;
    lock();
    esp_err_t err = vault_read(v);
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;  // 没卡就只删 NVS / Without a card only NVS is cleared
    else if (err == ESP_OK && wifi_vault_remove(v, ssid)) err = vault_write(v);
    char name[33];
    bool configured = false, session = false;
    if (err == ESP_OK && read_pico_transfer_get_saved_wifi(name, &configured) == ESP_OK && configured &&
        !strcmp(name, ssid))
        err = read_pico_transfer_forget_wifi();
    if (err == ESP_OK && read_pico_transfer_get_current_wifi(name, &configured, &session) == ESP_OK && session &&
        !strcmp(name, ssid))
        err = read_pico_transfer_set_session_wifi(NULL, NULL);
    unlock();
    vault_free(v);
    tr_wifi_refresh_name();
    ESP_LOGI(TAG, "forget result=%s", esp_err_to_name(err));
    return err;
}

void tr_wifi_joined_current(void) {
    char ssid[33], pass[65];
    bool session = false;
    if (read_pico_transfer_load_current_wifi(ssid, pass, &session) == ESP_OK && !session) {
        lock();
        card_put_locked(ssid, pass, true);
        unlock();
    }
    wifi_vault_wipe(pass, sizeof(pass));
}
