/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：耗时诊断的计数与开机对比测试，只在 TREADER_PERF_DIAG 诊断固件里编进来，见 perf_diag.h。
 *
 * English: Tallies and the boot comparison for the timing diagnostics; built only into the TREADER_PERF_DIAG
 * diagnostic firmware, see perf_diag.h.
 */
#include "perf_diag.h"

#ifdef TREADER_PERF_DIAG
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_long s_glyphs, s_glyph_us;

void perf_glyph_add(long us) {
    atomic_fetch_add(&s_glyphs, 1);
    atomic_fetch_add(&s_glyph_us, us);
}

void perf_glyph_take(long* count, long* us) {
    *count = atomic_exchange(&s_glyphs, 0);
    *us = atomic_exchange(&s_glyph_us, 0);
}

// 与 zip_reader.c 的逐位 CRC32 相同。/ Same as zip_reader.c's bitwise CRC32.
static uint32_t crc_bitwise(const uint8_t* data, size_t len) {
    uint32_t crc = UINT32_MAX;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}

void perf_loop_tick(void) {
    static int64_t last;
    const int64_t now = esp_timer_get_time();
    if (last && now - last > 100000) PERF_LOG("main loop stalled %ld ms", (long)((now - last) / 1000));
    last = now;
}

#define STATS_MAX 40

// 每 10 秒打一次这段时间里各任务占的 CPU（两个核加起来 200%），只列 1% 以上的。
// Every 10 s print each task's CPU share over the interval (both cores sum to 200%), listing those at 1% or more.
static void stats_task(void* arg) {
    (void)arg;
    static TaskStatus_t now[STATS_MAX], before[STATS_MAX];
    UBaseType_t before_n = 0;
    configRUN_TIME_COUNTER_TYPE before_total = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        configRUN_TIME_COUNTER_TYPE total = 0;
        const UBaseType_t n = uxTaskGetSystemState(now, STATS_MAX, &total);
        const uint64_t span = (uint64_t)(total - before_total);
        if (before_total && span) {
            char line[600];
            int at = 0;
            for (UBaseType_t i = 0; i < n; i++) {
                configRUN_TIME_COUNTER_TYPE prev = 0;
                for (UBaseType_t j = 0; j < before_n; j++)
                    if (before[j].xHandle == now[i].xHandle) { prev = before[j].ulRunTimeCounter; break; }
                const unsigned pct = (unsigned)((uint64_t)(now[i].ulRunTimeCounter - prev) * 100 / span);
                if (pct >= 1 && at < (int)sizeof(line) - 40)
                    at += snprintf(line + at, sizeof(line) - at, " %s(p%u)=%u%%", now[i].pcTaskName,
                                   (unsigned)now[i].uxCurrentPriority, pct);
            }
            PERF_LOG("cpu 10s:%s", at ? line : " idle");
        }
        memcpy(before, now, sizeof(now[0]) * n);
        before_n = n;
        before_total = total;
    }
}

void perf_diag_boot(void) {
    xTaskCreate(stats_task, "perf_stats", 4096, NULL, 1, NULL);
    const size_t n = 1024 * 1024;
    uint8_t* buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 2654435761u >> 13);
    PERF_T0(t0);
    const uint32_t a = crc_bitwise(buf, n);
    const long bit_us = PERF_US(t0);
    PERF_T0(t1);
    const uint32_t b = esp_rom_crc32_le(0, buf, n);
    const long rom_us = PERF_US(t1);
    PERF_LOG("crc32 1 MB: bitwise %ld ms, rom %ld ms, %s", bit_us / 1000, rom_us / 1000, a == b ? "same" : "DIFFERENT");
    heap_caps_free(buf);
}
#endif
