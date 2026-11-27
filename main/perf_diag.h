/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：耗时诊断。诊断固件（-DTREADER_PERF_DIAG=1）里在串口打读卡、解压、校验、解图、字形光栅化和刷屏的耗时，
 * 标签 perf；正式固件里全是空宏。
 *
 * English: Timing diagnostics. A diagnostic build (-DTREADER_PERF_DIAG=1) prints card reads, inflate, checksum,
 * image decode, glyph raster and refresh times on the serial port under the tag perf; the product build compiles
 * every macro to nothing.
 */
#pragma once

#include <stdint.h>

#ifdef TREADER_PERF_DIAG
#include "esp_log.h"
#include "esp_timer.h"

/// 开始计时，变量名为 name。/ Start a timer named name.
#define PERF_T0(name) const int64_t name = esp_timer_get_time()
/// 从 name 起经过的毫秒（带一位小数用的微秒）。/ Microseconds elapsed since name.
#define PERF_US(name) ((long)(esp_timer_get_time() - (name)))
/// 打一行 perf 日志。/ Print one perf line.
#define PERF_LOG(fmt, ...) ESP_LOGI("perf", fmt, ##__VA_ARGS__)

/// 光栅化计数：每次光栅化一个字形加一次。/ Raster tally: add one per rasterized glyph.
void perf_glyph_add(long us);
/// 取出并清零自上次以来的光栅化次数与总微秒。/ Take and reset the raster count and microseconds since last time.
void perf_glyph_take(long* count, long* us);
/// 开机跑一次 CRC32 的逐位与 ROM 查表版对比，并起每 10 秒打任务 CPU 占用的任务。
/// Run the bitwise vs ROM table CRC32 comparison once at boot and start a task printing task CPU use every 10 s.
void perf_diag_boot(void);
/// 主循环每轮读触摸前调一次；两轮相隔超过 100 ms 就打一行。/ Called by the main loop before each touch read; logs a
/// gap over 100 ms between ticks.
void perf_loop_tick(void);
#else
#define PERF_T0(name) do { } while (0)
#define PERF_US(name) 0L
#define PERF_LOG(fmt, ...) do { } while (0)
static inline void perf_glyph_add(long us) { (void)us; }
static inline void perf_glyph_take(long* count, long* us) { *count = 0; *us = 0; }
static inline void perf_diag_boot(void) { }
static inline void perf_loop_tick(void) { }
#endif
