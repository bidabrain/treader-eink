/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 局域网代理主机测试：起一个真的 SOCKS5 / HTTP CONNECT 代理服务端，对 tomato_proxy.c 的握手、认证、域名转交、拒绝码、
 * 局域网直连和“握手后不多读数据”逐项核对。运行：tools/run_proxy_host_test.sh。
 * Host test for the LAN proxy: a real SOCKS5 / HTTP CONNECT proxy server checks tomato_proxy.c's handshake,
 * authentication, name hand-off, refusal codes, LAN bypass and that nothing after the handshake is over-read.
 * Run: tools/run_proxy_host_test.sh.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>

#include "lwip/sockets.h"
#include "tomato.h"

// 被测文件里的拦截函数，以及它们转调的“原函数”（这里就是系统的）。/ The wrappers under test and the "real" functions
// they forward to (the system's here).
int __wrap_lwip_connect(int s, const struct sockaddr* name, socklen_t namelen);
int __wrap_lwip_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
int __real_lwip_connect(int s, const struct sockaddr* name, socklen_t namelen) { return connect(s, name, namelen); }
int __real_lwip_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    return getaddrinfo(node, service, hints, res);
}

static int fails, checks;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            fails++;                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ---- 假代理服务端 / Fake proxy server ---- */

typedef struct {
    int kind;        // 0 SOCKS5，1 HTTP / 0 SOCKS5, 1 HTTP
    int need_auth;
    int refuse;      // SOCKS5 回复码或 HTTP 状态码，0 表示放行 / SOCKS5 reply code or HTTP status; 0 lets it through
} mode_t_;

static struct {
    int atyp, port, conns;
    char host[300], user[64], pass[64], http_req[1024];
} rec;
static mode_t_ mode;
static int listen_fd, listen_port;

