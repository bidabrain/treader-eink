/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：ObfuscationUtils.h 的实现。密文格式：base64("WR1" | nonce 12 | 密文 | tag 16)。
 *
 * English: Implementation of ObfuscationUtils.h. Ciphertext: base64("WR1" | nonce 12 | ciphertext | tag 16).
 */

#include "ObfuscationUtils.h"

#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"

namespace {

constexpr char kNs[] = "weread";
constexpr char kKeyName[] = "skey";
constexpr char kMagic[] = "WR1";
constexpr size_t kMagicLen = 3, kNonceLen = 12, kTagLen = 16, kKeyLen = 32;

void wipe(void* p, size_t n) {
    volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
    while (n--) *b++ = 0;
}

// 读钥匙；create 时没有就生成一把存进去。/ Read the key; with create, make and store one when missing.
bool load_key(uint8_t key[kKeyLen], bool create) {
    nvs_handle_t h;
    if (nvs_open(kNs, create ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = kKeyLen;
    bool ok = nvs_get_blob(h, kKeyName, key, &n) == ESP_OK && n == kKeyLen;
    if (!ok && create) {
        esp_fill_random(key, kKeyLen);
        ok = nvs_set_blob(h, kKeyName, key, kKeyLen) == ESP_OK && nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    if (!ok) wipe(key, kKeyLen);
    return ok;
}

psa_key_id_t import_key(const uint8_t key[kKeyLen]) {
    if (psa_crypto_init() != PSA_SUCCESS) return 0;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_key_id_t id = 0;
    return psa_import_key(&attr, key, kKeyLen, &id) == PSA_SUCCESS ? id : 0;
}

}  // namespace

namespace obfuscation {

String obfuscateToBase64(const std::string& plain) {
    uint8_t key[kKeyLen];
    if (!load_key(key, true)) return String();
    const psa_key_id_t id = import_key(key);
    wipe(key, sizeof(key));
    if (!id) return String();
    std::vector<uint8_t> raw(kMagicLen + kNonceLen + plain.size() + kTagLen);
    memcpy(raw.data(), kMagic, kMagicLen);
    esp_fill_random(raw.data() + kMagicLen, kNonceLen);
    size_t out_len = 0;
    const psa_status_t st = psa_aead_encrypt(id, PSA_ALG_GCM, raw.data() + kMagicLen, kNonceLen, nullptr, 0,
                                             reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
                                             raw.data() + kMagicLen + kNonceLen, plain.size() + kTagLen, &out_len);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || out_len != plain.size() + kTagLen) return String();
    size_t b64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, raw.data(), raw.size());
    std::string b64(b64_len, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64.data()), b64.size(), &b64_len, raw.data(),
                              raw.size()) != 0)
        return String();
    b64.resize(b64_len);
    return String(std::move(b64));
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
    if (ok) *ok = false;
    if (!encoded) return {};
    const size_t in_len = strlen(encoded);
    std::vector<uint8_t> raw(in_len);
    size_t raw_len = 0;
    if (mbedtls_base64_decode(raw.data(), raw.size(), &raw_len, reinterpret_cast<const unsigned char*>(encoded),
                              in_len) != 0 ||
        raw_len < kMagicLen + kNonceLen + kTagLen || memcmp(raw.data(), kMagic, kMagicLen))
        return {};
    uint8_t key[kKeyLen];
    if (!load_key(key, false)) return {};
    const psa_key_id_t id = import_key(key);
    wipe(key, sizeof(key));
    if (!id) return {};
    const size_t ct_len = raw_len - kMagicLen - kNonceLen;
    std::string plain(ct_len - kTagLen, '\0');
    size_t out_len = 0;
    const psa_status_t st = psa_aead_decrypt(id, PSA_ALG_GCM, raw.data() + kMagicLen, kNonceLen, nullptr, 0,
                                             raw.data() + kMagicLen + kNonceLen, ct_len,
                                             reinterpret_cast<uint8_t*>(plain.data()), plain.size(), &out_len);
    psa_destroy_key(id);
    wipe(raw.data(), raw.size());
    if (st != PSA_SUCCESS || out_len != plain.size()) {
        wipe(plain.data(), plain.size());
        return {};
    }
    if (ok) *ok = true;
    return plain;
}

void forgetKey() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, kKeyName);
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace obfuscation
