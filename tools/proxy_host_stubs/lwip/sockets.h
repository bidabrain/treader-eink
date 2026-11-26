// 主机测试桩：lwIP 的套接字函数换成 POSIX 的。/ Host-test stub: lwIP socket calls become POSIX ones.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define lwip_send send
#define lwip_recv recv
#define lwip_fcntl fcntl
#define lwip_select select
#define lwip_getsockopt getsockopt
#define lwip_setsockopt setsockopt
