# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 验证器：验证码计算和 TF 卡加密文件，密码学用 PSA Crypto。/ Authenticator codes and the encrypted TF file, crypto via
# PSA Crypto.
idf_component_register(SRCS "totp.c" "totp_vault.c" "totp_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls)
