/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * PSRAM 占用诊断：只在 idf.py -B build-diag -DTREADER_HEAP_DIAG=1 build 编出的诊断固件里工作。每分钟遍历一次 PSRAM 堆，
 * 在串口打出总量、最大连续空闲块、按大小归类的占用和最大的几块（带开头字节，好认出是谁的）。正式固件里是空函数。
 *
 * English: PSRAM usage diagnostics, active only in a diagnostic build (idf.py -B build-diag -DTREADER_HEAP_DIAG=1
 * build). Once a minute it walks the PSRAM heap and prints the totals, the largest free block, usage grouped by size and
 * the biggest blocks (with their first bytes, to tell whose they are). An empty function in the product firmware.
 *
 * 小块（<4 KB）另做存活对比：第一份报告记下当时活着的小块地址做基准，之后每份报告列出基准之后分配、到现在还活着的小块
 * （按大小归类，抽样打出开头字节）。回到空闲的书库后还留着、并且越来越多的，就是泄漏或没上限的缓存。
 * Small blocks (<4 KB) also get a survivor diff: the first report records the live small-block addresses as a baseline,
 * and later reports list the small blocks allocated since then that are still alive (grouped by size, with sampled
 * leading bytes). Those that stay, and keep growing, after returning to an idle library are leaks or unbounded caches.
 *
 * 冻结：遍历回调里只记数、不打日志、不分配（回调时堆是锁着的）；不改任何内存。
 * Frozen: the walk callback only counts, never logs or allocates (the heap is locked during the callback); no memory is
 * changed.
 */

#include "heap_diag.h"

#if TREADER_HEAP_DIAG

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "heap_diag"
#define PERIOD_MS 60000
#define TOP_N 24              // 列出最大的几块 / How many of the biggest blocks to list
#define GROUP_N 48            // 按大小归类的种数上限 / Distinct sizes tracked
#define GROUP_MIN 4096        // 小于这个的块只计总数 / Blocks under this only count toward the totals
#define PEEK 12               // 每块看开头几个字节 / Leading bytes shown per block

typedef struct {
    void* ptr;
    size_t size;
    uint8_t head[PEEK];
} block_t;

typedef struct {
    size_t size;
    int count;
} group_t;

typedef struct {
    size_t used, free, largest_free, small_used;
    int used_blocks, small_blocks;
    block_t top[TOP_N];
    int top_n;
    group_t groups[GROUP_N];
    int group_n;
    int group_overflow;
} walk_t;

static walk_t s_walk;   // 静态：回调里不能分配 / Static: the callback must not allocate

#define SMALL_MAX 8000         // 记得下的小块个数 / Small blocks that can be recorded
#define SAMPLE_N 30            // 抽样打出的新存活小块 / Sampled new survivors printed
#define SAMPLE_PEEK 24
EXT_RAM_BSS_ATTR static uintptr_t s_base[SMALL_MAX];
EXT_RAM_BSS_ATTR static uintptr_t s_cur[SMALL_MAX];
EXT_RAM_BSS_ATTR static uint16_t s_cur_size[SMALL_MAX];
static int s_base_n = -1, s_cur_n;
static bool s_cur_full;

static bool walker(walker_heap_into_t heap, walker_block_info_t b, void* user) {
    (void)heap;
    walk_t* w = user;
    if (!b.used) {
        w->free += b.size;
        if (b.size > w->largest_free) w->largest_free = b.size;
        return true;
    }
    w->used += b.size;
    w->used_blocks++;
    if (b.size < GROUP_MIN) {
        w->small_used += b.size;
        w->small_blocks++;
        if (s_cur_n < SMALL_MAX) {
            s_cur[s_cur_n] = (uintptr_t)b.ptr;
            s_cur_size[s_cur_n++] = (uint16_t)b.size;
        } else {
            s_cur_full = true;
        }
        return true;
    }
    int g = 0;
    while (g < w->group_n && w->groups[g].size != b.size) g++;
    if (g < w->group_n) w->groups[g].count++;
    else if (w->group_n < GROUP_N) w->groups[w->group_n++] = (group_t){b.size, 1};
    else w->group_overflow++;
    // 保持 top 按大小从大到小。/ Keep top sorted from largest down.
    int at = w->top_n < TOP_N ? w->top_n : TOP_N - 1;
    if (w->top_n == TOP_N && b.size <= w->top[TOP_N - 1].size) return true;
    while (at > 0 && w->top[at - 1].size < b.size) {
        w->top[at] = w->top[at - 1];
        at--;
    }
    w->top[at].ptr = b.ptr;
    w->top[at].size = b.size;
    memcpy(w->top[at].head, b.ptr, PEEK);
    if (w->top_n < TOP_N) w->top_n++;
    return true;
}

static int cmp_ptr(const void* a, const void* b) {
    const uintptr_t x = *(const uintptr_t*)a, y = *(const uintptr_t*)b;
    return x < y ? -1 : x > y;
}

static bool in_base(uintptr_t p) {
    return bsearch(&p, s_base, (size_t)s_base_n, sizeof(s_base[0]), cmp_ptr) != NULL;
}

