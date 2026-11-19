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
static bool s_sntp_allowed = true;
static bool s_public_dns;

// 路由器的 DNS 解析失败时改用公共 DNS（阿里 223.5.5.5，腾讯 119.29.29.29 备用）。很多路由器开了“DNS 重绑定保护”，
// 会拦掉解析到内网 IP 的公网域名（比如自建服务器），电脑、手机若用公共 DNS 就看不出来。只在本次联网期间生效。
// When the router's DNS fails, switch to public DNS (AliDNS 223.5.5.5, Tencent 119.29.29.29 as backup). Many routers
// enable DNS-rebinding protection and drop public names that resolve to LAN IPs (such as a self-hosted server), which
// phones and PCs on public DNS never notice. Lasts for this connection only.
bool tomato_net_use_public_dns(void) {
    if (!s_netif || s_public_dns) return false;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("223.5.5.5");
    if (esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return false;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("119.29.29.29");
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns);
    s_public_dns = true;
    ESP_LOGW(TAG, "router DNS failed, using public DNS");
    return true;
}
static volatile bool s_time_fresh;

bool tomato_take_time_synced(void) {
    bool fresh = s_time_fresh;
    s_time_fresh = false;
    return fresh;
}

// 2025 年以后才算对过时。/ Treat time as set once it is past 2025.
void tomato_net_set_sntp(bool on) { s_sntp_allowed = on; }

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
    // 传书页正占着 Wi-Fi：返回 ESP_ERR_NOT_SUPPORTED，和“取消”（ESP_ERR_INVALID_STATE）区分开，提示才不会被吞掉。
    // The transfer page holds Wi-Fi: return ESP_ERR_NOT_SUPPORTED, distinct from a cancel (ESP_ERR_INVALID_STATE), so the
    // message is not swallowed.
    if (ts.state != READ_PICO_TRANSFER_STOPPED) {
        tomato_http_set_detail("传书还在占用 Wi-Fi");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_netif) return ESP_ERR_NOT_SUPPORTED;

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
    // 按中国信道（1～13），和传书页一致。/ Chinese channel plan (1-13), matching the transfer page.
    esp_wifi_set_country_code("CN", true);
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
    if (err == ESP_FAIL) tomato_http_set_detail("密码不对或信号太弱");
    else if (err == ESP_ERR_TIMEOUT) tomato_http_set_detail("连上了但没拿到 IP");
    if (err != ESP_OK) goto fail;
    memset(pass, 0, sizeof(pass));
    ESP_LOGI(TAG, "connected");
    // 顺便对时，最多等 3 秒；失败不影响后续操作。SNTP 给的是 UTC，显示用哪个时区由主程序设定；关了自动校时就不对。
    // Sync time on the way, waiting at most 3 s; failure is harmless. SNTP gives UTC and the app sets the display
    // zone; with auto time off nothing is synced.
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (s_sntp_allowed && esp_netif_sntp_init(&sc) == ESP_OK) {
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
    s_public_dns = false;
    if (s_sntp) { esp_netif_sntp_deinit(); s_sntp = false; }
    // 先注销回调，免得断开事件又触发重连。/ Unregister first so the disconnect event cannot trigger a reconnect.
    if (s_wifi_h) { esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_h); s_wifi_h = NULL; }
    if (s_ip_h) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_h); s_ip_h = NULL; }
    if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    if (s_inited) { esp_wifi_deinit(); s_inited = false; }
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    if (s_loop_owned) { esp_event_loop_delete_default(); s_loop_owned = false; }
}
