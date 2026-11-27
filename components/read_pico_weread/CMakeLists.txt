# SPDX-FileCopyrightText: 2026 Treader contributors
# SPDX-License-Identifier: Apache-2.0
# 微信读书：vendor/ 是 CrossMux 的 WeReadWebApi 引擎和 FreeInk SDK 的 JSON 解析器（MIT，改动见 LICENSE），
# port/ 是接到 ESP-IDF 和 TF 卡的适配层。/ WeRead: vendor/ holds CrossMux's WeReadWebApi engine and the FreeInk SDK
# JSON parser (MIT, changes listed in LICENSE); port/ adapts them to ESP-IDF and the TF card.
idf_component_register(
    SRCS "weread_service.cpp" "port/HalStorage.cpp" "port/ObfuscationUtils.cpp" "port/TextOut.cpp"
         "vendor/StreamingJsonParser.cpp" "vendor/WeReadProtocol.cpp" "vendor/WeReadStore.cpp"
         "vendor/WeReadBrowse.cpp" "vendor/WeReadXhtmlCodec.cpp" "vendor/WeReadClient.cpp"
         "vendor/WeReadHttpClient.cpp" "vendor/Epub/htmlEntities.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "port" "vendor"
    REQUIRES esp_common
    PRIV_REQUIRES tomato esp_http_client esp-tls mbedtls nvs_flash esp_wifi esp_timer esp_rom esp_hw_support)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
# 引擎只在 ENABLE_CHINESE_VERSION 下编译，另有 LOG_LEVEL 和 BOARD_HAS_PSRAM 的分支；按尺寸优化，翻页和刷屏不走这里。
# The engine builds only under ENABLE_CHINESE_VERSION and branches on LOG_LEVEL and BOARD_HAS_PSRAM; it is built for
# size since page turns and refreshes never run it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ENABLE_CHINESE_VERSION=1 ENABLE_SERIAL_LOG=1 LOG_LEVEL=1 BOARD_HAS_PSRAM=1)
# 上游按 uint32_t 是 unsigned int 写日志格式（Xtensa 上是 unsigned long），switch 里有故意的贯穿：都不当错误，
# 不改引擎。/ Upstream log formats assume uint32_t is unsigned int (unsigned long on Xtensa) and its switches fall
# through on purpose: neither is an error, and the engine stays unpatched.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wno-format -Wno-implicit-fallthrough)
