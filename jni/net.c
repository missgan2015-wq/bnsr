/*
 * Net 实现：纯 BSD socket，零外部依赖。
 *
 * 关键技术点：
 *   1) 连接超时：socket 设 O_NONBLOCK -> connect -> select 等可写 -> 取 SO_ERROR
 *      回到阻塞模式，让后续 send/recv 能正常用（recv/send 超时另设 SO_RCVTIMEO/SO_SNDTIMEO）。
 *   2) DNS：getaddrinfo + AF_INET（IPv4 only，避免 IPv6 在国内某些 ROM 上的奇怪解析延迟）。
 *   3) HTTP：状态行 + 头部用 \r\n\r\n 分隔，body 按 Content-Length 或 chunked 解析。
 *   4) HTTPS fallback：fork+exec /system/bin/curl，避免引 OpenSSL/mbedTLS 多链一份 .so。
 *
 * 错误日志：所有错误 fprintf(stderr) 带 "[easylua-c] error:" 前缀，
 * 与 easylua.c 中 EL_ERR 风格保持一致，方便 ScriptRunner 日志收敛。
 */

#include "net.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <sys/wait.h>

#define NET_ERR(fmt, ...) do { \
    fprintf(stderr, "[easylua-c] error: Net: " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)

/* ---------- 内部工具 ---------- */

/* 把 host(域名/IP) 解析成第一个 IPv4 sockaddr_in。
 * 成功返回 0；失败 -1（已打错日志）。 */
static int resolve_ipv4(const char *host, int port, struct sockaddr_in *sin)
{
    if (!host || !*host) {
        NET_ERR("resolve: host is null/empty");
        return -1;
    }
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port   = htons((uint16_t)port);

    /* 先试纯数字 IPv4，避免对每次连接都跑 DNS */
    if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) return 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        NET_ERR("DNS '%s' failed: %s", host, gai_strerror(rc));
        if (res) freeaddrinfo(res);
        return -1;
    }
    struct sockaddr_in *got = (struct sockaddr_in *)res->ai_addr;
    sin->sin_addr = got->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* 临时把 fd 改成阻塞 / 非阻塞 */
static int set_blocking(int fd, int blocking)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (blocking) fl &= ~O_NONBLOCK;
    else          fl |=  O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
}

/* 把毫秒拆成 timeval；ms <=0 返回 NULL（select 等永久） */
static struct timeval *fill_tv(struct timeval *tv, int ms)
{
    if (ms <= 0) return NULL;
    tv->tv_sec  = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return tv;
}

/* ---------- TCP ---------- */

int Net_TcpConnect(const char *host, int port, int timeout_ms)
{
    struct sockaddr_in sin;
    if (resolve_ipv4(host, port, &sin) != 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { NET_ERR("socket: %s", strerror(errno)); return -1; }

    /* 走非阻塞 connect 才能控制超时；超时不设就直接阻塞 connect */
    if (timeout_ms > 0) {
        if (set_blocking(fd, 0) != 0) {
            NET_ERR("set_blocking: %s", strerror(errno));
            close(fd);
            return -1;
        }
        int rc = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
        if (rc != 0 && errno != EINPROGRESS) {
            NET_ERR("connect %s:%d: %s", host, port, strerror(errno));
            close(fd);
            return -1;
        }
        if (rc != 0) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv;
            int sel = select(fd + 1, NULL, &wfds, NULL, fill_tv(&tv, timeout_ms));
            if (sel <= 0) {
                NET_ERR("connect timeout %s:%d (%dms)", host, port, timeout_ms);
                close(fd);
                return -1;
            }
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
                NET_ERR("connect %s:%d failed: %s", host, port,
                        strerror(err ? err : errno));
                close(fd);
                return -1;
            }
        }
        /* 回阻塞模式，方便后面 send/recv */
        set_blocking(fd, 1);
    } else {
        if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
            NET_ERR("connect %s:%d: %s", host, port, strerror(errno));
            close(fd);
            return -1;
        }
    }
    return fd;
}