// 基准之后分配、现在还活着的小块。/ Small blocks allocated since the baseline and still alive.
static void survivors(void) {
    if (s_base_n < 0) {
        memcpy(s_base, s_cur, sizeof(s_base[0]) * (size_t)s_cur_n);
        s_base_n = s_cur_n;
        qsort(s_base, (size_t)s_base_n, sizeof(s_base[0]), cmp_ptr);
        ESP_LOGI(TAG, "small-block baseline: %d blocks%s", s_base_n, s_cur_full ? " (table full)" : "");
        return;
    }
    group_t g[GROUP_N];
    int gn = 0, n = 0, shown = 0;
    size_t bytes = 0;
    for (int i = 0; i < s_cur_n; i++) {
        if (in_base(s_cur[i])) continue;
        n++;
        bytes += s_cur_size[i];
        int k = 0;
        while (k < gn && g[k].size != s_cur_size[i]) k++;
        if (k < gn) g[k].count++;
        else if (gn < GROUP_N) g[gn++] = (group_t){s_cur_size[i], 1};
    }
    ESP_LOGI(TAG, "new small survivors since baseline: %d blocks, %u bytes%s", n, (unsigned)bytes,
             s_cur_full ? " (table full)" : "");
    for (int i = 1; i < gn; i++)
        for (int j = i; j > 0 && g[j - 1].count < g[j].count; j--) {
            const group_t t = g[j];
            g[j] = g[j - 1];
            g[j - 1] = t;
        }
    for (int i = 0; i < gn && i < 12; i++)
        ESP_LOGI(TAG, "  small %4u x %4d", (unsigned)g[i].size, g[i].count);
    // 隔着抽样，免得只看到同一批。/ Sample spread out so it is not one batch only.
    const int step = n > SAMPLE_N ? n / SAMPLE_N : 1;
    for (int i = 0, seen = 0; i < s_cur_n && shown < SAMPLE_N; i++) {
        if (in_base(s_cur[i])) continue;
        if (seen++ % step) continue;
        const uint8_t* p = (const uint8_t*)s_cur[i];
        char hex[SAMPLE_PEEK * 2 + 1], txt[SAMPLE_PEEK + 1];
        for (int k = 0; k < SAMPLE_PEEK; k++) {
            snprintf(hex + k * 2, 3, "%02x", p[k]);
            txt[k] = p[k] >= 32 && p[k] < 127 ? (char)p[k] : '.';
        }
        txt[SAMPLE_PEEK] = 0;
        ESP_LOGI(TAG, "  s %p %4u %s |%s|", (void*)p, (unsigned)s_cur_size[i], hex, txt);
        shown++;
    }
}

static void dump(void) {
    memset(&s_walk, 0, sizeof(s_walk));
    s_cur_n = 0;
    s_cur_full = false;
    heap_caps_walk(MALLOC_CAP_SPIRAM, walker, &s_walk);
    const walk_t* w = &s_walk;
    ESP_LOGI(TAG, "psram used %u in %d blocks (small <4K: %u in %d), free %u, largest free %u",
             (unsigned)w->used, w->used_blocks, (unsigned)w->small_used, w->small_blocks, (unsigned)w->free,
             (unsigned)w->largest_free);
    // 按总字节从多到少打出各种大小。/ Sizes by total bytes, most first.
    group_t g[GROUP_N];
    memcpy(g, w->groups, sizeof(g));
    for (int i = 1; i < w->group_n; i++)
        for (int j = i; j > 0 && g[j - 1].size * g[j - 1].count < g[j].size * g[j].count; j--) {
            const group_t t = g[j];
            g[j] = g[j - 1];
            g[j - 1] = t;
        }
    for (int i = 0; i < w->group_n && i < 20; i++)
        ESP_LOGI(TAG, "  size %7u x %3d = %8u", (unsigned)g[i].size, g[i].count, (unsigned)(g[i].size * g[i].count));
    if (w->group_overflow) ESP_LOGI(TAG, "  (+%d blocks of other sizes)", w->group_overflow);
    for (int i = 0; i < w->top_n; i++) {
        char hex[PEEK * 3 + 1], txt[PEEK + 1];
        for (int k = 0; k < PEEK; k++) {
            const uint8_t c = w->top[i].head[k];
            snprintf(hex + k * 3, 4, "%02x ", c);
            txt[k] = c >= 32 && c < 127 ? (char)c : '.';
        }
        txt[PEEK] = 0;
        ESP_LOGI(TAG, "  #%02d %p %7u  %s |%s|", i + 1, w->top[i].ptr, (unsigned)w->top[i].size, hex, txt);
    }
    survivors();
}

static void task(void* arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        dump();
    }
}

void heap_diag_start(void) {
    ESP_LOGW(TAG, "diagnostic build: PSRAM report every %d s", PERIOD_MS / 1000);
    xTaskCreate(task, "heap_diag", 4096, NULL, 1, NULL);
}

#else

void heap_diag_start(void) {}

#endif
