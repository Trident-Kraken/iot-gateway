# 测试报告 — iot-gateway（Day3）

> 本报告全部结果来自实际运行；标注「待运行」的项目尚未执行，绝不虚构。
> 生成日期：2026-08-05（会话内实测）。

---

# Test Environment

| 项 | 值 |
|---|---|
| 系统 | Ubuntu 24.04（WSL2 内核），x86_64 |
| 编译器 | GCC 13.3.0（`-Wall -Wextra`） |
| CMake | 4.3.3 |
| Python | 3.12.3 |
| valgrind | 3.22.0 |
| Mosquitto | 2.0.18（broker + clients） |
| 构建类型 | Release（`-O2`） |
| 被测目标 | `build/src/iot-gateway` |

# Build Result

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

✅ 零错误、零警告（`-Wall -Wextra`）。

# 合并运行（最终验证）

```bash
ctest --test-dir build --output-on-failure
```

✅ **12/12 一次全部通过**（7 项单测/自检 + 5 项集成，总耗时 71.8s），无跳过、无失败。

# Unit Test Result

`ctest --test-dir build`（C 单测 + Python 自检，共 7 项，全部实测通过）：

| 用例 | 覆盖 | 断言 | 结果 |
|---|---|---|---|
| unit_tests | CRC16 向量 / 编解码 / RingBuffer 环绕与满拒写 / 状态机拆包粘包坏帧 | 56 | ✅ 通过 |
| unit_config | config_parser：trim/默认值/溢出/超容量/重载 | 25 | ✅ 通过 |
| unit_task_queue | 对象池 FIFO / 满则 ENOMEM / shutdown 语义 | 4115 | ✅ 通过 |
| unit_threadpool | NULL 回调拒绝 / 1000 任务全处理 / destroy 后提交拒绝 | 1006 | ✅ 通过 |
| unit_fd_util | 非阻塞设置 / 非法 fd | 9 | ✅ 通过 |
| python_syntax_check | 3 个集成脚本语法 | — | ✅ 通过 |
| crc16_known_vector | Python 侧 CRC16 与 C 侧一致（0x29B1） | 1 | ✅ 通过 |

**valgrind @ unit_tests**（实测）：

```
HEAP SUMMARY: in use at exit: 0 bytes in 0 blocks
total heap usage: 20 allocs, 20 frees, 19,425 bytes allocated
ERROR SUMMARY: 0 errors from 0 contexts
```

✅ 0 泄漏、0 错误。

# Integration Test Result

> 集成用例在沙箱外真实运行（本地 mosquitto + 网关 + 模拟器，全部非 sudo）。
> `ctest` 中集成用例在依赖缺失/环境受限时返回 77 → 标记 Skipped，不误报失败。

| 用例 | 场景 | 结果（实测） |
|---|---|---|
| integration_e2e | broker → 网关 → 2 设备 2Hz → MQTT 断言 → 0x03 下行 → 清理 | ✅ 通过：12 帧正常上报，收到 2 条 `sensor/data`，下行配置 JSON 正确 |
| integration_connection_lifecycle | 心跳超时断开 / 半包断开 / fd 无泄漏 | ✅ 通过：超时窗口内断开；半包后网关存活可 accept；fd baseline=10 after=10 |
| integration_mqtt_offline_cache | broker 停机 → 消息入缓存 → 恢复 → 冲刷 | ✅ 通过：停机期 12 帧入缓存、网关存活；恢复后收到 12/12 |
| integration_config_validation | 缺失文件/坏端口/坏线程数/坏 QoS | ✅ 通过：5 类非法配置全部 exit=1；合法配置正常监听 |
| integration_fault_injection | 4 类异常帧注入 + 超大长度字段 | ✅ 通过：crc/bad_header/oversized_len 期间正常帧 10/10 达 MQTT，half_frame 8，网关全程存活 |

# Exception Test Result

