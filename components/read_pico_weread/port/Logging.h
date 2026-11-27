/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：引擎的 LOG_* 转到 ESP_LOG*，标签前加 wr_ 方便按组件过滤。
 *
 * English: The engine's LOG_* macros go to ESP_LOG*, with a wr_ tag prefix for per-component filtering.
 */

#pragma once

#include "esp_log.h"

#define LOG_ERR(origin, format, ...) ESP_LOGE("wr_" origin, format, ##__VA_ARGS__)
#define LOG_WRN(origin, format, ...) ESP_LOGW("wr_" origin, format, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) ESP_LOGI("wr_" origin, format, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) ESP_LOGD("wr_" origin, format, ##__VA_ARGS__)
