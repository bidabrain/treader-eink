/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 画锁屏书卡；没有在读的书返回 false，调用方改画官方锁屏图。
/// Draw the lock-screen book card; returns false when no book is in progress so the caller draws the stock image.
/// 屏幕上电前读出书卡要用的数据（文件、NVS、PMU）。/ Read the card's data (files, NVS, PMU) before the panel powers on.
void tr_lock_prepare(void);
bool tr_lock_draw(uint8_t* fb);
