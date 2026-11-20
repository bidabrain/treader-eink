/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：界面预览渲染用的替身。/ English: Shim for the UI preview renderer.
 * 冻结：仅用于主机预览。/ Frozen: host previews only.
 */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define heap_caps_malloc(n, c) malloc(n)
#define heap_caps_calloc(n, s, c) calloc(n, s)
#define heap_caps_realloc(p, n, c) realloc(p, n)
#define heap_caps_free(p) free(p)
#define heap_caps_get_largest_free_block(c) ((size_t)64 << 20)
