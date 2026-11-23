# Changelog

本项目所有值得记录的变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### 修复

- `fix(epoll)`: 修复 `conn_pool_acquire()` 在 1024 连接池满时误将 `g_conn_free == NULL` 判为未初始化并重新调用 `conn_pool_init()` 覆写全部活跃连接槽位的 P0 缺陷，改为独立 `g_conn_pool_initialized` 标志；调整 `handle_listen()` 先取槽再注册解析器/连接管理器；`EPOLLIN` 优先于 `EPOLLERR | EPOLLHUP` 处理避免短连接末帧丢失；`epoll_worker_run()` 致命错误退出路径同样统一清理全部活跃连接；`pipe_conn` 注册失败不再静默忽略；新增 `PIPE_BUF` 编译期断言与部分写/发送日志区分。
- `fix(main)`: 统一逆序停机清理 `cleanup_gateway()`（先停心跳与线程池再销毁 self-pipe，消除停机期间写 `g_close_pipe[1]` 的数据竞争，并补齐失败回滚缺失的 `conn_manager_destroy()`）；进程启动忽略 `SIGPIPE`；提前初始化 `logger` 以落盘配置解析告警，并补充 `heartbeat_interval`/`heartbeat_timeout` 范围校验。
- `fix(server)`: 新增 `server_close()` 清理 `g_reserve_fd`；`server_drain_backlog_reject()` 排空遇到 `EINTR`/`ECONNABORTED`/`EPROTO` 时继续排空；`server_accept()` 对先前重开失败的 `g_reserve_fd` 做静默懒补开并重置循环内 `client_addr_len`。
- `fix(business)`: 修复 `json_extract()` 同名 value 碰撞并严格按 `json_len` 限界扫描（消除每帧 `malloc/free`）；新增 RFC 8259 数字格式校验与 `device_id` 字符清洗防止拼出非法 JSON。
- `fix(mqtt)`: `mqtt_cache_flush()` 先冲刷完缓存再置 `connected=1`，冲刷中途断线时将未发出消息与新入队消息按序合并保留在离线缓存中不再丢弃；`rand()` 改为管理线程专属 `rand_r()`。
- `fix(utils/protocol)`: `logger_rotate()` 在 `rename()` 失败重开原文件时重置 `g_log_size` 防止文件开关风暴，并加锁保护 `logger_init`/`logger_destroy`；`protocol.c` 使用编译期常量 CRC16 查找表、`packet_checksum_verify()` 空指针校验与 `packet_encode()` payload 上限检查；`config_parser.c` 丢弃超长行尾部并用后值覆盖重复 key。
- `fix(tests/docs)`: 修复 `run_e2e.sh` 使用 Mosquitto 2.x 不存在的 `--pid-file` 参数；修复 `stress_test.py` 限速时钟基准混用及实时进度条恒为 0；清理集成脚本未关闭文件句柄；新增 `unit_business` 并注册 `unit_ringbuffer_wrap` 至 CTest，补齐 `Threads::Threads` 链接；修复 `docs/wireshark_guide.md` 过时导入路径。

## [1.0.0] - 2026-08-23

首个正式版本。核心链路（TCP 接入 → 状态机拆包 → 线程池分发 → MQTT 上报/下行）功能完整，通过单元测试、集成测试、ASan/TSan 回归、并发压测与 30 分钟 soak 稳定性验证。

- 性能：100 连接 × 10fps 压测客户端发送速率 988.3 帧/秒（目标 1000，达成率 98.8%），客户端构帧及发送耗时 P99 0.42ms，0 发送错误；后续补充 0x03 RTT 端到端探针 P99 5.73ms。
- 稳定性：30 分钟 soak ~7.2 万帧 RSS +124kB（阈值 8192kB）；SIGTERM 优雅停机全链路清理 exit 0。
- 质量门槛：Release / ASan / TSan 三构建 CTest 全绿；接入 GitHub Actions CI。

### 新增