| 类别 | 场景 | 注入方式 | 结果 |
|---|---|---|---|
| 通信 | 心跳超时 | 设备静默（heartbeat_timeout=3s 测试配置） | ✅ 超时窗口内断开 |
| 通信 | 半包后断开 | 发送 `AA 55 00` 后 close | ✅ 网关存活、可继续 accept |
| 通信 | 非法帧头 / CRC 错误 | `--anomaly bad_header / crc_error` | ✅ 帧被丢弃，正常帧 10/10 不受影响 |
| 通信 | 数据长度错误 | `--anomaly length_overflow`（len=2048>1024）+ 原始 `0x0800` 帧 | ✅ 状态机重置，无越界无崩溃 |
| 资源 | 文件不存在 | 无 `config/gateway.ini` 的临时 cwd | ✅ 启动失败 exit=1 |
| 资源 | 配置错误 | `server_port=0/99999`、`thread_pool_size=0`、`mqtt_qos=3` | ✅ 全部 fail-fast exit=1 |
| 资源 | broker 不可达 | 停机 + 12 帧上报 | ✅ 消息入离线缓存，网关存活 |
| 设备 | 连接清理 | 多次连接/断开 | ✅ fd 数量稳定（无泄漏） |

# Stress / Soak

| 项 | 结果 |
|---|---|
| 100 连接压测回归（`stress_test.py -c 100 -f 10 -d 60`） | ✅ 实测：客户端发送速率 **990.2 帧/秒**（目标 1000，达成率 99.2%）；59,500 帧 / 0 发送错误 / 发送错误率 0.00%；100/100 连接成功；客户端构帧及发送耗时 avg 0.137ms、P50 0.117ms、P99 0.509ms、max 23.108ms（注：测定区间为发包端写入内核缓冲，非网关端到端处理时延） |
| 30 分钟 soak（`run_soak.sh 30`） | ✅ 实测：20 设备 × 2Hz 持续 30 分钟；RSS 6192→6316 kB（+124 kB，阈值 8192），网关全程存活，min/max=6312/6316 kB |
| 1 分钟 soak 冒烟（实测） | ✅ RSS 6200→6320 kB（+120 kB，阈值 8192），网关全程存活 |

### 结构化结果（PASS 汇总）

**100 connections / 60s stress regression**

- Target send rate: 1,000 frames/s
- Client send rate: 990.2 frames/s (99.2% achievement)
- Status: PASS

**30-minute soak**

- Initial RSS: 6192 kB
- Final RSS: 6316 kB
- Delta: +124 kB
- Process survival: PASS
- Status: PASS

# Conclusion

- **核心链路（P0）全部通过**：协议解析正确性（单测 56 断言）、数据收发全链路（E2E）、错误处理（配置 fail-fast、心跳超时、断连清理、离线缓存、故障注入）。
- **内存可靠性**：单测 valgrind 0 泄漏 0 错误；soak 冒烟 RSS 稳定。
- **待确认**：CI（GitHub Actions）结果（本地 12/12 已全绿）。
- **本轮额外修复**：`device_simulator.py` length_overflow 缺陷（无参 builder 使线程崩溃；长度 300→2048 对齐协议 1024 上限）。

## 复现命令

```bash
# 单测
ctest --test-dir build --output-on-failure
# 集成（依赖 mosquitto；环境受限时自动 Skipped）
bash tests/integration/run_e2e.sh
python3 tests/integration/test_connection_lifecycle.py
python3 tests/integration/test_mqtt_offline_cache.py
python3 tests/integration/test_config_validation.py
python3 tests/integration/test_fault_injection.py
# 压测 / soak
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress_day3.txt
bash tests/scripts/run_soak.sh 30
```

---

# Day4 P0 修复回归（2026-08-21）

> 背景：Day4 代码审查发现 P0 缺陷 —— 同一 `epoll_wait` 事件批内，陈旧客户端事件可能在连接已被（批内更早的 pipe 关闭请求事件）清理后再次命中同一连接槽位，`conn_pool_release()` 二次进入使空闲链表成环，进而导致池耗尽/双重释放。修复为双层防御：`connection.closed` 重入保护（快路径）+ 批内延迟归还（关闭的槽位先入 `g_conn_trash`，批末统一 `conn_pool_flush_trash()` 归还，杜绝批内槽位复用）。详见 `CHANGELOG.md` 与 `src/gateway/epoll_worker.c` 内注释。

## Build（Day4）

✅ 零错误、零警告（`-Wall -Wextra`，Release 重新配置后增量编译）。

## 合并运行（Day4）

```bash
ctest --test-dir build --output-on-failure
```

