#!/usr/bin/env bash
# iot-gateway 运行时状态与统计快照诊断脚本
# 用法: bash tools/gateway_stats.sh [pid] [log_path]
set -u

PID="${1:-}"
LOG_PATH="${2:-logs/gateway.log}"

if [ -z "$PID" ]; then
    PID="$(pgrep -xo iot-gateway 2>/dev/null || true)"
fi

if [ -z "$PID" ]; then
    echo "iot-gateway is not running" >&2
    exit 1
fi

if [ ! -r "/proc/$PID/status" ]; then
    echo "cannot inspect /proc/$PID/status" >&2
    exit 1
fi

echo "=== Process Status (PID=$PID) ==="
grep -E '^(Name|State|Threads|VmRSS):' "/proc/$PID/status" 2>/dev/null || true
if [ -d "/proc/$PID/fd" ]; then
    FD_COUNT="$(ls -1 "/proc/$PID/fd" 2>/dev/null | wc -l)"
    echo "OpenFDs:	${FD_COUNT}"
fi

kill -USR1 "$PID"
sleep 0.15

if [ -f "$LOG_PATH" ]; then
    echo "=== Latest Gateway Stats ($LOG_PATH) ==="
    grep 'stats\[' "$LOG_PATH" | tail -n 1 || true
else
    echo "log file not found at $LOG_PATH (SIGUSR1 sent to PID=$PID)"
fi
