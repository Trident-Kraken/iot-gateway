# AGENTS.md — Industrial IoT TCP Gateway

本文件为 AI 编码助手提供项目背景、架构约定、编码规范与质量要求，适用于 `iot-gateway` 项目所有目录与子模块。

---

## 1. 项目概述

- **项目名称**：iot-gateway（Linux 工业物联网 TCP 通信网关）
- **目标平台**：Linux 用户空间，POSIX API
- **语言与标准**：纯 C11，GCC 编译，CMake 构建
- **交付周期**：校招/课程设计级别，两周迭代
- **设计哲学**：半解耦 — I/O 线程与工作线程分离，但允许在关键路径上为性能做合理耦合，不追求过度抽象的通用框架

---

## 2. 严格技术约束

### 2.1 语言与平台

- 只允许 C11 标准 C 与 POSIX API。
- 禁止使用 C++ 特性：`class`、`template`、STL、`std::string`、`std::vector`、异常、RTTI。
- 禁止使用 Windows API、Boost、libevent、libuv、ACE 等第三方事件/网络框架。
- MQTT 协议实现允许临时依赖 **Eclipse Mosquitto 客户端库**，但必须封装在独立模块内，未来可替换为自实现。

### 2.2 线程模型（当前事实状态）

```
1 个 epoll I/O 线程  +  N 个工作线程（N = 2~4）
```

| 线程 | 职责范围 |
|------|---------|
| **epoll I/O 线程** | `accept()`、非阻塞 `recv()`、将原始字节写入该连接 `parse_ctx->rb`（每连接独立的 RingBuffer）、运行 `packet_parser` 帧解析状态机、CRC 校验、将完整帧入队到任务队列。 |
| **工作线程** | 从任务队列取出完整帧，处理业务逻辑与上报（JSON 打包、MQTT publish、数据库/云端转发等）。 |

> **说明**：当前实现中，协议帧解析状态机和 CRC 校验均位于 epoll I/O 线程。这种设计降低了工作线程复杂度，并使坏帧在入队前即被丢弃，符合半解耦架构目标。工作线程不跑状态机。

### 2.3 内存管理

- 禁止内存泄漏。所有 `malloc/calloc` 必须有对应的 `free`。
- **epoll 线程关键路径**（`recv` → RingBuffer → 入队）应尽量避免动态分配。
- epoll_worker 连接上下文（struct connection）已落地固定大小对象池（1024）、task_queue 任务节点已落地固定大小任务池（2048）；`parse_ctx`（含每连接 RingBuffer）与 `conn_entry` 在连接建立/拆除路径分配（每连接一次），`recv` → RingBuffer → 入队 热路径零动态分配；禁止实现“通用内存池”，只实现项目够用的连接池/任务池即可。
- 柔性数组成员仅用于 `struct packet` 等协议结构体，不得用于频繁分配的热路径。

### 2.4 全局状态

- 禁止裸全局变量。
- 模块级单例/配置状态必须使用 `static` 限定，并加注释说明为什么需要全局。
- 配置解析器、任务队列、线程池、日志器等允许作为进程级单例存在。

---

## 3. 项目结构

必须保持以下目录布局：

```
iot-gateway/
├── CMakeLists.txt          # 根构建脚本（project、编译选项、add_subdirectory(src)）
├── .github/workflows/      # CI：build + ctest + 端到端集成冒烟
├── README.md               # 项目说明与压测基准
├── CHANGELOG.md            # 版本变更记录
├── LICENSE                 # MIT License
├── AGENTS.md               # 本文件（AI 助手必读）
├── src/CMakeLists.txt      # 子目录构建脚本（add_executable(iot-gateway ...)）
├── src/                    # 实现文件 .c，按 gateway/ protocol/ business/ utils/ 分组
├── include/                # 公共头文件 .h，与 src/ 分组镜像（gateway/ protocol/ business/ utils/）
├── config/                 # 配置文件（gateway.ini）
├── tests/                  # unit/（C 单测占位）+ integration/（Python 模拟器 + 压测 + CTest）
├── tools/                  # 运维/调试辅助脚本（当前为空占位）
├── logs/                   # 运行时日志输出目录（logger 写入）
└── docs/                   # 架构、协议设计、部署、压测报告、Wireshark 指南、规则变更历史
```

