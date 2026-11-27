/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：引擎用到的两个时间函数。/ English: The two time helpers the engine uses.
 */

#pragma once

#include <cstdint>

#include "HalClock.h"

namespace TimeUtils {

inline bool isClockValid() { return halClock.hasValidTime(); }

inline uint32_t getCurrentValidTimestamp() {
    const time_t now = halClock.nowUtc();
    return now > 0 && static_cast<uint64_t>(now) <= UINT32_MAX ? static_cast<uint32_t>(now) : 0;
}

}  // namespace TimeUtils
