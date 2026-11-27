/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：SHA-256，用 PSA Crypto（S3 上走硬件加速）。/ English: SHA-256 via PSA Crypto (hardware accelerated on the S3).
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "psa/crypto.h"

class HalSha256 {
    psa_hash_operation_t op_ = PSA_HASH_OPERATION_INIT;
    bool healthy_ = false;

 public:
    HalSha256() { healthy_ = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&op_, PSA_ALG_SHA_256) == PSA_SUCCESS; }
    ~HalSha256() { psa_hash_abort(&op_); }
    HalSha256(const HalSha256&) = delete;
    HalSha256& operator=(const HalSha256&) = delete;

    bool update(const uint8_t* data, size_t len) {
        healthy_ = healthy_ && psa_hash_update(&op_, data, len) == PSA_SUCCESS;
        return healthy_;
    }
    bool finish(uint8_t out[32]) {
        size_t n = 0;
        const bool ok = healthy_ && psa_hash_finish(&op_, out, 32, &n) == PSA_SUCCESS && n == 32;
        healthy_ = false;
        return ok;
    }
};
