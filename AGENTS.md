# AGENTS.md — Industrial IoT TCP Gateway

本文件定义 `iot-gateway` 项目的架构边界、编码规范、并发约束与验证标准，适用于所有目录与子模块。

---

## 1. 项目概述

- **项目名称**：`iot-gateway`（Linux 工业物联网 TCP 通信网关）
- **目标平台**：Linux 用户空间，POSIX API
- **语言与标准**：纯 C11，GCC 编译，CMake 构建
- **设计哲学**：半解耦架构 —— I/O 线程与工作线程分离，关键路径允许为性能与确定性延迟做适当耦合，避免过度抽象

---

## 2. 严格技术约束

### 2.1 语言与平台

- 只允许使用 C11 标准 C 与 POSIX API。
- 禁止使用 C++ 特性：`class`、`template`、STL、`std::string`、`std::vector`、异常、RTTI。
- 禁止使用 Windows API、Boost、libevent、libuv、ACE 等第三方事件/网络框架。
- MQTT 客户端允许依赖 **Eclipse Mosquitto 客户端库**，但必须封装在独立模块内，保持上层接口可替换。

### 2.2 线程模型

```
1 个 epoll I/O 线程  +  N 个工作线程（默认 4） + 1 个心跳检测线程 + 1 个 MQTT 管理线程
```

| 线程 | 职责范围 |
|---|---|
| **epoll I/O 线程** | `accept()`、非阻塞 `recv()`、将原始字节写入该连接 `parse_ctx->rb`（每连接独立 RingBuffer）、运行 `packet_parser` 状态机与 CRC16 校验、完整帧入队到任务队列、读取 self-pipe 执行连接关闭与下行发送。 |
| **工作线程** | 从任务队列取出完整帧，通过函数指针表分发处理业务逻辑（JSON 解析/校验、心跳刷新、`CMD_CONFIG` 下行组帧、`mqtt_publish` 入队）。 |
| **心跳检测线程** | 周期性遍历 `conn_manager` 链表收集超时 `(fd, generation)`，出锁后经 self-pipe 委托 epoll 线程关闭。 |
| **MQTT 管理线程** | 维护 broker 连接、驱动 `mosquitto_loop()`（收发/keepalive/QoS1&2 inflight 重传）、断线退避重连与离线缓存冲刷。 |

> 说明：协议帧解析状态机和 CRC 校验保留在 epoll I/O 线程，使工作线程专注于业务处理且坏帧在入队前即被丢弃。

### 2.3 内存管理

- 禁止内存泄漏。所有 `malloc/calloc/strdup` 必须有对应释放路径。
- **epoll 线程关键路径**（`recv` → RingBuffer → 任务入队）保持零动态分配：
  - `epoll_worker` 连接上下文（`struct connection`）使用固定大小对象池（1024）；
  - `task_queue` 任务节点使用固定大小任务池（2048，满则 `ENOMEM` 拒绝入队）；
  - `parse_ctx`（含每连接 RingBuffer）与 `conn_entry` 仅在连接建立/拆除路径分配一次。
- 不实现通用内存池，只维护上述满足项目边界的对象池。
- 柔性数组成员仅用于 `struct packet` 等协议结构体，不得用于频繁分配的热路径。

### 2.4 全局状态

- 禁止暴露裸全局变量。
- 模块级单例/配置状态必须使用 `static` 限定，并附注释说明作用域与同步机制。
- 配置解析器、任务队列、线程池、日志器等进程级单例需保证启动/停机路径有序清理。

---

## 3. 项目结构

保持以下目录布局：

```
iot-gateway/
├── CMakeLists.txt          # 根构建脚本（C11、-Wall -Wextra、add_subdirectory）
├── .github/workflows/      # CI：Release 构建 + CTest（单测与集成测试）
├── README.md               # 项目说明与基准验证结果
├── CHANGELOG.md            # 版本变更记录
├── LICENSE                 # MIT License
├── AGENTS.md               # 架构约定与编码规范
├── src/CMakeLists.txt      # 主程序目标与源文件清单
├── src/                    # 实现文件 .c，按 gateway/ protocol/ business/ utils/ 分组
├── include/                # 公共头文件 .h，与 src/ 分组镜像
├── config/                 # 运行时配置文件（gateway.ini）
├── tests/                  # unit/（C 单元测试）+ integration/（Python 模拟器/压测/集成用例）
├── tools/                  # 运维/调试辅助脚本占位
├── logs/                   # 运行时日志输出目录
└── docs/                   # 架构、协议、部署、测试报告、设计权衡与抓包指南
```

核心模块说明：

