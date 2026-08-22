# C 单元测试：直接编译被测模块源码（不链接 iot-gateway 主程序），
# 验证 CRC16 / 协议编解码 / RingBuffer / 粘包拆包状态机。
add_executable(unit_tests
    test_core.c
    ${CMAKE_SOURCE_DIR}/src/protocol/protocol.c
    ${CMAKE_SOURCE_DIR}/src/protocol/packet_parser.c
    ${CMAKE_SOURCE_DIR}/src/utils/ringbuffer.c
    ${CMAKE_SOURCE_DIR}/src/utils/logger.c
)

target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_test(NAME unit_tests COMMAND unit_tests)

# config_parser 单元测试（含 F4：行内注释剥离 + 非法 int 回退告警，
# config_get_int 依赖 logger 输出告警，故一并编译 logger.c）。
add_executable(test_config
    test_config.c
    ${CMAKE_SOURCE_DIR}/src/utils/config_parser.c
    ${CMAKE_SOURCE_DIR}/src/utils/logger.c
)

target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_config PRIVATE Threads::Threads)

add_test(NAME unit_config COMMAND test_config)

# task_queue 单元测试（对象池 FIFO / 有界水位线 / 关闭语义）。
add_executable(test_task_queue
    test_task_queue.c
    ${CMAKE_SOURCE_DIR}/src/utils/task_queue.c
)

target_include_directories(test_task_queue PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_test(NAME unit_task_queue COMMAND test_task_queue)

# threadpool 单元测试（并发处理 / 优雅退出）。
add_executable(test_threadpool
    test_threadpool.c
    ${CMAKE_SOURCE_DIR}/src/utils/threadpool.c
    ${CMAKE_SOURCE_DIR}/src/utils/task_queue.c
)

target_include_directories(test_threadpool PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_threadpool PRIVATE Threads::Threads)

add_test(NAME unit_threadpool COMMAND test_threadpool)

# fd_util 单元测试（非阻塞设置）。
add_executable(test_fd_util
    test_fd_util.c
    ${CMAKE_SOURCE_DIR}/src/utils/fd_util.c
)

target_include_directories(test_fd_util PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_test(NAME unit_fd_util COMMAND test_fd_util)
