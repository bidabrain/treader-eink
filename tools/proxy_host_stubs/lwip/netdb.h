// 主机测试桩：lwIP 的名字解析换成系统的。/ Host-test stub: lwIP name resolution becomes the system's.
#pragma once
#include <netdb.h>
#define lwip_freeaddrinfo freeaddrinfo
