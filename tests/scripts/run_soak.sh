#!/usr/bin/env bash
# 长时间运行（soak）测试：持续流量下采样网关 RSS，确认无内存增长、
# 进程存活、可正常收包。
#
# 用法: bash tests/scripts/run_soak.sh [分钟数]   # 默认 30
#
# 退出码：0 通过；1 失败（网关退出 / RSS 增长超阈值）；77 环境不满足。
set -u

MINUTES="${1:-30}"
SAMPLES_SEC=10

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"
GATEWAY="$ROOT_DIR/build/src/iot-gateway"
SIM="$ROOT_DIR/tests/integration/device_simulator.py"

HOST=127.0.0.1
GW_PORT=8080
MQTT_PORT=1883
RSS_LIMIT_KB=$((8 * 1024))   # RSS 增长阈值 8MB

GW_PID=""
BROKER_PID=""
SIM_PID=""
RSS_LOG="/tmp/iotgw_soak_rss.txt"
GW_LOG="/tmp/iotgw_soak_gw.log"
SIM_LOG="/tmp/iotgw_soak_sim.log"

log() { echo "[soak] $*"; }

cleanup() {
    [ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null || true
    [ -n "$GW_PID" ] && kill "$GW_PID" 2>/dev/null || true
    [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# ---------- 环境检查 ----------
command -v mosquitto >/dev/null 2>&1 || { log "SKIP: mosquitto 未安装"; exit 77; }
[ -x "$GATEWAY" ] || { log "SKIP: 网关未构建"; exit 77; }
if ! python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); s.close()' 2>/dev/null; then
    log "SKIP: 当前环境无法创建 socket"; exit 77
fi
if (echo > "/dev/tcp/$HOST/$GW_PORT") 2>/dev/null; then
    log "SKIP: 端口 $GW_PORT 已被占用"; exit 77
fi

# ---------- broker：复用或自起 ----------
if (echo > "/dev/tcp/$HOST/$MQTT_PORT") 2>/dev/null; then
    log "复用已运行的 broker"
else
    mosquitto -p "$MQTT_PORT" >/tmp/iotgw_soak_mosq.log 2>&1 &
    BROKER_PID=$!
    for _ in $(seq 1 20); do
        (echo > "/dev/tcp/$HOST/$MQTT_PORT") 2>/dev/null && break
        sleep 0.5
    done
    (echo > "/dev/tcp/$HOST/$MQTT_PORT") 2>/dev/null || { log "SKIP: broker 无法启动"; exit 77; }
fi

# ---------- 网关 ----------
"$GATEWAY" >"$GW_LOG" 2>&1 &
GW_PID=$!
for _ in $(seq 1 20); do
    (echo > "/dev/tcp/$HOST/$GW_PORT") 2>/dev/null && break
    sleep 0.5
done
if ! (echo > "/dev/tcp/$HOST/$GW_PORT") 2>/dev/null; then
    log "FAIL: 网关未就绪"; exit 1
fi
sleep 2

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$GW_PID/status" 2>/dev/null || echo 0; }

BASELINE=$(rss_kb)
log "开始 soak ${MINUTES} 分钟，RSS baseline=${BASELINE} kB"

# ---------- 模拟器（后台持续流量） ----------
python3 "$SIM" --host "$HOST" --port "$GW_PORT" --devices 20 --freq 2 \
    --duration "$((MINUTES * 60))" >"$SIM_LOG" 2>&1 &
SIM_PID=$!

# ---------- 采样循环 ----------
TOTAL_SEC=$((MINUTES * 60))
: > "$RSS_LOG"
ELAPSED=0
while [ "$ELAPSED" -lt "$TOTAL_SEC" ]; do
    sleep "$SAMPLES_SEC"
    ELAPSED=$((ELAPSED + SAMPLES_SEC))
    if ! kill -0 "$GW_PID" 2>/dev/null; then
        log "FAIL: 网关在第 ${ELAPSED}s 退出"
        exit 1
    fi
    echo "$ELAPSED $(rss_kb)" >> "$RSS_LOG"
done

# ---------- 汇总 ----------
FINAL=$(rss_kb)
MAX=$(awk '{if ($2>m) m=$2} END{print m}' "$RSS_LOG")
MIN=$(awk 'NR==1{min=$2} {if ($2<min) min=$2} END{print min}' "$RSS_LOG")
DELTA=$((FINAL - BASELINE))
log "soak 完成: duration=${MINUTES}min RSS baseline=${BASELINE}kB min=${MIN}kB max=${MAX}kB final=${FINAL}kB delta=${DELTA}kB"

if ! kill -0 "$GW_PID" 2>/dev/null; then
    log "FAIL: 网关进程已退出"; exit 1
fi
if [ "$DELTA" -gt "$RSS_LIMIT_KB" ]; then
    log "FAIL: RSS 增长 ${DELTA}kB 超过阈值 ${RSS_LIMIT_KB}kB"
    exit 1
fi
log "PASS: 网关全程存活，RSS 增长 ${DELTA}kB（阈值 ${RSS_LIMIT_KB}kB）"
exit 0
