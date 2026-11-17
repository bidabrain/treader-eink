# SPDX-License-Identifier: Apache-2.0
# 番茄小说同步：联网、HTTP、书架元数据、后台任务。/ Tomato sync: network, HTTP, shelf metadata, background task.
idf_component_register(
    SRCS "tomato_net.c" "tomato_http.c" "tomato_store.c" "tomato_sync.c" "tomato_webdav.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
    PRIV_REQUIRES read_pico_transfer esp_wifi esp_netif lwip esp_event esp_http_client mbedtls heap fatfs
)

# 静态分析构建：idf.py -B build-analyze -DTREADER_ANALYZE=1 build
# Static-analysis build: idf.py -B build-analyze -DTREADER_ANALYZE=1 build
if(TREADER_ANALYZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fanalyzer -fstack-usage -Wno-error)
endif()
