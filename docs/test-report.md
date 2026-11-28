# 测试报告 — iot-gateway

> 本报告全部结果来自本地实测运行；标注「待运行」的项目尚未执行。

---

## 1. 测试环境

| 项 | 值 |
|---|---|
| 系统 | Ubuntu 24.04（WSL2 内核），x86_64 |
| 编译器 | GCC 13.3.0（`-Wall -Wextra`，C11） |
| CMake | 4.3.3 |
| Python | 3.12.3 |
| valgrind | 3.22.0 |
| Mosquitto | 2.0.18（broker + clients） |
| 被测目标 | `build/src/iot-gateway` |

---

## 2. 最新全量回归（Release / ASan / TSan 三重门槛）

在最近一次全仓缺陷修复后，使用 `-Wall -Wextra` 编译全部目标（零错误、零警告），执行 16 项 CTest（9 项单元测试/脚本自检 + 7 项集成测试，无跳过、无失败）：

| 构建类型 | 编译选项 | CTest 结果 | 耗时 | Sanitizer / 备注 |
|---|---|---|---|---|
| **Release** | `-O2` | **16/16 PASS** | 99.98 s | 零警告；全部集成测试真实执行 |
| **ASan + UBSan + LSan** | `-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1` | **16/16 PASS** | 99.52 s | `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`，零越界、零泄漏、零 UB |
| **TSan** | `-fsanitize=thread -fno-omit-frame-pointer -g -O1` | **16/16 PASS** | 99.25 s | `setarch $(uname -m) -R` 下执行，零数据竞争报告 |

### 单元测试明细

| 用例 | 覆盖范围 | 断言数 | 结果 |
|---|---|---|---|
| `unit_tests` | CRC16 向量 / 编解码（含 `packet_encode_raw`、空指针与上限校验） / RingBuffer / 状态机拆包粘包坏帧及大载荷跨绕回分片批量读取 | 124 | ✅ 通过 |
| `unit_config` | `config_parser`：trim / 行内注释 / 重复 key 覆盖 / 超长行丢弃 / 溢出与非法整数告警 | 34 | ✅ 通过 |
| `unit_task_queue` | 对象池 FIFO / 有界水位线（2048 满拒入） / shutdown 语义 / 二次初始化完整池容量回归 | 8217 | ✅ 通过 |
| `unit_threadpool` | NULL 回调拒绝 / 1000 任务全处理 / destroy 后提交拒绝 | 1006 | ✅ 通过 |
| `unit_fd_util` | 非阻塞设置 / `FD_CLOEXEC` 设置 / 非法 fd | 17 | ✅ 通过 |
| `unit_ringbuffer_wrap` | head/tail 绕回、满拒写、跨数组边界读写完整性 | 1672 | ✅ 通过 |
| `unit_business` | JSON 同名 value 碰撞、RFC 8259 数字校验、超长数字拒绝、字符串清洗（含 `>=0x7F`）、无 NUL 堆边界、`CMD_CONFIG`/`CMD_HEARTBEAT` 分发 | 42 | ✅ 通过 |
| `python_syntax_check` | 9 个集成 Python 脚本语法校验 | — | ✅ 通过 |
| `crc16_known_vector` | Python 侧 CRC16 与 C 侧一致（`0x29B1`） | 1 | ✅ 通过 |

**valgrind @ unit_tests**（最新全量回归实测）：

```
HEAP SUMMARY: in use at exit: 0 bytes in 0 blocks
total heap usage: 23 allocs, 23 frees, 20,040 bytes allocated
ERROR SUMMARY: 0 errors from 0 contexts
```

---

## 3. 集成测试与故障注入明细

| 用例 | 场景 | 结果 |
|---|---|---|
| `integration_e2e` | broker → 网关 → 2 设备 2Hz → MQTT 断言 → `0x03` 下行 → 清理 | ✅ 通过：正常上报 `sensor/data`，`CMD_CONFIG` 下行 JSON 正确 |
| `integration_connection_lifecycle` | 心跳超时断开 / 半包断开 / fd 稳定性 / `SIGTERM` 优雅停机 | ✅ 通过：超时窗口内断开；半包后网关存活可 accept；fd 无泄漏；`SIGTERM` exit 0 |
| `integration_mqtt_offline_cache` | broker 停机 → 消息入离线缓存 → 恢复 → 冲刷 | ✅ 通过：停机期 12 帧入缓存，恢复后冲刷投递 |
| `integration_config_validation` | 缺失文件 / 非法端口 / 非法线程数 / 非法 QoS | ✅ 通过：5 类非法配置全部 fail-fast exit 1；合法配置正常监听 |
| `integration_fault_injection` | 4 类异常帧注入（`crc_error` / `length_overflow` / `bad_header` / `half_frame`）+ 原始超大长度字段 | ✅ 通过：网关全程存活，正常帧仍可达 MQTT |
| `integration_stale_event_race` | 心跳超时边界 churn ×10 轮 + 双连接并发 `0x03` 回显金丝雀 | ✅ 通过：每轮回显正常，无陈旧事件双重释放/对象池自环，fd 数量稳定 |
| `integration_fd_exhaustion` | `RLIMIT_NOFILE=96` 网关 + 120 连接风暴 | ✅ 通过：耗尽期新连接被及时拒绝（reserve-fd 排空 backlog），释放后完全恢复 |

### 异常与边界矩阵