当前已存在模块：

| 模块 | 头文件 | 实现 | 核心职责 |
|------|--------|------|---------|
| server | `include/gateway/server.h` | `src/gateway/server.c` | TCP 监听 socket 创建、bind、listen、accept |
| epoll_worker | `include/gateway/epoll_worker.h` | `src/gateway/epoll_worker.c` | epoll ET 模式事件循环 |
| ringbuffer | `include/utils/ringbuffer.h` | `src/utils/ringbuffer.c` | 每个连接独立的循环缓冲区 |
| packet_parser | `include/protocol/packet_parser.h` | `src/protocol/packet_parser.c` | 二进制协议状态机拆包 |
| protocol | `include/protocol/protocol.h` | `src/protocol/protocol.c` | 帧编码/解码/CRC16 |
| conn_manager | `include/gateway/conn_manager.h` | `src/gateway/conn_manager.c` | 活跃连接链表、心跳时间戳维护、两段式遍历回调 |
| heartbeat | `include/gateway/heartbeat.h` | `src/gateway/heartbeat.c` | 独立心跳线程，超时断开 |
| task_queue | `include/utils/task_queue.h` | `src/utils/task_queue.c` | 全局线程安全任务队列 |
| threadpool | `include/utils/threadpool.h` | `src/utils/threadpool.c` | 固定数量工作线程 |
| config_parser | `include/utils/config_parser.h` | `src/utils/config_parser.c` | INI 配置文件解析 |
| logger | `include/utils/logger.h` | `src/utils/logger.c` | 分级日志、日志轮转、互斥锁保护多线程写入 |
| main | — | `src/gateway/main.c` | 初始化、启动、资源释放 |
| business_handler | `include/business/business_handler.h` | `src/business/business_handler.c` | 工作线程入口：`g_cmd_handlers[256]` 函数指针分发表、`strstr` 轻量 JSON 解析、调用 `mqtt_publish` |
| mqtt_client | `include/business/mqtt_client.h` | `src/business/mqtt_client.c` | libmosquitto 封装：QoS0/1/2 publish（配置 `mqtt_qos`）、管理线程驱动 mosquitto_loop、断线自动重连、有界离线缓存 |

> 说明：`conn_manager` 不再持有 RingBuffer（v1.4 后所有权归 `parse_ctx`）；它在 epoll 线程被 add/remove，在心跳线程被 foreach 遍历，锁粒度仅覆盖链表操作与回调，不持有期间做 I/O。

---

## 4. 编码规范

### 4.1 命名

- 全部使用 `snake_case`。
- 鼓励公共 API 加模块前缀（如 `server_init`、`packet_parser_feed`、`rb_write`），但不强制统一加 `gw_` 前缀。
- 函数名应清晰表达模块和动作，避免缩写。

### 4.2 函数与接口

- 每个 `.h` 文件必须有 include guard。
- 错误码统一使用 `errno`，或在模块内部定义清晰的 `enum`/`int` 返回值（`-1` 表示错误，`0` 表示成功）。
- 不允许在头文件中暴露内部数据结构细节，除非该结构体确实需要调用者直接操作。

### 4.3 错误处理

- 所有系统调用（`socket`、`epoll_*`、`pthread_*`、`malloc` 等）必须检查返回值。
- 错误路径必须释放已分配资源，禁止半成品对象泄漏。
- 错误日志使用 `LOG_ERROR`，禁止静默吞错。

### 4.4 日志

- 统一使用 `logger.h` 提供的宏：
  - `LOG_DEBUG(fmt, ...)`
  - `LOG_INFO(fmt, ...)`
  - `LOG_ERROR(fmt, ...)`
- 日志格式：`[YYYY-MM-DD HH:MM:SS] [LEVEL] [file:line] message`
- 支持运行时级别开关，从 `config/gateway.ini` 的 `log_level` 读取。
- 禁止在 `LOG_DEBUG` 中使用高开销计算，生产环境可关闭 DEBUG。

### 4.5 注释

