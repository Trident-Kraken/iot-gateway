#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stddef.h>
#include <stdint.h>

#include "business/business_handler.h"
#include "utils/config_parser.h"
#include "gateway/heartbeat.h"
#include "utils/logger.h"
#include "business/mqtt_client.h"
#include "gateway/server.h"
#include "gateway/epoll_worker.h"
#include "utils/threadpool.h"

#define DEFAULT_CONFIG_PATH "config/gateway.ini"
#define DEFAULT_LOG_PATH    "logs/gateway.log"

int main(int argc, char *argv[])
{
    const char *config_path;
    int server_port;
    int thread_pool_size;
    int heartbeat_interval;
    int heartbeat_timeout;
    const char *log_level_str;
    enum log_level log_level;
    const char *mqtt_broker;
    int mqtt_port;
    int listen_fd;

    (void)argc;
    (void)argv;

    config_path = DEFAULT_CONFIG_PATH;

    if (config_load(config_path) != 0) {
        fprintf(stderr, "failed to load config from %s\n", config_path);
        return EXIT_FAILURE;
    }

    server_port = config_get_int("server_port", 8080);
    thread_pool_size = config_get_int("thread_pool_size", 4);
    heartbeat_interval = config_get_int("heartbeat_interval", 5);
    heartbeat_timeout = config_get_int("heartbeat_timeout", 15);
    log_level_str = config_get_string("log_level", "INFO");
    log_level = logger_level_from_string(log_level_str);

    if (logger_init(DEFAULT_LOG_PATH, log_level) != 0) {
        fprintf(stderr, "failed to initialize logger\n");
        config_free();
        return EXIT_FAILURE;
    }

    mqtt_broker = config_get_string("mqtt_broker", "localhost");
    mqtt_port = config_get_int("mqtt_port", 1883);

    LOG_INFO("iot-gateway starting...");
    LOG_INFO("config loaded from %s", config_path);
    LOG_INFO("server_port=%d thread_pool_size=%d "
             "heartbeat_interval=%d heartbeat_timeout=%d log_level=%s "
             "mqtt_broker=%s mqtt_port=%d",
             server_port, thread_pool_size, heartbeat_interval,
             heartbeat_timeout, log_level_str, mqtt_broker, mqtt_port);

    /* 初始化命令分发表（必须在工作线程启动前完成）。*/
    cmd_handlers_init();

    /* 初始化MQTT 客户端（连接 Mosquitto Broker）。*/
    if (mqtt_client_init(mqtt_broker, mqtt_port, NULL) != 0) {
        LOG_ERROR("failed to initialize MQTT client");
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (threadpool_init(thread_pool_size) != 0) {
        LOG_ERROR("failed to initialize thread pool");
        mqtt_client_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (epoll_worker_init() != 0) {
        LOG_ERROR("failed to initialize epoll worker pipe");
        threadpool_destroy();
        mqtt_client_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (heartbeat_init(heartbeat_interval, heartbeat_timeout) != 0) {
        LOG_ERROR("failed to initialize heartbeat monitor");
        epoll_worker_destroy();
        threadpool_destroy();
        mqtt_client_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (heartbeat_start() != 0) {
        LOG_ERROR("failed to start heartbeat monitor");
        mqtt_client_destroy();
        heartbeat_stop();
        epoll_worker_destroy();
        threadpool_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    listen_fd = server_init((uint16_t)server_port);
    if (listen_fd < 0) {
        LOG_ERROR("failed to initialize server on port %d", server_port);
        mqtt_client_destroy();
        heartbeat_stop();
        epoll_worker_destroy();
        threadpool_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    LOG_INFO("server listening on port %d", server_port);

    /* 永久阻塞。*/
    epoll_worker_run(listen_fd);

    /* 正常运行时不会到达此处。*/
    heartbeat_stop();
    epoll_worker_destroy();
    threadpool_destroy();
    logger_destroy();
    config_free();

    return EXIT_SUCCESS;
}

/*
 * packet_parser 在完整帧组装完毕后调用的回调函数。 * 帧被转发到工作线程池进行业务处理。 */
void on_frame_complete(int fd, const uint8_t *frame, size_t len)
{
    struct task t;

    if (frame == NULL || len == 0 || len > sizeof(t.frame)) {
        LOG_ERROR("on_frame_complete: invalid frame from fd=%d", fd);
        return;
    }

    memset(&t, 0, sizeof(t));
    t.fd = fd;
    t.frame_len = len;
    memcpy(t.frame, frame, len);

    /* 从协议帧中提取命令字（偏移量 4，见 protocol.h 帧布局）。*/
    t.cmd = t.frame[4];

    if (threadpool_submit(&t) != 0) {
        LOG_ERROR("on_frame_complete: threadpool_submit failed for fd=%d", fd);
    }
}
