/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 一次性 STA 连网：用传书页保存的凭据，连上、用完、全部释放。
 * One-shot STA networking: uses credentials saved by the transfer page; connect, use, release everything.
 *
 * 冻结：传书服务运行时不连网；不保存凭据副本，密码用毕擦除。
 * Frozen: never connect while the transfer service runs; keep no credential copy, wipe the password after use.
 */

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <stdlib.h>
#include <time.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "read_pico_transfer.h"
#include "tomato_internal.h"

#define TAG "tomato_net"
#define BIT_IP BIT0
#define BIT_FAIL BIT1
#define RETRY_MAX 3

static EventGroupHandle_t s_bits;
static esp_netif_t* s_netif;
static esp_event_handler_instance_t s_wifi_h, s_ip_h;
static bool s_loop_owned, s_inited, s_started;
static int s_retries;
static bool s_sntp;
static volatile bool s_time_fresh;

bool tomato_take_time_synced(void) {
    bool fresh = s_time_fresh;
    s_time_fresh = false;
    return fresh;
}

// 2025 年以后才算对过时。/ Treat time as set once it is past 2025.
bool tomato_time_valid(void) { return time(NULL) > 1735689600; }

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < RETRY_MAX) esp_wifi_connect();
        else xEventGroupSetBits(s_bits, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_bits, BIT_IP);
    }
}

esp_err_t tomato_net_up(int timeout_ms, const volatile bool* cancel) {
    read_pico_transfer_status_t ts;
    read_pico_transfer_get_status(&ts);
    if (ts.state != READ_PICO_TRANSFER_STOPPED || s_netif) return ESP_ERR_INVALID_STATE;

    char ssid[33], pass[65];
    // 设置页读 SSID 时会短暂占着凭据存储，稍等重试。/ The settings page briefly holds the credential store; retry.
    esp_err_t err = ESP_ERR_INVALID_STATE;
    for (int i = 0; i < 5 && err == ESP_ERR_INVALID_STATE; i++) {
        err = read_pico_transfer_load_wifi(ssid, pass);
        if (err == ESP_ERR_INVALID_STATE) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) return err;

    if (!s_bits) s_bits = xEventGroupCreate();
    if (!s_bits) { err = ESP_ERR_NO_MEM; goto fail; }
    xEventGroupClearBits(s_bits, BIT_IP | BIT_FAIL);
    s_retries = 0;

    err = esp_netif_init();
    if (err != ESP_OK) goto fail;
    err = esp_event_loop_create_default();
    if (err == ESP_OK) s_loop_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) goto fail;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) goto fail;
    s_inited = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL, &s_wifi_h);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &s_ip_h);
    if (err != ESP_OK) goto fail;

    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    memcpy(wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    // 与传书页一致，WPA3 网络也能连。/ Match the transfer page so WPA3 networks work too.
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    if (err != ESP_OK) goto fail;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_started = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) goto fail;

    // 分段等待，取消时及时退出。/ Wait in slices so a cancel takes effect promptly.
    EventBits_t bits = 0;
    for (int waited = 0; waited < timeout_ms && !(bits & (BIT_IP | BIT_FAIL)); waited += 250) {
        if (cancel && *cancel) break;
        bits = xEventGroupWaitBits(s_bits, BIT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(250));
    }
    err = (bits & BIT_IP) ? ESP_OK : (bits & BIT_FAIL) ? ESP_FAIL : (cancel && *cancel) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    if (err != ESP_OK) goto fail;
    memset(pass, 0, sizeof(pass));
    ESP_LOGI(TAG, "connected");
    // 顺便对时，最多等 3 秒；失败不影响后续操作。/ Sync time on the way, waiting at most 3 s; failure is harmless.
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&sc) == ESP_OK) {
        s_sntp = true;
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000)) == ESP_OK && tomato_time_valid()) s_time_fresh = true;
    }
    return ESP_OK;

fail:
    memset(pass, 0, sizeof(pass));
    ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    tomato_net_down();
    return err;
}

void tomato_net_down(void) {
    if (s_sntp) { esp_netif_sntp_deinit(); s_sntp = false; }
    // 先注销回调，免得断开事件又触发重连。/ Unregister first so the disconnect event cannot trigger a reconnect.
    if (s_wifi_h) { esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_h); s_wifi_h = NULL; }
    if (s_ip_h) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_h); s_ip_h = NULL; }
    if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    if (s_inited) { esp_wifi_deinit(); s_inited = false; }
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    if (s_loop_owned) { esp_event_loop_delete_default(); s_loop_owned = false; }
}