- 函数头保留 AI 生成的原有注释风格（Doxygen 块注释 `/** ... */`）。
- 只在以下 5 个深水区模块补充“面试点注释”，帮助开发者亲手理解核心机制：
  1. **epoll_worker**：ET 模式、`EAGAIN` 循环、`EPOLLET` 注册、连接清理。
  2. **packet_parser**：粘包/拆包状态机、逐字节消费、`temp_buf` 边界防御、`rb_write` 背压。
  3. **threadpool**：工作线程循环、条件变量 `while` 等待、优雅退出、`g_running` 标志。
  4. **ringbuffer**：循环覆盖策略、head/tail/len 一致性、单生产者-单消费者前提。
  5. **business_handler**：完整帧到业务对象的转换、JSON 打包、MQTT publish 流程、错误重传策略。

---

## 5. 模块级特殊要求

### 5.1 RingBuffer

- 每个 TCP 连接必须拥有独立 RingBuffer，禁止多连接共享。
- RingBuffer 由 `struct parse_ctx`（packet_parser）持有，不存放在 `conn_entry` 中。
- `packet_parser_register()` 创建并初始化 RingBuffer，`packet_parser_unregister()` 释放。
- `conn_entry` 仅维护 fd、心跳时间戳和链表指针，不持有缓冲区。

### 5.2 epoll_worker

- 必须使用 **ET（边缘触发）** 模式。
- `listen_fd` 和每个 `client_fd` 都必须设为非阻塞。
- ET 模式下必须 `while` 循环 `accept` / `recv` 直到 `EAGAIN`。
- 连接关闭、异常、`EPOLLERR` / `EPOLLHUP` 必须统一走 `connection_close()` 清理。

### 5.3 packet_parser

- 状态机状态：`WAIT_HEAD → WAIT_LEN → WAIT_PAYLOAD → WAIT_CRC → FRAME_COMPLETE`。
- 必须逐字节消费 RingBuffer，处理粘包/拆包三种场景。
- `temp_buf` 必须有运行时 `sizeof(ctx->temp_buf)` 边界检查，作为 `PARSER_MAX_PAYLOAD_LEN` 之后的最后一道防线。
- `rb_write()` 返回值必须检查，RingBuffer 满时不得静默丢帧；应重置状态机并返回错误，由上层关闭连接。

### 5.4 任务队列与线程池

- 任务队列使用 `pthread_mutex_t + pthread_cond_t`。
- `task_queue_pop()` 必须使用 `while` 循环判断队列非空，防止伪唤醒。
- 线程池优雅退出：设置运行标志位 → `pthread_cond_broadcast()` → `pthread_join()`。
- 工作线程处理任务时禁止直接阻塞 I/O。

### 5.5 business_handler & mqtt_client

- `business_handler`：工作线程取任务后按 `task->frame[4]` 提取命令字，查 `g_cmd_handlers[256]` 函数指针表 O(1) 分发；未注册命令记 `LOG_ERROR` 并丢帧（当前注册 3/256：`CMD_SENSOR_DATA`、`CMD_HEARTBEAT`、`CMD_CONFIG`）。
- JSON 解析使用 `strstr`+`snprintf` 手写（`json_extract()`），不引入 cJSON；重打包后用 `mqtt_publish` 上报 `sensor/data`。
- `CMD_CONFIG`（0x03）为请求/响应式下行：设备发配置请求 → 网关回 `0x03` 帧（默认设备配置 JSON）。下行帧经 self-pipe 委托 epoll 线程发送（fd + generation 双重匹配，best-effort，可重发）。
- `mqtt_client`：静态单例 `g_mqtt` + `pthread_mutex_t` 封装 libmosquitto；单一 MQTT 管理线程驱动 `mosquitto_loop()`（收发/PINGREQ/keepalive/断线重连）；publish QoS 由配置 `mqtt_qos` 决定（0/1/2，默认 0），**不阻塞工作线程**。
- 离线缓存：broker 不可达或 publish 失败时消息进入有界环形缓存（`MQTT_CACHE_MAX`=512，满淘汰最旧），管理线程重连成功后锁外冲刷，不再 fast-fail 丢消息。
- QoS1/2 的确认与重传由 `mosquitto_loop()` 的 inflight 机制处理，`on_publish` 回调记录每次 PUBACK/PUBCOMP。
- 禁止直接阻塞 I/O，所有网络发布必须是非阻塞或交给独立发布线程。

