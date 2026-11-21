/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：界面预览渲染用的替身。/ English: Shim for the UI preview renderer.
 * 冻结：仅用于主机预览。/ Frozen: host previews only.
 */
#pragma once
#include "FreeRTOS.h"
// 互斥锁：主机上单线程，拿锁和放锁什么也不做。/ Mutexes: single-threaded on the host, so take and give do nothing.
typedef struct { int unused; } StaticSemaphore_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buf) { return (SemaphoreHandle_t)buf; }
#ifndef xSemaphoreTake
#define xSemaphoreTake(m, t) ((void)(m), (void)(t), 1)
#define xSemaphoreGive(m) ((void)(m), 1)
#endif
