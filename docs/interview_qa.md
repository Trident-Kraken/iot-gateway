# 面试追问与回答 — iot-gateway

> 以下 10 个问题覆盖线程模型、I/O、协议、数据结构、调试、架构决策。
> 每个答案控制在 150–300 字，口语化表达，适合面试现场陈述。

---

### 1. 为什么用 epoll ET 而不是 LT？

LT（水平触发）和 ET（边缘触发）的本质区别在于通知策略。LT 模式下只要 fd 还有数据没读完，下一次 `epoll_wait` 会再次通知你——很安全但多余的通知会吃掉 syscall 开销。ET 只在 fd 从不可读变为可读的那一瞬间通知一次，这就逼你在一次通知中把数据全部读完，直到 `recv` 返回 `EAGAIN`。

对于高并发网关，ET 节省了那些 "还有数据吗？哦有的" 的无效往返。副作用是你的代码必须写 `while` 循环来 drain fd——但这条约束在实际项目中反而变成纪律，让你不敢偷懒只读一次就返回。Trade-off 是复杂度上去了但性能提上来了，工业场景值得。

---

### 2. 为什么 ET 模式下要 while 循环读到 EAGAIN？

因为 ET 只通知一次。假如内核缓冲区里来了 4KB 数据，你只读 1KB 就停了，剩下 3KB 不会触发新通知——epoll 认为 fd 的状态还是"可读"（没有从不可读变成可读的边沿），所以不会再次唤醒你。那 3KB 就永远躺在内核缓冲区里，直到对端发来下一批数据触发新的边沿。

`while` 循环一直读到 `recv` 返回 -1 和 `errno == EAGAIN/EWOULDBLOCK`，确保内核缓冲区被完全排空。这是在告诉内核："我已经把你手里所有数据都拿走了"，为下一次边沿通知做好准备。

---

### 3. RingBuffer 为什么要每个连接独立？全局一个不行吗？

不行，因为 TCP 是字节流，不同连接的数据没有边界。如果 100 个连接的帧混在一个全局 RingBuffer 里，设备 A 的 0xAA 帧头可能被设备 B 的 payload 字节插在中间，状态机当场错乱——它根本分不清哪一段属于谁。

每个连接独立 RingBuffer 是实现 "per-connection parser context" 的前提：每个连接有自己的状态机实例、自己的临时缓冲，A 连接在 `WAIT_PAYLOAD`，B 连接在 `WAIT_HEAD`，互不干扰。代价是多消耗 N×RingBuffer 内存，但对于少量长连接（工业传感器通常 < 100 个）完全可接受。

---

### 4. 粘包拆包状态机怎么设计的？画一下状态转移图。

```
                  ┌──────────┐
                  │WAIT_HEAD │ ← 初始状态
                  │ 找 AA 55 │
                  └────┬─────┘
                       │ 读到 AA 55
                       ▼
                  ┌──────────┐
                  │WAIT_LEN  │
                  │ 读 2B 长度│
                  └────┬─────┘
                       │ 读到 payload_len
                       ▼
                  ┌──────────┐
                  │WAIT_     │
                  │PAYLOAD   │ ← 读 cmd + N 字节
                  └────┬─────┘
                       │ 读完 payload_len 字节
                       ▼
                  ┌──────────┐
                  │WAIT_CRC  │
                  │ 读 2B CRC│
                  └────┬─────┘
                       │ CRC 校验通过
                       ▼
                  ┌──────────┐
                  │FRAME_    │
                  │COMPLETE  │ → 回调 → 立即回到 WAIT_HEAD
                  └──────────┘
```

三种拆包场景：① 刚好一帧——正常回调；② 半帧——状态机停在中间状态，等下次收包继续消费；③ 同一批数据两帧——读完第一帧回调后，剩下的字节进入 WAIT_HEAD 继续解析，自然处理粘包。

---

### 5. 线程池条件变量为什么用 while 不用 if？

POSIX 允许 `pthread_cond_wait` 发生虚假唤醒（spurious wakeup）——条件变量可能在没有 signal 的情况下返回，而且就算有 signal，多个 worker 被唤醒后只有一个能真正拿到队列里唯一的那条任务，其他线程醒来后发现队列已经被取空了，如果不重新检查就直接 `pop`，就是 NULL 指针解引用。

`while (queue_empty && !shutdown)` 的作用是：醒来后重新持锁、重新判断条件，没任务就继续睡。用 `if` 只检查一次，余下的就全赌运气。

---

### 6. 心跳线程和业务线程怎么同步？超时断开时资源如何回收？

心跳线程通过 `conn_manager_foreach` 遍历连接链表，内部持有 `conn_manager` 的 mutex。关键设计是 "两段收集"：在持锁期间只收集超时 fd 到临时数组，出锁后不直接 `close(fd)`，而是通过 `epoll_worker_request_close(fd)` 写入 self-pipe，委托 epoll I/O 线程执行完整清理。

