/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 一次性 STA 连网：用传书页保存的凭据，连上、用完、全部释放。
 * One-shot STA networking: uses credentials saved by the transfer page; connect, use, release everything.
 *
 * 冻结：传书服务运行时不连网；不保存凭据副本，密码用毕擦除。任务做完 Wi-Fi 最多再留 LINGER_MS，没人用就全部释放；
 * 锁屏（tomato_sync_stop）、传书页和试连都先立即断开，浅睡前绝不留着 Wi-Fi。
 * 1.2.3 修订（用户反映图片下载慢）：原来每个任务做完立刻断网，切帖子时上一个任务被取消、下一个任务又从连 Wi-Fi、
 * 建连接、TLS 握手重来，实测每次多 15 秒以上；现在紧接着的任务直接用还连着的 Wi-Fi。
 * Frozen: never connect while the transfer service runs; keep no credential copy, wipe the password after use. After a
 * job Wi-Fi stays at most LINGER_MS and is then fully released if unused; lock (tomato_sync_stop), the transfer page
 * and the connection test drop it at once, so Wi-Fi is never left up into light sleep.
 * Revised in 1.2.3 (the user reported slow image downloads): every job used to drop the network the moment it ended,
 * so switching posts cancelled one job and made the next one rejoin Wi-Fi, reconnect and redo the TLS handshake,
 * measured at 15 s or more each time; a job right after another now uses the Wi-Fi that is still up.
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <stdlib.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "read_pico_transfer.h"
#include "tomato_internal.h"

#define TAG "tomato_net"
#define BIT_IP BIT0
#define BIT_FAIL BIT1
#define RETRY_MAX 3
// 任务做完 Wi-Fi 再留这么久，紧接着的任务（比如切到下一篇帖子）就不用重新连。/ How long Wi-Fi stays after a job, so
// the next job right after it (say, the next post) need not reconnect.
#define LINGER_MS 20000

static EventGroupHandle_t s_bits;
static esp_netif_t* s_netif;
static esp_event_handler_instance_t s_wifi_h, s_ip_h;
static bool s_loop_owned, s_inited, s_started;
static int s_retries;
static bool s_sntp;
static bool s_sntp_allowed = true;
static bool s_public_dns;
// 联网、断网和留网的状态只在这把锁里改。/ Up, down and linger state change only under this lock.
static SemaphoreHandle_t s_net_mx;
static StaticSemaphore_t s_net_mx_buf;
static portMUX_TYPE s_net_mx_init = portMUX_INITIALIZER_UNLOCKED;
static bool s_lingering;             // 任务做完、Wi-Fi 还留着 / A job ended and Wi-Fi is still up
static TaskHandle_t s_linger_task;   // 到时断网的小任务 / The small task that drops Wi-Fi when time is up

static void net_lock(void) {
    if (!s_net_mx) {
        portENTER_CRITICAL(&s_net_mx_init);
        if (!s_net_mx) s_net_mx = xSemaphoreCreateMutexStatic(&s_net_mx_buf);
        portEXIT_CRITICAL(&s_net_mx_init);
    }
    xSemaphoreTake(s_net_mx, portMAX_DELAY);
}
static void net_unlock(void) { xSemaphoreGive(s_net_mx); }

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
static bool s_synced_now;   // 这次联网已经对过时 / Already synced during this connection
static volatile int32_t s_time_fix_ms;   // 最近一次对时把系统时间改了多少 / How far the last sync moved the clock

bool tomato_take_time_synced(void) {
    bool fresh = s_time_fresh;
    s_time_fresh = false;
    return fresh;
}

int32_t tomato_last_time_fix_ms(void) { return s_time_fix_ms; }

// 2025 年以后才算对过时。/ Treat time as set once it is past 2025.
void tomato_net_set_sntp(bool on) { s_sntp_allowed = on; }

// 跑一次 SNTP，记下系统时间被改了多少（对时前时间无效时记 INT32_MIN）。/ Run SNTP once and record how far the
// clock moved (INT32_MIN when the time was invalid before).
static bool sntp_once(int wait_ms) {
    if (!s_sntp) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        if (esp_netif_sntp_init(&sc) != ESP_OK) return false;
        s_sntp = true;
    }
    const bool was_valid = tomato_time_valid();
    struct timeval a, b;
    gettimeofday(&a, NULL);
    const int64_t m0 = esp_timer_get_time();
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(wait_ms)) != ESP_OK || !tomato_time_valid()) return false;
    gettimeofday(&b, NULL);
    const int64_t moved = ((int64_t)b.tv_sec - a.tv_sec) * 1000 + (b.tv_usec - a.tv_usec) / 1000 -
                          (esp_timer_get_time() - m0) / 1000;
    s_time_fix_ms = !was_valid || moved > INT32_MAX || moved < -INT32_MAX ? INT32_MIN : (int32_t)moved;
    s_time_fresh = true;
    s_synced_now = true;
    return true;
}

bool tomato_net_sync_time(int wait_ms) { return s_inited && (s_synced_now || sntp_once(wait_ms)); }

bool tomato_time_valid(void) { return time(NULL) > 1735689600; }

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 断了就不算有 IP，留着的 Wi-Fi 不会被当成还能用。/ A drop clears the IP, so lingering Wi-Fi is not taken as usable.
        xEventGroupClearBits(s_bits, BIT_IP);
        if (s_retries++ < RETRY_MAX) esp_wifi_connect();
        else xEventGroupSetBits(s_bits, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_bits, BIT_IP);
    }
}

