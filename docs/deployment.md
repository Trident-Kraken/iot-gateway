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
| `[heartbeat]` | `heartbeat_timeout` | 15 | 心跳超时断开（秒），超过阈值无任何数据则断开（任意收包均刷新活跃时间） |
| `[log]` | `log_level` | INFO | 日志级别：DEBUG/INFO/ERROR |
| `[mqtt]` | `mqtt_broker` | localhost | Broker 地址 |
| `[mqtt]` | `mqtt_port` | 1883 | Broker 端口 |
| `[mqtt]` | `mqtt_qos` | 0 | 发布 QoS（0/1/2；QoS1/2 确认与重传由 `mosquitto_loop` 处理） |

> 配置语法：支持整行 `#`/`;` 注释与**行内注释**（值中首个 `#` 或 `;` 起视为注释；值本身含这两个字符时不能写行内注释）。整数值非法（非数字/空值/超范围）时回退默认值并在日志中记 `ERROR` 告警；`server_port`/`thread_pool_size`/`mqtt_port`/`mqtt_qos` 超出合法范围则拒绝启动（fail-fast）。

## 4. 启动与停止

```bash
# 1. 启动 Mosquitto Broker
mosquitto -d

# 2. 启动网关（前台，可选传入自定义配置文件路径）
./build/src/iot-gateway [config_path]

# 3. 验证监听端口
ss -tlnp | grep 8080
```

日志写入 `logs/gateway.log`（格式 `[YYYY-MM-DD HH:MM:SS] [LEVEL] [file:line] message`，支持自动创建单层父目录与轮转）。

### 运行时指标快照（SIGUSR1）与诊断脚本

```bash
# 向运行中的网关触发一次统计日志快照（active/frames_ok/q_depth/dl_ok/mqtt_pub 等）
kill -USR1 <pid>

# 或使用一键诊断脚本查看进程线程/RSS/FD 数及最新统计行
bash tools/gateway_stats.sh [pid] [log_path]
```

### 停止（优雅停机）

```bash
kill -TERM <pid>    # 或 Ctrl+C（SIGINT）
```

网关收到 `SIGTERM`/`SIGINT` 后执行优雅停机：关闭全部客户端连接 → 停心跳线程 → 排空并停止工作线程 → 断开 MQTT（释放离线缓存残留）→ 释放其余资源 → **exit 0**。正常情况下 1 秒内完成。仅 `SIGKILL` 会跳过清理直接终止（离线缓存与 inflight 消息丢失）。

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
| 连接被周期性断开 | 心跳超时（默认 15s）：确认设备持续上报数据或按协议发送 `CMD_HEARTBEAT`（任意收包都会刷新活跃时间） |
| 日志无输出 | 检查 `logs/` 目录可写、`log_level` 配置 |

## 7. 压测

```bash
# 小规模快速验证（10 连接 / 3s）
python3 tests/integration/stress_test.py -c 10 -f 10 -d 3

# 大规模正式压测（100 连接 / 60s）
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress_large.txt
```

历史压测数据见 [docs/test-report.md](test-report.md)。
