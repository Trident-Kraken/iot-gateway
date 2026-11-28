# 架构设计文档 — iot-gateway

> 目标读者：代码审查者、项目维护者。

---

## 1. 模块依赖与数据流

```
┌───────────┐     accept     ┌──────────────┐    epoll_wait    ┌───────────────┐
│  server   │ ──────────────→│ epoll_worker │ ───────────────→│  packet_parser│
│ :8080     │   listen_fd    │ (1 I/O 线程) │   client_fd     │  状态机拆包    │
└───────────┘                └──────┬────────┘                │  + CRC 校验   │
                                    │ recv → RingBuffer       └──────┬────────┘
                                    │ (per-conn, 4KB)               │
                                    │                                │ 完整帧
                                    ▼                                ▼
                            ┌───────────────────────────────────────────┐
                            │          task_queue (有界 2048)           │
                            │  pthread_mutex + pthread_cond            │
                            └──────────────────┬────────────────────────┘
                                               │ task_queue_pop
                                               ▼
                            ┌───────────────────────────────────────────┐
                            │    threadpool (2-4 个工作线程)             │
                            │  → business_handler 命令分发表             │
                            │    ├─ 0x01 sensor_data  → JSON → MQTT     │
                            │    ├─ 0x02 heartbeat    → 刷新活跃时间     │
                            │    └─ 0x03 config       → 下行配置响应     │
                            └──────────────────┬────────────────────────┘
                                               │ mqtt_publish (QoS 0/1/2，可配置)
                                               ▼
                            ┌───────────────────────────────────────────┐
                            │         mqtt_client (libmosquitto)         │
                            │         无限重连, pthread_mutex 保护        │
                            └──────────────────┬────────────────────────┘
                                               │ mosquitto_publish
                                               ▼
                            ┌───────────────────────────────────────────┐
                            │          Mosquitto Broker :1883            │
                            │          Topic: sensor/data                │
                            └───────────────────────────────────────────┘

               ┌─────────────────────────────────────────────┐
               │  heartbeat 线程 (5s 间隔)                     │
               │  → conn_manager 遍历（持锁收集超时 fd）         │
               │  → 出锁后 epoll_worker_request_close(fd)      │
               │    （写 self-pipe，委托 epoll 线程清理）        │
               └─────────────────────────────────────────────┘

               ┌─────────────────────────────────────────────┐
               │  epoll 线程（读 self-pipe）                   │
               │  → connection_close(): epoll_ctl DEL →       │
               │    parser_unregister → conn_manager_remove → │
               │    close(fd) → 归还连接对象池（批末统一）      │
               └─────────────────────────────────────────────┘

               ┌─────────────────────────────────────────────┐
               │  mqtt 管理线程                                │
               │  → connect + mosquitto_loop + 断线重连        │
               │  → 重连后冲刷离线缓存（锁外 publish）          │
               │  → worker 线程 publish 永不阻塞               │
               └─────────────────────────────────────────────┘
```

数据流：设备发送自定义二进制帧 over TCP → epoll 线程 recv 写入 per-conn RingBuffer → 状态机逐字节拆包 + CRC 校验 → 完整帧入任务队列 → 工作线程取出 → 命令分发表 → JSON 解析 → MQTT 发布。

心跳超时清理链路：heartbeat 线程扫描连接链表 → 持锁收集超时 fd → 出锁后写 self-pipe → epoll 线程读 self-pipe → `connection_close()` 执行完整清理（epoll_ctl DEL → parser_unregister → conn_manager_remove → close → 归还连接对象池）。心跳线程不直接 close fd，避免与 epoll 线程的 I/O 操作竞争。

**陈旧事件防护**：`epoll_wait` 返回的 events[] 是调用时刻的快照，同一事件批内，先处理的 pipe 关闭消息可能已清理某连接，而该连接的事件仍留在批内后续位置。防护分两层：`connection.closed` 标志阻止重入清理；被关闭的槽位挂入 `g_conn_trash` 链，**批末**才归还对象池——保证批内槽位绝不被 accept 复用，陈旧事件即使到达也只能看到 `closed=1` 被跳过。回归用例：`integration_stale_event_race`（心跳超时边界 churn + 下行回显金丝雀）。

