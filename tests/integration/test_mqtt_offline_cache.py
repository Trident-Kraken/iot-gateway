#!/usr/bin/env python3
"""
MQTT 离线缓存恢复集成测试（自带独立 broker 与网关实例，非 sudo）：

  Phase 1: broker 停机 → 网关运行 → 模拟器上报 12 帧 → 消息应进入
           有界离线缓存（而非丢弃）
  Phase 2: 启动 broker → 网关管理线程退避重连 → 冲刷缓存 → 订阅端
           收到缓存消息

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
GW_BIN = os.environ.get("IOT_GATEWAY_BIN") or os.path.join(
    ROOT, "build", "src", "iot-gateway")
SIM = os.path.join(ROOT, "tests", "integration", "device_simulator.py")

HOST = "127.0.0.1"
GW_PORT = 8082
MQTT_PORT = 1884
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

EXPECTED = 12   # 2 设备 × 2Hz × 3s
MIN_ACCEPT = 10 # 至少收到 10/12 才判定冲刷成功


def _build_crc_table():
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        table.append(crc & 0xFFFF)
    return table


_CRC_TABLE = _build_crc_table()


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
    # --- 环境检查 ---
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
    if not port_free(MQTT_PORT):
        print(f"[SKIP] 端口 {MQTT_PORT} 已被占用")
        return 77
    if not port_free(GW_PORT):
        print(f"[SKIP] 端口 {GW_PORT} 已被占用")
        return 77

    # --- 临时 cwd + 网关（Phase 1：broker 未启动） ---
    tmp = tempfile.mkdtemp(prefix="iotgw_cache_")
    os.makedirs(os.path.join(tmp, "config"))
    with open(os.path.join(tmp, "config", "gateway.ini"), "w") as f:
        f.write(CONFIG)

    gw = subprocess.Popen([GW_BIN], cwd=tmp,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    broker_proc = None
    sub = None
    try:
        if not wait_port(GW_PORT):
            print("[FAIL] 网关未就绪")
            return 1

        # Phase 1: broker 停机，模拟器上报 → 应全部进离线缓存。
        time.sleep(1.0)
        sim = subprocess.run(
            [sys.executable, SIM, "--host", HOST, "--port", str(GW_PORT),
             "--devices", "2", "--freq", "2", "--duration", "3"],
            capture_output=True, text=True, timeout=30)
        print(sim.stdout.strip().splitlines()[-3:])
        time.sleep(1.0)  # 让缓存入队落定
        if gw.poll() is not None:
            print("[FAIL] broker 停机期间网关进程退出")
            return 1
        print(f"[PASS] Phase1: broker 停机期间网关存活，12 帧应已入缓存")

        # Phase 2: 前台启动 broker（Popen 管理，mosquitto 2.x 无 --pid-file）
        broker_proc = subprocess.Popen(
            ["mosquitto", "-p", str(MQTT_PORT)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_port(MQTT_PORT):
            print("[FAIL] broker 未就绪")
            return 1

        out_file = os.path.join(tmp, "mqtt_cache_msgs.txt")
        with open(out_file, "w") as sub_out:
            sub = subprocess.Popen(
                ["mosquitto_sub", "-h", HOST, "-p", str(MQTT_PORT),
                 "-t", TOPIC, "-C", str(EXPECTED), "-W", "30"],
                stdout=sub_out, stderr=subprocess.DEVNULL)

            # 等待订阅建立 + 网关退避重连（1~3s）与冲刷。
            time.sleep(2)
            sub.wait(timeout=35)
        try:
            with open(out_file) as f:
                count = sum(1 for _ in f)
        except OSError:
            count = 0

        if count >= MIN_ACCEPT:
            print(f"[PASS] Phase2: 离线缓存冲刷成功，收到 {count}/{EXPECTED} 条")
        else:
            print(f"[FAIL] Phase2: 仅收到 {count}/{EXPECTED} 条（期望 >= {MIN_ACCEPT}）")
            return 1

        if gw.poll() is not None:
            print("[FAIL] broker 恢复后网关进程退出")
            return 1
        print("[PASS] broker 恢复后网关存活")
    finally:
        for proc, name in ((sub, "subscriber"), (gw, "gateway"),
                           (broker_proc, "broker")):
            if proc is not None and proc.poll() is None:
                proc.terminate()
        for proc, name in ((sub, "subscriber"), (gw, "gateway"),
                           (broker_proc, "broker")):
            if proc is not None:
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    print("mqtt_offline_cache: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
