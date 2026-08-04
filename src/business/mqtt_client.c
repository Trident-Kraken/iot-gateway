/**
 * @file mqtt_client.c
 * @brief 基于 libmosquitto 的轻量 MQTT 发布客户端。
 *
 * 设计约束（见 AGENTS.md §2.1、§5.5）：
 *   - MQTT 协议实现封装在独立模块内，未来可替换为自实现。
 *   - 只实现 publish，不实现 subscribe（不过度设计）。
 *   - 使用静态单例 + 互斥锁保证多工作线程并发 publish 的安全。
 *
 * 线程模型（v1.7 重构）：
 *   - 单一「MQTT 管理线程」负责连接、mosquitto_loop() 网络驱动（收发、
 *     PINGREQ/keepalive）与断线自动重连（带退避）。
 *   - 工作线程的 mqtt_publish() 只做 QoS0 入队，快速返回（fast-fail），
 *     绝不阻塞；实际网络发送由管理线程的 mosquitto_loop() 驱动。
 *   - mosquitto_threaded_set(true) 使 libmosquitto 内部加锁，publish 与
 *     loop 可在不同线程并发操作同一句柄。
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

/* MQTT keepalive 秒数（PINGREQ 间隔，由 mosquitto_loop 维护）。 */
#define MQTT_KEEPALIVE_SEC 30

/* 管理线程每次 mosquitto_loop 的超时（毫秒）。 */
#define MQTT_LOOP_TIMEOUT_MS 100

/* 断线重连退避下限/上限（秒）。 */
#define MQTT_RECONNECT_BASE_DELAY_SEC 1
#define MQTT_RECONNECT_MAX_DELAY_SEC  4

/* 单例 MQTT 客户端状态。 */
static struct {
    struct mosquitto *mosq;      /* libmosquitto 实例句柄。 */
    char             *broker;    /* Broker 地址副本（用于重连）。 */
    int               port;      /* Broker 端口。 */
    pthread_mutex_t   lock;      /* 保护 connected/stop/mosq 指针的互斥锁。 */
    int               connected; /* 当前连接状态（0 = 断开, 1 = 已连接）。 */
    int               stop;      /* 管理线程退出标志。 */
    pthread_t         thread;    /* MQTT 管理线程。 */
    int               initialized;
} g_mqtt;

/* =========================================================================
 * 内部辅助函数
 * ========================================================================= */

/**
 * @brief 带退避的重连等待（分小段 sleep，可被 destroy 快速打断）。
 *
 * 不持锁等待：publish 的锁内状态检查不受影响。
 */
static void mqtt_backoff_sleep(void)
{
    /* 退避 = base(1) + rand()%3 → 1~3 秒，jitter 避免多实例同时重连。 */
    unsigned int delay_sec = MQTT_RECONNECT_BASE_DELAY_SEC
                             + (unsigned int)(rand() % 3);
    unsigned int i;

    if (delay_sec > MQTT_RECONNECT_MAX_DELAY_SEC) {
        delay_sec = MQTT_RECONNECT_MAX_DELAY_SEC;
    }

    for (i = 0; i < delay_sec * 10u; i++) {
        int stop;

        pthread_mutex_lock(&g_mqtt.lock);
        stop = g_mqtt.stop;
        pthread_mutex_unlock(&g_mqtt.lock);

        if (stop) {
            return;
        }

        {
            struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms */
            nanosleep(&ts, NULL);
        }
    }
}

/**
 * @brief MQTT 管理线程：连接 + loop 驱动 + 自动重连。
 *
 * 连接与网络 I/O 全部在此线程完成（锁外执行阻塞调用），工作线程的
 * publish 永不被 broker 可达性拖慢。mosq 句柄生命周期 = init→destroy，
 * 本线程不重建句柄，断线重连复用同一实例。
 */