下行发送链路：self-pipe 除关闭请求外还承载**下行数据帧**（`PIPE_MSG_SEND`，如 `CMD_CONFIG` 配置响应）。工作线程的 `handle_config()` 编码帧后调用 `epoll_worker_request_send(fd, generation, frame, len)`，epoll 线程按 fd + generation 双重匹配确认连接仍是同一代，再非阻塞 `send()`（`MSG_NOSIGNAL`）。所有对客户端 fd 的 I/O 依旧收敛在 epoll 线程，工作线程只写 pipe，避免多线程并发读写同一 socket 与 fd 复用竞态。

MQTT 断连/重连链路：`mqtt_publish()` 发现连接断开或发布失败时，消息进入**有界离线缓存**（512 条，满淘汰最旧）而非丢弃 → MQTT 管理线程在后台执行阻塞式重连（带退避）→ 重连成功后锁外冲刷缓存中的消息（QoS 由 `mqtt_qos` 配置决定）。工作线程永不因 broker 不可达而阻塞。

## 2. 半解耦设计

I/O 线程负责网络收发、拆包、CRC 校验，在入队前丢弃坏帧；工作线程只拿到已验证的完整帧，专注业务逻辑和 MQTT 上报。这区别于"全解耦"（工作线程自己拆包/CRC）和"全耦合"（单线程全干）——拆包留在 I/O 线程是刻意的性能决策，让入队即完成一切校验。

## 3. 模块职责边界

| 模块 | 所属线程 | 输入 | 输出 | 持有锁 |
|---|---|---|---|---|
| **server** | main 线程 | config (port) | listen_fd | 无 |
| **epoll_worker** | I/O 线程 | listen_fd, client_fd | 触发 recv/accept（双向链表 + 1024 桶 O(1) 查找） | 无（epoll 内在同步） |
| **ringbuffer** | I/O 线程（写） / I/O 线程（读） | 原始字节流 | 双段 `memcpy` 读写 | 无（SPSC 无锁） |
| **packet_parser** | I/O 线程 | RingBuffer 字节流 | 完整帧 (on_frame_complete，1024 桶 O(1) 查找 + 载荷批量读取) | 无 |
| **protocol** | I/O 线程 / 工作线程 | 帧字节 / 帧字段 | CRC 验证 / 编码后的帧（含 `packet_encode_raw`） | 无（纯函数） |
| **conn_manager** | 多线程 | fd, last_active（`CLOCK_MONOTONIC`） | 1024 桶哈希表 CRUD | `g_mutex` |
| **heartbeat** | 独立心跳线程 | 连接表（`CLOCK_MONOTONIC`） | 超时 fd（两段分批收集，写 self-pipe） | `g_mutex`（遍历时持有） |
| **task_queue** | I/O 线程（写） / 工作线程（读） | struct task | 线程安全 FIFO | `g_mutex + g_cond` |
| **threadpool** | 工作线程 ×N | task_queue_pop | worker_process_task | 无（任务队列在 pop 内部持锁） |
| **business_handler** | 工作线程 | 完整帧 | JSON / MQTT publish | 无（MQTT 模块自行保护） |
| **mqtt_client** | 工作线程（并发） + MQTT 管理线程 | topic + payload | mosquitto_publish / 离线缓存 | `g_mqtt.lock`（状态检查/缓存入队快速持锁，网络 I/O 在管理线程） |
| **config_parser** | main 线程 | INI 文件 | 键值对查询 | 无（单次加载） |
| **logger** | 所有线程 | msg + level | 日志文件 | `g_log_mutex` |

### 锁的持有范围

```
conn_manager.lock  →  conn_manager_foreach (遍历期间持有, 微秒级)
task_queue.lock    →  push / pop (链表操作期间持有, 微秒级)
mqtt_client.lock   →  publish 状态检查 + 离线缓存入队 (快速持锁)
                       断连时消息入缓存, 重连成功后在管理线程锁外冲刷
                       网络 I/O（connect/loop/publish）均在管理线程, 不持锁
logger.lock        →  fprintf + fflush + 轮转检查 (每次日志写入持有)
```

## 4. 停机生命周期

`SIGTERM` / `SIGINT` 触发优雅停机，全程不使用非 async-signal-safe 调用：

