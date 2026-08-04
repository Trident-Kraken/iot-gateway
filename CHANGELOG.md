# Changelog

本项目所有值得记录的变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### 修复（Day2 代码审查）

- `fix(protocol)`: RingBuffer 满则拒绝写入，修复解析器背压检查形同虚设导致的静默丢帧（AGENTS §5.3）。
- `fix(network)`: 连接代际号（generation）双重匹配，修复心跳超时关闭时 fd 复用误关新连接的竞态。
- `fix(mqtt)`: 新增 MQTT 管理线程驱动 `mosquitto_loop()`，修复长连接无 PINGREQ/keepalive 与发布依赖阻塞 socket 写的问题；publish 改为锁外 QoS0 入队快速返回。
- `fix(config)`: 校验 server_port / thread_pool_size / mqtt_port 范围，非法配置拒绝启动（原静默截断 + 日志误导）。
- `fix(logger)`: 日志目录缺失时降级 stderr，网关不再因 `logs/` 不存在而启动失败。

## [0.1.0] - 2026-08-04

### 新增

- Linux 工业物联网 TCP 通信网关骨架：epoll ET I/O 线程 + 工作线程池（半解耦架构）。
- 自定义二进制协议：`0xAA55 | 长度(BE) | 命令字 | Payload | CRC16-CCITT-FALSE`，状态机拆包处理粘包/拆包。
- 命令分发表 `g_cmd_handlers[256]`：`CMD_SENSOR_DATA`（JSON → MQTT 上报 `sensor/data`）、`CMD_HEARTBEAT`。
- MQTT QoS0 发布 + 异步重连线程（fast-fail 不阻塞工作线程）。
- Python 设备模拟器（含异常帧注入）与 asyncio 并发压测脚本。
- CTest 自检用例：Python 语法检查 + CRC16 已知向量校验。

### 仓库结构整理

- `src/`、`include/` 按职责分组：`gateway/`、`protocol/`、`business/`、`utils/`。
- `tests/` 拆分为 `unit/`（占位）与 `integration/`（模拟器/压测）。
- 新增 `tools/`（占位）、`docs/protocol-design.md`、`docs/deployment.md`、`docs/test-report.md`。
- 新增 `.gitignore`、`LICENSE`（MIT），解除对 `build/`、`logs/`、`__pycache__` 的版本跟踪。
- 新增 GitHub Actions CI（`.github/workflows/ci.yml`）：build + CTest 自检 + 网关/MQTT 端到端集成冒烟。

### 修复

- 修复 7 个源文件注释中的 UTF-8 乱码（273 处损坏字符，编码统一为 UTF-8）。
