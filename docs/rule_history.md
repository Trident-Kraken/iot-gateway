# iot-gateway 规则变更历史

> 追溯 [`AGENTS.md`](../AGENTS.md) 中规则的来源：哪些属于初始设计，哪些是后期新增或修订。
> 从 `AGENTS.md` 第 9 节抽出单独存放 —— AI 干活时不必每次通读 60+ 行历史；追溯某条规则的来源、或对某条规则存疑时再查本文件。

## v1.0 — 初始规则（项目骨架阶段）

- 确定 C11 + GCC + CMake 技术栈。
- 确定目录结构：`src/`、`include/`、`config/`、`tests/`、`docs/`。
- 确定半解耦架构：1 个 epoll I/O 线程 + N 个工作线程。
- 确定禁止 C++、禁止通用线程池/内存池、禁止第三方事件库。
- 确定日志宏 `LOG_DEBUG/LOG_INFO/LOG_ERROR` 与日志轮转要求。
- 确定 INI 配置解析接口。

## v1.1 — 连接管理与 RingBuffer 要求（后期新增）

- 新增：`struct conn_entry` 必须包含独立 `ringbuffer_t rb`。**（已被 v1.4 取消：RingBuffer 所有权归 `parse_ctx`，消除每连接重复 4KB 缓冲）**
- 新增：`conn_manager_add()` 初始化 RingBuffer，失败回滚；`conn_manager_remove()` 显式 `rb_destroy()`。**（已被 v1.4 取消）**
- 新增：`rb_init()` 改为返回 `int` 以支持失败判定；新增 `rb_destroy()` 接口。
- 影响文件：`include/conn_manager.h`、`src/conn_manager.c`、`include/ringbuffer.h`、`src/ringbuffer.c`、`src/packet_parser.c`。

## v1.2 — packet_parser P0 安全修复（后期新增）

- 新增：`parser_consume_byte()` 入口使用 `sizeof(ctx->temp_buf)` 做运行时边界检查。
- 新增：`packet_parser_feed()` 检查 `rb_write()` 返回值，RingBuffer 满时重置状态机并返回 `-1`（背压）。
- 影响文件：`src/packet_parser.c`。

## v1.3 — AGENTS.md 定稿与编译策略调整

- 新增：本文档本身，汇总上述所有规则。
- 修订：明确工作线程**不跑状态机**，状态机与 CRC 均保留在 epoll 线程。
- 修订：编译选项从 `-Wall -Wextra -Werror` 调整为 `-Wall -Wextra`，允许警告存在但要求零错误。
- 修订：不强制 `gw_` 前缀，列为技术债。
- 修订：不强制单元测试。
- 新增：注释规则，仅在 5 个深水区模块（`epoll_worker`、`packet_parser`、`threadpool`、`ringbuffer`、`business_handler`）补充面试点注释。
- 新增：变更日志模块。

## v1.4 — P0/P1 修复（代码审查后）

- 修订：`struct conn_entry` 移除 `ringbuffer_t rb` 字段。RingBuffer 所有权归 `parse_ctx`，消除每连接重复 4KB 缓冲。
- 修订：CMakeLists.txt 移除 `-Werror`，与 v1.3 规则对齐。
- 修订：`g_cmd_handlers` 改为 `static`，移除头文件中的 `extern` 声明。
- 新增：`epoll_worker` 新增 self-pipe 机制，心跳线程通过 `epoll_worker_request_close()` 委托 epoll 线程关闭连接。
- 新增：`logger` 新增互斥锁保护多线程并发写入。
- 新增：`task_queue.frame` 大小对齐 `PROTOCOL_MAX_FRAME_SIZE`，不再丢弃合法大帧。
- 新增：`mqtt_client` 新增异步重连线程，`mqtt_publish` 不再阻塞工作线程。
- 影响文件：`include/conn_manager.h`、`src/conn_manager.c`、`CMakeLists.txt`、`include/business_handler.h`、`src/business_handler.c`、`include/epoll_worker.h`、`src/epoll_worker.c`、`src/heartbeat.c`、`src/main.c`、`src/logger.c`、`include/protocol.h`、`include/packet_parser.h`、`include/task_queue.h`、`src/mqtt_client.c`、`AGENTS.md`。

