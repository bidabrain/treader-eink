#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 番茄书架主机测试会写 /sdcard/tomato，只在一次性 Docker 容器里跑。在仓库根运行。
# The Tomato shelf host test writes /sdcard/tomato, so it runs only in a throwaway Docker container. Run from the repo root.
set -euo pipefail
docker run --rm -v "$(pwd):/src" -w /src gcc:14 bash -c '
gcc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Itools/tomato_store_stubs -Icomponents/tomato -Icomponents/tomato/include \
    -Imanaged_components/espressif__cjson/cJSON \
    tools/tomato_store_host_test.c managed_components/espressif__cjson/cJSON/cJSON.c -lm -o /tmp/tomato-store
/tmp/tomato-store'
