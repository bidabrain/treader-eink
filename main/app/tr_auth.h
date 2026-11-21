/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 验证器的会话：设备随机数和输错次数存 NVS（命名空间 totp），账号表加密放在 TF 卡 treader/totp/vault.bin；
 * 解锁后钥匙和账号表只在内部 RAM，上锁时清零。除 tr_auth_lock() 外只在主循环任务里调用。
 * Authenticator session: the device secret and failure count live in NVS (namespace totp), the encrypted table on
 * the TF card at treader/totp/vault.bin; once unlocked the key and table sit in internal RAM only and are zeroed on
 * lock. Call from the main loop task only, except tr_auth_lock().
 *
 * 冻结：不动 eFuse，不用 flash / NVS 加密；口令只在设备屏幕上输；没有找回口令的办法，忘了只能清空。
 * Frozen: no eFuse, no flash or NVS encryption; the passphrase is typed on the device only; there is no recovery, a
 * forgotten passphrase means clearing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "read_pico_totp.h"

#define TR_AUTH_PATH "/sdcard/treader/totp/vault.bin"
#define TR_AUTH_PASS_MIN 8
#define TR_AUTH_PASS_MAX 64
#define TR_AUTH_FREE_TRIES 4   ///< 不用等的输错次数 / Wrong tries allowed without waiting

/// 当前状态。/ Current state.
typedef enum {
    TR_AUTH_NOCARD = 0,  ///< 没有可用的 TF 卡 / No usable TF card
    TR_AUTH_UNSET,       ///< 还没设置 / Not set up
    TR_AUTH_LOCKED,      ///< 已设置、上锁 / Set up and locked
    TR_AUTH_OPEN,        ///< 已解锁 / Unlocked
    TR_AUTH_FOREIGN,     ///< 卡上有文件，设备里没有随机数 / File on the card, no device secret
    TR_AUTH_MISSING,     ///< 设备里有随机数，卡上没有文件 / Device secret, no file on the card
} tr_auth_state_t;

/// 自动上锁方式。/ Auto-lock mode.
typedef enum {
    TR_AUTH_LOCK_ON_SLEEP = 0,  ///< 只在锁屏或重启时 / Only on lock or restart
    TR_AUTH_LOCK_AWAY = 1,      ///< 离开验证器 5 分钟后 / Five minutes after leaving
} tr_auth_auto_t;

tr_auth_state_t tr_auth_state(void);
/// 口令是否合格：8～64 字节，字母和数字都有；不合格时 why 写原因。/ Whether a passphrase qualifies (8-64 bytes with
/// letters and digits); why gets the reason otherwise.
bool tr_auth_pass_ok(const char* pass, const char** why);
/// 还要等几秒才能再输口令（0 = 不用等）。/ Seconds to wait before another try (0 = none).
int tr_auth_wait_s(void);
/// 连续输错次数。/ Consecutive wrong tries.
int tr_auth_fails(void);

/// 第一次设置：生成设备随机数、量迭代次数、写空文件并解锁。/ First setup: create the device secret, time the
/// iterations, write an empty file and unlock.
totp_err_t tr_auth_setup(const char* pass);
/// 解锁；输错会计数。/ Unlock; wrong tries are counted.
totp_err_t tr_auth_unlock(const char* pass);
/// 改口令：旧口令对了才换，换后旧口令什么都解不开。/ Change the passphrase; only with the right old one, after which the
/// old one opens nothing.
totp_err_t tr_auth_change(const char* old_pass, const char* new_pass);
/// 上锁：钥匙和账号表清零释放。任何任务都可以调用。/ Lock: zero and free the key and table. Any task may call it.
void tr_auth_lock(void);
/// 清空：删卡上文件、删 NVS 里的随机数和计数，并上锁。/ Clear: delete the card files, the NVS secret and counters, and
/// lock.
void tr_auth_wipe(void);

/// 已解锁时的账号表（上锁返回 NULL）。改完调 tr_auth_save()。/ The table while unlocked (NULL when locked); call
/// tr_auth_save() after changes.
totp_table_t* tr_auth_table(void);
/// 把账号表加密写回卡上。/ Encrypt the table back to the card.
totp_err_t tr_auth_save(void);
/// 账号个数（上锁时 -1）。/ Account count (-1 when locked).
int tr_auth_count(void);

tr_auth_auto_t tr_auth_auto_lock(void);
void tr_auth_set_auto_lock(tr_auth_auto_t mode);
/// 进入验证器：离开超过 5 分钟（按设置）就先上锁。/ Entering the authenticator: lock first when away too long (per
/// the setting).
void tr_auth_enter(void);
/// 离开验证器：按设置开始计时。/ Leaving the authenticator: start the away timer per the setting.
void tr_auth_leave(void);
