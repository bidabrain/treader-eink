/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Arduino MD5Builder 的子集，用芯片 ROM 里的 MD5（服务器给的校验码要用 MD5 比对）。
 *
 * English: A subset of Arduino's MD5Builder on the chip's ROM MD5 (server checksums are MD5).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "Arduino.h"
#include "esp_rom_md5.h"

class MD5Builder {
    md5_context_t ctx_ = {};
    uint8_t digest_[16] = {};

 public:
    void begin() { esp_rom_md5_init(&ctx_); }
    void add(const uint8_t* data, size_t len) { esp_rom_md5_update(&ctx_, data, len); }
    void calculate() { esp_rom_md5_final(digest_, &ctx_); }
    String toString() const {
        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", digest_[i]);
        return String(hex);
    }
};