- `feat(config)`: 实现 `CMD_CONFIG`（0x03）请求/响应式下行——设备发配置请求，网关回发默认设备配置 JSON 帧；下行经 self-pipe 委托 epoll 线程发送，fd + generation 双重匹配防 fd 复用误发（分发表 3/256）。
- `feat(mqtt)`: broker 不可达时消息进入有界离线缓存（512 条，满淘汰最旧），管理线程重连成功后锁外冲刷。
- `feat(mqtt)`: 新增 `mqtt_qos` 配置（0/1/2，默认 0）贯穿发布链路；注册 `on_publish` 回调记录 PUBACK/PUBCOMP，QoS1/2 确认与重传交由 `mosquitto_loop()` inflight 机制。
- `feat(pool)`: 任务节点固定对象池（2048）与连接上下文固定对象池（1024），`recv → 入队` 关键路径零动态分配；队列满时返回 `ENOMEM` 拒绝入队，连接池满时拒绝 accept 并记日志。
- `test(unit)`: 新增 `unit_tests`、`unit_config`、`unit_task_queue`、`unit_threadpool`、`unit_fd_util` C 单元测试；新增 `integration_e2e`、`integration_connection_lifecycle`、`integration_mqtt_offline_cache`、`integration_config_validation`、`integration_fault_injection`、`integration_stale_event_race`、`integration_fd_exhaustion` 集成测试。

### 修复

- `fix(epoll)`: 修复 epoll 事件批内陈旧事件导致连接对象池节点双重释放的 P0 缺陷：`struct connection` 增加 `closed` 标志阻止重入清理，关闭槽位挂入 `g_conn_trash` 在批末由 `conn_pool_flush_trash()` 统一归还空闲链，消除池空闲链自环。
- `fix(server)`: `server_accept` 对 `ECONNABORTED`/`EPROTO` 瞬时错误改为继续 accept 排空队列；`EMFILE`/`ENFILE` 时通过 reserve fd（`/dev/null`）排空并拒绝积压连接，避免监听 fd 在 ET 模式下饥饿挂起。
- `fix(shutdown)`: 新增 `SIGTERM`/`SIGINT` 优雅停机：信号处理器仅置 `volatile sig_atomic_t` 标志，`epoll_wait` 使用 500ms 超时轮询退出并统一走 `connection_close()` 清理全部连接，心跳线程改为 100ms 分段可中断睡眠，`main()` 执行完整逆序资源释放后返回 0。
- `fix(config)`: `config_load` 剥离行内 `#`/`;` 注释；`config_get_int` 增加 `INT_MIN`/`INT_MAX` 溢出检查与空值处理，非法整数回退默认值时输出 `LOG_ERROR` 告警。
- `fix(protocol)`: RingBuffer 满则拒绝写入并报错关闭连接（背压）；`packet_decode` 增加 `payload_len > PROTOCOL_MAX_PAYLOAD_LEN` 防御；self-pipe 消息写入前清零消除未初始化字节读取。
- `fix(network)`: 引入连接代际号（`generation`）双重匹配，消除心跳超时关闭与 fd 复用之间的误关/误发竞态；self-pipe 异常事件不进入对象池释放路径。
- `fix(mqtt)`: 新增独立 MQTT 管理线程驱动 `mosquitto_loop()`，解决长连接 keepalive 与发布阻塞工作线程问题。
- `fix(main)`: 统一失败路径与正常停机清理顺序，在停止工作线程后再销毁 MQTT 客户端；校验 `server_port`/`thread_pool_size`/`mqtt_port`/`mqtt_qos` 配置范围，非法值 fail-fast 退出。
- `fix(logger)`: 日志目录缺失时降级至 stderr，避免网关无法启动。
- `fix(business)`: `json_extract` 字符串值按 `json_len` 限界查找，消除对 NUL 结尾的隐式依赖。

### 重构

- `refactor(epoll)`: 将 `is_listen` 三态整型改为 `enum conn_role`（`CLIENT`/`LISTEN`/`PIPE`）；self-pipe 接收端复用 `handle_pipe_messages()`。
- `refactor(protocol)`: `struct parse_ctx` 转为不透明句柄，移除零调用方接口，`PARSER_MAX_*` 收敛至实现文件。
- `refactor(threadpool)`: 任务处理改为回调注入（`threadpool_init(num, task_handler)`），消除 `utils → business` 反向依赖；`on_frame_complete` 移入 `epoll_worker`。
- `refactor(utils)`: 抽取 `fd_set_nonblocking()` 共享工具消除重复实现；错误日志统一走 `LOG_ERROR`，传感器高频上报日志降为 `DEBUG`。
- `refactor(comment)`: 修复多处源文件注释 UTF-8 乱码，统一文件编码与 Doxygen 注释风格。

### 文档

- 同步架构、协议、部署与测试文档（QoS 0/1/2、有界队列、对象池上限、心跳任意收包刷新语义与设计权衡说明）。

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
