/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 验证器的计算和加密文件：RFC 6238 验证码、base32、账号表的二进制格式，以及 TF 卡上的 AES-256-GCM 加密文件
 * （钥匙由口令 PBKDF2 再和设备随机数 HKDF 得出）。不碰 NVS、界面和时间来源，主机上可以直接测试。
 *
 * English: authenticator math and the encrypted file: RFC 6238 codes, base32, the binary account table, and the
 * AES-256-GCM file on the TF card (key = PBKDF2 of the passphrase, then HKDF with the device secret). No NVS, UI or
 * clock source here, so it runs in host tests.
 *
 * 冻结：密钥只在内存里出现；不提供导出；不支持 HOTP。文件格式改动要升版本号并能读旧版。
 * Frozen: secrets exist only in memory; no export; no HOTP. A file-format change bumps the version and keeps reading
 * the old one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOTP_MAX_ACCOUNTS 100
#define TOTP_TEXT_MAX 64     ///< 发行方、名称最多字节数 / Max bytes for issuer and name
#define TOTP_SECRET_MAX 64   ///< 密钥最多字节数 / Max secret bytes
#define TOTP_KEY_LEN 32
#define TOTP_SALT_LEN 16
#define TOTP_DEV_SECRET_LEN 32
#define TOTP_MIN_ITERS 100000u   ///< 字母数字口令（旧版）用的下限 / Floor used by the old alphanumeric passphrase
#define TOTP_PIN_ITERS 10000u    ///< 4 位 PIN 用的次数，设备上解锁约 1 秒 / Count for the 4-digit PIN, about 1 s to unlock on the device
#define TOTP_FLOOR_ITERS 1000u   ///< 库接受的最少迭代次数 / Fewest iterations the library accepts

/// 返回码。/ Return codes.
typedef enum {
    TOTP_OK = 0,
    TOTP_ERR_ARG,     ///< 参数不对 / Bad argument
    TOTP_ERR_NOENT,   ///< 文件不存在 / File missing
    TOTP_ERR_IO,      ///< 读写失败 / Read or write failed
    TOTP_ERR_FORMAT,  ///< 文件损坏或版本不认识 / Corrupt or unknown version
    TOTP_ERR_AUTH,    ///< 口令不对（或文件被改过）/ Wrong passphrase (or tampered file)
    TOTP_ERR_NOMEM,   ///< 内存不够 / Out of memory
    TOTP_ERR_CRYPTO,  ///< 加密库失败 / Crypto library failure
} totp_err_t;

typedef enum { TOTP_SHA1 = 0, TOTP_SHA256 = 1, TOTP_SHA512 = 2 } totp_algo_t;

/// 一个账号。/ One account.
typedef struct {
    uint32_t id;                       ///< 递增编号，网页改名、排序、删除用 / Increasing id for web rename, move, delete
    char issuer[TOTP_TEXT_MAX + 1];    ///< 发行方，可空 / Issuer, may be empty
    char name[TOTP_TEXT_MAX + 1];      ///< 账号名 / Account name
    uint8_t secret[TOTP_SECRET_MAX];
    uint8_t secret_len;
    uint8_t algo;                      ///< totp_algo_t
    uint8_t digits;                    ///< 6 或 8 / 6 or 8
    uint8_t period;                    ///< 30 或 60 秒 / 30 or 60 s
} totp_account_t;

/// 账号表。/ Account table.
typedef struct {
    uint32_t next_id;
    int count;
    totp_account_t items[TOTP_MAX_ACCOUNTS];
} totp_table_t;

/// 加密文件的钥匙：盐和迭代次数随文件走，换口令时一起换。/ File key; salt and iterations travel with the file and
/// change with the passphrase.
typedef struct {
    uint8_t key[TOTP_KEY_LEN];
    uint8_t salt[TOTP_SALT_LEN];
    uint32_t iters;
} totp_key_t;

/* ---- 验证码 / Codes ---- */

/// 算 unix_time（UTC 秒）时的验证码。/ Code at unix_time (UTC seconds).
totp_err_t totp_code(const totp_account_t* a, int64_t unix_time, uint32_t* out);
/// 把验证码按位数写成文字，6 位写成“123 456”，8 位写成“1234 5678”。/ Format a code with a gap in the middle.
void totp_format(uint32_t code, int digits, char* out, size_t cap);
/// 本周期还剩几秒（1..period）。/ Seconds left in the current period (1..period).
int totp_remaining(const totp_account_t* a, int64_t unix_time);
/// 账号字段是否合法（位数、周期、算法、密钥长度、名称）。/ Whether the account fields are valid.
bool totp_account_valid(const totp_account_t* a);

