/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 里的用户设置：睡眠档、字体路径、上次唤醒/开机原因、拿起唤醒开关。
 *
 * User settings in NVS: sleep mode, font path, last wake/boot reason,
 * pickup-wake switch.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/// 默认关机。浅睡：按键回原页；拿起唤醒默认关。软睡：SOFT_SLEEP 拉低 EN，再短按开机。关机：EN=0，长按开机。
/// Default is off. Light: key returns to the page; pickup-wake defaults off. Soft sleep: SOFT_SLEEP drops EN, then a short press boots. Off: EN=0, long-press to boot.
typedef enum {
    APP_SLEEP_LIGHT = 0,
    APP_SLEEP_DEEP = 1,
    APP_SLEEP_OFF = 2,
} app_sleep_mode_t;

void app_settings_init(void);
app_sleep_mode_t app_settings_sleep_mode(void);
void app_settings_set_sleep_mode(app_sleep_mode_t mode);
const char* app_sleep_mode_name(app_sleep_mode_t mode);
/// 空路径表示固件内建字体；非空为 SD 上的 TTF。/ Empty path is the built-in font; non-empty is a TTF on the SD card.
const char* app_settings_font_path(void);
void app_settings_set_font_path(const char* path);
/// 上次浅睡唤醒源（app_wake_source_t），掉电也保留。/ Last light-sleep wake source (app_wake_source_t); kept across power loss.
uint8_t app_settings_last_wake(void);
void app_settings_set_last_wake(uint8_t src);
/// 最近一次非 0 的 PMU wake_reason。STATUS 报 0 时用这个回显。/ Last non-zero PMU wake_reason. Used when STATUS reports 0.
uint8_t app_settings_last_boot(void);
void app_settings_set_last_boot(uint8_t reason);
/// 浅睡拿起唤醒。默认关；有加速度计也不会自动开。/ Light-sleep pickup wake. Defaults off; an accelerometer does not turn it on.
bool app_settings_pickup_wake(void);
void app_settings_set_pickup_wake(bool on);
/// 阅读默认字号，36..72、步长 4，默认 48。/ Default reading size, 36..72 in steps of 4, initially 48.
uint8_t app_settings_book_px(void);
/// 无效字号恢复 48。/ Invalid sizes fall back to 48.
void app_settings_set_book_px(uint8_t px);
/// 实验性晃动翻页，默认关闭。/ Experimental shake page turn, off by default.
bool app_settings_book_shake(void);

/// 阅读排版。/ Reading layout.
typedef struct {
    uint8_t line;      ///< 行距：0 紧凑 1 标准 2 宽松 / Line spacing: compact, standard, loose
    uint8_t para;      ///< 段距：0 无 1 小 2 大 / Paragraph spacing: none, small, large
    uint8_t margin;    ///< 页边距：0 窄 1 中 2 宽 / Page margins: narrow, medium, wide
    bool indent;       ///< 首行自动缩进两字（否则按原文）/ Indent first lines by two characters (else as written)
    bool underline;    ///< 每行下面画虚线 / Draw a dashed rule under each line
} app_book_style_t;

/// 读写阅读排版，存 NVS（一个字节）。/ Get or set the reading layout, stored in NVS as one byte.
app_book_style_t app_settings_book_style(void);
void app_settings_set_book_style(const app_book_style_t* style);

/// 隐藏的底部标签，按 tr_tab_t 位号的掩码；隐藏的标签底栏和菜单都不列出。书库和设置的位总是 0。
/// 旧版“隐藏虎扑”开着时，第一次读取换算成只隐藏虎扑。
/// Hidden tabs as a mask by tr_tab_t bit; hidden tabs leave the tab bar and the menu. The library and settings bits
/// are always 0. The old "hide Hupu" switch migrates to a mask hiding Hupu only.
uint8_t app_settings_hidden_tabs(void);
void app_settings_set_hidden_tabs(uint8_t mask);

/// 自动翻页档位（0 关，1～10 对应 3～30 秒），阅读和漫画各存一份。/ Auto-turn level (0 off, 1-10 = 3-30 s), kept
/// separately for the reader and comics.
uint8_t app_settings_auto_turn(bool comic);
void app_settings_set_auto_turn(bool comic, uint8_t level);

/// 不操作多久自动锁屏的档位：0～3 对应 1、2、5、10 分钟，4 是从不；默认 1（2 分钟）。
/// Idle auto-lock level: 0-3 are 1, 2, 5 and 10 minutes, 4 is never; 1 (2 minutes) by default.
#define APP_LOCK_IDLE_LEVELS 5
uint8_t app_settings_lock_idle(void);
void app_settings_set_lock_idle(uint8_t level);
/// 当前档位的毫秒数，从不返回 0。/ Milliseconds for the current level; 0 means never.
int64_t app_settings_lock_idle_ms(void);
/// 档位的文字（“2 分钟”“从不”）。/ The level's label ("2 分钟", "从不").
const char* app_settings_lock_idle_label(uint8_t level);

/// 锁屏密码（4 位数字）。NVS 里只存加盐的 SHA-256，不存原文；默认关。
/// The lock PIN (4 digits). NVS keeps only a salted SHA-256, never the digits; off by default.
bool app_settings_pin_enabled(void);
/// 和存的密码比对。/ Compare against the stored PIN.
bool app_settings_pin_check(const char* pin);
/// 设新密码；NULL 或空串关闭密码。/ Set a new PIN; NULL or empty turns the PIN off.
esp_err_t app_settings_pin_set(const char* pin);
/// 保存实验性晃动翻页开关。/ Persist the experimental shake page-turn switch.
void app_settings_set_book_shake(bool on);