```
信号到达
↓ 信号处理器：仅置 volatile sig_atomic_t 标志（epoll_worker_request_stop）
epoll 线程：epoll_wait 以 500ms 超时轮询感知标志
↓ 处理完当前事件批（conn_pool_flush_trash 归还批内关闭槽位）
关闭全部存活客户端连接（统一走 connection_close 清理路径）
↓ epoll_worker_run 返回 0
main：按初始化逆序清理（server_close → heartbeat_stop → threadpool_destroy →
      epoll_worker_destroy → mqtt_client_destroy（含离线缓存残留释放）→
      conn_manager_destroy → logger_destroy → config_free）
↓ exit 0
```

- 心跳线程的扫描间隔睡眠为 100ms 分段可中断，`heartbeat_stop()` 的 join 延迟约 100ms（而非整段间隔）。
- 工作线程由 `threadpool_destroy()` 广播唤醒后随队列排空退出，正在处理中的任务会先完成。
- 集成断言：`integration_connection_lifecycle` D 节（SIGTERM 后 exit=0）。

## 5. 可靠性设计汇总

| 故障/边界 | 机制（真实实现） | 验证 |
|---|---|---|
| TCP 半包/粘包 | per-conn RingBuffer + 五态拆包状态机逐字节消费 | unit_tests、integration_e2e |
| CRC 错误/非法帧头 | 状态机重置丢帧，坏帧在入队前被丢弃（不进任务队列） | unit_tests、integration_fault_injection |
| 超大长度字段（length overflow） | `PROTOCOL_MAX_PAYLOAD_LEN=1024` 拒绝 + `temp_buf` 运行时 `sizeof` 边界检查（双防线） | unit_tests、integration_fault_injection |
| RingBuffer 满（背压） | `rb_write` 失败 → 重置状态机并返回错误 → 上层关闭连接，绝不静默丢帧 | unit_tests（满拒写） |
| 心跳超时 | 两段收集（持锁仅收集 fd）→ self-pipe 委托 epoll 线程关闭 → fd+generation 双重匹配防误关复用 fd | integration_connection_lifecycle |
| 事件批内陈旧事件 | `closed` 重入防护 + trash 链批末归还（见 §1） | integration_stale_event_race |
| accept 瞬时错误（ECONNABORTED/EPROTO） | `server_accept` 内继续重试排空队列，不让剩余 backlog 饿死 | —（代码路径，随全量回归覆盖） |
| fd 耗尽（EMFILE/ENFILE） | reserve fd（`/dev/null`）换位排空 backlog 并逐个拒绝，防止 ET 模式下监听 fd 永久失活 | integration_fd_exhaustion（低 ulimit 120 连接风暴） |
| MQTT broker 断线 | 管理线程退避重连（1+rand()%3 秒）；期间消息入有界离线缓存（512 条，满淘汰最旧），重连后锁外冲刷 | integration_mqtt_offline_cache |
| 下行发送失败 | best-effort：非阻塞 send + `MSG_NOSIGNAL`，fd+generation 不匹配即丢弃（可重发） | integration_e2e（0x03 下行） |
| 停机信号 | 优雅退出全链路清理（见 §4） | integration_connection_lifecycle D 节 |

## 6. 关键边界

| 参数 | 值 | 说明 |
|---|---|---|
| RingBuffer 大小 | 4 KB / 连接 | SPSC 循环缓冲区 |
| 最大 Payload 长度 | 1024 字节 | 状态机 `temp_buf` 硬限制 |
| 任务队列容量 | 2048 条（任务节点对象池） | 固定上限，满则 `ENOMEM` 拒绝入队（水位线），epoll 关键路径零动态分配 |
| 心跳间隔 / 超时 | 5s / 15s | 超过超时阈值仍无任何数据则断开（任意收包均刷新活跃时间） |
| MQTT 重连 | 无限（带退避） | 退避 1 + rand()%3 秒，可被 destroy 打断 |
| MQTT QoS | 0/1/2（配置 mqtt_qos，默认 0） | 高频遥测默认 QoS0，允许少量丢失 |
| 帧头 | `0xAA 0x55` | 互补字节，误匹配概率 1/65536 |
| 最大连接数 | 1024（连接对象池） | 固定上限，超出后 accept 被拒绝并记日志 |
| reserve fd | 1 个（`/dev/null`） | EMFILE/ENFILE 时释放换位、排空拒绝积压连接后补开 |
| 停机轮询间隔 | 500 ms | `epoll_wait` 超时轮询停机标志（其余时间行为与阻塞一致） |
