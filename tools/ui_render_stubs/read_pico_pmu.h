/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：界面预览渲染用的替身。/ English: Shim for the UI preview renderer.
 * 冻结：仅用于主机预览。/ Frozen: host previews only.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint16_t soc_permille; } pmu_snapshot_t;
bool read_pico_pmu_ready(void);
const pmu_snapshot_t* read_pico_pmu_get(void);
