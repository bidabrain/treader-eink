/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：保存的 Wi-Fi 列表和它在 TF 卡上的 AES-256-GCM 加密文件。钥匙由调用方给（设备 NVS 里的随机数），本组件
 * 不碰 NVS、SD 挂载、Wi-Fi 和界面，主机上可以直接测试。
 *
 * English: the saved Wi-Fi list and its AES-256-GCM file on the TF card. The caller supplies the key (a random secret
 * in device NVS); this component touches no NVS, SD mounting, Wi-Fi or UI, so it runs in host tests.
 *
 * 冻结：密码只在内存和加密文件里出现；文件读不开一律当空列表，不尝试修复。文件格式改动要升版本号。
 * Frozen: passwords exist only in memory and in the encrypted file; an unreadable file counts as an empty list and is
 * never repaired. A file-format change bumps the version.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_VAULT_MAX 16      ///< 最多保存几个网络 / Most networks kept
#define WIFI_VAULT_KEY_LEN 32  ///< 设备钥匙和文件钥匙的字节数 / Bytes in the device and file keys

/// 返回码。/ Return codes.
typedef enum {
    WIFI_VAULT_OK = 0,
    WIFI_VAULT_ERR_ARG,     ///< 参数不对 / Bad argument
    WIFI_VAULT_ERR_NOENT,   ///< 文件不存在 / File missing
    WIFI_VAULT_ERR_IO,      ///< 读写失败 / Read or write failed
    WIFI_VAULT_ERR_FORMAT,  ///< 文件损坏或版本不认识 / Corrupt or unknown version
    WIFI_VAULT_ERR_AUTH,    ///< 钥匙不对或文件被改过 / Wrong key or tampered file
    WIFI_VAULT_ERR_CRYPTO,  ///< 加密库失败 / Crypto library failure
} wifi_vault_err_t;

/// 一个保存的网络。/ One saved network.
typedef struct {
    char ssid[33];      ///< 网络名，1～32 字节 / Network name, 1-32 bytes
    char password[65];  ///< 空表示开放网络 / Empty means an open network
    uint32_t last_ok;   ///< 上次连上的 Unix 时间，0 表示不知道 / Unix time of the last success, 0 when unknown
} wifi_vault_entry_t;

/// 网络列表。/ The network list.
typedef struct {
    int count;
    wifi_vault_entry_t items[WIFI_VAULT_MAX];
} wifi_vault_t;

/// 网络名和密码是否能存（规则和传书配网相同）。/ Whether a name and password may be stored (same rules as provisioning).
bool wifi_vault_valid(const char* ssid, const char* password);
/// 按网络名找，返回下标，没有返回 -1。/ Find by name; index or -1.
int wifi_vault_find(const wifi_vault_t* v, const char* ssid);
/// 加入或更新一个网络；满了挤掉 last_ok 最旧的。last_ok 为 0 时保留原来的时间。
/// Add or update a network; when full, the one with the oldest last_ok goes. A last_ok of 0 keeps the old time.
bool wifi_vault_put(wifi_vault_t* v, const char* ssid, const char* password, uint32_t last_ok);
/// 删除一个网络，返回是否删到了。/ Remove a network; whether one was removed.
bool wifi_vault_remove(wifi_vault_t* v, const char* ssid);
/// 按 last_ok 从新到旧排。/ Sort by last_ok, newest first.
void wifi_vault_sort(wifi_vault_t* v);

/// 由设备钥匙派生文件钥匙（HKDF-SHA256）。/ Derive the file key from the device key (HKDF-SHA256).
wifi_vault_err_t wifi_vault_derive(const uint8_t dev[WIFI_VAULT_KEY_LEN], uint8_t key[WIFI_VAULT_KEY_LEN]);
/// 读出列表；正式文件读不开时依次试 .tmp、.bak。失败时 out 清零。
/// Load the list; when the file will not open, try .tmp then .bak. On failure out is zeroed.
wifi_vault_err_t wifi_vault_load(const char* path, const uint8_t key[WIFI_VAULT_KEY_LEN], wifi_vault_t* out);
/// 写入列表：先写 .tmp，再换下正式文件，成功后删掉 .bak（不留下已删网络的旧密码）。
/// Save the list: write .tmp, swap out the current file, then delete .bak so no forgotten password lingers.
wifi_vault_err_t wifi_vault_save(const char* path, const uint8_t key[WIFI_VAULT_KEY_LEN], const wifi_vault_t* v);
/// 取随机数；拿不到时清零并返回 false。/ Random bytes; zeroed with false when unavailable.
bool wifi_vault_random(uint8_t* out, size_t n);
/// 擦掉内存，编译器不会省掉。/ Wipe memory in a way the compiler keeps.
void wifi_vault_wipe(void* p, size_t n);

#ifdef __cplusplus
}
#endif
