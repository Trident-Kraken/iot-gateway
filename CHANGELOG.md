# Changelog

本项目所有值得记录的变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### 修复（Day4 代码审查 — P1：F2/F3/F4）

- `fix(server)`: **F2** — `server_accept` 对 `ECONNABORTED`/`EPROTO` 由"记日志退出"改为继续 accept（瞬时错误：该连接已被对端放弃，内核已移出队列，后方可能还有待处理连接；ET 模式下提前跳出会使剩余 backlog 连接饿死）；`EMFILE`/`ENFILE` 时通过 reserve fd（启动时预开的 `/dev/null`）排空并拒绝积压连接——否则 backlog 不清空、监听 fd 在 ET 模式下永不再触发事件，新设备全部无声挂起。新增 `test_fd_exhaustion.py`（低 ulimit 网关 + 120 连接风暴，端口 8084）：断言耗尽期间网关存活、新连接被**及时拒绝**而非无声挂起、释放后完全恢复；已在未修复二进制上验证该测试确实失败（区分度确认）。
- `fix(shutdown)`: **F3** — SIGTERM/SIGINT 优雅停机。信号处理器仅置 `volatile sig_atomic_t` 标志（async-signal-safe），`epoll_wait` 由永久阻塞改为 500ms 超时轮询感知停机请求；退出时经统一 `connection_close()` 路径关闭全部存活客户端连接后返回 0，`main()` 既有逆序清理序列（心跳/线程池/MQTT/conn_manager/logger/config）由死代码变为真实执行路径，退出码 0。心跳线程 `sleep()` 改 100ms 分段可中断睡眠，停机 join 延迟从最长一个扫描间隔降到 ~100ms。`test_connection_lifecycle.py` 新增 D 节断言 SIGTERM 后 exit=0（未修复二进制上实测 exit=-15，区分度确认）。
- `fix(config)`: **F4** — `config_load` 剥离行内注释（值中首个 `#`/`;` 起，此前 `log_level=INFO ; prod` 会把注释存进值导致级别解析静默失败）；`config_get_int` 非法整数回退默认值时输出 `LOG_ERROR` 告警（原静默回退，误配置无任何可观测信号），并修复空值 `"key="` 被 `strtol` 解析为 0 而非回退默认值的边界。`unit_config` 新增 3 例回归。

### 修复（Day4 代码审查 — P0）

- `fix(epoll)`: 修复 epoll 事件批内陈旧事件导致连接对象池节点双重释放的 P0 缺陷。epoll_wait 返回的 events[] 是快照，同批内先处理的 pipe 消息（心跳超时关闭）可能已释放某连接槽位，而该连接的陈旧 EPOLLIN/EPOLLHUP 事件仍留在批内后续位置，二次 `connection_close()` 会使对象池空闲链自环，后续所有新连接共享同一上下文（池整体腐坏）。修复方案：
  1. `struct connection` 增加 `closed` 标志，`connection_close()` 首次进入即置位，重入直接返回；
  2. 事件分发循环中跳过 `closed=1` 的客户端连接（陈旧事件防护）；
  3. 关闭的连接槽挂入 `g_conn_trash` 链，批末由 `conn_pool_flush_trash()` 统一归还空闲链，保证批内槽位不被 accept 复用（陈旧事件即使到达也只能看到 `closed=1` 被跳过，不会误伤复用该槽位的新连接）。
- `test(integration)`: 新增 `test_stale_event_race.py` 竞态回归用例（端口 8083）：10 轮「连接→短暂活跃→心跳超时边界突发数据→立即断连」churn，每轮以 CMD_CONFIG 下行回显验证连接池/连接链表完整性，末尾双连接并发回显断言无对象池自环。

### 修复（Day2 代码审查）

- `fix(protocol)`: RingBuffer 满则拒绝写入，修复解析器背压检查形同虚设导致的静默丢帧（AGENTS §5.3）。
- `fix(network)`: 连接代际号（generation）双重匹配，修复心跳超时关闭时 fd 复用误关新连接的竞态。
- `fix(mqtt)`: 新增 MQTT 管理线程驱动 `mosquitto_loop()`，修复长连接无 PINGREQ/keepalive 与发布依赖阻塞 socket 写的问题；publish 改为锁外 QoS0 入队快速返回。
- `fix(config)`: 校验 server_port / thread_pool_size / mqtt_port 范围，非法配置拒绝启动（原静默截断 + 日志误导）。
- `fix(logger)`: 日志目录缺失时降级 stderr，网关不再因 `logs/` 不存在而启动失败。

