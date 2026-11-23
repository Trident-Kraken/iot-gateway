# Wireshark 抓包验证指南 — iot-gateway

> 适用环境：WSL2 + Ubuntu 24.04，iot-gateway 端口 **8080**，MQTT Broker 端口 **1883**

---

## 1. 启动 Wireshark 并选择网卡

WSL2 下推荐两种方式：

### 方式 A：Linux 原生（tshark 命令行，最稳定）

```bash
# 安装
sudo apt install tshark

# 抓取 lo（回环接口）上的流量，限 8080 + 1883 两个端口
sudo tshark -i lo -f "tcp port 8080 or tcp port 1883" -w /tmp/iot-gateway.pcap
```

`-w` 写入 pcap 文件，稍后可用 Wireshark GUI 打开分析。

### 方式 B：Windows Wireshark GUI（图形化操作）

1. 打开 Windows 上的 Wireshark
2. 选择网卡：WSL2 流量走 `vEthernet (WSL)` 或 `Ethernet 2`（视你的 WSL 版本而定）
   - 不确定哪个网卡有流量？先选一个，抓包时在 Gateway 上发几帧，看是否有 `8080` 端口的包出现
3. 双击网卡名即开始抓包

> **WSL2 特殊说明**：虚拟机与宿主机之间通过 Hyper-V 虚拟交换机通信。`127.0.0.1:8080` 的请求在 Windows Host 抓包时帧会出现在 `vEthernet (WSL)` 网卡上。如果看不到，直接用 `tshark -i lo` 在 Linux 侧抓，然后用 Wireshark 打开 pcap 文件。

---

## 2. 过滤器表达式

在 Wireshark 顶部 Filter 栏输入以下任一表达式，按回车：

| 目的 | 表达式 |
|---|---|
| 只看网关协议流量（TCP 8080） | `tcp.port == 8080` |
| 只看 MQTT 流量（TCP 1883） | `mqtt` |
| 只看本地回环流量 | `ip.addr == 127.0.0.1` |
| 组合：8080 且不含 MQTT | `tcp.port == 8080 and not mqtt` |
| 只看特定帧头（0xAA 55） | `tcp.payload contains aa:55` |
| 只看特定命令字（0x01=传感器数据） | `tcp.payload[4:1] == 01` |

常用组合：

```
tcp.port == 8080 and tcp.payload contains aa:55
```

这行过滤出所有携带 `AA 55` 帧头的 TCP segment，一键看到所有自定义协议帧。

---

## 3. 验证 TCP 三次握手

在 Filter 栏输入：

```
tcp.port == 8080 and tcp.flags != 0
```

Wireshark 会高亮显示 SYN / SYN-ACK / ACK 三次握手：

### 期望看到的三条报文

| 序号 | 方向 | Flags | 说明 |
|---|---|---|---|
| ① | 客户端 → 网关 (8080) | `[SYN]` | 客户端发起连接，Seq=0 |  
| ② | 网关 (8080) → 客户端 | `[SYN, ACK]` | 网关确认，Seq=0, Ack=1 |
| ③ | 客户端 → 网关 (8080) | `[ACK]` | 客户端确认，Seq=1, Ack=1 |

### 确认握手成功

双击第②条报文（SYN-ACK），展开 `Transmission Control Protocol` 层：
- **Source Port = 8080** → 说明网关在 8080 端口上响应了
- **Flags = 0x012 (SYN, ACK)** → 两个标志位同时置位，握手第 2 步正确

三次握手之后，客户端和网关之间的 TCP 连接进入 `ESTABLISHED` 状态，开始传输数据帧。

---

## 4. 查看自定义协议帧内容

### 方法：Follow TCP Stream → Hex Dump

1. 在 Filter 栏输入 `tcp.port == 8080 and tcp.payload`
2. 右键任意一条匹配的报文 → **Follow** → **TCP Stream**
3. 在弹窗中：
   - "Show and save data as" → 选择 **Hex Dump**
   - 对话框底部选择流向：`<client> → <gateway:8080>`（只看客户端发的帧）

### 预期看到的内容

Hex Dump 中每条帧格式：

```
0000  aa 55 00 4a 01 7b 22 64  65 76 69 63 65 5f 69 64   .U.J.{"device_id
0010  22 3a 20 22 64 65 76 5f  30 30 31 22 2c 20 22 74   ": "dev_001", "t
0020  65 6d 70 22 3a 20 32 35  2e 35 2c 20 22 68 75 6d   emp": 25.5, "hum
0030  22 3a 20 36 30 2c 20 22  74 69 6d 65 73 74 61 6d   ": 60, "timestam
0040  70 22 3a 20 31 37 31 38  31 38 38 38 30 30 7d b9   p": 1718188800}.
0050  f3                                                .
```

