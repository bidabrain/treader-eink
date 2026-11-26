/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 局域网代理（SOCKS5 / HTTP CONNECT）。链接时拦截 lwIP 的 connect 和 getaddrinfo：所有走 esp-tls 的流量
 * （esp_http_client、WebDAV、升级……）不改调用方就经代理出去。要走代理的域名拿到一个假地址（198.18.0.x），connect 时
 * 换回域名交给代理解析；TLS 仍在设备和目标之间端到端，代理只转发密文。代理的那条连接就是传数据的那条，不多占连接。
 *
 * English: LAN proxy (SOCKS5 / HTTP CONNECT). lwIP's connect and getaddrinfo are intercepted at link time, so everything
 * going through esp-tls (esp_http_client, WebDAV, OTA, ...) leaves via the proxy without touching the callers. A name
 * that must go through the proxy gets a fake address (198.18.0.x) and connect swaps the name back in for the proxy to
 * resolve; TLS stays end to end between the device and the target and the proxy only relays ciphertext. The connection
 * to the proxy is the one that carries the data, so no extra connection is held.
 *
 * 冻结：局域网（私网、回环、本地链路地址，.local / .lan 和不带点的名字）永远直连；UDP（校时、DNS）不走代理；握手最多
 * 阻塞 PX_TIMEOUT_MS；日志不写用户名和密码；握手缓冲在栈上用完擦掉。
 * Frozen: the LAN (private, loopback and link-local addresses, .local / .lan and dotless names) always goes direct; UDP
 * (time sync, DNS) never uses the proxy; the handshake blocks at most PX_TIMEOUT_MS; logs never carry the user name or
 * password; handshake buffers live on the stack and are wiped after use.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "tomato.h"

#define TAG "tomato_proxy"
#define NS "proxy"
#define PX_TIMEOUT_MS 8000          // 连代理和握手各自的上限 / Limit for reaching the proxy and for the handshake
#define FAKE_N 32                   // 假地址表容量，满了从最早的开始换 / Fake address slots; the oldest is replaced when full
#define FAKE_BASE 0xC6120000u       // 198.18.0.0，基准测试专用段，不会是真地址 / Benchmark range, never a real address
#define NAME_MAX_LEN 64

// 由 CMakeLists.txt 的 -Wl,--wrap=lwip_connect 和 --wrap=lwip_getaddrinfo 接上。
// Wired up by -Wl,--wrap=lwip_connect and --wrap=lwip_getaddrinfo in CMakeLists.txt.
int __real_lwip_connect(int s, const struct sockaddr* name, socklen_t namelen);
int __real_lwip_getaddrinfo(const char* nodename, const char* servname, const struct addrinfo* hints,
                            struct addrinfo** res);

/* ---- 设置 / Settings ---- */

static tomato_proxy_t s_px;
static bool s_px_loaded;
static portMUX_TYPE s_px_lock = portMUX_INITIALIZER_UNLOCKED;

static void px_read(tomato_proxy_t* out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "on", &v) == ESP_OK) out->on = v != 0;
    if (nvs_get_u8(h, "type", &v) == ESP_OK) out->type = v == TOMATO_PROXY_HTTP ? TOMATO_PROXY_HTTP : TOMATO_PROXY_SOCKS5;
    nvs_get_u16(h, "port", &out->port);
    size_t n = sizeof(out->host);
    if (nvs_get_str(h, "host", out->host, &n) != ESP_OK) out->host[0] = 0;
    n = sizeof(out->user);
    if (nvs_get_str(h, "user", out->user, &n) != ESP_OK) out->user[0] = 0;
    n = sizeof(out->pass);
    if (nvs_get_str(h, "pass", out->pass, &n) != ESP_OK) out->pass[0] = 0;
    nvs_close(h);
}

static void px_snapshot(tomato_proxy_t* out) {
    if (!s_px_loaded) {
        tomato_proxy_t t;
        px_read(&t);
        portENTER_CRITICAL(&s_px_lock);
        if (!s_px_loaded) { s_px = t; s_px_loaded = true; }
        portEXIT_CRITICAL(&s_px_lock);
        memset(&t, 0, sizeof(t));
    }
    portENTER_CRITICAL(&s_px_lock);
    *out = s_px;
    portEXIT_CRITICAL(&s_px_lock);
}

