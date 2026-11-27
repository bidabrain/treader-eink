/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：可重建缓存的释放表，见 mem_reclaim.h。
 *
 * English: The table of rebuildable-cache release functions; see mem_reclaim.h.
 */

#include "mem_reclaim.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "mem_reclaim"
#define FN_MAX 12
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static mem_reclaim_fn s_fns[FN_MAX];

void mem_reclaim_register(mem_reclaim_fn fn) {
    int free_at = -1;
    for (int i = 0; i < FN_MAX; i++) {
        if (s_fns[i] == fn) return;
        if (!s_fns[i] && free_at < 0) free_at = i;
    }
    if (free_at >= 0) s_fns[free_at] = fn;
    else ESP_LOGW(TAG, "table full");
}

void mem_reclaim_unregister(mem_reclaim_fn fn) {
    for (int i = 0; i < FN_MAX; i++) if (s_fns[i] == fn) s_fns[i] = NULL;
}

size_t mem_reclaim(const char* why) {
    const size_t before = heap_caps_get_largest_free_block(PSRAM_CAPS);
    const size_t free_before = heap_caps_get_free_size(PSRAM_CAPS);
    for (int i = 0; i < FN_MAX; i++) if (s_fns[i]) s_fns[i]();
    const size_t after = heap_caps_get_largest_free_block(PSRAM_CAPS);
    ESP_LOGW(TAG, "%s: largest block %u -> %u, free %u -> %u", why ? why : "?", (unsigned)before, (unsigned)after,
             (unsigned)free_before, (unsigned)heap_caps_get_free_size(PSRAM_CAPS));
    return after;
}
