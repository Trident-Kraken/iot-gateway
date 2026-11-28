# Python 工具链自检用例（无需运行网关）：
#   1. 两个脚本语法检查
#   2. CRC16-CCITT-FALSE 已知向量校验（与 src/protocol.c 保持一致的防线）
#
# 运行方式：
#   ctest --test-dir build
#
# 注意：device_simulator.py / stress_test.py 需要先启动网关
#   （./build/src/iot-gateway）才能做真实联调，故不注册为常驻用例。

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
    NAME python_syntax_check
    COMMAND ${Python3_EXECUTABLE} -m py_compile
            ${CMAKE_CURRENT_SOURCE_DIR}/device_simulator.py
            ${CMAKE_CURRENT_SOURCE_DIR}/stress_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/config_downlink_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test_connection_lifecycle.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mqtt_offline_cache.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test_config_validation.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test_fault_injection.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test_stale_event_race.py
            ${CMAKE_CURRENT_SOURCE_DIR}/test_fd_exhaustion.py
)

add_test(
    NAME crc16_known_vector
    COMMAND ${Python3_EXECUTABLE} -c
            "import sys; sys.path.insert(0, '${CMAKE_CURRENT_SOURCE_DIR}'); from stress_test import crc16; assert crc16(b'123456789') == 0x29B1, hex(crc16(b'123456789'))"
)

add_test(
    NAME shell_syntax_check
    COMMAND bash -n
            ${CMAKE_SOURCE_DIR}/tools/gateway_stats.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.sh
            ${CMAKE_SOURCE_DIR}/tests/scripts/run_soak.sh
)

# E2E 冒烟：mosquitto + 网关 + 模拟器 + MQTT 断言 + 0x03 下行。
# 依赖缺失/环境限制时脚本返回 77，CTest 标记为 SKIP（不算失败）。
add_test(
    NAME integration_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.sh
)
set_tests_properties(integration_e2e PROPERTIES
                     TIMEOUT 120
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")

# 连接生命周期：心跳超时断开 / 半包断开 / fd 无泄漏（自带网关实例，端口 8081）。
add_test(
    NAME integration_connection_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_connection_lifecycle.py
)
set_tests_properties(integration_connection_lifecycle PROPERTIES
                     TIMEOUT 60
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")

# MQTT 离线缓存恢复：broker 停机期间消息入缓存，恢复后冲刷（自带 broker，端口 1884）。
add_test(
    NAME integration_mqtt_offline_cache
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_mqtt_offline_cache.py
)
set_tests_properties(integration_mqtt_offline_cache PROPERTIES
                     TIMEOUT 90
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")

# 配置校验：非法配置 fail-fast（临时 cwd，不改仓库 config/）。
add_test(
    NAME integration_config_validation
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_config_validation.py
)
set_tests_properties(integration_config_validation PROPERTIES
                     TIMEOUT 60
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")

# 故障注入：4 类异常帧 + 超大长度字段，断言网关存活且正常帧仍可达 MQTT。
add_test(
    NAME integration_fault_injection
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fault_injection.py
)
set_tests_properties(integration_fault_injection PROPERTIES
                     TIMEOUT 120
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")

# 超时关闭竞态 churn 回归：心跳超时边界突发数据/断连，
# 每轮以 CMD_CONFIG 下行回显验证连接池/连接链表完整性（端口 8083）。
add_test(
    NAME integration_stale_event_race
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_stale_event_race.py
)
set_tests_properties(integration_stale_event_race PROPERTIES
                     TIMEOUT 90
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")

# fd 耗尽处理回归：低 ulimit 网关 + 120 连接风暴，
# 断言耗尽期新连接被及时拒绝（EMFILE reserve-fd 排空）且释放后完全恢复
# （端口 8085）。
add_test(
    NAME integration_fd_exhaustion
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fd_exhaustion.py
)
set_tests_properties(integration_fd_exhaustion PROPERTIES
                     TIMEOUT 90
                     SKIP_RETURN_CODE 77
                     ENVIRONMENT "IOT_GATEWAY_BIN=$<TARGET_FILE:iot-gateway>")
