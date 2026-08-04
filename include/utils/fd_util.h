#ifndef IOT_GATEWAY_FD_UTIL_H
#define IOT_GATEWAY_FD_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将文件描述符设置为非阻塞模式。
 *
 * @param fd 待修改的文件描述符。
 *
 * @return 成功返回 0，失败返回 -1 并设置 errno。
 */
int fd_set_nonblocking(int fd);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_FD_UTIL_H */
