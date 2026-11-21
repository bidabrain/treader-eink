/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：totp_crypto.h 的 PSA Crypto 实现。临时导入的钥匙用完立即销毁。
 *
 * English: PSA Crypto implementation of totp_crypto.h. Keys imported for one call are destroyed right after.
 */

#include "totp_crypto.h"

#include <string.h>

#include "psa/crypto.h"

static bool ready(void) {
    // 重复调用无害。/ Repeated calls are harmless.
    return psa_crypto_init() == PSA_SUCCESS;
}

static psa_algorithm_t hash_of(totp_algo_t algo) {
    return algo == TOTP_SHA256 ? PSA_ALG_SHA_256 : algo == TOTP_SHA512 ? PSA_ALG_SHA_512 : PSA_ALG_SHA_1;
}

totp_err_t totp_hmac(totp_algo_t algo, const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                     uint8_t* out, size_t* out_len) {
    if (!ready()) return TOTP_ERR_CRYPTO;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(hash_of(algo)));
    psa_key_id_t id = 0;
    if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) return TOTP_ERR_CRYPTO;
    const psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(hash_of(algo)), msg, msg_len, out, 64, out_len);
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? TOTP_OK : TOTP_ERR_CRYPTO;
}

totp_err_t totp_pbkdf2(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len, uint32_t iters,
                       uint8_t out[32]) {
    if (!ready()) return TOTP_ERR_CRYPTO;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iters);
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, pass, pass_len);
    if (st == PSA_SUCCESS) st = psa_key_derivation_output_bytes(&op, out, 32);
    psa_key_derivation_abort(&op);
    return st == PSA_SUCCESS ? TOTP_OK : TOTP_ERR_CRYPTO;
}

totp_err_t totp_hkdf(const uint8_t* ikm, size_t ikm_len, const uint8_t* salt, size_t salt_len, const uint8_t* info,
                     size_t info_len, uint8_t out[32]) {
    if (!ready()) return TOTP_ERR_CRYPTO;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, ikm_len);
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len);
    if (st == PSA_SUCCESS) st = psa_key_derivation_output_bytes(&op, out, 32);
    psa_key_derivation_abort(&op);
    return st == PSA_SUCCESS ? TOTP_OK : TOTP_ERR_CRYPTO;
}

static psa_key_id_t aes_key(const uint8_t key[32]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_key_id_t id = 0;
    return psa_import_key(&attr, key, 32, &id) == PSA_SUCCESS ? id : 0;
}

totp_err_t totp_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                         const uint8_t* in, size_t len, uint8_t* out) {
    if (!ready()) return TOTP_ERR_CRYPTO;
    const psa_key_id_t id = aes_key(key);
    if (!id) return TOTP_ERR_CRYPTO;
    size_t n = 0;
    const psa_status_t st = psa_aead_encrypt(id, PSA_ALG_GCM, nonce, 12, aad, aad_len, in, len, out, len + 16, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == len + 16 ? TOTP_OK : TOTP_ERR_CRYPTO;
}

totp_err_t totp_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                         const uint8_t* in, size_t len, uint8_t* out) {
    if (!ready()) return TOTP_ERR_CRYPTO;
    const psa_key_id_t id = aes_key(key);
    if (!id) return TOTP_ERR_CRYPTO;
    size_t n = 0;
    const psa_status_t st = psa_aead_decrypt(id, PSA_ALG_GCM, nonce, 12, aad, aad_len, in, len + 16, out, len, &n);
    psa_destroy_key(id);
    if (st == PSA_ERROR_INVALID_SIGNATURE) return TOTP_ERR_AUTH;
    return st == PSA_SUCCESS && n == len ? TOTP_OK : TOTP_ERR_CRYPTO;
}

bool totp_random(uint8_t* out, size_t n) {
    if (ready() && psa_generate_random(out, n) == PSA_SUCCESS) return true;
    // 拿不到随机数时不给出可预测的数。/ Never hand out predictable bytes.
    memset(out, 0, n);
    return false;
}
