/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：系统升级页对主循环公开的一个钩子。
 *
 * English: The one hook the system upgrade page exposes to the main loop.
 */
#pragma once

/// 每次开机第一次见到 TF 卡挂好时调用：新固件正常启动了，就删掉在线升级下载的那个升级包；退回旧版本则保留。
/// Call the first time a TF card is mounted after boot: if the new firmware came up, delete the package the online
/// upgrade downloaded; keep it after a rollback.
void tr_update_boot_cleanup(void);
