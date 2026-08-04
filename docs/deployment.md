# 部署文档 — iot-gateway

> 目标读者：部署/运维者。对应实现：`src/gateway/main.c`（初始化、启动、资源释放）、
> `config/gateway.ini`（运行时配置）、`include/utils/logger.h`（日志）。

---

## 1. 环境与依赖

- Linux 用户空间，POSIX API；编译器要求支持 C11。
- 依赖包（Debian/Ubuntu）：

```bash
sudo apt install cmake gcc libmosquitto-dev mosquitto mosquitto-clients
```

| 组件 | 用途 |
|---|---|
| `libmosquitto-dev` | MQTT 客户端库（编译期） |
| `mosquitto` | MQTT Broker（运行期） |
| `mosquitto-clients` | 订阅验证工具 |

## 2. 构建

```bash
# Release（默认）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Debug
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

产物：`build/src/iot-gateway`。构建选项见根 `CMakeLists.txt`（`-Wall -Wextra`，C11）。

## 3. 配置

`config/gateway.ini`（INI 格式，`config_parser` 解析）：

| 节 | 键 | 默认 | 说明 |
|---|---|---|---|
| `[server]` | `server_port` | 8080 | TCP 监听端口 |
| `[thread_pool]` | `thread_pool_size` | 4 | 工作线程数（2~4 推荐） |
| `[heartbeat]` | `heartbeat_interval` | 5 | 心跳扫描间隔（秒） |
| `[heartbeat]` | `heartbeat_timeout` | 15 | 心跳超时断开（秒），3 次未收到即断开 |
| `[log]` | `log_level` | INFO | 日志级别：DEBUG/INFO/ERROR |
| `[mqtt]` | `mqtt_broker` | localhost | Broker 地址 |
| `[mqtt]` | `mqtt_port` | 1883 | Broker 端口 |
| `[mqtt]` | `mqtt_qos` | 0 | 发布 QoS（0/1/2；QoS1/2 确认与重传由 `mosquitto_loop` 处理） |

## 4. 启动

```bash
# 1. 启动 Mosquitto Broker
mosquitto -d

# 2. 启动网关（前台）
./build/src/iot-gateway

# 3. 验证监听端口
ss -tlnp | grep 8080
```

日志写入 `logs/gateway.log`（格式 `[YYYY-MM-DD HH:MM:SS] [LEVEL] [file:line] message`，支持轮转）。

## 5. 端到端验证

```bash
# 另开终端：启动设备模拟器（2 台设备，每台 2Hz）
python3 tests/integration/device_simulator.py --devices 2 --freq 2

# 另开终端：订阅 MQTT 验证数据可消费
mosquitto_sub -t 'sensor/data' -h localhost -p 1883
```

完整抓包验证步骤见 [docs/wireshark_guide.md](wireshark_guide.md)。

## 6. 常见问题

| 现象 | 排查 |
|---|---|
| 网关启动失败，端口占用 | 换 `server_port` 或 `ss -tlnp` 查占用进程 |
| MQTT 上报无数据 | 确认 broker 已启动：`mosquitto -d`；`mosquitto_sub -t 'sensor/data'` 应能收到 |
| 连接被周期性断开 | 心跳超时（默认 15s）：确认设备按协议发送 `CMD_HEARTBEAT` |
| 日志无输出 | 检查 `logs/` 目录可写、`log_level` 配置 |

## 7. 压测

```bash
# 小规模快速验证（10 连接 / 3s）
python3 tests/integration/stress_test.py -c 10 -f 10 -d 3

# 大规模正式压测（100 连接 / 60s）
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress_large.txt
```

历史压测数据见 [docs/test-report.md](test-report.md)。