int Net_TcpSend(int fd, const char *buf, int len, int timeout_ms)
{
    if (fd < 0 || !buf || len < 0) {
        NET_ERR("Send: bad args fd=%d buf=%p len=%d", fd, (void *)buf, len);
        return -1;
    }
    if (len == 0) return 0;
    if (timeout_ms > 0) {
        struct timeval tv;
        fill_tv(&tv, timeout_ms);
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
            /* 不致命，继续尝试 */
            NET_ERR("set SO_SNDTIMEO: %s", strerror(errno));
        }
    }
    int sent = 0;
    while (sent < len) {
        int n = (int)send(fd, buf + sent, (size_t)(len - sent), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            NET_ERR("send: %s (sent=%d/%d)", strerror(errno), sent, len);
            return -1;
        }
        if (n == 0) break;
        sent += n;
    }
    return sent;
}

int Net_TcpRecv(int fd, char *buf, int cap, int timeout_ms)
{
    if (fd < 0 || !buf || cap <= 0) {
        NET_ERR("Recv: bad args fd=%d buf=%p cap=%d", fd, (void *)buf, cap);
        return -1;
    }
    if (timeout_ms > 0) {
        struct timeval tv;
        fill_tv(&tv, timeout_ms);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            NET_ERR("set SO_RCVTIMEO: %s", strerror(errno));
        }
    }
    while (1) {
        int n = (int)recv(fd, buf, (size_t)cap, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* SO_RCVTIMEO 触发：EAGAIN/EWOULDBLOCK，静默 */
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                NET_ERR("recv: %s", strerror(errno));
            }
            return -1;
        }
        return n;
    }
}

