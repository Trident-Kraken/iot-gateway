/*
 * config_parser 单元测试。
 *
 * 覆盖：节/注释/空白跳过、键值 trim、字符串/整数取值、
 * 默认值回退、非数字/溢出/超 int 范围回退、文件缺失、
 * 超容量拒绝、重载替换旧键。
 */
#define _POSIX_C_SOURCE 200809L /* mkstemp */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils/config_parser.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* 将内容写入临时文件并加载，返回 0 表示成功。 */
static int load_content(const char *content, char *path, size_t path_sz)
{
    int fd;
    ssize_t n;
    int rc;

    snprintf(path, path_sz, "/tmp/iotgw_cfg_XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }

    n = write(fd, content, strlen(content));
    close(fd);

    if (n != (ssize_t)strlen(content)) {
        unlink(path);
        return -1;
    }

    rc = config_load(path);
    unlink(path);
    return rc;
}

static void test_valid_ini(void)
{
    char path[64];
    const char *ini =
        "# comment\n"
        "; another comment\n"
        "[server]\n"
        "server_port=8080\n"
        "  key_with_space =  value_with_space  \n"
        "[log]\n"
        "log_level=INFO\n"
        "[empty_section]\n"
        "line_without_equal\n";

    CHECK(load_content(ini, path, sizeof(path)) == 0);

    /* 字符串取值 + trim。 */
    CHECK(strcmp(config_get_string("server_port", NULL), "8080") == 0);
    CHECK(strcmp(config_get_string("key_with_space", NULL),
                 "value_with_space") == 0);
    CHECK(strcmp(config_get_string("log_level", NULL), "INFO") == 0);

    /* 缺失键返回默认值；无 '=' 的行被跳过。 */
    CHECK(strcmp(config_get_string("missing", "def"), "def") == 0);
    CHECK(config_get_string("line_without_equal", NULL) == NULL);

    /* 整数取值。 */
    CHECK(config_get_int("server_port", -1) == 8080);
    CHECK(config_get_int("missing", -1) == -1);

    config_free();
}

static void test_int_edge_cases(void)
{
    char path[64];
    const char *ini =
        "num_ok=42\n"
        "num_neg=-5\n"
        "num_alpha=abc\n"
        "num_overflow=99999999999999999999\n"
        "num_too_big=3000000000\n";

    CHECK(load_content(ini, path, sizeof(path)) == 0);

    CHECK(config_get_int("num_ok", -1) == 42);
    CHECK(config_get_int("num_neg", -1) == -5);
    CHECK(config_get_int("num_alpha", -1) == -1);       /* 非数字 → 默认值 */
    CHECK(config_get_int("num_overflow", -1) == -1);    /* long 溢出 → 默认值 */
    CHECK(config_get_int("num_too_big", -1) == -1);     /* 超 int 范围 → 默认值 */

    config_free();
}

static void test_missing_file(void)
{
    errno = 0;
    CHECK(config_load("/nonexistent/iotgw_no_such.ini") == -1);
    CHECK(errno != 0);
}

static void test_too_many_entries(void)
{
    char path[64];
    char buf[4096];
    size_t off = 0;
    int i;

    /* 65 行（上限 64）应拒绝。 */
    for (i = 0; i < 65; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "key%d=%d\n", i, i);
    }

    CHECK(load_content(buf, path, sizeof(path)) == -1);
    CHECK(errno == ENOSPC);

    /* 失败后内部状态应干净，可再次正常加载。 */
    CHECK(load_content("a=1\n", path, sizeof(path)) == 0);
    CHECK(config_get_int("a", -1) == 1);
    config_free();
}

static void test_reload_replaces(void)
{
    char path[64];

    CHECK(load_content("old_key=1\n", path, sizeof(path)) == 0);
    CHECK(config_get_int("old_key", -1) == 1);

    CHECK(load_content("new_key=2\n", path, sizeof(path)) == 0);
    CHECK(config_get_int("old_key", -1) == -1);  /* 旧键已清空 */
    CHECK(config_get_int("new_key", -1) == 2);

    config_free();
}

/*
 * 行内注释剥离与非法整数回退告警回归。
 */
static void test_inline_comments(void)
{
    char path[64];
    const char *ini =
        "server_port=8080  # inline hash comment\n"
        "log_level=ERROR ; inline semicolon comment\n"
        "thread_pool_size=abc # non-integer value\n"
        "empty_val=\n";

    CHECK(load_content(ini, path, sizeof(path)) == 0);

    /* 例 1：'#' 行内注释剥离后整数值正确。 */
    CHECK(config_get_int("server_port", -1) == 8080);
    /* 例 2：';' 行内注释剥离后字符串值正确（不再是 "ERROR ; ..."）。 */
    CHECK(strcmp(config_get_string("log_level", "def"), "ERROR") == 0);
    /* 例 3：非法整数回退默认值，且输出告警日志（stderr 可见）。 */
    CHECK(config_get_int("thread_pool_size", 4) == 4);
    /* 附带：空值同样视为非法（strtol 未消费任何数字 → 不再误当 0）。 */
    CHECK(config_get_int("empty_val", 4) == 4);

    config_free();
}

static void test_duplicate_key_and_long_line(void)
{
    char path[64];
    char ini[1024];
    char long_pad[301];

    memset(long_pad, 'a', 300);
    long_pad[300] = '\0';

    snprintf(ini, sizeof(ini),
             "dup_key=first\n"
             "dup_key=second\n"
             "# %sfoo=bar\n"
             "after_long=42\n",
             long_pad);

    CHECK(load_content(ini, path, sizeof(path)) == 0);
    /* 重复 key 采用后值覆盖前值。 */
    CHECK(strcmp(config_get_string("dup_key", ""), "second") == 0);
    /* >255 字节超长注释行的尾部 foo=bar 不被误解析为新配置项。 */
    CHECK(config_get_string("foo", NULL) == NULL);
    CHECK(config_get_int("after_long", -1) == 42);

    config_free();
}

int main(void)
{
    test_valid_ini();
    test_int_edge_cases();
    test_inline_comments();
    test_duplicate_key_and_long_line();
    test_missing_file();
    test_too_many_entries();
    test_reload_replaces();

    printf("unit_config: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
