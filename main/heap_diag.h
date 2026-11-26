/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * PSRAM 占用诊断的入口，见 heap_diag.c。/ Entry point of the PSRAM usage diagnostics, see heap_diag.c.
 */
#pragma once

/// 诊断固件里起一个每分钟打一次 PSRAM 清单的任务；正式固件里什么也不做。
/// In a diagnostic build, start a task printing a PSRAM report every minute; does nothing in the product firmware.
void heap_diag_start(void);
