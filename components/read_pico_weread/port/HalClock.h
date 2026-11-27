/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：引擎要的时钟。校时由 tomato_net 联网时做，这里只读系统时间；时间不对时 requestSync 不另起 SNTP，
 * 引擎等到超时后报时钟错误。
 *
 * English: The clock the engine wants. tomato_net sets the time when it brings the network up, so this only reads
 * the system time; with a bad clock requestSync starts no SNTP of its own and the engine reports a clock error after
 * its timeout.
 */

#pragma once

#include <cstdint>
#include <ctime>

enum class ClockSyncState : uint8_t {
    Idle,
    Syncing,
    Succeeded,
    Failed,
};

/// 2024-01-01 之前算没校过时。/ Anything before 2024-01-01 counts as unset.
constexpr time_t kWeReadValidClock = 1704067200;

class HalClock {
 public:
    time_t nowUtc() const { return time(nullptr); }
    bool hasValidTime() const { return nowUtc() >= kWeReadValidClock; }
    bool requestSync() { return true; }
    ClockSyncState syncState() const { return hasValidTime() ? ClockSyncState::Succeeded : ClockSyncState::Syncing; }
};
inline HalClock halClock;
