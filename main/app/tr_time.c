/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tr_time.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "tomato.h"

#define TAG "tr_time"
#define VALID_AFTER 1735689600   // 2025-01-01

void tr_time_apply_settings(void) {
    setenv("TZ", app_settings_tz_posix(app_settings_tz()), 1);
    tzset();
    tomato_net_set_sntp(app_settings_time_auto());
}

bool tr_time_valid(void) {
    return time(NULL) > VALID_AFTER;
}

static esp_err_t push_to_pmu(void) {
    if (!read_pico_pmu_ready()) return ESP_ERR_INVALID_STATE;
    uint32_t now = (uint32_t)time(NULL);
    uint8_t payload[4] = {(uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24)};
    return read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
}

esp_err_t tr_time_set_local(int year, int month, int day, int hour, int minute) {
    struct tm tm = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = hour, .tm_min = minute,
                    .tm_isdst = -1};
    const time_t t = mktime(&tm);
    if (t <= VALID_AFTER) return ESP_ERR_INVALID_ARG;
    struct timeval tv = {.tv_sec = t};
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
    const esp_err_t err = push_to_pmu();
    ESP_LOGI(TAG, "time set by hand, PMU RTC: %s", esp_err_to_name(err));
    return ESP_OK;
}

void tr_time_restore(void) {
    tr_time_apply_settings();
    if (time(NULL) > VALID_AFTER || !read_pico_pmu_ready()) return;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t* s = read_pico_pmu_get();
    if (!s || !s->time_synced || s->unix_sec < VALID_AFTER) return;
    struct timeval tv = {.tv_sec = s->unix_sec};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "restored from PMU RTC");
}

void tr_time_poll(void) {
    if (!read_pico_pmu_ready() || !tomato_take_time_synced()) return;
    ESP_LOGI(TAG, "PMU RTC sync: %s", esp_err_to_name(push_to_pmu()));
}
