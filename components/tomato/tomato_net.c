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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < RETRY_MAX) esp_wifi_connect();
        else xEventGroupSetBits(s_bits, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_bits, BIT_IP);
    }
}

esp_err_t tomato_net_up(int timeout_ms) {
    read_pico_transfer_status_t ts;
    read_pico_transfer_get_status(&ts);
    if (ts.state != READ_PICO_TRANSFER_STOPPED || s_netif) return ESP_ERR_INVALID_STATE;

    char ssid[33], pass[65];
    esp_err_t err = read_pico_transfer_load_wifi(ssid, pass);
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
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    if (err != ESP_OK) goto fail;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_started = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) goto fail;

    EventBits_t bits = xEventGroupWaitBits(s_bits, BIT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    err = (bits & BIT_IP) ? ESP_OK : (bits & BIT_FAIL) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    if (err != ESP_OK) goto fail;
    memset(pass, 0, sizeof(pass));
    ESP_LOGI(TAG, "connected");
    return ESP_OK;

fail:
    memset(pass, 0, sizeof(pass));
    ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    tomato_net_down();
    return err;
}

void tomato_net_down(void) {
    if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    if (s_wifi_h) { esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_h); s_wifi_h = NULL; }
    if (s_ip_h) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_h); s_ip_h = NULL; }
    if (s_inited) { esp_wifi_deinit(); s_inited = false; }
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    if (s_loop_owned) { esp_event_loop_delete_default(); s_loop_owned = false; }
}
