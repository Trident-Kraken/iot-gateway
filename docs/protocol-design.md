# 协议设计文档 — iot-gateway

> 目标读者：对接设备端开发、协议扩展者、代码审查者。
> 对应实现：`include/protocol/protocol.h`、`src/protocol/protocol.c`（编解码/CRC）、
> `include/protocol/packet_parser.h`、`src/protocol/packet_parser.c`（流式状态机拆包）。

---

## 1. 帧格式

自定义二进制协议，**大端（网络字节序）**：

```
 偏移    大小   字段
 ------  ----   ---------------------------------------------
    0     2     帧头：固定 0xAA 0x55（互补字节，误匹配概率 1/65536）
    2     2     长度：Payload 长度（不含帧头、长度字段、CRC）
    4     1     命令字：见 §2
    5     N     Payload（N = 长度字段的值）
  5+N     2     CRC16 覆盖范围：[长度 + 命令字 + Payload]
```

总帧大小 = 2 + 2 + 1 + N + 2 = **7 + N** 字节。

对应常量（`protocol.h`）：

| 常量 | 值 | 含义 |
|---|---|---|
| `PACKET_HEADER_0` / `PACKET_HEADER_1` | `0xAA` / `0x55` | 帧头 |
| `PACKET_MIN_SIZE` | 7 | 最小帧（空 Payload） |
| `PROTOCOL_MAX_PAYLOAD_LEN` | 1024 | 最大 Payload，防御超大帧 |
| `PROTOCOL_MAX_FRAME_SIZE` | 1031 | 最大整帧 = 7 + 1024 |

## 2. 命令字

| 命令字 | 宏 | 方向 | Payload | 当前状态 |
|---|---|---|---|---|
| `0x01` | `CMD_SENSOR_DATA` | 设备 → 网关 | JSON 字符串 | ✅ 已实现（MQTT 上报 `sensor/data`） |
| `0x02` | `CMD_HEARTBEAT` | 设备 → 网关 | 通常为空 | ✅ 已实现（刷新活跃时间） |
| `0x03` | `CMD_CONFIG` | 设备 → 网关（配置请求） / 网关 → 设备（下行响应） | JSON/配置数据 | ✅ 已实现（请求/响应式下行，见 §6） |

## 3. CRC16

- 算法：**CRC16-CCITT-FALSE**，多项式 `0x1021`，初值 `0xFFFF`，无反射、无异或输出。
- 覆盖范围：`[长度(2B) + 命令字(1B) + Payload(NB)]`，**不含帧头**。
- 校验值以大端写入帧尾。
- 已知向量：`crc16("123456789") == 0x29B1`（CTest 用例 `crc16_known_vector` 持续守护该防线）。
- 实现：`protocol.c` 用查表法（`crc16_init()` 建表，`crc16_compute()` 计算）；Python 侧
  `tests/integration/device_simulator.py` / `stress_test.py` 内各有一份等价实现，压测/模拟器与网关必须严格一致。

## 4. 粘包/拆包状态机

`packet_parser` 在 epoll I/O 线程逐字节消费每连接独立的 RingBuffer：

```
WAIT_HEAD → WAIT_LEN → WAIT_PAYLOAD → WAIT_CRC → FRAME_COMPLETE
```

| 状态 | 消费内容 | 进入条件 |
|---|---|---|
| `WAIT_HEAD` | 2B 帧头 | 匹配 `0xAA 0x55` |
| `WAIT_LEN` | 2B 长度（BE） | 已收齐帧头 |
| `WAIT_PAYLOAD` | 1B 命令字 + N 字节 Payload | 已收齐长度 |
| `WAIT_CRC` | 2B CRC | 已收齐 Payload |
| `FRAME_COMPLETE` | 整帧回调 `on_frame_complete()` | CRC 校验通过 |

三种场景处理：
- **粘包**：一帧完成后继续消费 RingBuffer 中剩余字节进入下一帧解析；
- **拆包**：字节不足时停留在当前状态，等待后续 `recv` 数据追加；
- **坏帧**：帧头不匹配或 CRC 校验失败即重置状态机丢弃该帧，不进入任务队列。

边界防御：`temp_buf` 大小 = `PARSER_MAX_FRAME_SIZE`（运行时按 `sizeof(ctx->temp_buf)` 检查，
是 `PARSER_MAX_PAYLOAD_LEN` 之后的最后一道防线）；`rb_write()` 满时重置状态机并返回错误，由上层关闭连接，绝不静默丢帧。

## 5. 示例帧（CRC 已验算）

最小帧（Payload 为 `{}`，2 字节）：

```
AA 55 00 02 01 7B 7D B0 CD
│   │  │  │  │   │   └─ CRC(2B, BE)
│   │  │  │  │   └─ Payload: 7B 7D ("{}")
│   │  │  │  └─ 命令字: 01 (CMD_SENSOR_DATA)
│   │  │  └─ 长度: 00 02
│   │  └─ 帧头: AA 55
```

设备模拟器真实帧（`dev-001`, temp=25.5, hum=60.0, timestamp=1700000000，共 83 字节）：

```
AA 55 00 4C 01 7B 22 64 65 76 69 63 65 5F 69 64 22 3A 20 22 64 65 76 2D 30 30 31 22 2C 20
22 74 65 6D 70 22 3A 20 32 35 2E 35 2C 20 22 68 75 6D 22 3A 20 36 30 2E 30 2C 20 22 74
69 6D 65 73 74 61 6D 70 22 3A 20 31 37 30 30 30 30 30 30 30 30 7D B2 83
```

其中 `00 4C` 是长度（76 字节 JSON Payload），`B2 83` 是 CRC。

---

## 6. CMD_CONFIG 配置下行（请求/响应）

协议表 §2 中 `0x03 CMD_CONFIG` 原始方向为「网关 → 设备（下行）」。当前网关没有云端
下行触发源（MQTT 侧仅实现 publish、不实现 subscribe），也没有设备配置数据源，因此
落地为**请求/响应式下行**：

```
设备 ── CMD_CONFIG 请求（Payload 可为空或 JSON）──▶ 网关
网关 ── CMD_CONFIG 响应（Payload = 默认设备配置 JSON）──▶ 设备
```

- 设备发送 `0x03` 帧（Payload 通常为空，表示「请求我的配置」）。
- 网关 `business_handler` 的 `handle_config()` 打包默认配置 JSON
  （`{"sampling_interval":5,"upload_freq":2,"heartbeat_interval":5}`），
  用 `packet_encode()` 编码为 `0x03` 帧。
- 下行帧经 self-pipe 委托 **epoll 线程**发送（`epoll_worker_request_send()`），
  非阻塞 + `MSG_NOSIGNAL`，并携带 fd + generation 双重匹配，连接已关闭或 fd 被
  新连接复用时静默丢弃（best-effort，可重发）。
- 验证：`tests/integration/config_downlink_test.py`（连接网关 → 发空 `0x03` 请求 →
  校验回帧的帧头/CRC/命令字/配置 JSON）。