static void *mqtt_thread(void *arg)
{
    (void)arg;

    for (;;) {
        struct mosquitto *mosq;
        int connected;
        int rc;

        pthread_mutex_lock(&g_mqtt.lock);
        if (g_mqtt.stop) {
            pthread_mutex_unlock(&g_mqtt.lock);
            break;
        }
        connected = g_mqtt.connected;
        mosq = g_mqtt.mosq;
        pthread_mutex_unlock(&g_mqtt.lock);

        if (mosq == NULL) {
            /* 句柄不可用（OOM 后遗症），稍后重试。 */
            mqtt_backoff_sleep();
            continue;
        }

        if (!connected) {
            /* 断开：尝试（重）连接。 */
            rc = mosquitto_connect(mosq, g_mqtt.broker, g_mqtt.port,
                                   MQTT_KEEPALIVE_SEC);
            if (rc == MOSQ_ERR_SUCCESS) {
                pthread_mutex_lock(&g_mqtt.lock);
                g_mqtt.connected = 1;
                pthread_mutex_unlock(&g_mqtt.lock);
                LOG_INFO("mqtt: connected to %s:%d",
                         g_mqtt.broker, g_mqtt.port);
                continue;
            }

            LOG_ERROR("mqtt: connect to %s:%d failed: %s",
                      g_mqtt.broker, g_mqtt.port,
                      mosquitto_strerror(rc));
            mqtt_backoff_sleep();
            continue;
        }

        /* 已连接：驱动网络 I/O（收发、PINGREQ/keepalive）。 */
        rc = mosquitto_loop(mosq, MQTT_LOOP_TIMEOUT_MS, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            pthread_mutex_lock(&g_mqtt.lock);
            g_mqtt.connected = 0;
            pthread_mutex_unlock(&g_mqtt.lock);
            LOG_ERROR("mqtt: loop error (%s), reconnecting",
                      mosquitto_strerror(rc));
        }
    }

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

    g_mqtt.broker = strdup(broker);
    if (g_mqtt.broker == NULL) {
        pthread_mutex_destroy(&g_mqtt.lock);
        mosquitto_lib_cleanup();
        return -1;
    }

    g_mqtt.port = port;
    g_mqtt.mosq = mosquitto_new(client_id, true, NULL);
    if (g_mqtt.mosq == NULL) {
        LOG_ERROR("mqtt: mosquitto_new failed (OOM?)");
        free(g_mqtt.broker);
        g_mqtt.broker = NULL;
        pthread_mutex_destroy(&g_mqtt.lock);
        mosquitto_lib_cleanup();
        return -1;
    }

    /* 线程安全模式：工作线程 publish 与管理线程 loop 并发操作同一句柄。 */
    mosquitto_threaded_set(g_mqtt.mosq, true);

    /* 播种随机数，保证重连 jitter 有效。 */
    srand((unsigned int)time(NULL));

    g_mqtt.connected = 0;
    g_mqtt.stop = 0;
    g_mqtt.initialized = 1;

    if (pthread_create(&g_mqtt.thread, NULL, mqtt_thread, NULL) != 0) {
        LOG_ERROR("mqtt: failed to create mqtt thread");
        mosquitto_destroy(g_mqtt.mosq);
        g_mqtt.mosq = NULL;
        free(g_mqtt.broker);
        g_mqtt.broker = NULL;
        pthread_mutex_destroy(&g_mqtt.lock);
        mosquitto_lib_cleanup();
        g_mqtt.initialized = 0;
        return -1;
    }

    LOG_INFO("mqtt: client_id=%s broker=%s:%d",
             client_id ? client_id : "(auto)", broker, port);
    return 0;
}

int mqtt_publish(const char *topic, const char *payload)
{
    struct mosquitto *mosq;
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

    /* 锁内只做状态检查与句柄快照；网络调用在锁外执行。 */
    pthread_mutex_lock(&g_mqtt.lock);
    if (!g_mqtt.connected || g_mqtt.mosq == NULL) {
        pthread_mutex_unlock(&g_mqtt.lock);
        LOG_ERROR("mqtt_publish: not connected, message dropped (topic=%s)",
                  topic);
        return -1;
    }
    mosq = g_mqtt.mosq;
    pthread_mutex_unlock(&g_mqtt.lock);

    /*
     * QoS 0: fire-and-forget。mosquitto_publish 仅将消息入队到 libmosquitto
     * 内部，实际发送由管理线程的 mosquitto_loop() 驱动；mosquitto_threaded_set
     * 保证并发安全。调用快速返回，工作线程永不因 broker 阻塞。
     */
    rc = mosquitto_publish(mosq, &mid, topic,
                           (int)strlen(payload), payload,
                           0,    /* QoS 0 */
                           false /* retain = false */);

    if (rc == MOSQ_ERR_SUCCESS) {
        LOG_DEBUG("mqtt_publish: topic=%s mid=%d len=%zu OK",
                  topic, mid, strlen(payload));
        return 0;
    }

    /*
     * 发布失败：标记断开并丢弃当前消息（fast-fail）。管理线程的
     * mosquitto_loop 会检测到断开并自动重连，broker 恢复后自动续传。
     */
    LOG_ERROR("mqtt_publish: mosquitto_publish failed: %s",
              mosquitto_strerror(rc));

    pthread_mutex_lock(&g_mqtt.lock);
    g_mqtt.connected = 0;
    pthread_mutex_unlock(&g_mqtt.lock);

    return -1;
}

void mqtt_client_destroy(void)
{
    if (!g_mqtt.initialized) {
        return;
    }

    /* 停止管理线程（最迟在 loop 超时或退避分段内退出）。 */
    pthread_mutex_lock(&g_mqtt.lock);
    g_mqtt.stop = 1;
    pthread_mutex_unlock(&g_mqtt.lock);
    pthread_join(g_mqtt.thread, NULL);

    pthread_mutex_lock(&g_mqtt.lock);
    if (g_mqtt.mosq != NULL) {
        /*
         * mosquitto_disconnect() 发送 DISCONNECT 报文并等待完成。
         * 若 Broker 已不可达，可改用 mosquitto_destroy() 强制释放；
         * 当前选择优雅断开（退出路径，可接受短等待）。
         */
        mosquitto_disconnect(g_mqtt.mosq);
        mosquitto_destroy(g_mqtt.mosq);
        g_mqtt.mosq = NULL;
    }
    g_mqtt.connected = 0;
    pthread_mutex_unlock(&g_mqtt.lock);

    pthread_mutex_destroy(&g_mqtt.lock);
    free(g_mqtt.broker);
    g_mqtt.broker = NULL;

    mosquitto_lib_cleanup();
    g_mqtt.initialized = 0;
    LOG_INFO("mqtt: client destroyed");
}
