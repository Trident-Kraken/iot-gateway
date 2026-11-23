#ifndef IOT_GATEWAY_SERVER_H
#define IOT_GATEWAY_SERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化一个非阻塞 TCP 监听 socket。
 *
 * 创建 IPv4 TCP socket，启用 SO_REUSEADDR，绑定到指定端口的所有接口，
 * 设置为非阻塞模式，以 128 的 backlog 开始监听。
 *
 * @param port TCP 端口号（主机字节序）。
 *
 * @return 成功返回监听 socket 的文件描述符，失败返回 -1。
 *         失败时 errno 已设置，调用方无需关闭任何文件描述符。
 */
int server_init(uint16_t port);

/**
 * @brief 接受一个新的传入连接。
 *
 * 从监听 socket 接受一个客户端连接。返回的客户端 socket 已设置为非阻塞模式。
 *
 * @param listen_fd 由 server_init() 返回的监听 socket 文件描述符。
 *
 * @return 成功返回已接受客户端 socket 的文件描述符，失败返回 -1。
 *         EINTR 会在内部重试处理。EAGAIN/EWOULDBLOCK 返回给调用方，
 *         使其可以在之后重试（例如 poll 后）。
 */
int server_accept(int listen_fd);

/**
 * @brief 关闭监听 socket 并释放 server 模块内部保留的 reserve fd。
 *
 * @param listen_fd 由 server_init() 返回的监听 socket 文件描述符（<0 时跳过关闭）。
 */
void server_close(int listen_fd);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_SERVER_H */
