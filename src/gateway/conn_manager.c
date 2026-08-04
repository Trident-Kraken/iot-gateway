#include "gateway/conn_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct conn_entry *g_head = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

int conn_manager_init(void)
{
    if (g_initialized) {
        return 0;
    }

    if (pthread_mutex_init(&g_mutex, NULL) != 0) {
        return -1;
    }

    g_head = NULL;
    g_initialized = 1;
    return 0;
}

void conn_manager_destroy(void)
{
    struct conn_entry *entry;
    struct conn_entry *next;

    if (!g_initialized) {
        return;
    }

    pthread_mutex_lock(&g_mutex);

   entry = g_head;
   while (entry != NULL) {
       next = entry->next;
       free(entry);
       entry = next;
   }

    g_head = NULL;
    pthread_mutex_unlock(&g_mutex);

    pthread_mutex_destroy(&g_mutex);
    g_initialized = 0;
}

int conn_manager_add(int fd, uint64_t generation)
{
    struct conn_entry *entry;

    if (!g_initialized || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return -1;
    }

   entry->fd = fd;
   entry->generation = generation;
   entry->last_active = time(NULL);

   pthread_mutex_lock(&g_mutex);
   entry->next = g_head;
   g_head = entry;
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

    pp = &g_head;
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

    if (!g_initialized || fd < 0) {
        return;
    }

    pthread_mutex_lock(&g_mutex);

    entry = g_head;
    while (entry != NULL) {
        if (entry->fd == fd) {
            entry->last_active = time(NULL);
            break;
        }
        entry = entry->next;
    }

    pthread_mutex_unlock(&g_mutex);
}

void conn_manager_foreach(void (*callback)(struct conn_entry *, void *), void *arg)
{
    struct conn_entry *entry;

    if (!g_initialized || callback == NULL) {
        return;
    }

    pthread_mutex_lock(&g_mutex);

    entry = g_head;
    while (entry != NULL) {
        callback(entry, arg);
        entry = entry->next;
    }

    pthread_mutex_unlock(&g_mutex);
}