static int read_n(int c, void* buf, size_t n) {
    uint8_t* p = buf;
    while (n) {
        const ssize_t r = recv(c, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void echo(int c) {
    char b[256];
    ssize_t r;
    while ((r = recv(c, b, sizeof(b), 0)) > 0) send(c, b, (size_t)r, 0);
}

static void serve_socks5(int c) {
    uint8_t h[2], methods[8], b[300];
    if (read_n(c, h, 2) || h[0] != 5 || read_n(c, methods, h[1])) return;
    const uint8_t pick = mode.need_auth ? 2 : 0;
    const uint8_t sel[2] = {5, pick};
    send(c, sel, 2, 0);
    if (pick == 2) {
        uint8_t v, l;
        if (read_n(c, &v, 1) || read_n(c, &l, 1) || read_n(c, b, l)) return;
        memcpy(rec.user, b, l);
        rec.user[l] = 0;
        if (read_n(c, &l, 1) || read_n(c, b, l)) return;
        memcpy(rec.pass, b, l);
        rec.pass[l] = 0;
        const uint8_t ok[2] = {1, 0};
        send(c, ok, 2, 0);
    }
    uint8_t q[4];
    if (read_n(c, q, 4) || q[0] != 5 || q[1] != 1) return;
    rec.atyp = q[3];
    if (q[3] == 3) {
        uint8_t l;
        read_n(c, &l, 1);
        read_n(c, b, l);
        memcpy(rec.host, b, l);
        rec.host[l] = 0;
    } else {
        read_n(c, b, 4);
        snprintf(rec.host, sizeof(rec.host), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    uint8_t pt[2];
    read_n(c, pt, 2);
    rec.port = pt[0] << 8 | pt[1];
    const uint8_t rep[10] = {5, (uint8_t)mode.refuse, 0, 1, 0, 0, 0, 0, 0, 0};
    send(c, rep, 10, 0);
    if (!mode.refuse) echo(c);
}

static void serve_http(int c) {
    char req[1024];
    size_t n = 0;
    while (n < sizeof(req) - 1) {
        char ch;
        if (recv(c, &ch, 1, 0) != 1) return;
        req[n++] = ch;
        if (n >= 4 && !memcmp(req + n - 4, "\r\n\r\n", 4)) break;
    }
    req[n] = 0;
    snprintf(rec.http_req, sizeof(rec.http_req), "%s", req);
    if (mode.need_auth && !strstr(req, "Proxy-Authorization: Basic ")) {
        const char* r = "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic\r\n\r\n";
        send(c, r, strlen(r), 0);
        return;
    }
    if (mode.refuse) {
        char r[64];
        snprintf(r, sizeof(r), "HTTP/1.1 %d No\r\n\r\n", mode.refuse);
        send(c, r, strlen(r), 0);
        return;
    }
    // 紧跟着“目标”先发来的 4 个字节：客户端读握手时一个字节也不能多读。/ Four bytes the "target" sends right after:
    // the client must not eat even one byte of them while reading the handshake.
    const char* ok = "HTTP/1.1 200 Connection established\r\nX-Test: yes\r\n\r\nSRV!";
    send(c, ok, strlen(ok), 0);
    echo(c);
}

static void* accept_loop(void* arg) {
    (void)arg;
    for (;;) {
        const int c = accept(listen_fd, NULL, NULL);
        if (c < 0) return NULL;
        rec.conns++;
        if (mode.kind == 0) serve_socks5(c);
        else serve_http(c);
        close(c);
    }
}

static void start_server(void) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(listen_fd, (struct sockaddr*)&a, sizeof(a));
    listen(listen_fd, 4);
    socklen_t l = sizeof(a);
    getsockname(listen_fd, (struct sockaddr*)&a, &l);
    listen_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, accept_loop, NULL);
    pthread_detach(t);
}

/* ---- 测试用的小工具 / Test helpers ---- */

static void use_proxy(int kind, const char* user, const char* pass, bool on) {
    tomato_proxy_t p = {.on = on, .type = (uint8_t)kind, .port = (uint16_t)listen_port};
    snprintf(p.host, sizeof(p.host), "127.0.0.1");
    snprintf(p.user, sizeof(p.user), "%s", user);
    snprintf(p.pass, sizeof(p.pass), "%s", pass);
    mode.kind = kind;
    mode.need_auth = user[0] != 0;
    mode.refuse = 0;
    memset(&rec, 0, sizeof(rec));
    CHECK(tomato_proxy_save(&p) == ESP_OK, "save");
}

// 按名字走一遍 getaddrinfo + connect，返回已连上的套接字或 -1（errno 是原因）。/ getaddrinfo plus connect for a name;
// returns the connected socket or -1 (errno has the cause).
static int dial_name(const char* name, int port) {
    char svc[8];
    snprintf(svc, sizeof(svc), "%d", port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai = NULL;
    if (__wrap_lwip_getaddrinfo(name, svc, &hints, &ai) != 0 || !ai) return -2;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    const int rc = __wrap_lwip_connect(s, ai->ai_addr, ai->ai_addrlen);
    const int e = errno;
    freeaddrinfo(ai);
    if (rc != 0) { close(s); errno = e; return -1; }
    return s;
}

static int dial_ip(const char* ip, int port, int nonblock) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (nonblock) fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    const int rc = __wrap_lwip_connect(s, (struct sockaddr*)&a, sizeof(a));
    const int e = errno;
    if (rc != 0) { close(s); errno = e; return -1; }
    return s;
}

// 直连到服务端时，connect 比服务端 accept 并计数先返回，等一会儿再看。/ A direct dial to the server returns before it
// accepts and counts, so wait a little before looking.
static bool wait_conns(int n) {
    for (int i = 0; i < 200 && rec.conns < n; i++) usleep(5000);
    return rec.conns == n;
}

static bool echoes(int s, const char* text) {
    char b[32] = {0};
    const size_t n = strlen(text);
    send(s, text, n, 0);
    return read_n(s, b, n) == 0 && !memcmp(b, text, n);
}

/* ---- 用例 / Cases ---- */

static void test_socks5(void) {
    printf("SOCKS5\n");
    use_proxy(0, "", "", true);
    int s = dial_name("example.com", 443);
    CHECK(s >= 0, "dial failed errno=%d", errno);
    CHECK(rec.atyp == 3 && !strcmp(rec.host, "example.com") && rec.port == 443, "target atyp=%d host=%s port=%d", rec.atyp, rec.host, rec.port);
    CHECK(s >= 0 && echoes(s, "ping"), "stream after handshake");
    if (s >= 0) close(s);

    use_proxy(0, "alice", "s3cret", true);
    s = dial_name("books.example.org", 8443);
    CHECK(s >= 0, "auth dial failed errno=%d", errno);
    CHECK(!strcmp(rec.user, "alice") && !strcmp(rec.pass, "s3cret"), "creds %s/%s", rec.user, rec.pass);
    CHECK(!strcmp(rec.host, "books.example.org") && rec.port == 8443, "target %s:%d", rec.host, rec.port);
    if (s >= 0) close(s);

    use_proxy(0, "", "", true);
    s = dial_ip("8.8.4.4", 443, 1);   // 非阻塞套接字，公网地址字面量 / non-blocking socket, public address literal
    CHECK(s >= 0, "ip dial failed errno=%d", errno);
    CHECK(rec.atyp == 1 && !strcmp(rec.host, "8.8.4.4") && rec.port == 443, "ip target atyp=%d host=%s", rec.atyp, rec.host);
    CHECK(s >= 0 && (fcntl(s, F_GETFL, 0) & O_NONBLOCK), "non-blocking mode restored");
    if (s >= 0) close(s);

    mode.refuse = 5;
    s = dial_name("example.com", 443);
    CHECK(s == -1 && errno == ECONNREFUSED, "refused: s=%d errno=%d", s, errno);
    mode.refuse = 4;
    s = dial_name("example.com", 443);
    CHECK(s == -1 && errno == EHOSTUNREACH, "host unreachable: s=%d errno=%d", s, errno);

    // 代理要认证，设备没填用户名：代理只给“需要认证”，必须报错而不是假装通了。
    // The proxy wants a login and none is set: it must fail rather than pretend to be through.
    tomato_proxy_t p = {.on = true, .type = 0, .port = (uint16_t)listen_port};
    snprintf(p.host, sizeof(p.host), "127.0.0.1");
    mode.refuse = 0;
    mode.need_auth = 1;
    tomato_proxy_save(&p);
    s = dial_name("example.com", 443);
    CHECK(s == -1, "no-credentials against an auth proxy must fail, s=%d", s);
}

static void test_http(void) {
    printf("HTTP CONNECT\n");
    use_proxy(1, "", "", true);
    int s = dial_name("example.com", 443);
    CHECK(s >= 0, "dial failed errno=%d", errno);
    CHECK(strstr(rec.http_req, "CONNECT example.com:443 HTTP/1.1\r\n") == rec.http_req, "request line: %.40s", rec.http_req);
    CHECK(!strstr(rec.http_req, "Proxy-Authorization"), "no auth header without a user");
    char b[8] = {0};
    CHECK(s >= 0 && read_n(s, b, 4) == 0 && !strcmp(b, "SRV!"), "bytes right after the headers were eaten: '%s'", b);
    CHECK(s >= 0 && echoes(s, "ping"), "stream after handshake");
    if (s >= 0) close(s);

    use_proxy(1, "alice", "s3cret", true);
    s = dial_name("example.com", 443);
    CHECK(s >= 0, "auth dial failed errno=%d", errno);
    // base64("alice:s3cret")
    CHECK(strstr(rec.http_req, "Proxy-Authorization: Basic YWxpY2U6czNjcmV0\r\n") != NULL, "auth header: %s", rec.http_req);
    if (s >= 0) close(s);

    use_proxy(1, "", "", true);
    mode.need_auth = 1;     // 代理要认证，设备没填 / the proxy wants a login, the device has none
    s = dial_name("example.com", 443);
    CHECK(s == -1 && errno == EACCES, "407: s=%d errno=%d", s, errno);
    mode.need_auth = 0;
    mode.refuse = 403;
    s = dial_name("example.com", 443);
    CHECK(s == -1 && errno == ECONNREFUSED, "403: s=%d errno=%d", s, errno);

    mode.refuse = 0;
    s = dial_ip("8.8.4.4", 80, 0);
    CHECK(s >= 0 && strstr(rec.http_req, "CONNECT 8.8.4.4:80 HTTP/1.1") == rec.http_req, "ip target: %.40s", rec.http_req);
    if (s >= 0) close(s);
}

static void test_bypass(void) {
    printf("Bypass\n");
    use_proxy(0, "", "", true);
    // 局域网里的真实服务（一个回声服务）：直连，代理服务端不该看到连接。/ A real LAN service (an echo): direct, and the
    // proxy server must not see a connection.
    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(lfd, (struct sockaddr*)&a, sizeof(a));
    listen(lfd, 1);
    socklen_t l = sizeof(a);
    getsockname(lfd, (struct sockaddr*)&a, &l);
    const int before = rec.conns;
    const int s = dial_ip("127.0.0.1", ntohs(a.sin_port), 0);
    CHECK(s >= 0, "direct dial failed errno=%d", errno);
    CHECK(rec.conns == before, "LAN address went through the proxy");
    if (s >= 0) close(s);
    close(lfd);

    // 局域网名字不换成假地址。/ LAN names are not swapped for fake addresses.
    const char* const lan[] = {"localhost", "nas.local", "router.lan", "printer"};
    for (size_t i = 0; i < sizeof(lan) / sizeof(lan[0]); i++) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai = NULL;
        const int rc = __wrap_lwip_getaddrinfo(lan[i], "80", &hints, &ai);
        // 解析不出来没关系（容器里没有这些名字），只要给的不是假地址。/ Failing to resolve is fine; it must just not be a fake address.
        const bool fake = rc == 0 && ai && ntohl(((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr) >> 8 == 0xC61200u;
        CHECK(!fake, "%s got a fake address", lan[i]);
        if (ai) freeaddrinfo(ai);
    }

    // 关着代理：公网地址不碰代理；UDP 套接字不走代理。/ Proxy off: public addresses leave the proxy alone; UDP sockets too.
    use_proxy(0, "", "", false);
    const int c0 = rec.conns;
    const int s2 = dial_ip("127.0.0.1", listen_port, 0);   // 直连到代理服务端自己，算一次连接 / a direct dial to the server itself counts once
    CHECK(s2 >= 0 && wait_conns(c0 + 1), "off: dial to server");
    if (s2 >= 0) close(s2);
    use_proxy(0, "", "", true);
    const int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in d = {.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, "8.8.8.8", &d.sin_addr);
    const int c1 = rec.conns;
    CHECK(__wrap_lwip_connect(u, (struct sockaddr*)&d, sizeof(d)) == 0, "UDP connect");
    CHECK(rec.conns == c1, "UDP went through the proxy");
    close(u);
}

static void test_failures(void) {
    printf("Failures\n");
    // 代理没在听：很快报拒绝。/ The proxy is not listening: refused quickly.
    tomato_proxy_t p = {.on = true, .type = 0, .port = 1};
    snprintf(p.host, sizeof(p.host), "127.0.0.1");
    tomato_proxy_save(&p);
    int s = dial_name("example.com", 443);
    CHECK(s == -1 && errno == ECONNREFUSED, "closed proxy port: s=%d errno=%d", s, errno);
    // 没分配过的假地址（槽是空的）：报不可达，不能当成真地址连出去。/ An unassigned fake address (empty slot) is
    // unreachable and must not be dialed as a real one.
    use_proxy(0, "", "", true);
    s = dial_ip("198.18.0.31", 443, 0);
    CHECK(s == -1 && errno == EHOSTUNREACH, "empty slot: s=%d errno=%d", s, errno);
}

static void test_fake_table(void) {
    printf("Fake table\n");
    use_proxy(0, "", "", true);
    // 同一个名字反复解析拿到同一个地址；填满后换最早的，旧地址不会指向错误的名字。
    // The same name resolves to the same address; once full the oldest is replaced, and no old address points at a wrong name.
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *a1 = NULL, *a2 = NULL;
    __wrap_lwip_getaddrinfo("same.example.com", "1", &hints, &a1);
    __wrap_lwip_getaddrinfo("same.example.com", "1", &hints, &a2);
    CHECK(a1 && a2 && ((struct sockaddr_in*)a1->ai_addr)->sin_addr.s_addr == ((struct sockaddr_in*)a2->ai_addr)->sin_addr.s_addr,
          "same name, different addresses");
    if (a1) freeaddrinfo(a1);
    if (a2) freeaddrinfo(a2);
    for (int i = 0; i < 40; i++) {
        char n[48];
        snprintf(n, sizeof(n), "host%d.example.com", i);
        const int s = dial_name(n, 443);
        CHECK(s >= 0 && !strcmp(rec.host, n), "round %d: server saw '%s'", i, rec.host);
        if (s >= 0) close(s);
    }
}

static void test_parse(void) {
    printf("Parse\n");
    tomato_proxy_t p = {.type = TOMATO_PROXY_SOCKS5};
    CHECK(tomato_proxy_parse("192.168.1.5:7890", &p) && !strcmp(p.host, "192.168.1.5") && p.port == 7890 && p.type == 0, "plain");
    CHECK(tomato_proxy_parse("http://proxy.lan:3128/", &p) && !strcmp(p.host, "proxy.lan") && p.port == 3128 && p.type == 1, "http://");
    CHECK(tomato_proxy_parse("socks5://10.0.0.2:1080", &p) && p.port == 1080 && p.type == 0, "socks5://");
    CHECK(tomato_proxy_parse("SOCKS5H://h:1", &p) && p.type == 0 && p.port == 1, "socks5h upper case");
    CHECK(tomato_proxy_parse("h:65535", &p) && p.port == 65535, "port max");
    p.type = TOMATO_PROXY_HTTP;
    CHECK(tomato_proxy_parse("10.0.0.9:8080", &p) && p.type == 1, "a bare address keeps the current type");
    const char* const bad[] = {"", "host", ":80", "h:0", "h:65536", "h:abc", "a:b:c", "[::1]:80", "h:80x", "bad host:80",
                               "socks5://", "h:-1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        tomato_proxy_t q = {0};
        CHECK(!tomato_proxy_parse(bad[i], &q), "accepted '%s'", bad[i]);
    }
    char longhost[200];
    memset(longhost, 'a', sizeof(longhost));
    snprintf(longhost + 70, sizeof(longhost) - 70, ":80");
    tomato_proxy_t q = {0};
    CHECK(!tomato_proxy_parse(longhost, &q), "accepted an over-long host");
}

int main(void) {
    start_server();
    test_parse();
    test_failures();    // 先测空槽：这时假地址表还是空的 / empty slots first, while the table is still empty
    test_socks5();
    test_http();
    test_bypass();
    test_fake_table();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