int Net_TcpSetNoDelay(int fd, int on)
{
    if (fd < 0) return -1;
    int flag = on ? 1 : 0;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
        NET_ERR("TCP_NODELAY: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------- UDP ---------- */

int Net_UdpOpen(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { NET_ERR("udp socket: %s", strerror(errno)); return -1; }
    return fd;
}

int Net_UdpSendTo(int fd, const char *host, int port, const char *buf, int len)
{
    if (fd < 0 || !buf || len < 0) {
        NET_ERR("UdpSendTo: bad args");
        return -1;
    }
    struct sockaddr_in sin;
    if (resolve_ipv4(host, port, &sin) != 0) return -1;
    int n = (int)sendto(fd, buf, (size_t)len, 0,
                        (struct sockaddr *)&sin, sizeof(sin));
    if (n < 0) NET_ERR("sendto %s:%d: %s", host, port, strerror(errno));
    return n;
}

int Net_UdpRecvFrom(int fd, char *buf, int cap, int timeout_ms,
                    char *out_host, int out_n, int *out_port)
{
    if (fd < 0 || !buf || cap <= 0) {
        NET_ERR("UdpRecvFrom: bad args");
        return -1;
    }
    if (timeout_ms > 0) {
        struct timeval tv;
        fill_tv(&tv, timeout_ms);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    int n = (int)recvfrom(fd, buf, (size_t)cap, 0,
                          (struct sockaddr *)&src, &sl);
    if (n < 0) {
        /* EAGAIN/EWOULDBLOCK = SO_RCVTIMEO 触发，是预期路径，静默 */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            NET_ERR("recvfrom: %s", strerror(errno));
        }
        return -1;
    }
    if (out_host && out_n > 0) {
        inet_ntop(AF_INET, &src.sin_addr, out_host, (socklen_t)out_n);
    }
    if (out_port) *out_port = ntohs(src.sin_port);
    return n;
}

/* ---------- 通用 ---------- */

void Net_Close(int fd)
{
    if (fd >= 0) close(fd);
}

int Net_SetTimeout(int fd, int recv_ms, int send_ms)
{
    if (fd < 0) return -1;
    struct timeval tv;
    if (recv_ms >= 0) {
        fill_tv(&tv, recv_ms);
        if (recv_ms == 0) { tv.tv_sec = 0; tv.tv_usec = 0; }
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            NET_ERR("SO_RCVTIMEO: %s", strerror(errno));
            return -1;
        }
    }
    if (send_ms >= 0) {
        fill_tv(&tv, send_ms);
        if (send_ms == 0) { tv.tv_sec = 0; tv.tv_usec = 0; }
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
            NET_ERR("SO_SNDTIMEO: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* ---------- DNS / 本机 IP ---------- */

int Net_DnsResolve(const char *host, char *out, int n)
{
    if (!host || !*host || !out || n <= 0) {
        NET_ERR("DnsResolve: bad args");
        return -1;
    }
    struct sockaddr_in sin;
    if (resolve_ipv4(host, 0, &sin) != 0) return -1;
    if (!inet_ntop(AF_INET, &sin.sin_addr, out, (socklen_t)n)) {
        NET_ERR("inet_ntop: %s", strerror(errno));
        return -1;
    }
    return (int)strlen(out);
}

/* 候选接口名优先级（越靠前越优先）。
 * wlan0 = WiFi；rmnet0/rmnet_data0 = 移动数据；usb0 = USB tether；eth0 = 模拟器 */
static const char *kIfacePref[] = {
    "wlan0", "rmnet_data0", "rmnet0", "ccmni0", "usb0", "eth0", NULL,
};

int Net_LocalIp(char *out, int n)
{
    if (!out || n <= 0) {
        NET_ERR("LocalIp: bad args");
        return -1;
    }
    out[0] = 0;

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) {
        NET_ERR("getifaddrs: %s", strerror(errno));
        return -1;
    }

    char best[INET_ADDRSTRLEN] = {0};
    int  best_rank = 9999;

    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        /* 跳过 loopback */
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if ((a >> 24) == 127) continue;

        int rank = 100;  /* 未知接口默认排很后 */
        for (int i = 0; kIfacePref[i]; i++) {
            if (p->ifa_name && strcmp(p->ifa_name, kIfacePref[i]) == 0) {
                rank = i;
                break;
            }
        }
        if (rank < best_rank) {
            best_rank = rank;
            inet_ntop(AF_INET, &sin->sin_addr, best, sizeof(best));
        }
    }
    freeifaddrs(ifa);

    if (!best[0]) return -1;
    int len = (int)strlen(best);
    int copy = len < n - 1 ? len : n - 1;
    memcpy(out, best, (size_t)copy);
    out[copy] = 0;
    return copy;
}

/* ---------- HTTP（高阶） ---------- */

/* URL 拆分：支持 http://host[:port][/path] 和 https://...
 * 解析后写到 caller 的 buffer，不分配内存。 */
static int parse_url(const char *url,
                     int *is_https,
                     char *host, int host_n,
                     int *port,
                     char *path, int path_n)
{
    if (!url) return -1;
    *is_https = 0;
    *port = 80;
    if (host_n > 0) host[0] = 0;
    if (path_n > 0) { path[0] = '/'; path[1] = 0; }

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)       { p += 7; *is_https = 0; *port = 80; }
    else if (strncmp(p, "https://", 8) == 0) { p += 8; *is_https = 1; *port = 443; }
    else { NET_ERR("HttpRequest: only http/https supported, got '%s'", url); return -1; }

    /* host 段：到 '/' 或 '?' 或 '#' 或 ':' 截止 */
    const char *h = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') p++;
    int hl = (int)(p - h);
    if (hl <= 0) { NET_ERR("HttpRequest: empty host in '%s'", url); return -1; }
    if (hl >= host_n) hl = host_n - 1;
    memcpy(host, h, (size_t)hl);
    host[hl] = 0;

    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p && (*p >= '0' && *p <= '9')) p++;
    }

    /* path = 剩余部分；空就用 "/" */
    if (*p) {
        int pl = (int)strlen(p);
        if (pl >= path_n) pl = path_n - 1;
        memcpy(path, p, (size_t)pl);
        path[pl] = 0;
    } else {
        if (path_n > 0) { path[0] = '/'; path[1] = 0; }
    }
    return 0;
}

