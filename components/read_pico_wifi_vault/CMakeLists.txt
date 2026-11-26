# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 保存的 Wi-Fi 列表和 TF 卡上的加密文件，密码学用 PSA Crypto。/ The saved Wi-Fi list and its encrypted TF file, crypto
# via PSA Crypto.
idf_component_register(SRCS "wifi_vault.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls)