| 模块 | 头文件 | 实现 | 核心职责 |
|---|---|---|---|
| server | `include/gateway/server.h` | `src/gateway/server.c` | 监听 socket 创建、accept 瞬时错误重试、EMFILE reserve-fd 排空与释放 |
| epoll_worker | `include/gateway/epoll_worker.h` | `src/gateway/epoll_worker.c` | epoll ET 事件循环、连接池（1024）、self-pipe 跨线程关闭/下行发送、陈旧事件防护 |
| ringbuffer | `include/utils/ringbuffer.h` | `src/utils/ringbuffer.c` | 每连接独立的 SPSC 循环缓冲区 |
| packet_parser | `include/protocol/packet_parser.h` | `src/protocol/packet_parser.c` | 五态二进制协议状态机拆包 |
| protocol | `include/protocol/protocol.h` | `src/protocol/protocol.c` | 帧编解码与编译期表驱动 CRC16-CCITT-FALSE |
| conn_manager | `include/gateway/conn_manager.h` | `src/gateway/conn_manager.c` | 活跃连接链表、generation 管理、两段式遍历回调 |
| heartbeat | `include/gateway/heartbeat.h` | `src/gateway/heartbeat.c` | 独立心跳扫描线程（可中断睡眠），超时委托 epoll 线程关闭 |
| task_queue | `include/utils/task_queue.h` | `src/utils/task_queue.c` | 线程安全有界任务队列（2048 节点对象池） |
| threadpool | `include/utils/threadpool.h` | `src/utils/threadpool.c` | 固定数量工作线程与优雅退出 |
| config_parser | `include/utils/config_parser.h` | `src/utils/config_parser.c` | INI 解析、行内注释剥离、超长行防护、合法范围告警 |
| logger | `include/utils/logger.h` | `src/utils/logger.c` | 分级日志、文件大小轮转、互斥锁保护多线程写入 |
| main | — | `src/gateway/main.c` | 配置校验、`SIGPIPE` 忽略、信号停机、严格逆序资源清理 |
| business_handler | `include/business/business_handler.h` | `src/business/business_handler.c` | `g_cmd_handlers[256]` 分发表、长度受限 JSON 解析/校验、`CMD_CONFIG` 下行组帧 |
| mqtt_client | `include/business/mqtt_client.h` | `src/business/mqtt_client.c` | libmosquitto 封装（QoS0/1/2）、管理线程、退避重连、有界离线缓存（512） |

---

## 4. 编码规范

### 4.1 命名与接口

- 全部使用 `snake_case`，公共 API 鼓励带模块前缀（如 `server_init`、`packet_parser_feed`、`rb_write`）。
- 每个 `.h` 文件必须包含 include guard 与 `extern "C"` 兼容宏。
- 错误返回值统一：`0` 表示成功、`-1` 表示失败并设置 `errno`（或返回明确的枚举/长度语义）。
- 头文件不暴露不必要的内部结构体布局（如 `struct parse_ctx` 使用不透明句柄）。

### 4.2 错误处理与日志

- 所有系统调用（`socket`、`epoll_*`、`pthread_*`、`malloc`、`pipe`、`sigaction` 等）必须检查返回值，错误路径释放半成品资源。
- 统一使用 `logger.h` 提供的 `LOG_DEBUG` / `LOG_INFO` / `LOG_ERROR` 宏，格式 `[YYYY-MM-DD HH:MM:SS] [LEVEL] [file:line] message`。
- 高频路径避免无条件 `INFO` 日志，防止 I/O 成为吞吐瓶颈。

### 4.3 注释约定

- 公共函数与模块头部保持 Doxygen 块注释（`/** ... */`）。
- 在 5 个关键模块（`epoll_worker`、`packet_parser`、`threadpool`、`ringbuffer`、`business_handler`）的核心并发/状态机逻辑处补充“设计说明”注释，阐明并发假设、锁范围与边界防御理由。

---

## 5. 模块级约束

- **RingBuffer**：每连接独立，由 `parse_ctx` 持有，SPSC 无锁前提下仅在 epoll 线程读写；满时拒绝覆盖并返回实际写入字节，由上层断开连接防止静默丢帧。
- **epoll_worker**：强制 `EPOLLET` + 非阻塞 fd，循环 `accept`/`recv` 至 `EAGAIN`；先处理 `EPOLLIN` 再处理 `EPOLLERR | EPOLLHUP`；同批陈旧事件通过 `closed` 标志与 `g_conn_trash` 批末归还防护；`struct pipe_msg` 编译期断言 `<= PIPE_BUF`。
- **packet_parser**：`WAIT_HEAD → WAIT_LEN → WAIT_PAYLOAD → WAIT_CRC → FRAME_COMPLETE`，同时校验 `PARSER_MAX_PAYLOAD_LEN` 与 `sizeof(ctx->temp_buf)` 双重边界。
- **task_queue & threadpool**：`task_queue_pop()` 必须用 `while` 循环防虚假唤醒；退出顺序为 `g_running = 0` → `task_queue_shutdown()`（`pthread_cond_broadcast`）→ `pthread_join()`；工作线程禁止直接阻塞客户端 fd。
- **business_handler & mqtt_client**：工作线程仅对内存/队列操作，网络发送经由 `mqtt_client` 管理线程或 `epoll_worker` self-pipe；**严禁任何模块在持有互斥锁期间执行阻塞网络或磁盘 I/O**。

---

## 6. 构建、测试与开发注意事项

```bash
# Release 构建与全量测试
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Sanitizer 验证（ASan+LSan / TSan）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-Wall -Wextra -fsanitize=address -fno-omit-frame-pointer -g -O1"
cmake --build build && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build --output-on-failure
```

- 根 `CMakeLists.txt` 保持 `CMAKE_C_STANDARD 11`、`CMAKE_C_EXTENSIONS OFF`、`-Wall -Wextra`，提交前要求零编译错误零警告。
- 新增实现文件需同步更新 `src/CMakeLists.txt`，新增单元测试需同步更新 `tests/unit/CMakeLists.txt`。
- 使用 POSIX 特性（如 `sigaction`、`strdup`、`rand_r`、`PIPE_BUF`、`nanosleep`）的 `.c` 文件顶部须显式定义 `#define _POSIX_C_SOURCE 200809L`。
