#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 缓存包主机测试（ASan + UBSan）。在 gcc:14 容器里跑：
#   docker run --rm -v "$PWD:/w" -w /w gcc:14 bash tools/run_cache_pack_host_test.sh
# Host test for the cache pack (ASan + UBSan). Run in a gcc:14 container (command above).
set -euo pipefail
c=components/read_pico_cache_pack
gcc -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -DCPACK_HOST_TEST \
    -I"$c/include" -Itools/zip_host_stubs tools/cache_pack_host_test.c "$c/cache_pack.c" -lpthread -o /tmp/cache_pack_test
/tmp/cache_pack_test
