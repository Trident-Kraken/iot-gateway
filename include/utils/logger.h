#ifndef IOT_GATEWAY_LOGGER_H
#define IOT_GATEWAY_LOGGER_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 日志级别，按严重性升序排列。 */
enum log_level {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_ERROR = 2
};

/*
 * 全局日志级别过滤器。
 *
 * 为什么是全局（而非 static）：LOG_* 宏在调用点直接读取该值做零开销
 * 级别过滤，头文件宏无法访问 static 变量，必须 extern 暴露。
 *
 * 线程安全说明：本值仅在 logger_init()（所有线程启动前，单线程）写入，
 * 之后所有线程只读；logger_set_level() 为预留接口，当前无并发调用方。
 * 若未来需运行时并发修改日志级别，应改为原子类型或加锁。
 */
extern enum log_level g_log_level;

/**
 * @brief 初始化日志系统。
 *
 * @param path  日志文件路径。如果为 NULL，日志输出到 stderr。
 * @param level 初始日志级别。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int logger_init(const char *path, enum log_level level);

/**
 * @brief 关闭日志文件并释放资源。
 */
void logger_destroy(void);

/**
 * @brief 设置运行时日志级别。
 *
 * @param level 新级别。级别低于 @p level 的消息将被丢弃。
 */
void logger_set_level(enum log_level level);

/**
 * @brief 将级别字符串（如 "INFO"）转换为枚举值。
 *
 * @param str 级别字符串，大小写不敏感。
 *
 * @return 对应的级别值，无法识别时返回 LOG_LEVEL_INFO。
 */
enum log_level logger_level_from_string(const char *str);

/**
 * @brief 底层日志记录函数。
 *
 * 调用方应使用 LOG_DEBUG/LOG_INFO/LOG_ERROR 宏而非直接调用此函数，
 * 这样 __FILE__ 和 __LINE__ 会在调用点被捕获，无需调用方显式传递。
 *
 * @param level 日志级别。
 * @param file  源文件名。
 * @param line  源文件行号。
 * @param fmt   printf 风格的格式字符串。
 * @param ...   格式化参数。
 */
void logger_log(enum log_level level, const char *file, int line,
                const char *fmt, ...);

/*
 * 为什么用宏而不是函数？
 *
 * 1. 自动获取源代码位置：__FILE__ 和 __LINE__ 在精确调用点被解析，
 *    对于普通函数除非调用方手动传递，否则无法做到。
 *
 * 2. 避免函数调用开销：级别检查被内联展开，当消息被过滤时不会进入
 *    日志实现。这是禁用的日志级别的"零开销"抽象。
 *
 * 3. 变参转发：C99 __VA_ARGS__ 使 API 像 printf 一样易用。
 */
#define LOG_DEBUG(fmt, ...)                                                  \
    do {                                                                     \
        if (g_log_level > LOG_LEVEL_DEBUG) {                                 \
            break;                                                           \
        }                                                                    \
        logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while (0)

#define LOG_INFO(fmt, ...)                                                   \
    do {                                                                     \
        if (g_log_level > LOG_LEVEL_INFO) {                                  \
            break;                                                           \
        }                                                                    \
        logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__);  \
    } while (0)

#define LOG_ERROR(fmt, ...)                                                  \
    do {                                                                     \
        if (g_log_level > LOG_LEVEL_ERROR) {                                 \
            break;                                                           \
        }                                                                    \
        logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_LOGGER_H */
