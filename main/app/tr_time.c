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
#include "tomato.h"

#define TAG "tr_time"
#define VALID_AFTER 1735689600   // 2025-01-01

void tr_time_restore(void) {
    setenv("TZ", "CST-8", 1);
    tzset();
    if (time(NULL) > VALID_AFTER || !read_pico_pmu_ready()) return;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t* s = read_pico_pmu_get();
    if (!s || !s->time_synced || s->unix_sec < VALID_AFTER) return;
    struct timeval tv = {.tv_sec = s->unix_sec};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "restored from PMU RTC");
}

void tr_time_poll(void) {
    if (!tomato_take_time_synced() || !read_pico_pmu_ready()) return;
    uint32_t now = (uint32_t)time(NULL);
    uint8_t payload[4] = {(uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24)};
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    ESP_LOGI(TAG, "PMU RTC sync: %s", esp_err_to_name(err));
}
