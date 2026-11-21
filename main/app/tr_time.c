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
#include "nvs.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "tomato.h"

#define TAG "tr_time"
#define VALID_AFTER 1735689600   // 2025-01-01
#define NS "trtime"
#define DEFAULT_PPM 23           // 没测过时按每天约 2 秒 / About 2 s a day until measured
#define MIN_SPAN (6 * 3600)      // 两次校时至少隔 6 小时才算漂移 / Drift needs syncs at least 6 h apart

/* ---- 漂移估计 / Drift estimate ---- */

// 记在 NVS：上次校准的时刻、当时的误差（秒）和每百万分之几的漂移（没测过为 INT32_MIN）。
// Kept in NVS: when the clock was last set, its error then (seconds) and the drift in ppm (INT32_MIN unmeasured).
typedef struct {
    uint32_t last;
    int32_t ppm;
    uint8_t base;
} drift_t;

static drift_t s_drift;
static bool s_drift_loaded;

static void drift_load(void) {
    if (s_drift_loaded) return;
    s_drift_loaded = true;
    s_drift = (drift_t){0, INT32_MIN, 0};
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_drift);
    if (nvs_get_blob(h, "drift", &s_drift, &n) != ESP_OK || n != sizeof(s_drift)) s_drift = (drift_t){0, INT32_MIN, 0};
    nvs_close(h);
}

static void drift_save(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "drift", &s_drift, sizeof(s_drift));
    nvs_commit(h);
    nvs_close(h);
}

// 时间刚被校准：fix_ms 是这次改动（INT32_MIN 表示不可用），base_s 是校准后仍可能有的误差。
// The clock was just set: fix_ms is the change (INT32_MIN when unusable), base_s the error that may remain.
static void drift_note(int32_t fix_ms, int base_s) {
    drift_load();
    const uint32_t now = (uint32_t)time(NULL);
    if (fix_ms != INT32_MIN && s_drift.last && s_drift.base == 0 && now > s_drift.last + MIN_SPAN) {
        const int64_t ppm = (int64_t)fix_ms * 1000 / (int64_t)(now - s_drift.last);
        const int32_t p = ppm > 100000 ? 100000 : ppm < -100000 ? -100000 : (int32_t)ppm;
        s_drift.ppm = s_drift.ppm == INT32_MIN ? p : (2 * s_drift.ppm + p) / 3;
        ESP_LOGI(TAG, "drift %ld ppm (this sync %ld ms over %lu s)", (long)s_drift.ppm, (long)fix_ms,
                 (unsigned long)(now - s_drift.last));
    }
    s_drift.last = now;
    s_drift.base = (uint8_t)base_s;
    drift_save();
}

int tr_time_error_s(void) {
    if (!tr_time_valid()) return -1;
    drift_load();
    const uint32_t now = (uint32_t)time(NULL);
    // 没有记录（升级前就有时间）先从现在算，开机时会存下来；这里只读不写。/ No record (time set before this
    // firmware): count from now; boot stores it. Read-only here.
    if (!s_drift.last || now < s_drift.last) {
        s_drift.last = now;
        s_drift.base = 5;
    }
    const int32_t ppm = s_drift.ppm == INT32_MIN ? DEFAULT_PPM : s_drift.ppm < 0 ? -s_drift.ppm : s_drift.ppm;
    const int64_t err = s_drift.base + (int64_t)(now - s_drift.last) * ppm / 1000000;
    return err > 99999 ? 99999 : (int)err;
}

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
    // 手动只精确到分钟。/ By hand it is only accurate to the minute.
    drift_note(INT32_MIN, 30);
    return ESP_OK;
}

// 时间有效但还没有漂移记录（升级上来）：从现在开始记。/ Valid time but no drift record yet (an upgrade): start
// recording now.
static void drift_start(void) {
    drift_load();
    if (s_drift.last || !tr_time_valid()) return;
    s_drift.last = (uint32_t)time(NULL);
    s_drift.base = 5;
    drift_save();
}

void tr_time_restore(void) {
    tr_time_apply_settings();
    if (time(NULL) > VALID_AFTER) drift_start();
    if (time(NULL) > VALID_AFTER || !read_pico_pmu_ready()) return;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t* s = read_pico_pmu_get();
    if (!s || !s->time_synced || s->unix_sec < VALID_AFTER) return;
    struct timeval tv = {.tv_sec = s->unix_sec};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "restored from PMU RTC");
    drift_start();
}

void tr_time_poll(void) {
    if (!tomato_take_time_synced()) return;
    drift_note(tomato_last_time_fix_ms(), 0);
    if (!read_pico_pmu_ready()) return;
    ESP_LOGI(TAG, "PMU RTC sync: %s", esp_err_to_name(push_to_pmu()));
}
