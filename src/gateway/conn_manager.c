#define _POSIX_C_SOURCE 200809L /* clock_gettime, CLOCK_MONOTONIC */

#include "gateway/conn_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 哈希表桶数（2 的幂，O(1) 查找/更新/删除）。 */
#define CONN_MGR_BUCKETS 1024u

static struct conn_entry *g_buckets[CONN_MGR_BUCKETS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

static size_t conn_bucket(int fd)
{
    return (size_t)(((unsigned int)fd) & (CONN_MGR_BUCKETS - 1u));
}

/* 使用单调时钟避免 NTP 校时或系统时间跳变误判心跳超时。 */
static time_t monotonic_sec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ts.tv_sec;
    }

    return time(NULL);
}

int conn_manager_init(void)
{
    if (g_initialized) {
        return 0;
    }

    if (pthread_mutex_init(&g_mutex, NULL) != 0) {
        return -1;
    }

    memset(g_buckets, 0, sizeof(g_buckets));
    g_initialized = 1;
    return 0;
}

void conn_manager_destroy(void)
{
    size_t i;

    if (!g_initialized) {
        return;
    }

    pthread_mutex_lock(&g_mutex);

    for (i = 0; i < CONN_MGR_BUCKETS; i++) {
        struct conn_entry *entry = g_buckets[i];
        while (entry != NULL) {
            struct conn_entry *next = entry->next;
            free(entry);
            entry = next;
        }
        g_buckets[i] = NULL;
    }

    pthread_mutex_unlock(&g_mutex);

    pthread_mutex_destroy(&g_mutex);
    g_initialized = 0;
}

int conn_manager_add(int fd, uint64_t generation)
{
    struct conn_entry *entry;
    struct conn_entry *cur;
    size_t bucket;
    time_t now;

    if (!g_initialized || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return -1;
    }

    now = monotonic_sec();
    entry->fd = fd;
    entry->generation = generation;
    entry->last_active = now;
    bucket = conn_bucket(fd);

    pthread_mutex_lock(&g_mutex);

    /* 若同 fd 已有残留条目则原地覆盖并释放新节点，防止重复条目。 */
    for (cur = g_buckets[bucket]; cur != NULL; cur = cur->next) {
        if (cur->fd == fd) {
            cur->generation = generation;
            cur->last_active = now;
            pthread_mutex_unlock(&g_mutex);
            free(entry);
            return 0;
        }
    }

    entry->next = g_buckets[bucket];
    g_buckets[bucket] = entry;
    pthread_mutex_unlock(&g_mutex);

    return 0;
}

void conn_manager_remove(int fd)
{
    struct conn_entry **pp;
    struct conn_entry *cur;

    if (!g_initialized || fd < 0) {
        return;
    }

    pthread_mutex_lock(&g_mutex);

    pp = &g_buckets[conn_bucket(fd)];
    while (*pp != NULL) {
        cur = *pp;
        if (cur->fd == fd) {
            *pp = cur->next;
            free(cur);
            break;
        }
        pp = &cur->next;
    }

    pthread_mutex_unlock(&g_mutex);
}

void conn_manager_update_active_time(int fd)
{
    struct conn_entry *entry;
    time_t now;

    if (!g_initialized || fd < 0) {
        return;
    }

    now = monotonic_sec();

    pthread_mutex_lock(&g_mutex);

    entry = g_buckets[conn_bucket(fd)];
    while (entry != NULL) {
        if (entry->fd == fd) {
            entry->last_active = now;
            break;
        }
        entry = entry->next;
    }

    pthread_mutex_unlock(&g_mutex);
}

int conn_manager_get_generation(int fd, uint64_t *gen)
{
    struct conn_entry *entry;

    if (!g_initialized || fd < 0 || gen == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_mutex);

    entry = g_buckets[conn_bucket(fd)];
    while (entry != NULL) {
        if (entry->fd == fd) {
            *gen = entry->generation;
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
        entry = entry->next;
    }

    pthread_mutex_unlock(&g_mutex);
    errno = ENOENT;
    return -1;
}

void conn_manager_foreach(void (*callback)(struct conn_entry *, void *), void *arg)
{
    size_t i;

    if (!g_initialized || callback == NULL) {
        return;
    }

    pthread_mutex_lock(&g_mutex);

    for (i = 0; i < CONN_MGR_BUCKETS; i++) {
        struct conn_entry *entry = g_buckets[i];
        while (entry != NULL) {
            callback(entry, arg);
            entry = entry->next;
        }
    }

    pthread_mutex_unlock(&g_mutex);
}