/// base32（RFC 4648）解码；忽略空格、连字符和“=”，不分大小写。/ Base32 decode ignoring spaces, dashes and '=',
/// case-insensitive.
bool totp_base32_decode(const char* in, uint8_t* out, size_t cap, size_t* out_len);

/* ---- 账号表 / Table ---- */

/// 加一个账号（编号自动分配）；满了或无效返回 false。同发行方、名称、密钥的已存在也返回 false，dup 置 true。
/// Add an account (id assigned); false when full or invalid. An identical issuer, name and secret already present
/// returns false with dup set.
bool totp_table_add(totp_table_t* t, const totp_account_t* a, bool* dup);
/// 按编号找下标，没有返回 -1。/ Index by id, -1 when absent.
int totp_table_find(const totp_table_t* t, uint32_t id);
/// 删除第 i 个。/ Remove item i.
void totp_table_remove(totp_table_t* t, int i);
/// 把第 from 个移到 to。/ Move item from to index to.
void totp_table_move(totp_table_t* t, int from, int to);

/* ---- 加密文件 / Encrypted file ---- */

/// 读文件头里的盐和迭代次数（不需要口令）。/ Read salt and iterations from the header (no passphrase needed).
totp_err_t totp_vault_header(const char* path, uint8_t salt[TOTP_SALT_LEN], uint32_t* iters);
/// 由口令和设备随机数算出钥匙；k->salt 和 k->iters 先填好。/ Derive the key; fill k->salt and k->iters first.
totp_err_t totp_vault_derive(const char* pass, const uint8_t dev[TOTP_DEV_SECRET_LEN], totp_key_t* k);
/// 用口令打开：读头、算钥匙、解密；主文件坏了自动试 .bak。成功时 k 填好。
/// Open with the passphrase: read the header, derive, decrypt; falls back to .bak when the main file is bad. Fills k on
/// success.
totp_err_t totp_vault_unlock(const char* path, const char* pass, const uint8_t dev[TOTP_DEV_SECRET_LEN],
                             totp_key_t* k, totp_table_t* out);
/// 用已有钥匙解密（主文件坏了试 .bak）。/ Decrypt with a known key (falls back to .bak).
totp_err_t totp_vault_load(const char* path, const totp_key_t* k, totp_table_t* out);
/// 加密写入：先写 .tmp，旧文件改名成 .bak，再改名成正式文件。/ Encrypt and write: .tmp first, old file to .bak, then
/// rename into place.
totp_err_t totp_vault_save(const char* path, const totp_key_t* k, const totp_table_t* t);
/// 删掉 .bak 和 .tmp（换口令后调用，旧口令就什么都解不开了）。/ Remove .bak and .tmp (call after a passphrase
/// change so the old passphrase opens nothing).
void totp_vault_drop_backup(const char* path);
/// 删除正式文件、.bak 和 .tmp。/ Remove the file, .bak and .tmp.
void totp_vault_remove(const char* path);
/// 文件在不在（正式文件或 .bak）。/ Whether the file (or .bak) exists.
bool totp_vault_exists(const char* path);

/// 新钥匙：随机盐，迭代次数用 iters（低于 TOTP_FLOOR_ITERS 按它算）。/ New key material: random salt with the given
/// iterations (raised to TOTP_FLOOR_ITERS when lower).
totp_err_t totp_key_new(const char* pass, const uint8_t dev[TOTP_DEV_SECRET_LEN], uint32_t iters, totp_key_t* k);
/// 量一下 n 次 PBKDF2 要多少微秒，用来定迭代次数。now_us 给时钟。/ Time n PBKDF2 iterations in microseconds to pick the
/// iteration count; now_us supplies the clock.
int64_t totp_pbkdf2_time(uint32_t n, int64_t (*now_us)(void));

/// 随机字节；失败返回 false（输出清零）。/ Random bytes; false on failure (output zeroed).
bool totp_random(uint8_t* out, size_t n);
/// 清零，不会被编译器省掉。/ Zero memory in a way the compiler keeps.
void totp_wipe(void* p, size_t n);

#ifdef __cplusplus
}
#endif
