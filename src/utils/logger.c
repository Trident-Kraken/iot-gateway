#define _POSIX_C_SOURCE 200809L

#include "utils/logger.h"

#include <ctype.h>
#include <strings.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* 当日志文件超过此字节数时轮转�?*/
#define LOG_MAX_SIZE (10 * 1024 * 1024)

static FILE *g_log_fp = NULL;
static char *g_log_path = NULL;
static size_t g_log_size = 0;

/* Protects g_log_fp, g_log_size, and log rotation from concurrent worker threads. */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 全局日志级别（也可由配置文件修改）�?*/
enum log_level g_log_level = LOG_LEVEL_INFO;

int logger_init(const char *path, enum log_level level)
{
    if (path == NULL) {
        g_log_fp = stderr;
        g_log_path = NULL;
        g_log_size = 0;
    } else {
        g_log_fp = fopen(path, "a");
        if (g_log_fp == NULL) {
            return -1;
        }

        if (fseek(g_log_fp, 0, SEEK_END) == 0) {
            long size = ftell(g_log_fp);
            if (size > 0) {
                g_log_size = (size_t)size;
            }
        }

        g_log_path = strdup(path);
        if (g_log_path == NULL) {
            fclose(g_log_fp);
            g_log_fp = NULL;
            return -1;
        }
    }

    g_log_level = level;
    return 0;
}

void logger_destroy(void)
{
    if (g_log_fp != NULL && g_log_fp != stderr) {
        fclose(g_log_fp);
    }

    g_log_fp = NULL;
    free(g_log_path);
    g_log_path = NULL;
    g_log_size = 0;
}

void logger_set_level(enum log_level level)
{
    g_log_level = level;
}

enum log_level logger_level_from_string(const char *str)
{
    if (str == NULL) {
        return LOG_LEVEL_INFO;
    }

    if (strcasecmp(str, "DEBUG") == 0) {
        return LOG_LEVEL_DEBUG;
    }

    if (strcasecmp(str, "INFO") == 0) {
        return LOG_LEVEL_INFO;
    }

    if (strcasecmp(str, "ERROR") == 0) {
        return LOG_LEVEL_ERROR;
    }

    return LOG_LEVEL_INFO;
}

/**
 * @brief 当日志文件超�?LOG_MAX_SIZE 时轮转�? *
 * 当前文件被关闭并重命名为 "<path>.1"。然后打开一个新的空日志文件�? */
static int logger_rotate(void)
{
    char rotated_path[1024];

    if (g_log_path == NULL || g_log_fp == NULL || g_log_fp == stderr) {
        return 0;
    }

    if (g_log_size < LOG_MAX_SIZE) {
        return 0;
    }

    fclose(g_log_fp);
    g_log_fp = NULL;

    snprintf(rotated_path, sizeof(rotated_path), "%s.1", g_log_path);
    (void)remove(rotated_path);

    if (rename(g_log_path, rotated_path) != 0) {
        /* 尝试重新打开原文件�?*/
        g_log_fp = fopen(g_log_path, "a");
        return -1;
    }

    g_log_fp = fopen(g_log_path, "a");
    if (g_log_fp == NULL) {
        return -1;
    }

    g_log_size = 0;
    return 0;
}

void logger_log(enum log_level level, const char *file, int line,
                const char *fmt, ...)
{
    time_t now;
    struct tm tm_info;
    char time_buf[32];
    const char *level_str;
    va_list ap;
    char message[2048];
    int message_len;
    int line_len;

    /* Quick filter without lock: level check is a read of a monotonic enum.
       The actual write path below is fully serialized by g_log_mutex. */
    if (level < g_log_level) {
        return;
    }

    pthread_mutex_lock(&g_log_mutex);

    if (g_log_fp == NULL) {
        g_log_fp = stderr;
    }

    switch (level) {
    case LOG_LEVEL_DEBUG:
        level_str = "DEBUG";
        break;
    case LOG_LEVEL_INFO:
        level_str = "INFO";
        break;
    case LOG_LEVEL_ERROR:
        level_str = "ERROR";
        break;
    default:
        level_str = "UNKNOWN";
        break;
    }

    now = time(NULL);
    localtime_r(&now, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    va_start(ap, fmt);
    message_len = vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    if (message_len < 0) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    if ((size_t)message_len >= sizeof(message)) {
        message_len = (int)sizeof(message) - 1;
        message[message_len] = '\0';
    }

    /* 写前做简单的轮转检查�?*/
    (void)logger_rotate();

    if (g_log_fp == NULL) {
        g_log_fp = stderr;
    }

    line_len = fprintf(g_log_fp,
                       "[%s] [%s] [%s:%d] %s\n",
                       time_buf, level_str, file, line, message);

    if (line_len > 0) {
        g_log_size += (size_t)line_len;
    }

    fflush(g_log_fp);

    pthread_mutex_unlock(&g_log_mutex);
}
