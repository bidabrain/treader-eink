/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：今日塔罗：内置 78 张牌的牌义，按日期和设备算出今天的牌，不联网。只出数据，不画屏。
 *
 * English: Daily tarot: built-in meanings for the 78 cards and today's card from the date and the device, offline.
 * Data only; no drawing.
 *
 * 冻结：同一天同一台设备永远是同一张牌，换壁纸不影响；牌义只放 flash，不占 PSRAM。
 * Frozen: one device gets the same card all day, regardless of the wallpaper; meanings live in flash, not PSRAM.
 */
#pragma once

#include <stdbool.h>

/// 一张牌：中文名、英文名，下标 0 正位、1 逆位。/ One card: Chinese and English names; index 0 upright, 1 reversed.
typedef struct {
    const char* name;          ///< 中文名（英文版换成英文名）/ Chinese name (English in the English build)
    const char* name_en;       ///< 英文名，两版都显示 / English name, shown in both builds
    const char* keywords[2];   ///< 英文关键词 / English keywords
    const char* message[2];    ///< 今日讯息 / Today's message
    const char* advice[2];     ///< 今日建议 / Today's advice
} tr_tarot_card_t;

/// 今天抽到的牌。/ Today's draw.
typedef struct {
    int index;        ///< 0～21 大阿卡那，22 起每组 14 张：权杖、圣杯、宝剑、星币 / 0-21 major, then 14 each of Wands, Cups, Swords, Pentacles
    bool reversed;    ///< 逆位 / Reversed
    const tr_tarot_card_t* card;
} tr_tarot_pick_t;

/// 算今天的牌（读写 NVS，屏幕上电前调用）。时间没设过时用上一次的结果；从没抽过返回 false。
/// Work out today's card (reads and writes NVS; call before the panel powers on). Without a valid time the last result
/// is reused; false when none was ever drawn.
bool tr_tarot_today(tr_tarot_pick_t* out);
