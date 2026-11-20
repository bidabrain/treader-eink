/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：显示主机测试用的板级替身：撤驱动总是成功。/ English: Board shim for the display host test: drive release always succeeds.
 * 冻结：仅用于主机测试。/ Frozen: host tests only.
 */
#pragma once
#include <stdbool.h>
static inline bool read_pico_epd_release_drive(void) { return true; }
