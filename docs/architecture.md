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
                            │          task_queue (无界 FIFO)           │
                            │  pthread_mutex + pthread_cond            │
                            └──────────────────┬────────────────────────┘
                                               │ task_queue_pop
                                               ▼
                            ┌───────────────────────────────────────────┐
                            │    threadpool (2-4 个工作线程)             │
                            │  → business_handler 命令分发表             │
                            │    ├─ 0x01 sensor_data  → JSON → MQTT     │
                            │    └─ 0x02 heartbeat    → 刷新活跃时间     │
                            └──────────────────┬────────────────────────┘
                                               │ mqtt_publish (QoS 0)
                                               ▼
                            ┌───────────────────────────────────────────┐
                            │         mqtt_client (libmosquitto)         │
                            │         3 次重连, pthread_mutex 保护        │
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
               │    close(fd) → free(conn)                    │
               └─────────────────────────────────────────────┘

               ┌─────────────────────────────────────────────┐
               │  mqtt 异步重连线程                            │
               │  → 条件变量唤醒后阻塞式重连                   │
               │  → worker 线程 publish 永不阻塞（fast-fail）  │
               └─────────────────────────────────────────────┘
```

数据流：设备发送自定义二进制帧 over TCP → epoll 线程 recv 写入 per-conn RingBuffer → 状态机逐字节拆包 + CRC 校验 → 完整帧入任务队列 → 工作线程取出 → 命令分发表 → JSON 解析 → MQTT 发布。

心跳超时清理链路：heartbeat 线程扫描连接链表 → 持锁收集超时 fd → 出锁后写 self-pipe → epoll 线程读 self-pipe → `connection_close()` 执行完整清理（epoll_ctl DEL → parser_unregister → conn_manager_remove → close → free）。心跳线程不直接 close fd，避免与 epoll 线程的 I/O 操作竞争。

MQTT 重连链路：`mqtt_publish()` 发现连接断开时立即丢弃消息（fast-fail）并唤醒异步重连线程 → 重连线程在后台执行阻塞式重连 → 重连成功后后续 publish 恢复正常。工作线程永不因 broker 不可达而阻塞。

## 2. 半解耦设计

I/O 线程负责网络收发、拆包、CRC 校验，在入队前丢弃坏帧；工作线程只拿到已验证的完整帧，专注业务逻辑和 MQTT 上报。这区别于"全解耦"（工作线程自己拆包/CRC）和"全耦合"（单线程全干）——拆包留在 I/O 线程是刻意的性能决策，让入队即完成一切校验。

## 3. 模块职责边界

| 模块 | 所属线程 | 输入 | 输出 | 持有锁 |
|---|---|---|---|---|
| **server** | main 线程 | config (port) | listen_fd | 无 |
| **epoll_worker** | I/O 线程 | listen_fd, client_fd | 触发 recv/accept | 无（epoll 内在同步） |
| **ringbuffer** | I/O 线程（写） / I/O 线程（读） | 原始字节流 | 按字节读出 | 无（SPSC 无锁） |
| **packet_parser** | I/O 线程 | RingBuffer 单字节 | 完整帧 (on_frame_complete) | 无 |
| **protocol** | I/O 线程 / 工作线程 | 帧字节 / 帧字段 | CRC 验证 / 编码后的帧 | 无（纯函数） |
| **conn_manager** | 多线程 | fd, last_active | 连接链表 CRUD | `g_mutex` |
| **heartbeat** | 独立心跳线程 | 连接链表 | 超时 fd（两段收集，写 self-pipe） | `g_mutex`（遍历时持有） |
| **task_queue** | I/O 线程（写） / 工作线程（读） | struct task | 线程安全 FIFO | `g_mutex + g_cond` |
| **threadpool** | 工作线程 ×N | task_queue_pop | worker_process_task | 无（任务队列在 pop 内部持锁） |
| **business_handler** | 工作线程 | 完整帧 | JSON / MQTT publish | 无（MQTT 模块自行保护） |
| **mqtt_client** | 工作线程（并发） + 重连线程 | topic + payload | mosquitto_publish | `g_mqtt.lock`（publish 快速持锁，重连在独立线程） |
| **config_parser** | main 线程 | INI 文件 | 键值对查询 | 无（单次加载） |
| **logger** | 所有线程 | msg + level | 日志文件 | `g_log_mutex` |

### 锁的持有范围

```
conn_manager.lock  →  conn_manager_foreach (遍历期间持有, 微秒级)
task_queue.lock    →  push / pop (链表操作期间持有, 微秒级)
mqtt_client.lock   →  publish 状态检查 + mosquitto_publish (快速持锁)
                       断连时 fast-fail 丢弃消息, 唤醒异步重连线程
                       重连的阻塞等待在独立线程完成, 不持 publish 锁
logger.lock        →  fprintf + fflush + 轮转检查 (每次日志写入持有)
```

## 4. 关键边界

| 参数 | 值 | 说明 |
|---|---|---|
| RingBuffer 大小 | 4 KB / 连接 | SPSC 循环缓冲区 |
| 最大 Payload 长度 | 1024 字节 | 状态机 `temp_buf` 硬限制 |
| 任务队列容量 | 无上限（链表） | 仅受堆内存限制，生产环境建议加水位线 |
| 心跳间隔 / 超时 | 5s / 15s | 3 次心跳未收到即断开 |
| MQTT 重连次数 | 3 次 | 退避延迟 base×retry + rand(200)ms |
| MQTT QoS | 0 (fire-and-forget) | 传感器高频上报，允许少量丢失 |
| 帧头 | `0xAA 0x55` | 互补字节，误匹配概率 1/65536 |
| 最大连接数 | 无硬编码上限 | 受 `epoll_wait` maxevents 和内存限制 |
