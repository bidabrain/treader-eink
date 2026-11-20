/* SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：界面预览渲染用的替身。/ English: Shim for the UI preview renderer.
 * 冻结：仅用于主机预览。/ Frozen: host previews only.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "epdiy.h"
bool book_cover_draw(uint8_t* fb, EpdRect r, const char* path);
