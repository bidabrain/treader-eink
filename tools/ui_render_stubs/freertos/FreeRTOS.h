/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：界面预览渲染用的替身。/ English: Shim for the UI preview renderer.
 * 冻结：仅用于主机预览。/ Frozen: host previews only.
 */
#pragma once
typedef int SemaphoreHandle_t;
#define portMAX_DELAY 0
#define xSemaphoreCreateRecursiveMutex() 1
#define xSemaphoreTakeRecursive(s, w) 1
#define xSemaphoreGiveRecursive(s) 1
