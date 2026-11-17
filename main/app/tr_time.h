/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 时间保持：联网对时后写进 PMU 的 RTC，开机或唤醒时从 RTC 读回系统时间。只在主循环任务里调用（PMU 驱动不加锁）。
 * Timekeeping: after SNTP, write the time to the PMU RTC; on boot or wake, read it back. Call only from the
 * main loop task (the PMU driver has no lock).
 */
#pragma once

/// 系统时间无效时从 PMU 的 RTC 恢复，并设好时区。/ Restore system time from the PMU RTC when unset, and set the zone.
void tr_time_restore(void);
/// 刚对过时就写进 PMU。/ Push freshly synced time to the PMU.
void tr_time_poll(void);
