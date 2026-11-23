#define _POSIX_C_SOURCE 200809L

#include "utils/config_parser.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger.h"

#define CONFIG_MAX_ENTRIES 64
#define CONFIG_LINE_SIZE   256

struct config_entry {
    char *key;
    char *value;
};

static struct config_entry g_entries[CONFIG_MAX_ENTRIES];
static size_t g_entry_count = 0;

/**
 * @brief 原地修剪字符串前后的空白字符。
 */
static char *trim(char *str)
{
    char *end;

    if (str == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return str;
}

/**
 * @brief 按键查找条目。
 */
static struct config_entry *find_entry(const char *key)
{
    size_t i;

    for (i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].key, key) == 0) {
            return &g_entries[i];
        }
    }

    return NULL;
}

int config_load(const char *path)
{
    FILE *fp;
    char line[CONFIG_LINE_SIZE];

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    config_free();

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p;
        char *key;
        char *value;
        char *eq;

        /* 移除末尾换行符；超长行（>= CONFIG_LINE_SIZE）丢弃剩余字符，防止尾部误解析为新行。 */
        p = strchr(line, '\n');
        if (p != NULL) {
            *p = '\0';
        } else if (!feof(fp)) {
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }

        p = trim(line);

        /* 跳过空行和注释。 */
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }

        /* 跳过节标题，如 [server]。 */
        if (*p == '[') {
            continue;
        }

        eq = strchr(p, '=');
        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        key = trim(p);

        /* 剥离行内注释：值部分第一个 '#' 或 ';' 起视为注释。
         * 此前 "log_level=INFO ; prod" 会把 "INFO ; prod" 整体存为值，
         * 下游 logger_level_from_string() 解析失败后静默落到默认级别。
         * 限制：不支持引号转义，值本身含 '#' 或 ';' 时不能写行内注释。 */
        value = strpbrk(eq + 1, "#;");
        if (value != NULL) {
            *value = '\0';
        }
        value = trim(eq + 1);

        if (*key == '\0') {
            continue;
        }

        /* 重复 key 采用后值覆盖前值，避免插入重复项后 find_entry 永远返回旧值。 */
        {
            struct config_entry *existing = find_entry(key);
            if (existing != NULL) {
                char *new_val = strdup(value);
                if (new_val == NULL) {
                    fclose(fp);
                    config_free();
                    return -1;
                }
                free(existing->value);
                existing->value = new_val;
                continue;
            }
        }

        if (g_entry_count >= CONFIG_MAX_ENTRIES) {
            fprintf(stderr, "config_load: too many entries (max %d)\n",
                    CONFIG_MAX_ENTRIES);
            fclose(fp);
            config_free();
            errno = ENOSPC;
            return -1;
        }

        g_entries[g_entry_count].key = strdup(key);
        g_entries[g_entry_count].value = strdup(value);
        if (g_entries[g_entry_count].key == NULL ||
            g_entries[g_entry_count].value == NULL) {
            /* Free whichever strdup succeeded to avoid partial leak. */
            free(g_entries[g_entry_count].key);
            free(g_entries[g_entry_count].value);
            g_entries[g_entry_count].key = NULL;
            g_entries[g_entry_count].value = NULL;
            fclose(fp);
            config_free();
            return -1;
        }

        g_entry_count++;
    }

    fclose(fp);
    return 0;
}

const char *config_get_string(const char *key, const char *default_value)
{
    struct config_entry *entry;

    if (key == NULL) {
        return default_value;
    }

    entry = find_entry(key);
    if (entry == NULL) {
        return default_value;
    }

    return entry->value;
}

int config_get_int(const char *key, int default_value)
{
    const char *value;
    char *endptr;
    long result;

    value = config_get_string(key, NULL);
    if (value == NULL) {
        return default_value;
    }

    errno = 0;
    result = strtol(value, &endptr, 10);
    if (endptr == value || errno != 0 || *endptr != '\0' ||
        result < INT_MIN || result > INT_MAX) {
        /*
         * 非法整数回退告警：避免静默回退导致误配置
         *（如 thread_pool_size=abc）缺乏可观测信号。endptr == value
         * 额外覆盖空值（"key="），防止 strtol 对空串返回 0 被误当合法值。
         */
        LOG_ERROR("config_get_int: key '%s' has non-integer value '%s', "
                  "falling back to default %d",
                  key, value, default_value);
        return default_value;
    }

    return (int)result;
}

void config_free(void)
{
    size_t i;

    for (i = 0; i < g_entry_count; i++) {
        free(g_entries[i].key);
        free(g_entries[i].value);
        g_entries[i].key = NULL;
        g_entries[i].value = NULL;
    }

    g_entry_count = 0;
}
