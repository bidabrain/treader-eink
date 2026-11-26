#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 局域网代理主机测试：把 tomato_proxy.c 连同 lwIP / NVS 的桩编到电脑上，对一个真的 SOCKS5 / HTTP 代理服务端跑一遍。
# 在 gcc:14 容器里跑：docker run --rm -v "$PWD:/w" -w /w gcc:14 bash tools/run_proxy_host_test.sh
# Host test for the LAN proxy: tomato_proxy.c built with lwIP / NVS stubs and run against a real SOCKS5 / HTTP proxy
# server. Run in a gcc:14 container (command above).
set -euo pipefail
gcc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Itools/proxy_host_stubs -Icomponents/tomato/include tools/proxy_host_test.c components/tomato/tomato_proxy.c \
    -lpthread -o /tmp/proxy_host_test
/tmp/proxy_host_test
