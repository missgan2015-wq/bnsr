/*
 * Net 命名空间：socket / DNS / HTTP 客户端
 *
 * 设计原则：
 *   - 纯 BSD socket，零依赖（不引 libcurl，避免链接体积膨胀）
 *   - 阻塞 API + 可选超时（毫秒），没传超时按系统默认
 *   - fd 作为句柄直接暴露给 Lua FFI 层，Lua 端包装成 metatable + __gc
 *   - 字符串结果走调用方传入的 char* buffer，调用方自己 ffi.string
 *
 * HTTP 实现：
 *   - http://  : 纯 TCP 直发请求行 + 头，按 Content-Length / chunked 解析响应
 *   - https:// : fallback 到 /system/bin/curl 子进程（Android 12+ 系统自带）
 *
 * 不实现的部分（脚本场景用不上 / 实现成本高）：
 *   - WebSocket（自己用 TCP 拼）
 *   - SSL/TLS native 实现（依赖 mbedTLS / OpenSSL）
 *   - HTTP/2、HTTP/3
 *   - Cookie jar、自动重定向（curl fallback 路径默认带 -L）
 */

#ifndef EASYLUA_NET_H
#define EASYLUA_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- TCP 客户端 ---------- */

/**
 * 同步建连 TCP。
 * @param host        目标主机（域名或 IPv4 字符串），不允许为空
 * @param port        目标端口
 * @param timeout_ms  连接超时，<=0 走系统默认（一般 ~75s）
 * @return            成功返回 socket fd（>=0），失败 -1
 */
int Net_TcpConnect(const char *host, int port, int timeout_ms);

/**
 * 阻塞发送，全部发完才返回。
 * @return  实际写入的字节数（>=0），错误 -1
 */
int Net_TcpSend(int fd, const char *buf, int len, int timeout_ms);

/**
 * 阻塞接收，最多 cap 字节。
 * @return  >0 实际读到字节数；0 = 对端关闭；-1 = 错误 / 超时
 */
int Net_TcpRecv(int fd, char *buf, int cap, int timeout_ms);

/**
 * 设置 TCP_NODELAY（关闭 Nagle）。
 * @param on  1 = 关闭 Nagle，0 = 开启
 */
int Net_TcpSetNoDelay(int fd, int on);

/* ---------- UDP ---------- */

/**
 * 创建 UDP socket（IPv4 + DGRAM）。
 * @return  fd 或 -1
 */
int Net_UdpOpen(void);

/**
 * 向指定地址发包（一次一个 datagram）。
 * @return  发送字节数，失败 -1
 */
int Net_UdpSendTo(int fd, const char *host, int port, const char *buf, int len);

/**
 * 阻塞接收 UDP 包。可选写回源地址。
 * @param out_host  可为 NULL；不为 NULL 时写源 IP 字符串
 * @param out_n     out_host 容量
 * @param out_port  可为 NULL；不为 NULL 时写源端口
 * @return          收到字节数，失败/超时 -1
 */
int Net_UdpRecvFrom(int fd, char *buf, int cap, int timeout_ms,
                    char *out_host, int out_n, int *out_port);

/* ---------- 通用 ---------- */

/** 关闭 socket。重复关闭无副作用。 */
void Net_Close(int fd);

/**
 * 设置 send / recv 超时（毫秒，<=0 表示阻塞至系统默认）。
 * 主要给 TCP 用；UDP 也可以用来设 RecvFrom 默认超时。
 */
int Net_SetTimeout(int fd, int recv_ms, int send_ms);

/* ---------- DNS / 本机 IP ---------- */

/**
 * 域名解析为 IPv4 字符串（点分形式），写入 out。
 * @return  实际写入的字节数（不含 \0），失败 -1
 */
int Net_DnsResolve(const char *host, char *out, int n);

/**
 * 取本机非 loopback 的 IPv4 地址字符串。优先 wlan0，其次 rmnet/usb 等。
 * @return  写入字节数，失败 -1
 */
int Net_LocalIp(char *out, int n);

/* ---------- HTTP（高阶 API） ---------- */

/**
 * 通用 HTTP 请求。
 *
 * @param method        "GET" / "POST" / "PUT" / "DELETE" / "HEAD" 等
 * @param url           完整 URL（http:// 或 https://）
 * @param headers       附加请求头（多行，"\r\n" 或 "\n" 分隔，可为 NULL）
 *                      形如 "Authorization: Bearer xxx\r\nX-Foo: bar"
 *                      框架会自动加 Host / Content-Length / Connection: close
 * @param body          请求体；可为 NULL
 * @param body_len      请求体长度（字节）；body == NULL 时被忽略
 * @param out_buf       响应 body 写入这里（不含响应头）
 * @param out_cap       out_buf 容量
 * @param out_status    可为 NULL；不为 NULL 时写回 HTTP 状态码（如 200）
 *
 * @return  实际写入 out_buf 的字节数（>=0）；
 *          如果 body 大于 out_cap，超出部分被丢弃但仍返回实际"应该"的字节数
 *          （调用方据此判断 truncation）；
 *          网络错误 / DNS 失败 / 协议错误 返回 -1
 */
int Net_HttpRequest(const char *method,
                    const char *url,
                    const char *headers,
                    const char *body, int body_len,
                    char *out_buf, int out_cap,
                    int *out_status,
                    int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
