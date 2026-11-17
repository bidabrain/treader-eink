# SPDX-License-Identifier: Apache-2.0
# 番茄小说同步：联网、HTTP、书架元数据、后台任务。/ Tomato sync: network, HTTP, shelf metadata, background task.
idf_component_register(
    SRCS "tomato_net.c" "tomato_http.c" "tomato_store.c" "tomato_sync.c" "tomato_webdav.c" "tomato_rank.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
    PRIV_REQUIRES read_pico_transfer esp_wifi esp_netif lwip esp_event esp_http_client mbedtls heap fatfs
)

# 静态分析构建：idf.py -B build-analyze -DTREADER_ANALYZE=1 build
# Static-analysis build: idf.py -B build-analyze -DTREADER_ANALYZE=1 build
if(TREADER_ANALYZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fanalyzer -fstack-usage -Wno-error)
endif()

# 编译时注入默认番茄服务器，规则同安卓版：读项目根目录 local.properties 的 DOWNLOADER_URL / DOWNLOADER_PASSWORD，
# 文件里没有就读同名环境变量。local.properties 不入库；值异或混淆后写进构建目录的生成头文件，固件里没有明文。
# Inject a default Tomato server at build time, like the Android app: DOWNLOADER_URL / DOWNLOADER_PASSWORD from
# local.properties in the project root, else environment variables. The file stays untracked; values are XOR-obfuscated
# into a generated header in the build directory, so the firmware holds no plain text.
idf_build_get_property(treader_project_dir PROJECT_DIR)
set(treader_props "${treader_project_dir}/local.properties")
set(treader_url "$ENV{DOWNLOADER_URL}")
set(treader_pass "$ENV{DOWNLOADER_PASSWORD}")
# 书城榜单默认地址，不是秘密，明文写进固件。/ Default rank site; not secret, stored as plain text.
set(treader_store "$ENV{STORE_URL}")
if(EXISTS "${treader_props}")
    file(READ "${treader_props}" treader_text)
    foreach(treader_key DOWNLOADER_URL DOWNLOADER_PASSWORD STORE_URL)
        if(treader_text MATCHES "(^|\n)[ \t]*${treader_key}[ \t]*[=:][ \t]*([^\r\n]*)")
            set(treader_value "${CMAKE_MATCH_2}")
            # properties 转义：\: \= \ / Properties escapes
            string(REPLACE [=[\:]=] ":" treader_value "${treader_value}")
            string(REPLACE [=[\=]=] "=" treader_value "${treader_value}")
            string(REPLACE [=[\\]=] [=[\]=] treader_value "${treader_value}")
            string(STRIP "${treader_value}" treader_value)
            if(treader_key STREQUAL "DOWNLOADER_URL")
                set(treader_url "${treader_value}")
            elseif(treader_key STREQUAL "STORE_URL")
                set(treader_store "${treader_value}")
            else()
                set(treader_pass "${treader_value}")
            endif()
        endif()
    endforeach()
endif()
string(REGEX REPLACE "/+$" "" treader_url "${treader_url}")

# 密钥由内容推出，内容不变生成文件就不变，不会每次都重编。/ The key derives from the content, so the header only changes with it.
string(SHA256 treader_key_hex "treader|${treader_url}|${treader_pass}")
function(treader_obfuscate text out_var)
    string(HEX "${text}" hex)
    string(LENGTH "${hex}" hex_len)
    set(bytes "")
    set(i 0)
    set(n 0)
    while(i LESS hex_len)
        string(SUBSTRING "${hex}" ${i} 2 byte)
        math(EXPR k_at "(${n} % 32) * 2")
        string(SUBSTRING "${treader_key_hex}" ${k_at} 2 k)
        math(EXPR v "0x${byte} ^ 0x${k}" OUTPUT_FORMAT HEXADECIMAL)
        string(APPEND bytes "${v},")
        math(EXPR i "${i} + 2")
        math(EXPR n "${n} + 1")
    endwhile()
    set(${out_var} "${bytes}" PARENT_SCOPE)
    set(${out_var}_LEN ${n} PARENT_SCOPE)
endfunction()
treader_obfuscate("${treader_url}" treader_url_bytes)
treader_obfuscate("${treader_pass}" treader_pass_bytes)
set(treader_key_bytes "")
foreach(i RANGE 0 62 2)
    string(SUBSTRING "${treader_key_hex}" ${i} 2 k)
    string(APPEND treader_key_bytes "0x${k},")
endforeach()
set(treader_gen "${CMAKE_CURRENT_BINARY_DIR}/tomato_builtin.h")
set(treader_body "// 构建时生成，勿入库。/ Generated at build time; do not commit.\n#pragma once\n#include <stdint.h>\n")
string(APPEND treader_body "#define TOMATO_BUILTIN_URL_LEN ${treader_url_bytes_LEN}\n#define TOMATO_BUILTIN_PASS_LEN ${treader_pass_bytes_LEN}\n")
string(APPEND treader_body "static const uint8_t TOMATO_BUILTIN_KEY[32] __attribute__((unused)) = {${treader_key_bytes}};\n")
string(APPEND treader_body "static const uint8_t TOMATO_BUILTIN_URL[] __attribute__((unused)) = {${treader_url_bytes}0};\n")
string(REPLACE [=[\]=] [=[\\]=] treader_store_c "${treader_store}")
string(REPLACE "\"" "\\\"" treader_store_c "${treader_store_c}")
string(APPEND treader_body "#define TOMATO_STORE_DEFAULT \"${treader_store_c}\"\n")
string(APPEND treader_body "static const uint8_t TOMATO_BUILTIN_PASS[] __attribute__((unused)) = {${treader_pass_bytes}0};\n")
file(CONFIGURE OUTPUT "${treader_gen}" CONTENT "${treader_body}" @ONLY)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(treader_url_bytes_LEN GREATER 0)
    message(STATUS "Treader: built-in Tomato server enabled")
else()
    message(STATUS "Treader: no built-in Tomato server")
endif()