### 逐字节对照验证

对照 [protocol.h](../include/protocol/protocol.h) 帧布局：

| 偏移 | 原始字节 | 字段 | 预期值 |
|---|---|---|---|
| 0x00 | `AA 55` | 帧头 | `0xAA 0x55` |
| 0x02 | `00 4A` | Payload 长度（big-endian） | `0x004A` = 74 字节 |
| 0x04 | `01` | 命令字 | `0x01`（传感器数据） |
| 0x05 | `7B 22 64 …` | Payload（JSON） | `{"device_id": "dev_001"…`（含空格，json.dumps 默认格式） |
| 末尾 2 字节 | `B9 F3` | CRC16-CCITT-FALSE | 覆盖 [长度+命令+Payload] |

### 确认 CRC 正确

- 用 Python 快速验算一笔：

```python
import sys
sys.path.insert(0, "tests/integration")
from device_simulator import crc16, build_normal_frame

frame = build_normal_frame("dev_test", 25.5, 60, 1718188800)
print(frame.hex(' '))        # 打印十六进制
# 手动对比 Wireshark Hex Dump 的最后一帧，CRC 字段应一致
```

---

## 5. 查看 MQTT PUBLISH 报文

在 Filter 栏输入：

```
mqtt
```

Wireshark 会自动解析 MQTT 协议层。

### 展开 PUBLISH 报文

双击一条 `Publish Message` → 展开 `MQ Telemetry Transport Protocol`：

```
MQ Telemetry Transport Protocol, Publish Message
    Header Flags: 0x30 (Publish Message)
    Msg Len: 123
    Topic: sensor/data                     ← 确认 Topic
    Message: {"device_id":"dev_001","temp"... ← Payload（JSON）
```

三项核心检查：

| 字段 | 期望值 |
|---|---|
| Topic | `sensor/data` |
| QoS | `0`（fire-and-forget） |
| Payload | JSON 字符串，含 `device_id`、`temp`、`hum`、`timestamp`、`gateway_time`（网关重打包后无空格） |

### CONNECT 报文也值得一看

第一条 MQTT 报文通常是 `Connect Command`：

```
MQ Telemetry Transport Protocol, Connect Command
    Protocol Name: MQTT
    Protocol Version: 5.0 (or 3.1.1)
    Client ID: auto-<random>
    Keep Alive: 30
    Clean Session: 1
```

这说明 `mqtt_client_init()` 的 `mosquitto_new(NULL, true, NULL)` 和 `mosquitto_connect(…, 30)` 正确生效。

---

## 6. 截图保存建议

至少保存 **4 张**截图，按数据流向覆盖完整验证链路：

| 截图编号 | 内容 | Wireshark 视图 |
|---|---|---|
| ① | TCP 三次握手 | Filter: `tcp.port == 8080 and tcp.flags != 0`，展开前 3 个报文 |
| ② | 自定义协议帧 Hex Dump | Follow TCP Stream → Hex Dump，标出 `AA 55` 帧头 |
| ③ | MQTT CONNECT 报文 | Filter: `mqtt`，展开第一条 Connect Command |
| ④ | MQTT PUBLISH 报文 | Filter: `mqtt`，展开一条 Publish Message，显示 Topic 和 Payload |

截图命名建议：

```
screenshots/
├── 01_tcp_handshake.png
├── 02_custom_frame_hex.png
├── 03_mqtt_connect.png
└── 04_mqtt_publish.png
```

---

## 7. 快速操作清单

```bash
# 1. 启动抓包（Linux 侧）
sudo tshark -i lo -f "tcp port 8080 or tcp port 1883" -w /tmp/iot-demo.pcap

# 2. 另一个终端：启动 Mosquitto Broker
sudo systemctl start mosquitto

# 3. 启动网关
cd ~/projects/iot-gateway && ./build/src/iot-gateway

# 4. 另一个终端：启动模拟器
python3 tests/integration/device_simulator.py --host 127.0.0.1 --port 8080 --devices 1 --freq 2

# 5. 另一个终端：订阅 MQTT（验证数据可被消费）
mosquitto_sub -t 'sensor/data' -h localhost -p 1883

# 6. Ctrl+C 停止抓包，用 Wireshark 打开 pcap
# Windows: 双击 /tmp/iot-demo.pcap（需先复制到 Windows 可访问路径）
```
