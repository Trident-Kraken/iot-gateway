# tests/unit: C 单元测试（直接编译被测模块源码，验证 CRC/编解码/RingBuffer/状态机）
# tests/integration: Python 设备模拟器 + 压测脚本 + CTest 自检用例
add_subdirectory(unit)
add_subdirectory(integration)