为什么不让心跳线程直接关？因为 `close(fd)` 会与 epoll 线程的 I/O 操作产生竞争——如果心跳线程关了 fd，epoll 线程恰好在 `recv` 同一个 fd，就会读到错误数据或触发 `EBADF`。self-pipe 机制把关闭操作串行化到 epoll 线程的事件循环中：心跳线程只负责"发现超时"，epoll 线程负责"执行清理"，两者职责分离，无竞争。

资源回收链路（全部在 epoll 线程执行）：`epoll_worker_request_close(fd)`（心跳线程，写 self-pipe）→ epoll 线程读 self-pipe → `connection_close()`: `epoll_ctl(DEL)` → `packet_parser_unregister`（释放 parse_ctx + RingBuffer）→ `conn_manager_remove` → `close(fd)` → `free(conn)`。

---

### 7. 为什么选择 0xAA 0x55 做帧头？怎么考虑字节序？

`AA = 10101010`，`55 = 01010101`，这是两个"互补"字节，任意相邻比特不同。好处非常实在：

- **抗干扰**：没有自然文本（ASCII 编码）会连续出现这两个值，降低了误把日志或 HTTP 内容识别为帧头的概率。
- **自同步能力强**：状态机在 WAIT_HEAD 时可以逐字节滑动，一旦找到 AA 55 序列就锁定帧边界。两个字节 16 bit 的组合空间远大于单字节，误匹配概率是 1/65536。
- **字节序方面**：设计上把多字节字段（长度、CRC）统一为 big-endian（网络字节序），在 protocol.c 中用 `htons/ntohs` 显式转换。帧头是固定字节不涉及端序——它就是两个独立字节，谁先谁后和码流顺序一致。

---

### 8. 压测发现 QPS 上不去，你会怎么定位？

我会按 "自底向上" 的思路逐层排查，同时依赖日志和系统工具交叉验证：

1. **先看系统资源**：`htop` 看 CPU 是否打满。如果工作线程 100% 但 I/O 线程空闲，说明瓶颈在 JSON 解析或 MQTT 发送。`netstat -an | grep 8080` 看 Send-Q 是否堆积。
2. **查 I/O 层**：epoll 是否用 ET？有没有在 recv 后只读一次？日志中 `EAGAIN` 循环是否正常结束？
3. **查任务队列**：在 `task_queue_push` 中打计数，按 1s 频率打印 pending 队列长度——如果队列持续增长，说明消费者跟不上生产者。
4. **查 MQTT**：`mosquitto_publish` 的耗时——如果每条消息都要等 Broker 的 TCP ACK（QoS 1 会），换 QoS 0。另看 `mqtt_client` 锁竞争是否严重。
5. **最后手段**：`perf top` / `gprof` 看热点函数。通常瓶颈在 `snprintf` / `strstr` 的字符串开销或 `malloc/free` 抖动。

---

### 9. 为什么用函数指针数组做命令分发而不是 switch-case？

两个核心原因：**O(1) 确定性**和**扩展不修改**。

switch-case 在编译器优化下通常会生成跳转表，但当 case 值稀疏（比如只有 0x01、0x7F、0xFE 三个 case）时，编译器为了节省跳转表空间会退化为 if-else 链——O(n) 逐条比较。函数指针数组 `g_cmd_handlers[cmd]` 是真正的常数时间索引，不依赖编译器优化策略。

扩展性方面：加一个新命令（如 0x04 报警处理），只需新建一个 `handle_alarm` 函数，然后在 `cmd_handlers_init` 中加一行 `g_cmd_handlers[CMD_ALARM] = handle_alarm`。分发骨架不碰，已有命令不动，符合开闭原则。如果用 switch-case，每次都要在 switch 块里加 case，容易引入 merge conflict 和遗漏 break 的 bug。

---

### 10. MQTT 为什么用 QoS 0？工业场景下传感器数据怎么选 QoS？

QoS 选择取决于你对"丢失这条数据"的容忍度。工业传感器的典型特征是高频上报——温湿度每 1~10 秒一条——单条丢失只是损失一个采样点，对趋势分析几乎无影响。这种场景就是 QoS 0 的主场：fire-and-forget，无 PUBACK 握手，延迟最低，Broker 内存压力最小。

反过来，如果是控制指令（开阀门、停电机）或告警（温度超过阈值），丢一条可能造成安全事故，那必须 QoS 1（至少送达一次）。QoS 2（恰好送达一次）在工业边缘端很少用——两次握手代价太大，而且工业协议往往在应用层做去重。

本项目选 QoS 0 是精准的：传感器数据 → 云端存储，允许少量样本丢失，追求低延迟和低资源消耗。这符合 AWS IoT / Azure IoT Hub 在设备到云方向上的主流推荐。
