# SPDX-License-Identifier: Apache-2.0
# 番茄小说同步：联网、HTTP、书架元数据、后台任务。/ Tomato sync: network, HTTP, shelf metadata, background task.
idf_component_register(
    SRCS "tomato_net.c" "tomato_http.c" "tomato_store.c" "tomato_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
    PRIV_REQUIRES read_pico_transfer esp_wifi esp_netif lwip esp_event esp_http_client mbedtls heap fatfs
)