// 只问开没开，不把密码拷出来。/ Ask only whether it is on, without copying the password out.
static bool px_active(void) {
    if (!s_px_loaded) {
        tomato_proxy_t t;
        px_snapshot(&t);
        memset(&t, 0, sizeof(t));
    }
    portENTER_CRITICAL(&s_px_lock);
    const bool on = s_px.on && s_px.host[0] && s_px.port;
    portEXIT_CRITICAL(&s_px_lock);
    return on;
}

esp_err_t tomato_proxy_load(tomato_proxy_t* out) {
    px_snapshot(out);
    return ESP_OK;
}

esp_err_t tomato_proxy_save(const tomato_proxy_t* cfg) {
    if (strlen(cfg->host) >= sizeof(cfg->host) || strlen(cfg->user) >= sizeof(cfg->user) ||
        strlen(cfg->pass) >= sizeof(cfg->pass))
        return ESP_ERR_INVALID_SIZE;
    if (cfg->on && (!cfg->host[0] || !cfg->port)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "on", cfg->on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "type", cfg->type == TOMATO_PROXY_HTTP ? TOMATO_PROXY_HTTP : TOMATO_PROXY_SOCKS5);
    if (err == ESP_OK) err = nvs_set_u16(h, "port", cfg->port);
    if (err == ESP_OK) err = nvs_set_str(h, "host", cfg->host);
    if (err == ESP_OK) err = nvs_set_str(h, "user", cfg->user);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", cfg->pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_px_lock);
        s_px = *cfg;
        s_px_loaded = true;
        portEXIT_CRITICAL(&s_px_lock);
    }
    return err;
}

