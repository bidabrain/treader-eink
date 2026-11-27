/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：引擎只问“STA 连上没有”，用 esp_wifi 回答。联网和断网由 tomato_net 管，这里不碰。
 *
 * English: The engine only asks whether STA is connected; esp_wifi answers. tomato_net brings the network up and
 * down; nothing here touches it.
 */

#pragma once

#include "esp_wifi.h"

#define WL_CONNECTED 3
#define WL_DISCONNECTED 6

struct ArduinoWiFi {
    static wifi_mode_t getMode() {
        wifi_mode_t mode = WIFI_MODE_NULL;
        return esp_wifi_get_mode(&mode) == ESP_OK ? mode : WIFI_MODE_NULL;
    }
    static int status() {
        wifi_ap_record_t ap;
        return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? WL_CONNECTED : WL_DISCONNECTED;
    }
};
inline ArduinoWiFi WiFi;
