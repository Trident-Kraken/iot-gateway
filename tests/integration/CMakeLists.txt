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
)

add_test(
    NAME crc16_known_vector
    COMMAND ${Python3_EXECUTABLE} -c
            "import sys; sys.path.insert(0, '${CMAKE_CURRENT_SOURCE_DIR}'); from stress_test import crc16; assert crc16(b'123456789') == 0x29B1, hex(crc16(b'123456789'))"
)

# E2E 冒烟：mosquitto + 网关 + 模拟器 + MQTT 断言 + 0x03 下行。
# 依赖缺失/环境限制时脚本返回 77，CTest 标记为 SKIP（不算失败）。
add_test(
    NAME integration_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.sh
)
set_tests_properties(integration_e2e PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# 连接生命周期：心跳超时断开 / 半包断开 / fd 无泄漏（自带网关实例，端口 8081）。
add_test(
    NAME integration_connection_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_connection_lifecycle.py
)
set_tests_properties(integration_connection_lifecycle
                     PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# MQTT 离线缓存恢复：broker 停机期间消息入缓存，恢复后冲刷（自带 broker，端口 1884）。
add_test(
    NAME integration_mqtt_offline_cache
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_mqtt_offline_cache.py
)
set_tests_properties(integration_mqtt_offline_cache
                     PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

# 配置校验：非法配置 fail-fast（临时 cwd，不改仓库 config/）。
add_test(
    NAME integration_config_validation
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_config_validation.py
)
set_tests_properties(integration_config_validation
                     PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
