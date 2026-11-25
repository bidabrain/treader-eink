/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 主循环主机测试的 esp_heap_caps.h 替身：只给心跳日志用的空闲内存查询。
 * Host-test stand-in for esp_heap_caps.h: only the free-memory query the heartbeat log uses.
 */
#pragma once

#include <stddef.h>

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2

static inline size_t heap_caps_get_free_size(unsigned caps) {
    (void)caps;
    return 0;
}