bool tomato_proxy_parse(const char* text, tomato_proxy_t* cfg) {
    if (!text) return false;
    while (*text == ' ') text++;
    uint8_t type = cfg->type;
    if (!strncasecmp(text, "socks5h://", 10)) { type = TOMATO_PROXY_SOCKS5; text += 10; }
    else if (!strncasecmp(text, "socks5://", 9)) { type = TOMATO_PROXY_SOCKS5; text += 9; }
    else if (!strncasecmp(text, "http://", 7)) { type = TOMATO_PROXY_HTTP; text += 7; }
    const char* colon = strrchr(text, ':');
    if (!colon || colon == text || strchr(text, ':') != colon) return false;   // 只认 IPv4 / 主机名加端口 / IPv4 or host name plus port only
    size_t host_len = (size_t)(colon - text);
    if (host_len >= sizeof(cfg->host)) return false;
    char* end = NULL;
    const long port = strtol(colon + 1, &end, 10);
    while (end && *end == '/') end++;      // 容忍末尾的斜杠 / Tolerate a trailing slash
    if (!end || *end || port < 1 || port > 65535) return false;
    for (size_t i = 0; i < host_len; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (!isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    memcpy(cfg->host, text, host_len);
    cfg->host[host_len] = 0;
    cfg->port = (uint16_t)port;
    cfg->type = type;
    return true;
}

/* ---- 谁走代理 / Who goes through the proxy ---- */

static bool ip_is_lan(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 24) == 0 || (ip >> 16) == 0xC0A8 ||   // 10/8 127/8 0/8 192.168/16
           (ip >> 20) == 0xAC1 || (ip >> 16) == 0xA9FE;                                       // 172.16/12 169.254/16
}

static bool name_is_lan(const char* host) {
    if (!strchr(host, '.')) return true;   // 不带点：局域网里的短名、localhost / Dotless: LAN short names, localhost
    static const char* const TAILS[] = {".local", ".lan", ".home.arpa", ".internal"};
    const size_t n = strlen(host);
    for (size_t i = 0; i < sizeof(TAILS) / sizeof(TAILS[0]); i++) {
        const size_t t = strlen(TAILS[i]);
        if (n > t && !strcasecmp(host + n - t, TAILS[i])) return true;
    }
    return false;
}

/* ---- 假地址表 / Fake address table ---- */

static char s_fake[FAKE_N][NAME_MAX_LEN];
static uint8_t s_fake_next;
static portMUX_TYPE s_fake_lock = portMUX_INITIALIZER_UNLOCKED;

// 名字对应的槽号 1..FAKE_N，没有就占一个。/ The slot 1..FAKE_N for a name, taking one when absent.
static int fake_slot(const char* host) {
    int slot = 0;
    portENTER_CRITICAL(&s_fake_lock);
    for (int i = 0; i < FAKE_N && !slot; i++)
        if (s_fake[i][0] && !strcasecmp(s_fake[i], host)) slot = i + 1;
    if (!slot) {
        snprintf(s_fake[s_fake_next], NAME_MAX_LEN, "%s", host);
        slot = s_fake_next + 1;
        s_fake_next = (uint8_t)((s_fake_next + 1) % FAKE_N);
    }
    portEXIT_CRITICAL(&s_fake_lock);
    return slot;
}

// 0：不是假地址；1：是，out 是名字；-1：是假地址但槽是空的。/ 0: not fake; 1: fake, out holds the name; -1: fake but empty.
static int fake_name(uint32_t ip, char* out, size_t cap) {
    if ((ip & 0xFFFFFF00u) != FAKE_BASE) return 0;
    const uint32_t slot = ip & 0xFFu;
    if (slot < 1 || slot > FAKE_N) return 0;
    int ok = 0;
    portENTER_CRITICAL(&s_fake_lock);
    if (s_fake[slot - 1][0]) {
        snprintf(out, cap, "%s", s_fake[slot - 1]);
        ok = 1;
    }
    portEXIT_CRITICAL(&s_fake_lock);
    return ok ? 1 : -1;
}

/* ---- 握手 / Handshake ---- */

static bool send_all(int s, const uint8_t* data, size_t n) {
    while (n) {
        const int w = lwip_send(s, data, n, 0);
        if (w <= 0) return false;
        data += w;
        n -= (size_t)w;
    }
    return true;
}

static bool recv_all(int s, uint8_t* data, size_t n) {
    while (n) {
        const int r = lwip_recv(s, data, n, 0);
        if (r <= 0) return false;
        data += r;
        n -= (size_t)r;
    }
    return true;
}

static bool skip_bytes(int s, size_t n) {
    uint8_t b[16];
    while (n) {
        const size_t k = n < sizeof(b) ? n : sizeof(b);
        if (!recv_all(s, b, k)) return false;
        n -= k;
    }
    return true;
}

// 目标写成“名字:端口”（没名字就是点分 IPv4）。/ The target as "name:port" (dotted IPv4 when there is no name).
static void target_text(char* out, size_t cap, const char* host, uint32_t ip, uint16_t port) {
    if (host) snprintf(out, cap, "%s:%u", host, (unsigned)port);
    else snprintf(out, cap, "%u.%u.%u.%u:%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255),
                  (unsigned)(ip & 255), (unsigned)port);
}

static int socks5(int s, const tomato_proxy_t* p, const char* host, uint32_t ip, uint16_t port) {
    uint8_t b[300];
    int rc = EPROTO;
    const bool auth = p->user[0] != 0;
    size_t n = 0;
    b[n++] = 5;
    if (auth) { b[n++] = 2; b[n++] = 0; b[n++] = 2; }
    else { b[n++] = 1; b[n++] = 0; }
    if (!send_all(s, b, n) || !recv_all(s, b, 2) || b[0] != 5) { rc = ECONNRESET; goto out; }
    if (b[1] == 2) {
        if (!auth) { rc = EACCES; goto out; }
        const size_t ul = strlen(p->user), pl = strlen(p->pass);
        n = 0;
        b[n++] = 1;
        b[n++] = (uint8_t)ul;
        memcpy(b + n, p->user, ul);
        n += ul;
        b[n++] = (uint8_t)pl;
        memcpy(b + n, p->pass, pl);
        n += pl;
        if (!send_all(s, b, n) || !recv_all(s, b, 2) || b[1] != 0) { rc = EACCES; goto out; }
    } else if (b[1] != 0) {
        rc = EACCES;   // 代理不接受我们给的认证方式 / The proxy accepts none of our methods
        goto out;
    }
    n = 0;
    b[n++] = 5; b[n++] = 1; b[n++] = 0;
    if (host) {
        const size_t hl = strlen(host);
        b[n++] = 3;
        b[n++] = (uint8_t)hl;
        memcpy(b + n, host, hl);
        n += hl;
    } else {
        b[n++] = 1;
        b[n++] = (uint8_t)(ip >> 24); b[n++] = (uint8_t)(ip >> 16); b[n++] = (uint8_t)(ip >> 8); b[n++] = (uint8_t)ip;
    }
    b[n++] = (uint8_t)(port >> 8);
    b[n++] = (uint8_t)port;
    if (!send_all(s, b, n) || !recv_all(s, b, 4) || b[0] != 5) { rc = ECONNRESET; goto out; }
    if (b[1] != 0) {
        ESP_LOGW(TAG, "socks5 refused, code %u", (unsigned)b[1]);
        rc = b[1] == 5 ? ECONNREFUSED : b[1] == 4 ? EHOSTUNREACH : b[1] == 3 ? ENETUNREACH : ECONNREFUSED;
        goto out;
    }
    // 回复里的绑定地址用不着，读掉。/ The bound address in the reply is not needed; read it off.
    if (b[3] == 1) rc = skip_bytes(s, 4 + 2) ? 0 : ECONNRESET;
    else if (b[3] == 4) rc = skip_bytes(s, 16 + 2) ? 0 : ECONNRESET;
    else if (b[3] == 3) {
        uint8_t l;
        rc = recv_all(s, &l, 1) && skip_bytes(s, (size_t)l + 2) ? 0 : ECONNRESET;
    } else rc = EPROTO;
out:
    memset(b, 0, sizeof(b));
    return rc;
}

static int http_connect(int s, const tomato_proxy_t* p, const char* host, uint32_t ip, uint16_t port) {
    char hp[NAME_MAX_LEN + 8];
    char req[420];
    target_text(hp, sizeof(hp), host, ip, port);
    int n = snprintf(req, sizeof(req), "CONNECT %s HTTP/1.1\r\nHost: %s\r\n", hp, hp);
    if (p->user[0]) {
        char cred[110];
        unsigned char b64[160];
        size_t ol = 0;
        snprintf(cred, sizeof(cred), "%s:%s", p->user, p->pass);
        mbedtls_base64_encode(b64, sizeof(b64), &ol, (const unsigned char*)cred, strlen(cred));
        n += snprintf(req + n, sizeof(req) - (size_t)n, "Proxy-Authorization: Basic %.*s\r\n", (int)ol, (const char*)b64);
        memset(cred, 0, sizeof(cred));
        memset(b64, 0, sizeof(b64));
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
    int rc = ECONNRESET;
    if (n <= 0 || n >= (int)sizeof(req) || !send_all(s, (const uint8_t*)req, (size_t)n)) goto out;
    // 一个字节一个字节读到空行，不会多读到之后的 TLS 数据。/ Read byte by byte up to the blank line so no TLS data after it is eaten.
    char status[16] = {0};
    uint32_t tail = 0;
    for (int total = 0; total < 2048; total++) {
        char c;
        if (lwip_recv(s, &c, 1, 0) != 1) goto out;
        if (total < (int)sizeof(status) - 1) status[total] = c;
        tail = (tail << 8) | (uint8_t)c;
        if (tail == 0x0D0A0D0Au) {
            // “HTTP/1.1 200 …”：2xx 才算通。/ "HTTP/1.1 200 ...": only 2xx is through.
            if (strlen(status) >= 12 && status[9] == '2') { rc = 0; }
            else {
                ESP_LOGW(TAG, "http proxy refused: %.12s", status);
                rc = !strncmp(status + 9, "407", 3) ? EACCES : ECONNREFUSED;
            }
            goto out;
        }
    }
out:
    memset(req, 0, sizeof(req));
    return rc;
}

// 非阻塞连代理，最多 PX_TIMEOUT_MS。/ Connect to the proxy without blocking, at most PX_TIMEOUT_MS.
static int connect_proxy(int s, const tomato_proxy_t* p) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo* ai = NULL;
    // 代理自己的名字直接解析，不能经过假地址。/ The proxy's own name resolves directly, never through a fake address.
    if (__real_lwip_getaddrinfo(p->host, NULL, &hints, &ai) != 0 || !ai) return EHOSTUNREACH;
    struct sockaddr_in addr = *(struct sockaddr_in*)ai->ai_addr;
    lwip_freeaddrinfo(ai);
    addr.sin_port = htons(p->port);
    const int flags = lwip_fcntl(s, F_GETFL, 0);
    lwip_fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int rc = 0;
    if (__real_lwip_connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        rc = errno ? errno : ECONNREFUSED;
    } else {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {.tv_sec = PX_TIMEOUT_MS / 1000, .tv_usec = 0};
        if (lwip_select(s + 1, NULL, &w, NULL, &tv) <= 0) {
            rc = ETIMEDOUT;
        } else {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            rc = soerr;
        }
    }
    lwip_fcntl(s, F_SETFL, flags);
    return rc;
}

static int tunnel(int s, const tomato_proxy_t* p, const char* host, uint32_t ip, uint16_t port) {
    const int flags = lwip_fcntl(s, F_GETFL, 0);
    struct timeval old_rcv = {0}, old_snd = {0};
    socklen_t tl = sizeof(old_rcv);
    lwip_getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &old_rcv, &tl);
    tl = sizeof(old_snd);
    lwip_getsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &old_snd, &tl);
    int rc = connect_proxy(s, p);
    if (rc == 0) {
        // 握手用阻塞方式加超时，做完恢复原样。/ The handshake runs blocking with a timeout; everything is restored after.
        const struct timeval tv = {.tv_sec = PX_TIMEOUT_MS / 1000, .tv_usec = 0};
        lwip_fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
        lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        rc = p->type == TOMATO_PROXY_HTTP ? http_connect(s, p, host, ip, port) : socks5(s, p, host, ip, port);
        lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &old_rcv, sizeof(old_rcv));
        lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &old_snd, sizeof(old_snd));
        lwip_fcntl(s, F_SETFL, flags);
    }
    return rc;
}