/* 大小写不敏感 strstr，限定字节数 */
static const char *case_strstr_n(const char *hay, int hl, const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= hl; i++) {
        if (strncasecmp(hay + i, needle, (size_t)nl) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/* 解析 chunked 编码。src/srclen 是从 body 起的所有 raw 数据；
 * 把解码后的字节写到 dst（最多 dstcap）。返回写入字节数；坏帧返回 -1。 */
static int decode_chunked(const char *src, int srclen, char *dst, int dstcap)
{
    int si = 0, di = 0;
    while (si < srclen) {
        /* 读 chunk-size 行（hex + CRLF） */
        int line_start = si;
        while (si < srclen - 1 && !(src[si] == '\r' && src[si + 1] == '\n')) si++;
        if (si >= srclen - 1) return -1;
        char hex[20];
        int hlen = si - line_start;
        if (hlen <= 0 || hlen >= (int)sizeof(hex)) return -1;
        memcpy(hex, src + line_start, (size_t)hlen);
        hex[hlen] = 0;
        /* 忽略 chunk extension（"; ext"），strtol 自动停在分号 */
        long sz = strtol(hex, NULL, 16);
        si += 2;  /* skip CRLF */
        if (sz <= 0) break;  /* 0 chunk = end of body */
        if (si + sz > srclen) return -1;
        int copy = (int)sz;
        if (di + copy > dstcap) copy = dstcap - di;
        if (copy > 0) {
            memcpy(dst + di, src + si, (size_t)copy);
            di += copy;
        }
        si += (int)sz;
        /* skip trailing CRLF */
        if (si + 2 <= srclen && src[si] == '\r' && src[si + 1] == '\n') si += 2;
    }
    return di;
}

/* HTTPS 走 curl 子进程兜底。返回写入 out_buf 的字节数。 */
static int http_via_curl(const char *method, const char *url,
                         const char *headers,
                         const char *body, int body_len,
                         char *out_buf, int out_cap,
                         int *out_status, int timeout_ms)
{
    /* 用 curl 的 -w "\nEASYLUA_STATUS=%{http_code}\n" 把状态码写在末尾，
     * 之后我们 strstr 切掉它。-s 静默 -L 跟随 30x -k 不校验证书（root 场景常见自签）。
     * body 通过 stdin 传入（用 --data-binary @-），避免 shell 转义。 */
    int p[2];                  /* parent <- child stdout */
    int q[2];                  /* parent -> child stdin  */
    if (pipe(p) != 0 || pipe(q) != 0) {
        NET_ERR("pipe: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        NET_ERR("fork: %s", strerror(errno));
        close(p[0]); close(p[1]); close(q[0]); close(q[1]);
        return -1;
    }
    if (pid == 0) {
        /* child */
        dup2(q[0], STDIN_FILENO);
        dup2(p[1], STDOUT_FILENO);
        /* stderr 关掉，免得 curl 的进度污染输出 */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(p[0]); close(p[1]); close(q[0]); close(q[1]);

        /* 拼参数：curl -sSL -k -X METHOD -H ... --data-binary @- url -w ...
         * argv 容量按需估：基础 12 + 每行 header 2 + 1 body 标志 = 30 上限够用。 */
        const int MAX_ARGS = 64;
        char *argv[MAX_ARGS];
        int argc = 0;
        argv[argc++] = (char *)"curl";
        argv[argc++] = (char *)"-sS";          /* 静默但保留错误 */
        argv[argc++] = (char *)"-L";           /* 跟随 30x */
        argv[argc++] = (char *)"-k";           /* 不校验证书（root 场景常见自签） */
        argv[argc++] = (char *)"-X";
        argv[argc++] = (char *)(method ? method : "GET");

        if (timeout_ms > 0) {
            static char tbuf[24];
            snprintf(tbuf, sizeof(tbuf), "%d", (timeout_ms + 999) / 1000);
            argv[argc++] = (char *)"--max-time";
            argv[argc++] = tbuf;
        }

        /* 切 headers，每行一个 -H */
        if (headers && *headers) {
            static char hbuf[8192];
            strncpy(hbuf, headers, sizeof(hbuf) - 1);
            hbuf[sizeof(hbuf) - 1] = 0;
            char *line = hbuf;
            while (line && *line && argc < MAX_ARGS - 6) {
                char *eol = strpbrk(line, "\r\n");
                if (eol) { *eol = 0; eol++; while (*eol == '\r' || *eol == '\n') eol++; }
                if (*line) {
                    argv[argc++] = (char *)"-H";
                    argv[argc++] = line;
                }
                line = eol;
            }
        }

        if (body && body_len > 0) {
            argv[argc++] = (char *)"--data-binary";
            argv[argc++] = (char *)"@-";
        } else {
            /* 关闭 stdin，免得 curl 误判 */
        }

        argv[argc++] = (char *)"-o";
        argv[argc++] = (char *)"-";  /* body 写 stdout */
        argv[argc++] = (char *)"-w";
        argv[argc++] = (char *)"\n__EASYLUA_STATUS__=%{http_code}\n";
        argv[argc++] = (char *)url;
        argv[argc] = NULL;

        execv("/system/bin/curl", argv);
        _exit(127);
    }

    /* parent */
    close(p[1]);
    close(q[0]);

    /* 写 body 到 child stdin（如果没 body 就直接关） */
    if (body && body_len > 0) {
        int w = 0;
        while (w < body_len) {
            int n = (int)write(q[1], body + w, (size_t)(body_len - w));
            if (n <= 0) break;
            w += n;
        }
    }
    close(q[1]);

    /* 读 child stdout */
    int total = 0;
    /* +96 给状态码后缀留空间 */
    int real_cap = out_cap + 96;
    char *tmp = (char *)malloc((size_t)real_cap);
    if (!tmp) {
        NET_ERR("oom %d", real_cap);
        close(p[0]);
        return -1;
    }
    while (total < real_cap - 1) {
        int n = (int)read(p[0], tmp + total, (size_t)(real_cap - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    tmp[total] = 0;
    close(p[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    /* 拆 __EASYLUA_STATUS__=NNN
     * 标签全长："\n__EASYLUA_STATUS__=" = 1 + 18 + 1 = 20 字节 */
    int status = 0;
    char *tag = strstr(tmp, "\n__EASYLUA_STATUS__=");
    if (tag) {
        status = atoi(tag + 20);   /* 跳过整个标签到数字起点 */
        *tag = 0;                  /* 截断 body */
        total = (int)(tag - tmp);
    } else {
        /* 没 tag 说明 curl 失败（非 0 退出） */
        free(tmp);
        if (out_status) *out_status = 0;
        NET_ERR("curl failed (no status tag), wstatus=%d", wstatus);
        return -1;
    }
    if (out_status) *out_status = status;

    int copy = total < out_cap ? total : out_cap;
    memcpy(out_buf, tmp, (size_t)copy);
    free(tmp);
    return total;  /* 返回真实长度，截断由调用方 vs out_cap 判断 */
}

int Net_HttpRequest(const char *method, const char *url,
                    const char *headers,
                    const char *body, int body_len,
                    char *out_buf, int out_cap,
                    int *out_status,
                    int timeout_ms)
{
    if (!method) method = "GET";
    if (!url || !*url || !out_buf || out_cap <= 0) {
        NET_ERR("HttpRequest: bad args");
        return -1;
    }
    if (out_status) *out_status = 0;

    int is_https = 0;
    char host[256];
    char path[2048];
    int  port = 80;
    if (parse_url(url, &is_https, host, sizeof(host), &port,
                  path, sizeof(path)) != 0) return -1;

    if (is_https) {
        return http_via_curl(method, url, headers, body, body_len,
                             out_buf, out_cap, out_status, timeout_ms);
    }

    /* 纯 TCP HTTP/1.1 实现 */
    int fd = Net_TcpConnect(host, port, timeout_ms);
    if (fd < 0) return -1;
    if (timeout_ms > 0) {
        Net_SetTimeout(fd, timeout_ms, timeout_ms);
    }

    /* 拼请求头 */
    char req_head[8192];
    int  req_len = 0;
    req_len += snprintf(req_head + req_len, sizeof(req_head) - req_len,
                        "%s %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "User-Agent: easylua/1.0\r\n"
                        "Accept: */*\r\n"
                        "Connection: close\r\n",
                        method, path, host);
    if (body && body_len > 0) {
        /* 没有用户提供 Content-Type 时给个保守默认 */
        int has_ct = headers && case_strstr_n(headers, (int)strlen(headers),
                                              "Content-Type:") != NULL;
        if (!has_ct) {
            req_len += snprintf(req_head + req_len, sizeof(req_head) - req_len,
                                "Content-Type: application/octet-stream\r\n");
        }
        req_len += snprintf(req_head + req_len, sizeof(req_head) - req_len,
                            "Content-Length: %d\r\n", body_len);
    }
    if (headers && *headers) {
        /* 简单换行规范化：用户传 \n 也接受 */
        const char *p = headers;
        while (*p) {
            const char *eol = p;
            while (*eol && *eol != '\r' && *eol != '\n') eol++;
            int ll = (int)(eol - p);
            if (ll > 0 && req_len + ll + 2 < (int)sizeof(req_head)) {
                memcpy(req_head + req_len, p, (size_t)ll);
                req_len += ll;
                req_head[req_len++] = '\r';
                req_head[req_len++] = '\n';
            }
            while (*eol == '\r' || *eol == '\n') eol++;
            p = eol;
        }
    }
    if (req_len + 2 < (int)sizeof(req_head)) {
        req_head[req_len++] = '\r';
        req_head[req_len++] = '\n';
    }

    if (Net_TcpSend(fd, req_head, req_len, timeout_ms) != req_len) {
        Net_Close(fd);
        return -1;
    }
    if (body && body_len > 0) {
        if (Net_TcpSend(fd, body, body_len, timeout_ms) != body_len) {
            Net_Close(fd);
            return -1;
        }
    }

    /* 读响应：先读够 status + headers，再按 Content-Length / chunked 接 body */
    /* 用动态 buffer，因为响应可能比 out_cap 大也可能小，且要能完整解 chunked */
    int  rcap = out_cap > 16384 ? out_cap + 16384 : 32768;
    char *rbuf = (char *)malloc((size_t)rcap);
    if (!rbuf) { NET_ERR("oom %d", rcap); Net_Close(fd); return -1; }
    int  rlen = 0;
    int  hdr_end = -1;

    while (rlen < rcap) {
        int n = Net_TcpRecv(fd, rbuf + rlen, rcap - rlen, timeout_ms);
        if (n <= 0) break;
        rlen += n;
        /* 找 \r\n\r\n */
        if (hdr_end < 0) {
            for (int i = 0; i + 3 < rlen; i++) {
                if (rbuf[i] == '\r' && rbuf[i+1] == '\n' &&
                    rbuf[i+2] == '\r' && rbuf[i+3] == '\n') {
                    hdr_end = i + 4;
                    break;
                }
            }
        }
        /* 空间满了就先停下处理，可能要 realloc */
        if (rlen == rcap) {
            int new_cap = rcap * 2;
            char *nb = (char *)realloc(rbuf, (size_t)new_cap);
            if (!nb) break;
            rbuf = nb;
            rcap = new_cap;
        }
        /* 头部已经齐了：看是否能确定结束（Content-Length 或 chunked 末尾 0\r\n\r\n） */
        if (hdr_end > 0) {
            const char *hdrs = rbuf;
            int hl = hdr_end;
            const char *cl_p = case_strstr_n(hdrs, hl, "Content-Length:");
            if (cl_p) {
                int cl = atoi(cl_p + 15);
                if (rlen - hdr_end >= cl) break;
            } else {
                /* chunked 检查：找 \r\n0\r\n\r\n */
                const char *te = case_strstr_n(hdrs, hl, "Transfer-Encoding:");
                if (te) {
                    /* 检查最后 5 字节是 "0\r\n\r\n" 或末尾出现 chunked terminator */
                    if (rlen >= 5) {
                        for (int i = hdr_end; i + 5 <= rlen; i++) {
                            if (rbuf[i] == '0' && rbuf[i+1] == '\r' && rbuf[i+2] == '\n'
                                && rbuf[i+3] == '\r' && rbuf[i+4] == '\n') {
                                rlen = i + 5;
                                goto done_recv;
                            }
                        }
                    }
                }
                /* 既无 CL 也非 chunked：靠 Connection:close 读到 EOF，让循环自然走完 */
            }
        }
    }
done_recv:
    Net_Close(fd);

    if (hdr_end < 0) {
        NET_ERR("HTTP: malformed response (no header terminator)");
        free(rbuf);
        return -1;
    }

    /* 解析状态码 */
    int status = 0;
    if (rlen >= 12 && rbuf[8] == ' ') {
        status = atoi(rbuf + 9);
    }
    if (out_status) *out_status = status;

    /* 解析 body */
    int body_total = 0;
    const char *te = case_strstr_n(rbuf, hdr_end, "Transfer-Encoding:");
    int is_chunked = 0;
    if (te) {
        const char *eol = strstr(te, "\r\n");
        int linelen = eol ? (int)(eol - te) : (int)strlen(te);
        if (case_strstr_n(te, linelen, "chunked")) is_chunked = 1;
    }
    if (is_chunked) {
        int decoded = decode_chunked(rbuf + hdr_end, rlen - hdr_end,
                                     out_buf, out_cap);
        if (decoded < 0) {
            NET_ERR("HTTP: chunked decode failed");
            free(rbuf);
            return -1;
        }
        body_total = decoded;
    } else {
        body_total = rlen - hdr_end;
        int copy = body_total < out_cap ? body_total : out_cap;
        if (copy > 0) memcpy(out_buf, rbuf + hdr_end, (size_t)copy);
    }
    free(rbuf);
    return body_total;
}