---

## 6. 构建与验证

### 6.1 构建命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Debug 模式：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### 6.2 编译选项

根 `CMakeLists.txt` 应保持：

```cmake
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_FLAGS "-Wall -Wextra")
```

> 说明：启用 `-Wall -Wextra` 暴露潜在问题，但不强制 `-Werror`。交付前应保证零错误，允许存在不影响正确性的警告。

### 6.3 验证方式

- 手动运行 `./build/src/iot-gateway`，检查日志输出与端口监听。
- 使用 `nc` 或自定义客户端发送协议帧，验证拆包、CRC、心跳超时、任务队列消费。
- 可选：使用 `valgrind --leak-check=full` 检查内存泄漏（若环境允许）。

---

## 7. 当前已知技术债（按优先级）

1. **命名前缀不统一**：`gw_` 前缀未强制推行，当前函数名风格混杂，后续视时间决定是否统一。
2. **关键路径动态分配（已解决）**：epoll_worker 连接上下文与任务节点已改为固定大小对象池（连接池 1024、任务池 2048，见 §2.3），`recv → 入队` 热路径零动态分配；`parse_ctx`/`conn_entry` 仍为每连接一次分配（连接建立/拆除路径）；任务队列随之从无界链表变为有界（满则拒绝入队）。
3. **MQTT 已达成最小可用**：`mqtt_client` 实现 QoS0/1/2 publish（配置 `mqtt_qos`）+ 管理线程驱动 `mosquitto_loop()` + 断线自动重连 + 有界离线消息缓存（重连后冲刷）；`business_handler` 已接通 `CMD_SENSOR_DATA`/`CMD_HEARTBEAT`/`CMD_CONFIG`（3/256 handler，含请求/响应式配置下行）。待补项：QoS1/2 显式重传策略与离线消息持久化（当前缓存仅内存、进程重启即丢）、下行配置的真实数据源（当前回发固定默认 JSON）。

---

## 8. 质量检查清单（提交前必做）

- [ ] `cmake --build build` 在 `-Wall -Wextra` 下零错误通过。
- [ ] 手动运行网关，确认监听端口、日志输出正常。
- [ ] 新增函数均有基本注释。
- [ ] 新增系统调用均检查返回值。
- [ ] 新增动态内存均有释放路径。
- [ ] 多线程模块使用 `static` 单例并说明原因。
- [ ] 没有引入新的第三方网络/事件库。

---

## 9. 变更日志

> 规则修订历史已抽到 [docs/rule_history.md](docs/rule_history.md)，避免每次 AGENTS.md 加载都把 60+ 行历史塞进上下文。追溯某条规则的来源、或对某条规则存疑时查该文件。

当前最新：**v1.7**（新增 CI workflow、统一源文件编码为 UTF-8）。v1.0~v1.7 详见 rule_history.md。

---

## 10. 给 AI 助手的快速指引

仅列**项目独有**、未在正文/全局 AGENTS.md 重复的几条，其余通用原则（先读项目、最小改动、显式错误处理等）不在此复述。

- 新增模块时先更新 `src/CMakeLists.txt`（`add_executable(iot-gateway …)` 的源清单在此，不在根 CMakeLists.txt）。
- 涉及多线程时，先画清楚锁的持有范围，**禁止在持锁期间做 I/O**（`conn_manager_foreach` 回调、`g_mqtt.lock`、`task_queue` 锁均严守此界）。
- 修改 `epoll_worker` / `packet_parser` / `threadpool` / `ringbuffer` / `business_handler` 五个深水区模块后，在关键逻辑处补"面试点注释"（见 §4.5）。

## 11. 给 Claude Code 的补充约定

以下两条仅为 Claude Code（Anthropic 官方 CLI）补充，其他 AI 助手读到本文件时可忽略。

- Claude Code 特有能力的使用约定（如 plan mode 规则、特定 Skill 调用）统一在此处记录与维护，不再在 `CLAUDE.md` 内另行沉淀。
- 多工具调用的任务中，每次工具返回后必须在正文里至少写一句进度说明（如"已完成 x/y，正在编译..."），避免空响应导致 harness 推送空白消息给用户。
