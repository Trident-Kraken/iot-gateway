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
)

add_test(
    NAME crc16_known_vector
    COMMAND ${Python3_EXECUTABLE} -c
            "import sys; sys.path.insert(0, '${CMAKE_CURRENT_SOURCE_DIR}'); from stress_test import crc16; assert crc16(b'123456789') == 0x29B1, hex(crc16(b'123456789'))"
)