### 新增（Day3 P0：MQTT 最小可用收尾）

- `feat(config)`: 实现 `CMD_CONFIG`（0x03）请求/响应式下行——设备发配置请求，网关回发默认设备配置 JSON 帧；下行经 self-pipe 委托 epoll 线程发送，fd + generation 双重匹配防 fd 复用误发（分发表 3/256）。
- `feat(mqtt)`: broker 不可达时消息进入有界离线缓存（512 条，满淘汰最旧），管理线程重连成功后锁外冲刷，不再 fast-fail 丢消息。
- `feat(mqtt)`: 新增 `mqtt_qos` 配置（0/1/2，默认 0）贯穿发布链路；注册 `on_publish` 回调记录 PUBACK/PUBCOMP，QoS1/2 确认与重传交由 `mosquitto_loop()` inflight 机制。
- `fix(epoll)`: self-pipe 消息结构体写入前清零，消除向管道写入未初始化字节（valgrind 告警）。

### 新增（Day3 P1：关键路径对象池）

- `feat(pool)`: 任务节点固定对象池（2048），`task_queue` push/pop/release 零动态分配；队列从无界链表变为有界（满则 `ENOMEM` 拒绝入队，实现架构文档建议的水位线）。
- `feat(pool)`: 连接上下文固定对象池（1024），epoll 线程 accept/close 零动态分配；超出容量拒绝 accept 并记日志。

### 修复（Day2 代码 Review）

- `fix(main)`: 统一 teardown 逆序清理（先停工作线程再销毁 MQTT 客户端），修复失败路径并发访问已销毁单例的 use-after-free。
- `fix(main)`: `epoll_worker_run()` 致命错误时返回 `EXIT_FAILURE`，并补齐 `mqtt_client_destroy()`/`conn_manager_destroy()` 清理。
- `fix(config)`: `config_get_int` 增加 `INT_MIN/INT_MAX` 溢出检查，超范围值不再静默截断。
- `fix(business)`: `json_extract` 字符串值按 `json_len` 限界查找，消除对调用方 NUL 结尾的隐式依赖。
- `fix(packet_decode)`: 增加 `payload_len > PROTOCOL_MAX_PAYLOAD_LEN` 防御，与状态机上限一致。
- `fix(epoll)`: self-pipe 收到错误/挂起事件时不再走 `connection_close`（其上下文非对象池分配，原先存在误入空闲链的隐患）。

### 重构（Day2 代码 Review）

- `refactor(server)`: 错误路径统一走 `LOG_ERROR`，移除引导期后残留的 `fprintf(stderr)`。
- `refactor(log)`: 补充 `g_log_level` 全局理由与线程安全不变量注释；传感器上报日志降为 `DEBUG`（消除 991 QPS 下的日志刷屏）。
- `refactor(comment)`: 还原全仓被压扁的多行注释并修复 `logger.c` 残留 U+FFFD 乱码（与 v1.7 UTF-8 声明对齐）。
- `refactor(protocol)`: `struct parse_ctx` 转为不透明句柄；删除零调用者 API（`packet_parser_get_ctx`/`crc16_init`）；`PARSER_MAX_*` 移入实现文件。
- `refactor(threadpool)`: 任务处理改为回调注入（`threadpool_init(num, task_handler)`），消除 utils → business 反向依赖；`on_frame_complete` 移入 `epoll_worker`。
- `refactor(epoll)`: `is_listen` 三态 int 改为 `enum conn_role`（CLIENT/LISTEN/PIPE）。
- `refactor(utils)`: 抽取 `fd_set_nonblocking` 共享工具，消除 server/epoll_worker 重复实现。

### 新增（Day2 代码 Review）

- `test(unit)`: 新增 C 单元测试（`tests/unit`，56 断言）：CRC16 已知向量、协议编解码（含超大 Payload 防御）、RingBuffer 环绕/满拒写、粘包拆包状态机（分片/粘包/坏帧恢复）。

### 文档（Day2 代码 Review）

- 文档与实现同步：QoS 0/1/2（默认 0）、无限重连 + 退避、任务队列有界 2048、`PROTOCOL_MAX_FRAME_SIZE=1031`、AGENTS §2.3 对象池声明修正、心跳语义（任意收包刷新活跃时间）。
- `docs/interview_qa.md` 新增第 11 问：为什么网关没有全局状态机。

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
