/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：引擎存登录信息时用的“混淆”接口。这里换成真加密：AES-256-GCM，钥匙是第一次用时生成、存在本机 NVS 里的随机数，
 * 所以 TF 卡上的 session.bin 拿到别的设备或电脑上解不开。接口名照旧，引擎不用改。
 *
 * English: The "obfuscation" API the engine stores its sign-in with. This makes it real encryption: AES-256-GCM
 * under a random key created on first use and kept in this device's NVS, so session.bin on the TF card cannot be
 * opened on another device or a computer. The names stay so the engine is unchanged.
 *
 * 冻结：钥匙只在 NVS（命名空间 weread），不写 TF 卡、不打日志。
 * Frozen: the key lives only in NVS (namespace weread), never on the TF card or in logs.
 */

#pragma once

#include <string>

#include "Arduino.h"

namespace obfuscation {

/// 加密后转 base64；失败返回空串。/ Encrypt then base64; empty on failure.
String obfuscateToBase64(const std::string& plain);

/// 解 base64 后解密；ok 为 false 时返回空串。/ Decode base64 and decrypt; empty with ok false on failure.
std::string deobfuscateFromBase64(const char* encoded, bool* ok);

/// 删掉 NVS 里的钥匙（退出登录时）。/ Erase the NVS key (on sign-out).
void forgetKey();

}  // namespace obfuscation
