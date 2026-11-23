#!/usr/bin/env python3
"""
故障注入集成测试（自带 broker 1885 + 网关 8084，非 sudo）：

  1. device_simulator 周期注入 4 类异常帧（crc_error / length_overflow
     / bad_header / half_frame）期间：网关进程存活，正常帧仍到达 MQTT
  2. 原始超大长度字段帧（len=0x0800 > 1024）注入：网关不崩，
     后续正常连接仍可建立

退出码：0 通过；1 失败；77 环境不满足（跳过）。
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GW_BIN = os.path.join(ROOT, "build", "src", "iot-gateway")
SIM = os.path.join(ROOT, "tests", "integration", "device_simulator.py")

HOST = "127.0.0.1"
GW_PORT = 8084
MQTT_PORT = 1885
TOPIC = "sensor/data"

CONFIG = """[server]
server_port=%d
[thread_pool]
thread_pool_size=2
[heartbeat]
heartbeat_interval=5
heartbeat_timeout=15
[log]
log_level=ERROR
[mqtt]
mqtt_broker=127.0.0.1
mqtt_port=%d
mqtt_qos=0
""" % (GW_PORT, MQTT_PORT)

# 异常帧不替换正常发送槽（interval 5 > duration 3，仅起始各注入 1 帧）。
# 不破坏流的异常（crc/bad_header/oversized_len）：正常帧应基本全量到达；
# half_frame 会按设计污染注入设备的连接流，只要求另一设备帧到达 + 网关存活。
MIN_NORMAL = 8
MIN_HALF = 4
ANOMALIES = ["crc_error", "length_overflow", "bad_header", "half_frame"]


def _build_crc_table():
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        table.append(crc & 0xFFFF)
    return table


_CRC_TABLE = _build_crc_table()


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc = ((crc << 8) ^ _CRC_TABLE[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF
    return crc


def send_frame(sock: socket.socket, cmd: int = 0x02, payload: bytes = b"") -> None:
    head = b"\xAA\x55" + struct.pack(">H", len(payload)) + bytes([cmd])
    crc = crc16(head[2:] + payload)
    sock.sendall(head + payload + struct.pack(">H", crc))


def wait_port(port: int, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def port_free(port: int) -> bool:
    try:
        with socket.create_connection((HOST, port), timeout=0.3):
            return False
    except OSError:
        return True


def main() -> int:
    if not os.path.isfile(GW_BIN):
        print("[SKIP] 网关未构建")
        return 77
    if not shutil.which("mosquitto") or not shutil.which("mosquitto_sub"):
        print("[SKIP] mosquitto/mosquitto_sub 未安装")
        return 77
    try:
        probe = socket.socket()
        probe.bind((HOST, 0))
        probe.close()
    except OSError:
        print("[SKIP] 当前环境无法创建 socket")
        return 77
    if not port_free(MQTT_PORT) or not port_free(GW_PORT):
        print("[SKIP] 测试端口被占用")
        return 77

    failures = []

    tmp = tempfile.mkdtemp(prefix="iotgw_fault_")
    os.makedirs(os.path.join(tmp, "config"))
    with open(os.path.join(tmp, "config", "gateway.ini"), "w") as f:
        f.write(CONFIG)

    broker = subprocess.Popen(["mosquitto", "-p", str(MQTT_PORT)],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    gw = subprocess.Popen([GW_BIN], cwd=tmp,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_port(MQTT_PORT) or not wait_port(GW_PORT):
            print("[FAIL] broker/网关未就绪")
            return 1

        # --- 1. 4 类异常帧周期性注入 ---
        for anom in ANOMALIES:
            min_ok = MIN_HALF if anom == "half_frame" else MIN_NORMAL
            sub_out_path = os.path.join(tmp, f"fault_sub_{anom}.txt")
            # 订阅端先起，再注入流量（QoS0 不重放）
            with open(sub_out_path, "w") as sub_out:
                sub = subprocess.Popen(
                    ["mosquitto_sub", "-h", HOST, "-p", str(MQTT_PORT),
                     "-t", TOPIC, "-W", "12"],
                    stdout=sub_out,
                    stderr=subprocess.DEVNULL)
                time.sleep(1)
                subprocess.run(
                    [sys.executable, SIM, "--host", HOST, "--port", str(GW_PORT),
                     "--devices", "2", "--freq", "2", "--duration", "3",
                     "--anomaly", anom, "--anomaly-interval", "5"],
                    capture_output=True, text=True, timeout=30)
                sub.wait(timeout=15)
            try:
                with open(sub_out_path) as f:
                    n = sum(1 for _ in f)
            except OSError:
                n = 0
            alive = gw.poll() is None
            ok = alive and n >= min_ok
            status = "PASS" if ok else "FAIL"
            print(f"[{status}] anomaly={anom}: 网关存活={alive} 正常帧到 MQTT={n} (阈值 {min_ok})")
            if not ok:
                failures.append(f"anomaly:{anom}")

        # --- 2. 超大长度字段（0x0800 > 1024）原始注入 ---
        s = socket.create_connection((HOST, GW_PORT), timeout=1.0)
        s.sendall(b"\xAA\x55\x08\x00\x01" + b"\x00" * 8)
        time.sleep(0.5)
        s.close()
        alive = gw.poll() is None
        ok2 = alive
        print(f"[{'PASS' if ok2 else 'FAIL'}] oversized_len: 网关存活={alive}")
        if not ok2:
            failures.append("oversized_len")

        # 注入后正常连接仍可用
        s = socket.create_connection((HOST, GW_PORT), timeout=1.0)
        send_frame(s)
        time.sleep(0.3)
        s.close()
        print(f"[{'PASS' if gw.poll() is None else 'FAIL'}] 注入后正常连接可用")
        if gw.poll() is not None:
            failures.append("post_inject_connect")
    finally:
        for proc in (gw, broker):
            if proc.poll() is None:
                proc.terminate()
        for proc in (gw, broker):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"FAIL: {len(failures)} 项失败: {failures}")
        return 1
    print("fault_injection: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
