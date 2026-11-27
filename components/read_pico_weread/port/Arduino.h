/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：CrossMux 引擎用到的那一小块 Arduino 接口（String、millis、delay、ESP 内存查询），换成 ESP-IDF 实现。
 *
 * English: The small slice of the Arduino API the CrossMux engine uses (String, millis, delay, ESP memory queries),
 * implemented on ESP-IDF.
 */

#pragma once

#include <cstdint>
#include <string>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "Treader"
#endif

/// 引擎只用 c_str() 和 length()，std::string 就够。/ The engine only uses c_str() and length(); std::string suffices.
class String : public std::string {
 public:
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(std::string s) : std::string(std::move(s)) {}
};

inline unsigned long millis() { return static_cast<unsigned long>(esp_timer_get_time() / 1000); }

inline void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

/// Arduino 的 random(max) / random(min, max)，用硬件随机数。/ Arduino's random(max) / random(min, max) on the hardware RNG.
inline long random(long max) { return max > 0 ? static_cast<long>(esp_random() % static_cast<uint32_t>(max)) : 0; }
inline long random(long min, long max) { return max > min ? min + random(max - min) : min; }

/// Arduino 的 ESP 对象：内部堆和 PSRAM 的空闲与最大块。/ Arduino's ESP object: internal heap and PSRAM free / largest.
struct ArduinoEsp {
    static uint32_t getFreeHeap() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
    static uint32_t getMaxAllocHeap() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
    static uint32_t getFreePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
    static uint32_t getMaxAllocPsram() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
};
inline ArduinoEsp ESP;