/* ---- 链接期拦截 / Link-time wrappers ---- */

int __wrap_lwip_getaddrinfo(const char* nodename, const char* servname, const struct addrinfo* hints,
                            struct addrinfo** res) {
    struct in_addr literal;
    if (!nodename || !px_active() || strchr(nodename, ':') || inet_pton(AF_INET, nodename, &literal) == 1 ||
        name_is_lan(nodename) || strlen(nodename) >= NAME_MAX_LEN)
        return __real_lwip_getaddrinfo(nodename, servname, hints, res);
    char fake[16];
    snprintf(fake, sizeof(fake), "198.18.0.%d", fake_slot(nodename));
    return __real_lwip_getaddrinfo(fake, servname, hints, res);
}

int __wrap_lwip_connect(int s, const struct sockaddr* name, socklen_t namelen) {
    if (!name || name->sa_family != AF_INET || namelen < sizeof(struct sockaddr_in) || !px_active())
        return __real_lwip_connect(s, name, namelen);
    int type = 0;
    socklen_t tl = sizeof(type);
    if (lwip_getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM)
        return __real_lwip_connect(s, name, namelen);
    const struct sockaddr_in* sin = (const struct sockaddr_in*)name;
    const uint32_t ip = ntohl(sin->sin_addr.s_addr);
    const uint16_t port = ntohs(sin->sin_port);
    char host[NAME_MAX_LEN];
    const int fake = fake_name(ip, host, sizeof(host));
    if (fake < 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (!fake && ip_is_lan(ip)) return __real_lwip_connect(s, name, namelen);
    tomato_proxy_t p;
    px_snapshot(&p);
    const int rc = tunnel(s, &p, fake ? host : NULL, ip, port);
    memset(&p, 0, sizeof(p));
    if (rc) {
        ESP_LOGW(TAG, "proxy %s: %s", fake ? host : "ip", strerror(rc));
        errno = rc;
        return -1;
    }
    return 0;
}
