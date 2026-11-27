# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# TF 卡缓存包：只追加的段文件，带上限和后台回收。/ TF-card cache pack: append-only segment files with a cap and
# background collection.
idf_component_register(SRCS "cache_pack.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_rom heap freertos vfs)