| 类别 | 场景 | 注入方式 | 结果 |
|---|---|---|---|
| 通信 | 心跳超时 | 设备静默（`heartbeat_timeout=3s` 测试配置） | ✅ 超时窗口内断开 |
| 通信 | 半包后断开 | 发送 `AA 55 00` 后 close | ✅ 网关存活、可继续 accept |
| 通信 | 非法帧头 / CRC 错误 | `--anomaly bad_header / crc_error` | ✅ 帧被丢弃，正常帧不受影响 |
| 通信 | 数据长度错误 | `--anomaly length_overflow`（len=2048>1024）+ 原始 `0x0800` 帧 | ✅ 状态机重置，无越界无崩溃 |
| 资源 | 文件不存在 | 无 `config/gateway.ini` 的临时目录 | ✅ 启动失败 exit 1 |
| 资源 | 配置错误 | `server_port=0/99999`、`thread_pool_size=0`、`mqtt_qos=3` | ✅ 全部 fail-fast exit 1 |
| 资源 | broker 不可达 | 停机 + 12 帧上报 | ✅ 消息入离线缓存，恢复后冲刷 |
| 资源 | fd 耗尽 | `RLIMIT_NOFILE=96` + 120 连接风暴 | ✅ backlog 排空拒绝，不饿死监听 fd |
| 信号 | 优雅停机 | `SIGTERM` 退出信号 | ✅ 关闭全部连接并逆序清理资源，exit 0 |

### 关键修复区分度验证

| 验证场景 | 修复前行为 | 修复后行为 |
|---|---|---|
| `RLIMIT_NOFILE=96` 连接风暴（`test_fd_exhaustion.py` B 节） | backlog 残留导致 ET 监听失活，新连接超时挂起 | reserve fd 排空并关闭积压连接，客户端及时感知断开 |
| `SIGTERM` 优雅停机（`test_connection_lifecycle.py` D 节） | 默认信号终止进程（exit=-15） | 轮询停机标志并执行完整逆序清理（exit=0） |

---

## 4. 长时间稳定性（Soak）与并发压测基线

### 30 分钟 Soak 验证（20 设备 × 2Hz，~7.2 万帧）

- 初始 RSS：`6192 kB` → 结束 RSS：`6316 kB`（增长 `+124 kB`，阈值 `8192 kB`；min/max = `6312/6316 kB`）
- 进程全程存活，状态正常。

### 压测口径校准与 `0x03` RTT 端到端实测回归（2026-09-20）

> 校准说明：早期压测脚本统计的延迟区间为“客户端组包至写入本地内核发送缓冲耗时”，吞吐为“发包端定频速率”。为真实度量网关端到端处理时延，在保持 100 连接 × 10 fps（`0x01` 背景数据流）负载的同时，引入 `CMD_CONFIG`（`0x03`）RTT 探针，贯穿「接收 → 拆包 → 排队 → 工作线程处理 → self-pipe 委托 → 下行发送」全链路。

```bash
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 --probe-interval 1.0 -r /tmp/stress_large.txt
```

| 维度 | 指标项 | 实测结果 | 口径与边界说明 |
|---|---|---|---|
| **发送速率** | 客户端发送速率 | **990.7 帧/秒** | 目标 1,000 帧/秒（100 conn × 10 fps），达成率 99.2% |
| **发送总量** | 累计发送帧数 | 59,500 帧 | 目标 60,000 帧（0.8% 缺口为并发握手启动窗口与协程定时器漂移） |
| **发送错误** | 发送端错误率 | **0.00%**（0/59,500） | 仅统计客户端 socket 异常（未发生连接断开或 BrokenPipe） |
| **连接状态** | 成功连接数 | 100 / 100（100%） | 全量平稳接入，未发生连接拒绝 |
| **构帧/缓冲时延** | 客户端发送耗时 P50 | 0.083 ms | Python 组帧至写入本机内核发送缓冲时间，受宿主机调度波动 |
| **构帧/缓冲时延** | 客户端发送耗时 P99 | 0.312 ms | 同上（历次 0.241~0.509 ms） |
| **全链路处理 RTT** | `0x03` 配置请求探针总量 | 60 次 | 背景 1000 帧/秒负载下，每 1.0s 发送一帧 `CMD_CONFIG` 请求 |
| **全链路处理 RTT** | 探针成功率 | **100.0%**（60/60） | 0 超时、0 错误、0 下行丢帧 |
| **全链路处理 RTT** | 探针 RTT 平均值 | 2.174 ms | 包含网络传输、epoll、解包、队列、工作线程、self-pipe、回显全链路 |
| **全链路处理 RTT** | 探针 RTT P50 | **1.928 ms** | 中位数往返时延保持在 2ms 以内 |
| **全链路处理 RTT** | 探针 RTT P99 | **5.733 ms** | 高分位调度抖动下的全链路往返时延 |

---

## 5. 复现命令

```bash
# Release 构建 + 全量 CTest
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure

# ASan + UBSan / TSan 回归
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" && cmake --build build
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build --output-on-failure

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-Wall -Wextra -fsanitize=thread -fno-omit-frame-pointer -g -O1" && cmake --build build
setarch $(uname -m) -R ctest --test-dir build --output-on-failure

# 压测与 Soak
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 --probe-interval 1.0 -r /tmp/stress_large.txt
bash tests/scripts/run_soak.sh 30
```
