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
| 100 连接压测回归（`stress_test.py -c 100 -f 10 -d 60`） | ⏳ 待运行（历史存档：991.1 QPS / 0.00% 错误，来源 v0.1.0 存档，待复测后更新） |
| 30 分钟 soak（`run_soak.sh 30`） | ⏳ 待运行 |
| 1 分钟 soak 冒烟（实测） | ✅ RSS 6200→6320 kB（+120 kB，阈值 8192），网关全程存活 |

# Conclusion

- **核心链路（P0）全部通过**：协议解析正确性（单测 56 断言）、数据收发全链路（E2E）、错误处理（配置 fail-fast、心跳超时、断连清理、离线缓存、故障注入）。
- **内存可靠性**：单测 valgrind 0 泄漏 0 错误；soak 冒烟 RSS 稳定。
- **未覆盖/待运行**：100 连接压测回归、30 分钟 soak、CI（GitHub Actions）结果确认。
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
