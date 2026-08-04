/**
 * @file mqtt_client.c
 * @brief 基于 libmosquitto 的轻量 MQTT 发布客户端。
 *
 * 设计约束（见 AGENTS.md §2.1、§5.5）：
 *   - MQTT 协议实现封装在独立模块内，未来可替换为自实现。
 *   - 只实现 publish，不实现 subscribe（不过度设计）。
 *   - 使用静态单例 + 互斥锁保证多工作线程并发 publish 的安全。
 */

#define _POSIX_C_SOURCE 200809L   /* strdup */

#include "business/mqtt_client.h"

#include <errno.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "utils/logger.h"

/* 重连最大尝试次数。 */
#define MQTT_RECONNECT_MAX_RETRIES 3

/* 重连间隔基数（毫秒），带随机抖动避免惊群。 */
#define MQTT_RECONNECT_BASE_DELAY_MS 200

/* MQTT keepalive 秒数（PINGREQ 间隔）。 */
#define MQTT_KEEPALIVE_SEC 30

/* 单例 MQTT 客户端状态。 */
static struct {
    struct mosquitto *mosq;      /* libmosquitto 实例句柄。 */
    char             *broker;    /* Broker 地址副本（用于重连）。 */
    int               port;      /* Broker 端口。 */
    pthread_mutex_t   lock;      /* 保护 mosq 句柄的互斥锁。 */
    int               connected; /* 当前连接状态（0 = 断开, 1 = 已连接）。 */
    int               initialized;
    pthread_cond_t    reconnect_cond;   /* 唤醒异步重连线程的条件变量。 */
    int               reconnect_pending;/* 标记需要重连。 */
    int               started;          /* 重连线程运行标志。 */
    pthread_t         reconnect_thread; /* 异步重连线程。 */
} g_mqtt;

/* =========================================================================
 * 内部辅助函数
 * ========================================================================= */

/**
 * @brief 连接到 Broker（内部，调用方必须持有 g_mqtt.lock）。
 *
 * 如果已存在 mosquitto 实例，先销毁再重建（mosquitto_reinitialise 更轻量，
 * 但为清晰性直接新建）。
 *
 * @return 0 成功，-1 失败。
 */
static int mqtt_connect_locked(void)
{
    int retry;

    g_mqtt.connected = 0;

    if (g_mqtt.mosq != NULL) {
        mosquitto_destroy(g_mqtt.mosq);
        g_mqtt.mosq = NULL;
    }

    /* mosquitto_new(id, clean_session, user_data) */
    g_mqtt.mosq = mosquitto_new(NULL, true, NULL);
    if (g_mqtt.mosq == NULL) {
        LOG_ERROR("mqtt: mosquitto_new failed (OOM?)");
        return -1;
    }

    /*
     * 带退避的重试循环。
     *
     * 为什么在 mqtt_client 中做重连而不在调用方？
     *   调用方（business_handler）不应感知底层传输状态。如果每次 publish 失败
     *   都让上层决定是否重连，会导致业务代码充斥传输逻辑，违反单一职责原则。
     */
    for (retry = 0; retry < MQTT_RECONNECT_MAX_RETRIES; retry++) {
        int rc;

        rc = mosquitto_connect(g_mqtt.mosq, g_mqtt.broker, g_mqtt.port,
                               MQTT_KEEPALIVE_SEC);
        if (rc == MOSQ_ERR_SUCCESS) {
            g_mqtt.connected = 1;
            LOG_INFO("mqtt: connected to %s:%d", g_mqtt.broker, g_mqtt.port);
            return 0;
        }

        LOG_ERROR("mqtt: connect attempt %d/%d to %s:%d failed: %s",
                  retry + 1, MQTT_RECONNECT_MAX_RETRIES,
                  g_mqtt.broker, g_mqtt.port,
                  mosquitto_strerror(rc));

        if (retry < MQTT_RECONNECT_MAX_RETRIES - 1) {
            /*
             * 退避延迟 = base * (retry+1) + 随机 jitter。
             * 随机 jitter 避免多线程同时重连导致 Broker 短时间收到大量 SYN。
             */
            unsigned int delay_ms = MQTT_RECONNECT_BASE_DELAY_MS
                                    * (unsigned int)(retry + 1);
            struct timespec ts;

            delay_ms += (unsigned int)(rand() % 200);
            ts.tv_sec  = delay_ms / 1000;
            ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }

    /* 3 次全部失败，释放 mosquitto 实例。 */
    mosquitto_destroy(g_mqtt.mosq);
    g_mqtt.mosq = NULL;
    return -1;
}

/**
 * @brief 异步重连线程：被条件变量唤醒后执行阻塞式重连。
 *
 * 将重连的阻塞等待（nanosleep + connect 重试）从 mqtt_publish 移到
 * 独立线程，使 worker 线程的 publish 调用永远快速返回（fast-fail），
 * 不因 Broker 不可达而阻塞任务队列消费。
 */
static void *mqtt_reconnect_thread(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&g_mqtt.lock);

    while (g_mqtt.started) {
        while (g_mqtt.started &&
               (!g_mqtt.reconnect_pending || g_mqtt.connected)) {
            pthread_cond_wait(&g_mqtt.reconnect_cond, &g_mqtt.lock);
        }

        if (!g_mqtt.started) {
            break;
        }

        g_mqtt.reconnect_pending = 0;
        if (mqtt_connect_locked() != 0) {
            LOG_ERROR("mqtt: background reconnect failed, "
                      "will retry on next publish");
        }
    }

    pthread_mutex_unlock(&g_mqtt.lock);
    return NULL;
}

