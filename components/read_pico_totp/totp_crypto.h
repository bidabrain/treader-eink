/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证器用到的几样密码学操作，固件和主机测试都用 PSA Crypto 实现。
 *
 * English: the few crypto primitives the authenticator needs, implemented with PSA Crypto on device and in host tests.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "read_pico_totp.h"

/// HMAC；out 至少 64 字节，*out_len 返回长度。/ HMAC; out holds at least 64 bytes and *out_len gets the length.
totp_err_t totp_hmac(totp_algo_t algo, const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                     uint8_t* out, size_t* out_len);
/// PBKDF2-HMAC-SHA256，输出 32 字节。/ PBKDF2-HMAC-SHA256 with a 32-byte output.
totp_err_t totp_pbkdf2(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len, uint32_t iters,
                       uint8_t out[32]);
/// HKDF-SHA256，输出 32 字节。/ HKDF-SHA256 with a 32-byte output.
totp_err_t totp_hkdf(const uint8_t* ikm, size_t ikm_len, const uint8_t* salt, size_t salt_len, const uint8_t* info,
                     size_t info_len, uint8_t out[32]);
/// AES-256-GCM 加密：out 放密文再接 16 字节标签。/ AES-256-GCM seal: out gets the ciphertext then the 16-byte tag.
totp_err_t totp_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                         const uint8_t* in, size_t len, uint8_t* out);
/// AES-256-GCM 解密：in 是密文接标签，len 不含标签；标签不对返回 TOTP_ERR_AUTH。
/// AES-256-GCM open: in is ciphertext plus tag, len excludes the tag; a bad tag returns TOTP_ERR_AUTH.
totp_err_t totp_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                         const uint8_t* in, size_t len, uint8_t* out);