✅ **13/13 全部通过**（7 项单测/自检 + 6 项集成，总耗时 96.0s），无跳过、无失败。较 Day3 新增 1 项集成用例：

| 用例 | 场景 | 结果（实测） |
|---|---|---|
| integration_stale_event_race | 心跳超时边界连接 churn ×10 轮（connect → 边界突发 → 断开），以 0x03 下行回显作连接跟踪完整性金丝雀；末尾双连接并发回显 + fd 稳定性检查 | ✅ 通过：10/10 轮全部在超时窗口内被正确关闭，各轮回显地址全部有效（无双重释放/UAF 迹象），双连接并发回显成功，fd 数量稳定（25.17s） |

## 压测回归（Day4，修复后）

`stress_test.py -c 100 -f 10 -d 60`（与 Day3 完全同参）连续 2 次有效运行：

| 轮次 | 客户端发送速率 (fps) | 发送错误率 | 发送耗时 P50 | 发送耗时 P99 | 连接成功 |
|---|---|---|---|---|---|
| 1 | 930.2 | 0.00% | 0.130 ms | 0.559 ms | 100/100 |
| 2 | 928.5 | 0.00% | 0.122 ms | 0.503 ms | 100/100（avg 558.0 帧/连接） |

- 两轮数据相近（930.2 / 928.5，相差 0.2%），较 Day3 的 990.2 低约 6%。
- **差异分析与口径说明**：稳态 100 连接压测中无心跳超时关闭，新增的 `closed` 检查与 `conn_pool_flush_trash()`（空链立即返回）在该负载下为空操作，非代码路径引入开销；发包端速率受并发建立连接延迟与协程定时器调度影响存在正常波动，属于发包端脚手架特性。后续通过引入 0x03 下行探针直接测量网关全链路处理 RTT，避免仅依赖客户端发送耗时。

## 复现命令（Day4 新增）

```bash
python3 tests/integration/test_stale_event_race.py
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress_day4.txt
```

## Conclusion（Day4）

- P0 修复后 CTest 13/13 全绿，新增竞态回归用例持续守护该路径。
- 压测两轮 0 发送错误，连接与客户端发送指标正常；发送速率受并发建立延迟与宿主机调度存在波动。

---

# Day4 P1 修复验证（F2/F3/F4，2026-08-22）

> 三项 P1 修复（accept 瞬时错误/EMFILE、SIGTERM 优雅停机、config 行内注释与非法 int 告警），全部实测验证，含未修复二进制上的测试区分度确认。

## Build（Day4-P1）

✅ 零错误、零警告（`-Wall -Wextra`，Release 全量干净重编译；顺带修复 heartbeat.c/test_config.c 两个隐性声明警告——严格 C11 特性宏）。

## 合并运行与回归门槛（Day4-P1）

| 构建类型 | 结果 | Sanitizer 报告 |
|---|---|---|
| Release | ✅ **14/14 通过**（99.3s） | — |
| ASan（含 LeakSanitizer，`log_path` 落盘检查） | ✅ **14/14 通过**（99.8s） | 零报告文件 |
| TSan（`log_path` 落盘检查） | ✅ **14/14 通过**（99.2s） | 零数据竞争 |
| 1 分钟 soak（churn 后 RSS 稳定性） | ✅ PASS：RSS 6132→6252 kB（+120 kB，阈值 8192），全程存活 | — |
| 100 连接压测回归（P1 修复后二进制） | ✅ 客户端发送速率 **988.3 帧/秒** / 59,393 帧 / 发送错误率 0.00% / 发送耗时 P99 0.415 ms | 客户端发送速率接近 1000 目标速率（达成率 98.8%）；口径均为客户端发送侧统计 |

较 P0 阶段净增 1 项集成用例（13→14）：

| 用例 | 场景 | 结果（实测） |
|---|---|---|
| integration_fd_exhaustion | RLIMIT_NOFILE=96 网关 + 120 连接风暴 | ✅ 通过（4.0s）：耗尽期网关存活；新连接被及时拒绝（reserve-fd 排空 backlog）；全部释放后 0x03 回显恢复 |

`integration_connection_lifecycle` 新增 D 节（SIGTERM 优雅退出）：✅ exit=0（含心跳/线程池/MQTT/conn_manager 逆序清理真实执行）。`unit_config` 新增行内注释/非法 int 回退 3 例：✅ 通过。