/* =========================================================================
 * 公共接口
 * ========================================================================= */

int mqtt_client_init(const char *broker, int port, const char *client_id)
{
    int rc;

    if (g_mqtt.initialized) {
        return 0;
    }

    if (broker == NULL) {
        broker = "localhost";
    }
    if (port <= 0) {
        port = 1883;
    }

    /* 初始化 libmosquitto 全局状态（引用计数，多次调用安全）。 */
    rc = mosquitto_lib_init();
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mqtt: mosquitto_lib_init failed: %s",
                  mosquitto_strerror(rc));
        return -1;
    }

   if (pthread_mutex_init(&g_mqtt.lock, NULL) != 0) {
       mosquitto_lib_cleanup();
       return -1;
   }

   if (pthread_cond_init(&g_mqtt.reconnect_cond, NULL) != 0) {
       pthread_mutex_destroy(&g_mqtt.lock);
       mosquitto_lib_cleanup();
       return -1;
   }

   g_mqtt.broker = strdup(broker);
   if (g_mqtt.broker == NULL) {
       pthread_mutex_destroy(&g_mqtt.lock);
       pthread_cond_destroy(&g_mqtt.reconnect_cond);
       mosquitto_lib_cleanup();
       return -1;
   }

   g_mqtt.port = port;
   g_mqtt.initialized = 1;

   /* Start the async reconnect thread before attempting the initial
    * connection: if the first connect fails, the thread will handle
    * retries triggered by future mqtt_publish calls. */
   g_mqtt.started = 1;
   if (pthread_create(&g_mqtt.reconnect_thread, NULL,
                      mqtt_reconnect_thread, NULL) != 0) {
       LOG_ERROR("mqtt: failed to create reconnect thread");
       g_mqtt.started = 0;
       pthread_cond_destroy(&g_mqtt.reconnect_cond);
       pthread_mutex_destroy(&g_mqtt.lock);
       free(g_mqtt.broker);
       g_mqtt.broker = NULL;
       mosquitto_lib_cleanup();
       g_mqtt.initialized = 0;
       return -1;
   }

   /* 首次连接。 */
   pthread_mutex_lock(&g_mqtt.lock);
   rc = mqtt_connect_locked();
    pthread_mutex_unlock(&g_mqtt.lock);

    if (rc != 0) {
        LOG_ERROR("mqtt: initial connect to %s:%d failed", broker, port);
        /* 不把 initialized 置回 0：允许后续 publish 时自动重连。 */
    }

    LOG_INFO("mqtt: client_id=%s broker=%s:%d",
             client_id ? client_id : "(auto)", broker, port);
    return 0;
}

