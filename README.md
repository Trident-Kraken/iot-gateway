# Linux Industrial IoT TCP Gateway

[![CI](https://github.com/Trident-Kraken/iot-gateway/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Trident-Kraken/iot-gateway/actions/workflows/ci.yml)

基于 **epoll ET + pthread 线程池** 的 Linux 工业物联网通信网关，支持自定义二进制协议解析与 MQTT 数据上报。

## 核心特性

- **高性能 I/O**：epoll 边缘触发（ET）模式 + while 循环 drain 至 EAGAIN
- **半解耦架构**：I/O 线程负责网络收发、拆包、CRC 校验；工作线程执行业务逻辑与 MQTT 上报
- **自定义二进制协议**：帧头 `0xAA55` | 长度(BE) | 命令字 | Payload | CRC16-CCITT-FALSE，状态机拆包处理粘包/拆包
- **函数指针分发表**：`g_cmd_handlers[256]` O(1) 命令分发，注册新命令无需修改分发骨架
- **MQTT 可靠上报**：管理线程驱动 `mosquitto_loop`（QoS 0/1/2 可配置）+ 断线退避重连 + 有界离线缓存（512 条，重连后冲刷）
- **优雅停机与可观测性**：`SIGTERM`/`SIGINT` → 关闭全部连接 → 逆序清理全部资源 → exit 0；支持 `SIGUSR1` 输出全链路运行时统计快照与 `tools/gateway_stats.sh` 运维诊断
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

**上行（设备 → 云端）**：

```
设备 → 自定义二进制帧 over TCP → 网关 epoll recv
  → RingBuffer → 状态机拆包 + CRC 校验
  → 任务队列（线程安全 FIFO）→ 工作线程取任务
  → 命令分发表 → JSON 解析 / 心跳处理
  → MQTT publish (QoS 0/1/2，可配置) → Mosquitto Broker
```

**下行（配置响应，CMD_CONFIG 0x03）**：

```
设备发 0x03 请求帧 → 工作线程 handle_config 编码默认配置 JSON
  → self-pipe 委托（fd + generation 双重匹配）→ epoll 线程非阻塞 send → 设备
```

**MQTT 不可用（断线缓冲）**：

```
mqtt_publish 失败/未连接 → 有界离线缓存（512 条，满淘汰最旧）
  → MQTT 管理线程退避重连成功 → 锁外冲刷缓存 → Broker
```

## 验证结果

| Verification | Result |
|---|---|
| CTest | **16/16 PASS**（9 单测/自检 + 7 集成；ASan / TSan 构建同为 16/16，零报告） |
| Valgrind | 0 leaks / 0 errors（unit_tests；全量另经 ASan+LeakSanitizer 覆盖） |
| Stress | 100 connections × 10 fps × 60s，客户端发送速率 **988~991 帧/秒**（目标 1000，达成率 >98.8%） |
| RTT Probe (0x03) | 负载下配置请求 RTT P99 **5.73 ms**（60/60 成功率 100%，覆盖全链路处理与下行回显） |
| Historical Baseline | 发送速率 991.1 / 990.2 / 988.3 帧/秒（历次基线回归实测，受宿主机调度小幅波动） |
| Soak | 30 min，20 设备 × 2Hz，~72k 帧，全程存活 |
| RSS Growth | +124 kB（6192→6316 kB，阈值 8192 kB） |
| 故障注入 / 离线缓存 / 竞态回归 / fd 耗尽 | 全部通过（见测试报告） |

> 以上结果是在对应测试环境和负载条件下获得，不代表无限负载或生产环境 SLA。
> 完整数据与逐日用例结果见 [docs/test-report.md](docs/test-report.md)。

## 性能参考

压测环境：WSL2 Ubuntu 24.04, GCC 13.3.0, Release (`-O2`)，本机回环 127.0.0.1，Mosquitto Broker 本地。

```bash
# 小规模快速验证（含 0x03 RTT 探针）
python3 tests/integration/stress_test.py -c 10 -f 10 -d 3 --probe-interval 0.5 -r /tmp/stress_small.txt

# 大规模基准压测（100 连接 × 10 fps，每秒 1 次 0x03 RTT 探针）
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 --probe-interval 1.0 -r /tmp/stress_large.txt
```

| 指标 | 小规模 (10 conn / 3s) | 大规模基准 (100 conn / 60s) | 口径说明 |
|------|------|------|------|
| 客户端发送速率 | 99.9 帧/秒 | 990.7 帧/秒 | 目标 100 / 1000 帧/秒，达成率 100.0% / 99.2%（非网关饱和吞吐） |
| 总发送帧数 | 300 | 59,500 | 目标 300 / 60,000 帧（1% 缺口为握手建立期与协程定时器漂移） |
| 发送错误率 | 0.00% | 0.00% | 仅统计发包端 socket 异常（连接重置/BrokenPipe） |
| 连接成功率 | 100% (10/10) | 100% (100/100) | 全部连接成功握手接入 |
| 客户端构帧及发送耗时 P50 | 0.081 ms | 0.083 ms | Python 组帧至写入本机内核发送缓冲区耗时，受虚拟化调度波动 |
| 客户端构帧及发送耗时 P99 | 0.354 ms | 0.312 ms | 同上（历史单次测得 0.241~0.509 ms，非网关端到端处理耗时） |
| 客户端构帧及发送耗时 最大 | 0.571 ms | 4.835 ms | 同上 |
| 配置请求往返时延 (0x03 RTT) P50 | 1.89 ms | 1.93 ms | 真实端到端 RTT：epoll 接收 → 拆包 → 队列 → 工作线程 → self-pipe → 下行发送 |
| 配置请求往返时延 (0x03 RTT) P99 | 2.05 ms | 5.73 ms | 同上（负载下全链路处理及跨线程调度真实时延） |
| 配置请求探针成功率 | 100% (6/6) | 100% (60/60) | 0 超时、0 错误 |

> **指标口径与边界说明**：
> 1. **客户端发送速率**：为发包端定频发包达成率，验证网关在 100 连接、1000 帧/秒生产基准负载下平稳运行，**并非网关饱和容量上限**。
> 2. **构帧及发送耗时**：区间仅包住发包端组包与写入内核 Socket 发送缓冲，不代表网关处理耗时；受 WSL2 宿主机调度影响方差较大，保留历史数据供参考。
> 3. **0x03 RTT 探针**：在背景 0x01 高负载下定时注入 `CMD_CONFIG`（0x03）请求并测量响应返回，真实穿透网关接收、拆包、排队、跨线程分发与下行全链路，是网关端到端处理时延的实测依据。

## 测试

```bash
# 1. 单元测试 + 集成测试（共 16 项：9 单测/自检 + 7 集成；集成依赖 mosquitto，环境受限时自动跳过）
ctest --test-dir build --output-on-failure

# 2. 集成用例单独运行（等价于 ctest 中的 integration_* 用例）
bash tests/integration/run_e2e.sh                      # E2E 全链路
python3 tests/integration/test_connection_lifecycle.py # 心跳超时/断连清理/优雅停机
python3 tests/integration/test_mqtt_offline_cache.py   # MQTT 离线缓存恢复
python3 tests/integration/test_config_validation.py    # 非法配置 fail-fast
python3 tests/integration/test_fault_injection.py      # 异常帧/超大长度注入
python3 tests/integration/test_stale_event_race.py     # 心跳超时边界 churn 竞态回归
python3 tests/integration/test_fd_exhaustion.py        # fd 耗尽（低 ulimit 连接风暴）

# 3. 压测与长时间运行（可选）
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress.txt
bash tests/scripts/run_soak.sh 30                      # 30 分钟 RSS 稳定性

# 4. 内存检查
valgrind --leak-check=full --error-exitcode=99 ./build/tests/unit/unit_tests

# 5. Sanitizer 全量回归（可选，较慢）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -g -O1" && cmake --build build && ctest --test-dir build
```

> 退出码约定：集成脚本 `0` 通过、`1` 失败、`77` 环境不满足（CTest 标记为 Skipped，不误报失败）。
> 完整结果见 [docs/test-report.md](docs/test-report.md)。

## 项目结构

```
iot-gateway/
├── CMakeLists.txt              # 根构建脚本（C11、-Wall -Wextra、add_subdirectory）
├── README.md                   # 本文件
├── AGENTS.md                   # 架构约定与编码规范（技术约束、模块边界、并发锁规范）
├── CHANGELOG.md                # 版本变更记录
├── LICENSE                     # MIT License
├── .github/workflows/          # CI：构建 + CTest + 端到端集成冒烟
├── config/gateway.ini          # 网关配置
├── include/                    # 公共头文件，按模块分组
│   ├── gateway/                #   server / epoll_worker / conn_manager / heartbeat
│   ├── protocol/               #   protocol / packet_parser
│   ├── business/               #   business_handler / mqtt_client
│   └── utils/                  #   ringbuffer / task_queue / threadpool / fd_util / logger / config_parser
├── src/                        # 实现文件，与 include/ 分组镜像
│   ├── gateway/  protocol/  business/  utils/
├── tests/
│   ├── unit/                   # C 单元测试（协议/环形缓冲/状态机）
│   └── integration/            # Python 设备模拟器 + 压测脚本 + CTest 自检用例
├── tools/                      # 运维/调试辅助脚本（gateway_stats.sh 运行时指标快照）
├── docs/                       # 架构、协议设计、部署、压测报告、Wireshark 指南等
├── logs/                       # 运行时日志（已 gitignore）
└── build/                      # CMake 构建产物（已 gitignore）
```

> `build/`、`logs/` 为构建/运行产物，由 `.gitignore` 排除；测试自检用例通过
> `ctest --test-dir build` 运行（Python 语法检查 + CRC16 已知向量校验）。

## License

MIT License — 详见 [LICENSE](LICENSE)。
