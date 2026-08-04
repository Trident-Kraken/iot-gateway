# Linux Industrial IoT TCP Gateway

基于 **epoll ET + pthread 线程池** 的 Linux 工业物联网通信网关，支持自定义二进制协议解析与 MQTT 数据上报。

## 核心特性

- **高性能 I/O**：epoll 边缘触发（ET）模式 + while 循环 drain 至 EAGAIN
- **半解耦架构**：I/O 线程负责网络收发、拆包、CRC 校验；工作线程执行业务逻辑与 MQTT 上报
- **自定义二进制协议**：帧头 `0xAA55` | 长度(BE) | 命令字 | Payload | CRC16-CCITT-FALSE，状态机拆包处理粘包/拆包
- **函数指针分发表**：`g_cmd_handlers[256]` O(1) 命令分发，注册新命令无需修改分发骨架
- **压测验证**：Python asyncio 压测脚本，支持可配置并发连接数与发送频率

## 技术栈

C11 · epoll · pthread · RingBuffer · 自定义二进制协议 · CRC16 · MQTT(QoS 0/1/2) · libmosquitto · CMake

## 快速开始

```bash
# 1. 安装依赖
sudo apt install cmake gcc libmosquitto-dev mosquitto mosquitto-clients

# 2. 编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 3. 启动 Mosquitto Broker
mosquitto -d

# 4. 启动网关
./build/src/iot-gateway

# 5. 启动设备模拟器（另开终端）
python3 tests/integration/device_simulator.py --devices 2 --freq 2

# 6. 订阅 MQTT 消息验证（另开终端）
mosquitto_sub -t 'sensor/data' -h localhost -p 1883

# 7. 压测（可选）
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60
```

## 数据流

```
设备 → 自定义二进制帧 over TCP → 网关 epoll recv
  → RingBuffer → 状态机拆包 + CRC 校验
  → 任务队列（线程安全 FIFO）→ 工作线程取任务
  → 命令分发表 → JSON 解析 / 心跳处理
  → MQTT publish (QoS 0/1/2，可配置) → Mosquitto Broker
```

## 性能参考

压测环境：WSL2 Ubuntu 22.04, GCC 13.3.0, Release (`-O2`)，本机回环 127.0.0.1，Mosquitto Broker 本地。

```bash
# 小规模快速验证
python3 tests/integration/stress_test.py -c 10 -f 10 -d 3 -r /tmp/stress_small.txt

# 大规模正式压测
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress_large.txt
```

| 指标 | 小规模 (10 conn / 3s) | 大规模 (100 conn / 60s) |
|------|------|------|
| 吞吐量 (QPS) | 99.9 | 991.1 |
| 总帧数 | 300 | 59,500 |
| P50 延迟 | 0.044 ms | 0.043 ms |
| P99 延迟 | 0.370 ms | 0.241 ms |
| 平均延迟 | 0.065 ms | 0.061 ms |
| 最大延迟 | 0.444 ms | 4.161 ms |
| 错误率 | 0.00% | 0.00% |
| 连接成功率 | 100% (10/10) | 100% (100/100) |

完整压测报告见 [docs/test-report.md](docs/test-report.md)。

## 测试

```bash
# 1. 单元测试 + 集成测试（集成用例依赖 mosquitto，环境受限时自动跳过）
ctest --test-dir build --output-on-failure

# 2. 集成用例单独运行（等价于 ctest 中的 integration_* 用例）
bash tests/integration/run_e2e.sh                      # E2E 全链路
python3 tests/integration/test_connection_lifecycle.py # 心跳超时/断连清理
python3 tests/integration/test_mqtt_offline_cache.py   # MQTT 离线缓存恢复
python3 tests/integration/test_config_validation.py    # 非法配置 fail-fast
python3 tests/integration/test_fault_injection.py      # 异常帧/超大长度注入

# 3. 压测与长时间运行（可选）
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress.txt
bash tests/scripts/run_soak.sh 30                      # 30 分钟 RSS 稳定性

# 4. 内存检查
valgrind --leak-check=full --error-exitcode=99 ./build/tests/unit/unit_tests
```

> 退出码约定：集成脚本 `0` 通过、`1` 失败、`77` 环境不满足（CTest 标记为 Skipped，不误报失败）。
> 完整结果见 [docs/test-report.md](docs/test-report.md)。

## 项目结构

```
iot-gateway/
├── CMakeLists.txt              # 根构建脚本（C11、-Wall -Wextra、add_subdirectory）
├── CHANGELOG.md                # 版本变更记录
├── LICENSE                     # MIT License
├── config/gateway.ini          # 网关配置
├── include/                    # 公共头文件，按模块分组
│   ├── gateway/                #   server / epoll_worker / conn_manager / heartbeat
│   ├── protocol/               #   protocol / packet_parser
│   ├── business/               #   business_handler / mqtt_client
│   └── utils/                  #   ringbuffer / task_queue / threadpool / logger / config_parser
├── src/                        # 实现文件，与 include/ 分组镜像
│   ├── gateway/  protocol/  business/  utils/
├── tests/
│   ├── unit/                   # C 单元测试（协议/环形缓冲/状态机）
│   └── integration/            # Python 设备模拟器 + 压测脚本 + CTest 自检用例
├── tools/                      # 运维/调试辅助脚本占位
├── docs/                       # 架构、协议设计、部署、压测报告、Wireshark 指南等
├── logs/                       # 运行时日志（已 gitignore）
└── build/                      # CMake 构建产物（已 gitignore）
```

> `build/`、`logs/` 为构建/运行产物，由 `.gitignore` 排除；测试自检用例通过
> `ctest --test-dir build` 运行（Python 语法检查 + CRC16 已知向量校验）。

## License

MIT License — 校招/课程设计演示。