static void net_down_locked(void);

static esp_err_t net_up_locked(int timeout_ms, const volatile bool* cancel) {
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
    // 联网期间关掉 Wi-Fi 省电：默认的 modem sleep 在有的路由器上把下载拖到每秒几百字节。只在后台任务联网时开着，
    // 用完就断，耗电影响很小。
    // Wi-Fi power save off while online: the default modem sleep drags downloads to a few hundred bytes a second on some
    // routers. Wi-Fi is only up during a background job and dropped afterwards, so the battery cost is small.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "connected");
    // 顺便对时，最多等 3 秒；失败不影响后续操作。SNTP 给的是 UTC，显示用哪个时区由主程序设定；关了自动校时就不对。
    // Sync time on the way, waiting at most 3 s; failure is harmless. SNTP gives UTC and the app sets the display
    // zone; with auto time off nothing is synced.
    if (s_sntp_allowed) sntp_once(3000);
    return ESP_OK;

fail:
    memset(pass, 0, sizeof(pass));
    ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    net_down_locked();
    return err;
}

esp_err_t tomato_net_up(int timeout_ms, const volatile bool* cancel) {
    net_lock();
    if (s_lingering) {
        s_lingering = false;
        if (s_linger_task) xTaskNotifyGive(s_linger_task);
        // 留着的 Wi-Fi 还有 IP 就直接用，断过就整个重来。/ Reuse the lingering Wi-Fi while it has an IP; after a drop,
        // start over.
        if (s_bits && (xEventGroupGetBits(s_bits) & BIT_IP)) {
            ESP_LOGI(TAG, "reuse lingering Wi-Fi");
            net_unlock();
            return ESP_OK;
        }
        net_down_locked();
    }
    const esp_err_t err = net_up_locked(timeout_ms, cancel);
    net_unlock();
    return err;
}

// 到时没人用就断网；被叫醒说明有新任务接手、要立即断开，或又留了一轮。/ Drop Wi-Fi when time is up unused; a wake
// means a new job took it, it is being dropped now, or another linger round began.
static void linger_task(void* arg) {
    (void)arg;
    for (;;) {
        const bool woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINGER_MS)) != 0;
        net_lock();
        if (!s_lingering) break;
        if (!woken) {
            ESP_LOGI(TAG, "linger over, Wi-Fi off");
            s_lingering = false;
            net_down_locked();
            break;
        }
        net_unlock();
    }
    s_linger_task = NULL;
    net_unlock();
    vTaskDelete(NULL);
}

void tomato_net_linger(void) {
    net_lock();
    if (s_netif) {
        s_lingering = true;
        if (s_linger_task) xTaskNotifyGive(s_linger_task);
        else if (xTaskCreate(linger_task, "net_linger", 3072, NULL, 3, &s_linger_task) != pdPASS) {
            s_linger_task = NULL;
            s_lingering = false;
            net_down_locked();
        }
    }
    net_unlock();
}

void tomato_net_idle_off(void) {
    net_lock();
    if (s_lingering) {
        s_lingering = false;
        if (s_linger_task) xTaskNotifyGive(s_linger_task);
        net_down_locked();
    }
    net_unlock();
}

void tomato_net_down(void) {
    net_lock();
    s_lingering = false;
    if (s_linger_task) xTaskNotifyGive(s_linger_task);
    net_down_locked();
    net_unlock();
}

static void net_down_locked(void) {
    // 连接关掉，但留着 TLS 会话，下次重连能少做一次完整握手。/ Close the connection but keep the TLS session so the next
    // reconnect skips a full handshake.
    tomato_http_suspend();
    s_public_dns = false;
    s_synced_now = false;
    if (s_sntp) { esp_netif_sntp_deinit(); s_sntp = false; }
    // 先注销回调，免得断开事件又触发重连。/ Unregister first so the disconnect event cannot trigger a reconnect.
    if (s_wifi_h) { esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_h); s_wifi_h = NULL; }
    if (s_ip_h) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_h); s_ip_h = NULL; }
    if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    if (s_inited) { esp_wifi_deinit(); s_inited = false; }
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    if (s_loop_owned) { esp_event_loop_delete_default(); s_loop_owned = false; }
}

esp_err_t tomato_net_test(int timeout_ms, char* msg, size_t cap) {
    // 后台任务自己会联网断网，这时试连会抢 Wi-Fi。/ A background job brings Wi-Fi up and down itself; a test would
    // fight it for the radio.
    if (tomato_sync_busy()) {
        snprintf(msg, cap, "后台正在联网，稍后再试");
        return ESP_ERR_INVALID_STATE;
    }
    tomato_http_clear_detail();
    // 留着的 Wi-Fi 可能是改之前的网络：先断开，真的去连保存的这个。/ Lingering Wi-Fi may be the network from before the
    // change: drop it so the test really joins the saved one.
    tomato_net_idle_off();
    esp_err_t err = tomato_net_up(timeout_ms, NULL);
    if (err == ESP_OK) snprintf(msg, cap, "连上了");
    else if (err == ESP_ERR_NOT_FOUND) snprintf(msg, cap, "没有保存的网络");
    else if (tomato_http_detail()[0]) snprintf(msg, cap, "连不上：%s", tomato_http_detail());
    else snprintf(msg, cap, "连不上（%s）", esp_err_to_name(err));
    tomato_net_down();
    return err;
}
