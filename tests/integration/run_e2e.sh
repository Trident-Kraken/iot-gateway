#!/usr/bin/env bash
# iot-gateway 端到端冒烟：broker → 网关 → 设备模拟器 → MQTT 断言 → 0x03 下行 → 清理
#
# 退出码：0 = 通过；1 = 失败；77 = 环境不满足，跳过（CTest SKIP 语义）
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"   # 网关以仓库根为 cwd（依赖 config/、logs/ 相对路径）

GATEWAY="$ROOT_DIR/build/src/iot-gateway"
SIM="$ROOT_DIR/tests/integration/device_simulator.py"
CFG_TEST="$ROOT_DIR/tests/integration/config_downlink_test.py"
PID_FILE="/tmp/iotgw_e2e_mosquitto.pid"
GW_LOG="/tmp/iotgw_e2e_gw.log"
MQTT_OUT="/tmp/iotgw_e2e_mqtt.txt"

HOST=127.0.0.1
GW_PORT=8080
MQTT_PORT=1883
TOPIC="sensor/data"

BROKER_PID=""
GW_PID=""

log() { echo "[run_e2e] $*"; }

# ---------- 环境探测（不满足 → 77 跳过，避免误报失败） ----------
command -v mosquitto      >/dev/null 2>&1 || { log "SKIP: mosquitto 未安装"; exit 77; }
command -v mosquitto_sub  >/dev/null 2>&1 || { log "SKIP: mosquitto_sub 未安装"; exit 77; }
[ -x "$GATEWAY" ] || { log "SKIP: 网关未构建（先执行 cmake --build build）"; exit 77; }

# 当前环境是否允许创建 socket（如沙箱限制）
if ! python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); s.close()' 2>/dev/null; then
    log "SKIP: 当前环境无法创建 socket"
    exit 77
fi

# 网关端口被外部占用 → 跳过（避免误杀用户进程）
if (echo > "/dev/tcp/$HOST/$GW_PORT") 2>/dev/null; then
    log "SKIP: 端口 $GW_PORT 已被占用（外部网关在运行？）"
    exit 77
fi

cleanup() {
    [ -n "$GW_PID" ] && kill "$GW_PID" 2>/dev/null || true
    if [ -n "$BROKER_PID" ]; then
        kill "$BROKER_PID" 2>/dev/null || true
        rm -f "$PID_FILE"
    fi
    rm -f "$MQTT_OUT"
}
trap cleanup EXIT

# ---------- 1. Broker：已运行则复用，否则自起 ----------
if (echo > "/dev/tcp/$HOST/$MQTT_PORT") 2>/dev/null; then
    log "复用已运行的 broker ($HOST:$MQTT_PORT)"
else
    log "启动 mosquitto ($HOST:$MQTT_PORT)"
    rm -f "$PID_FILE"
    mosquitto -d -p "$MQTT_PORT" --pid-file "$PID_FILE" 2>/dev/null
    [ -f "$PID_FILE" ] && BROKER_PID="$(cat "$PID_FILE")"
    for _ in $(seq 1 20); do
        (echo > "/dev/tcp/$HOST/$MQTT_PORT") 2>/dev/null && break
        sleep 0.5
    done
    if ! (echo > "/dev/tcp/$HOST/$MQTT_PORT") 2>/dev/null; then
        log "SKIP: broker 无法启动（环境限制）"
        exit 77
    fi
fi

# ---------- 2. 网关 ----------
log "启动网关 ($GATEWAY)"
"$GATEWAY" >"$GW_LOG" 2>&1 &
GW_PID=$!
for _ in $(seq 1 20); do
    (echo > "/dev/tcp/$HOST/$GW_PORT") 2>/dev/null && break
    sleep 0.5
done
if ! (echo > "/dev/tcp/$HOST/$GW_PORT") 2>/dev/null; then
    log "FAIL: 网关未就绪（日志尾部）："
    tail -5 "$GW_LOG" 2>/dev/null
    exit 1
fi

# ---------- 3. MQTT 断言（先订阅再产生流量，QoS0 不重放） ----------
timeout 40 mosquitto_sub -h "$HOST" -p "$MQTT_PORT" -t "$TOPIC" -C 2 \
    >"$MQTT_OUT" 2>/dev/null &
SUB_PID=$!
sleep 1   # 等待订阅生效

log "运行设备模拟器（2 设备 × 2Hz × 3s）"
python3 "$SIM" --host "$HOST" --port "$GW_PORT" --devices 2 --freq 2 --duration 3 || true

wait "$SUB_PID" 2>/dev/null || true
if [ ! -s "$MQTT_OUT" ]; then
    log "FAIL: 未收到 MQTT 消息"
    exit 1
fi
log "MQTT 断言通过：收到 $(wc -l < "$MQTT_OUT") 条 $TOPIC"

# ---------- 4. CMD_CONFIG 下行 ----------
log "验证 CMD_CONFIG 下行"
python3 "$CFG_TEST" "$HOST" "$GW_PORT"

log "E2E 全部通过"
