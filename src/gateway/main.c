#include <stdio.h>
#include <stdlib.h>

#include <stdint.h>

#include "business/business_handler.h"
#include "utils/config_parser.h"
#include "gateway/conn_manager.h"
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
    int mqtt_qos;
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

    /* 配置范围校验：非法值直接拒绝启动，避免静默截断/误配。 */
    if (server_port <= 0 || server_port > 65535) {
        LOG_ERROR("invalid server_port=%d (must be 1..65535)", server_port);
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (thread_pool_size <= 0 || thread_pool_size > 64) {
        LOG_ERROR("invalid thread_pool_size=%d (must be 1..64)",
                  thread_pool_size);
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    mqtt_broker = config_get_string("mqtt_broker", "localhost");
    mqtt_port = config_get_int("mqtt_port", 1883);
    mqtt_qos = config_get_int("mqtt_qos", 0);

    if (mqtt_port <= 0 || mqtt_port > 65535) {
        LOG_ERROR("invalid mqtt_port=%d (must be 1..65535)", mqtt_port);
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (mqtt_qos < 0 || mqtt_qos > 2) {
        LOG_ERROR("invalid mqtt_qos=%d (must be 0..2)", mqtt_qos);
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    LOG_INFO("iot-gateway starting...");
    LOG_INFO("config loaded from %s", config_path);
    LOG_INFO("server_port=%d thread_pool_size=%d "
             "heartbeat_interval=%d heartbeat_timeout=%d log_level=%s "
             "mqtt_broker=%s mqtt_port=%d mqtt_qos=%d",
             server_port, thread_pool_size, heartbeat_interval,
             heartbeat_timeout, log_level_str, mqtt_broker, mqtt_port,
             mqtt_qos);

    /* 初始化命令分发表（必须在工作线程启动前完成）。*/
    cmd_handlers_init();

    /* 初始化MQTT 客户端（连接 Mosquitto Broker）。*/
    if (mqtt_client_init(mqtt_broker, mqtt_port, NULL, mqtt_qos) != 0) {
        LOG_ERROR("failed to initialize MQTT client");
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    if (threadpool_init(thread_pool_size, worker_process_task) != 0) {
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
        /* 按初始化逆序清理：先停工作线程（可能正在调用 mqtt_publish），
         * 再销毁 MQTT 客户端，避免并发访问已销毁的单例。 */
        heartbeat_stop();
        epoll_worker_destroy();
        threadpool_destroy();
        mqtt_client_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    listen_fd = server_init((uint16_t)server_port);
    if (listen_fd < 0) {
        LOG_ERROR("failed to initialize server on port %d", server_port);
        /* 心跳线程已运行：先停心跳，再停工作线程，最后销毁 MQTT 客户端
         * （工作线程可能正在调用 mqtt_publish）。 */
        heartbeat_stop();
        epoll_worker_destroy();
        threadpool_destroy();
        mqtt_client_destroy();
        logger_destroy();
        config_free();
        return EXIT_FAILURE;
    }

    LOG_INFO("server listening on port %d", server_port);

    /* 永久阻塞；仅当发生致命 epoll_wait 错误时返回 -1。 */
    int rc = epoll_worker_run(listen_fd);

    /* 正常运行不会到达此处（致命错误路径）。按初始化逆序清理全部资源。 */
    heartbeat_stop();
    epoll_worker_destroy();
    threadpool_destroy();
    mqtt_client_destroy();
    conn_manager_destroy();
    logger_destroy();
    config_free();

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