int mqtt_publish(const char *topic, const char *payload)
{
    int rc;
    int mid;

    if (topic == NULL || payload == NULL) {
        LOG_ERROR("mqtt_publish: topic or payload is NULL");
        return -1;
    }

    if (!g_mqtt.initialized) {
        LOG_ERROR("mqtt_publish: mqtt_client not initialized");
        return -1;
    }

    pthread_mutex_lock(&g_mqtt.lock);

    /* 如果已断开，尝试重连。 */
   if (!g_mqtt.connected) {
        g_mqtt.reconnect_pending = 1;
        pthread_cond_signal(&g_mqtt.reconnect_cond);
        pthread_mutex_unlock(&g_mqtt.lock);
        LOG_ERROR("mqtt_publish: not connected, message dropped (topic=%s)",
                  topic);
        return -1;
    }

    /*
     * mosquitto_publish(mosq, mid, topic, payloadlen, payload, qos, retain)
     * QoS 0: fire-and-forget，消息不持久化，不保证送达。
     * 适合传感器高频上报场景（允许少量丢失，追求低延迟）。
     */
    rc = mosquitto_publish(g_mqtt.mosq, &mid, topic,
                           (int)strlen(payload), payload,
                           0,    /* QoS 0 */
                           false /* retain = false */);

   if (rc == MOSQ_ERR_SUCCESS) {
       pthread_mutex_unlock(&g_mqtt.lock);
       LOG_DEBUG("mqtt_publish: topic=%s mid=%d len=%zu OK",
                 topic, mid, strlen(payload));
       return 0;
   }

    /*
     * 发布失败：标记为断开，释放当前句柄，触发下次 publish 时自动重连。
     *
     * 为什么不做同步重试？
     *   如果 Broker 已宕机，阻塞重试会导致所有工作线程卡在 mqtt_publish 上，
     *   进而阻塞任务队列消费，最终背压到 epoll I/O 线程。标记为断开并丢弃
     *   当前消息是务实的降级策略。
     */
    LOG_ERROR("mqtt_publish: mosquitto_publish failed: %s",
              mosquitto_strerror(rc));

   g_mqtt.connected = 0;
   mosquitto_destroy(g_mqtt.mosq);
   g_mqtt.mosq = NULL;
    g_mqtt.reconnect_pending = 1;
    pthread_cond_signal(&g_mqtt.reconnect_cond);

   pthread_mutex_unlock(&g_mqtt.lock);
   return -1;
}

void mqtt_client_destroy(void)
{
    if (!g_mqtt.initialized) {
        return;
    }

    /* Stop the reconnect thread: set started=0, wake it, join. */
    pthread_mutex_lock(&g_mqtt.lock);
    g_mqtt.started = 0;
    pthread_cond_signal(&g_mqtt.reconnect_cond);
    pthread_mutex_unlock(&g_mqtt.lock);
    pthread_join(g_mqtt.reconnect_thread, NULL);

    pthread_mutex_lock(&g_mqtt.lock);

    if (g_mqtt.mosq != NULL) {
        /*
         * mosquitto_disconnect() 发送 DISCONNECT 报文并等待完成。
         * 超时默认约 5 秒。若 Broker 已不可达，可调用 mosquitto_destroy()
         * 强制释放（不发送 DISCONNECT），当前选择优雅断开。
         */
        mosquitto_disconnect(g_mqtt.mosq);
        mosquitto_destroy(g_mqtt.mosq);
        g_mqtt.mosq = NULL;
    }

    g_mqtt.connected = 0;

    pthread_mutex_unlock(&g_mqtt.lock);

    pthread_mutex_destroy(&g_mqtt.lock);

    pthread_cond_destroy(&g_mqtt.reconnect_cond);

    free(g_mqtt.broker);
    g_mqtt.broker = NULL;

    mosquitto_lib_cleanup();

    g_mqtt.initialized = 0;
    LOG_INFO("mqtt: client destroyed");
}