## v1.5 — AGENTS.md 与代码现状同步（本次，纯文档）

代码早已演进到 `mqtt_client` + `business_handler` 落地，但 `AGENTS.md` 正文停留在 v1.3 之前的事实；本次仅同步文档，不动任何代码。

- 修订：模块表 `business_handler` 从「待创建」改为已实现行，新增 `mqtt_client` 模块行；`logger` 行补「互斥锁保护多线程写入」（v1.4 已加但表里漏记）。
- 修订：模块表 `conn_manager` 职责去掉「连接级 RingBuffer」残留，与 v1.4「RingBuffer 所有权归 `parse_ctx`」对齐。
- 修订：5.5 标题去掉「（待实现）」，按实际代码补充分发表 / JSON 解析 / 异步重连 / fast-fail 描述。
- 修订：技术债第 3 条从「MQTT 尚未实现」改为「仅做最小可用」，列出待补项（含 `mosquitto_loop_*` 心跳循环缺失这一观察，证据：全文零 `mosquitto_loop` 调用）。
- 修订：v1.1 变更日志条目标注「已被 v1.4 取消」，避免顺读矛盾。
- 修订：目录结构图补 `README.md`、`logs/`，`tests/` 描述改为「Python 设备模拟器 + 压测脚本」（不再是「测试目录占位」）。
- 新增：本文件 `docs/rule_history.md`，承接原 `AGENTS.md` 第 9 节历史，减少每次 AGENTS.md 上下文加载的信噪比。
- 修订：第 10 节快速指引精简，删除与正文/全局 AGENTS 重复的条目，保留具项目独特价值的两条。
- 影响：`AGENTS.md`、`docs/rule_history.md`。不改动任何 `.c` / `.h` / `CMakeLists.txt`。

## v1.6 — 目录结构按职责分组（本次，结构重构）

`src/` 与 `include/` 按职责分层分组，`tests/` 拆分 unit/integration，与仓库 README「项目结构」一致；纯结构变更，不改任何业务逻辑。

- 修订：`src/`、`include/` 分组为 `gateway/`（server/epoll_worker/conn_manager/heartbeat/main）、`protocol/`（protocol/packet_parser）、`business/`（business_handler/mqtt_client）、`utils/`（ringbuffer/task_queue/threadpool/logger/config_parser），头文件与实现镜像。
- 修订：所有项目内 `#include "xxx.h"` 更新为带分组前缀的路径（如 `"utils/logger.h"`）。
- 修订：`tests/` 拆为 `tests/unit/`（C 单测占位，暂不引入）与 `tests/integration/`（Python 模拟器/压测/CTest）。
- 新增：`tools/`（占位）、`docs/protocol-design.md`、`docs/deployment.md`、`docs/test-report.md`（原 `tests/stress_report.txt`）、`CHANGELOG.md`、`.gitignore`、`LICENSE`。
- 修订：`build/`、`logs/`、`__pycache__/` 解除版本跟踪（此前误入库）。
- 影响文件：全部 `src/**`、`include/**`、`tests/**`、`docs/**`、`README.md`、`AGENTS.md`、`CMakeLists.txt`（`src/`、`tests/`）。

## v1.7 — 新增 CI 与编码统一（本次，工程实践补强）

- 新增：`.github/workflows/ci.yml`（Ubuntu + CMake + CTest 自检 + mosquitto 端到端集成冒烟，订阅者先于流量启动以适配 QoS0）。
- 修订：7 个源文件（`include/gateway/epoll_worker.h`、`include/protocol/*.h`、`include/utils/task_queue.h`、`src/gateway/*.c`）注释中 273 处 UTF-8 损坏字符按语义修复，全仓编码统一为 UTF-8。
- 修订：`AGENTS.md` 目录布局补 `.github/workflows/`。
- 影响文件：`.github/workflows/ci.yml`、7 个 `.c/.h`、`CHANGELOG.md`、`AGENTS.md`、本文件。
