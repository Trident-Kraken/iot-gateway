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

# config_parser 单元测试。
add_executable(test_config
    test_config.c
    ${CMAKE_SOURCE_DIR}/src/utils/config_parser.c
)

target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_test(NAME unit_config COMMAND test_config)