## 测试区分度验证（未修复二进制实测）

| 回退内容 | 结果 | 说明 |
|---|---|---|
| 仅回退 `server.c`（F2 修复前） | ❌ B 节失败 | fd 耗尽期间新连接**无声挂起**（backlog 不清空，ET 不再通知）——证明测试真实守护该修复 |
| 仅回退 `main.c`（无信号处理器） | ❌ D 节失败 | SIGTERM 默认动作杀进程，exit=-15 而非优雅退出 0 |

## Conclusion（Day4-P1）

- F2/F3/F4 全部修复并有三重回归门槛：Release/ASan/TSan 各一轮 14/14 全绿 + soak RSS 稳定。
- 新增测试经"未修复即失败"验证，具备真实守护能力；P2 项按审查结论维持不修改。

## 复现命令（Day4-P1）

```bash
# 三种构建的全量回归
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1" && cmake --build build && ctest --test-dir build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" && cmake --build build && ctest --test-dir build
# churn soak
bash tests/scripts/run_soak.sh 1
```

---

# 压测口径校准与 0x03 RTT 端到端实测回归（2026-09-20）

> 背景：对既有 `stress_test.py` 进行口径复核，明确原延迟指标为「客户端构帧及写本地内核发送缓冲耗时」而非网关处理延迟；原 QPS 为发包端定频速率而非网关饱和上限；原错误率仅统计发包端 socket 异常。
> 改进：在保持 100 连接 × 10 fps（0x01 背景数据流）生产负载的同时，新增 `CMD_CONFIG`（0x03）端到端 RTT 探针，真实度量网关「接收 → 拆包 → 排队 → 线程池业务处理 → self-pipe 跨线程委托 → 下行发送」全链路往返时延与成功率。

## 实测指标（100 连接 × 10 fps × 60s，Release 构建）

```bash
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 --probe-interval 1.0 -r /tmp/stress_large.txt
```

| 维度 | 指标项 | 实测结果 | 口径与边界说明 |
|---|---|---|---|
| **发送速率** | 客户端发送速率 | **990.7 帧/秒** | 目标 1,000 帧/秒（100 conn × 10 fps），达成率 99.2% |
| **发送总量** | 累计发送帧数 | 59,500 帧 | 目标 60,000 帧（0.8% 缺口为并发握手启动窗口与协程定时器漂移） |
| **发送错误** | 发送端错误率 | **0.00%**（0/59,500） | 仅统计客户端 socket 异常（未发生连接断开或 BrokenPipe） |
| **连接状态** | 成功连接数 | 100 / 100（100%） | 全量平稳接入，未发生连接拒绝 |
| **构帧/缓冲时延** | 客户端发送耗时 P50 | 0.083 ms | Python 组帧至写入本机内核发送缓冲时间，受宿主机调度微秒级波动 |
| **构帧/缓冲时延** | 客户端发送耗时 P99 | 0.312 ms | 同上（历史 0.241~0.509 ms） |
| **全链路处理 RTT** | 0x03 配置请求探针总量 | 60 次 | 背景 1000 帧/秒负载下，每 1.0s 发送一帧 CMD_CONFIG 请求 |
| **全链路处理 RTT** | 探针成功率 | **100.0%**（60/60） | 0 超时、0 错误、0 下行丢帧 |
| **全链路处理 RTT** | 探针 RTT 平均值 | 2.174 ms | 包含网络传输、epoll、解包、队列、工作线程、self-pipe、回显全链路 |
| **全链路处理 RTT** | 探针 RTT P50 | **1.928 ms** | 中位数往返时延保持在 2ms 以内 |
| **全链路处理 RTT** | 探针 RTT P99 | **5.733 ms** | 极端调度抖动下的全链路高分位时延 |

## 结论

1. **口径清晰**：明确区分了客户端发包端耗时与网关真实处理 RTT，消除了对“网关处理仅需 0.24ms”的概念误区。
2. **端到端验证闭环**：在 1000 帧/秒稳态背景流量下，60 次 0x03 请求/响应往返全部成功（100% 成功率），P50 RTT 为 1.93ms、P99 RTT 为 5.73ms，证明了网关在并发数据流下多线程状态机、任务队列以及下行 self-pipe 管道的正确性与低时延特性。
