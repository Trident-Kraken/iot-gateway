add_executable(iot-gateway
    gateway/main.c
    gateway/server.c
    gateway/epoll_worker.c
    gateway/conn_manager.c
    gateway/heartbeat.c
    protocol/protocol.c
    protocol/packet_parser.c
    business/business_handler.c
    business/mqtt_client.c
    utils/ringbuffer.c
    utils/task_queue.c
    utils/threadpool.c
    utils/config_parser.c
    utils/logger.c
)

target_include_directories(iot-gateway
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(iot-gateway
    PRIVATE
        Threads::Threads
        ${MOSQUITTO_LIBRARY}
)
