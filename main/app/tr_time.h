/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 时间保持：联网对时（设置里开着“自动校时”时）或手动设置后写进 PMU 的 RTC，开机时从 RTC 读回系统时间；显示按设置
 * 里的时区。只在主循环任务里调用（PMU 驱动不加锁）。
 * Timekeeping: after SNTP (with "auto time" on) or a manual set, write the time to the PMU RTC; on boot read it back.
 * Display follows the zone in settings. Call only from the main loop task (the PMU driver has no lock).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/// 系统时间无效时从 PMU 的 RTC 恢复，并设好时区。/ Restore system time from the PMU RTC when unset, and set the zone.
void tr_time_restore(void);
/// 刚对过时就写进 PMU。/ Push freshly synced time to the PMU.
void tr_time_poll(void);
/// 按设置里的时区和自动校时开关生效（改设置后调用）。/ Apply the zone and the auto-time switch from settings (call
/// after changing them).
void tr_time_apply_settings(void);
/// 手动设时间（按当前时区的本地时间），同时写进 PMU 的 RTC。/ Set the time by hand (local time in the current zone) and
/// write it to the PMU RTC too.
esp_err_t tr_time_set_local(int year, int month, int day, int hour, int minute);
/// 系统时间有没有设过。/ Whether the system time has been set.
bool tr_time_valid(void);
/// 估计现在的时间可能差几秒：上次校准时的误差（联网 0、手动 30 秒）加上按测得的漂移（没测过按每天 2 秒）
/// 走过的部分。时间无效返回 -1。验证器用它提醒校时。
/// Estimated clock error in seconds: the error at the last set (0 online, 30 by hand) plus drift since then at the
/// measured rate (2 s a day until measured). -1 when the time is invalid. The authenticator uses it to ask for a sync.
int tr_time_error_s(void);
