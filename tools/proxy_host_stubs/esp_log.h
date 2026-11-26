// 主机测试桩：警告打到 stderr，其余忽略。/ Host-test stub: warnings go to stderr, the rest are dropped.
#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "  [W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
