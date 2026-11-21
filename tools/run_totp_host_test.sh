#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 验证器组件主机测试：用 ESP-IDF 自带的 tf-psa-crypto 在电脑上编出 PSA Crypto，再编测试。
# 在 gcc:14 容器里跑，IDF 的 tf-psa-crypto 挂到 /psa：
#   docker run --rm -v "$PWD:/w" -w /w -v "$IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto:/psa:ro" gcc:14 \
#     bash tools/run_totp_host_test.sh
# Host test for the authenticator component: build PSA Crypto from ESP-IDF's tf-psa-crypto, then the test. Run in a
# gcc:14 container with IDF's tf-psa-crypto mounted at /psa (command above).
set -euo pipefail
PSA=${PSA:-/psa}
OUT=build/psa-host
# IDF 的副本把内部头文件放在 mbedtls/private/，内置驱动却按 mbedtls/ 引用，用一层链接补上。
# IDF's copy keeps internal headers under mbedtls/private/ while the builtin drivers include them as mbedtls/; a link
# layer bridges that.
mkdir -p /tmp/psa-shim/mbedtls
for f in "$PSA"/drivers/builtin/include/mbedtls/private/*.h; do ln -sf "$f" /tmp/psa-shim/mbedtls/; done
if [ ! -f "$OUT/core/libtfpsacrypto.a" ]; then
    command -v cmake >/dev/null || { apt-get update -qq >/dev/null && apt-get install -y -qq cmake >/dev/null; }
    cmake -S "$PSA" -B "$OUT" -DGEN_FILES=OFF -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-I/tmp/psa-shim >/dev/null
    cmake --build "$OUT" -j8 >/dev/null
fi
c=components/read_pico_totp
gcc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$c/include" -I"$c" -I"$PSA/include" \
    -I"$PSA/drivers/builtin/include" -I/tmp/psa-shim tools/totp_host_test.c "$c/totp.c" "$c/totp_vault.c" \
    "$c/totp_crypto.c" "$OUT/core/libtfpsacrypto.a" "$OUT/drivers/builtin/libbuiltin.a" \
    "$OUT/core/libtfpsacrypto.a" -o /tmp/totp_test
/tmp/totp_test
