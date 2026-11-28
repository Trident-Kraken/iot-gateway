#!/usr/bin/env python3
"""
连接生命周期集成测试（自带网关实例，使用临时目录 + 短心跳超时配置）：
  A. 心跳超时断开：设备发送一帧后静默，网关应在超时窗口内关闭连接
  B. 半包后断开：发送不完整帧后直接 close，网关应存活且可继续 accept
  C. 重复连接清理：多次连接/断开后 fd 数量保持稳定（无泄漏）
  D. SIGTERM 优雅退出：停机信号后清理路径可达，退出码 0

退出码：0 通过；1 失败；77 环境不满足（跳过）。
"""
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GW_BIN = os.environ.get("IOT_GATEWAY_BIN") or os.path.join(
    ROOT, "build", "src", "iot-gateway")
HOST = "127.0.0.1"
PORT = 8081  # 与 E2E 默认 8080 错开

CONFIG = """[server]
server_port=%d
[thread_pool]
thread_pool_size=2
[heartbeat]
heartbeat_interval=1
heartbeat_timeout=3
[log]
log_level=INFO
[mqtt]
mqtt_broker=localhost
mqtt_port=1883
mqtt_qos=0
""" % PORT


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
    """发送完整帧（默认 CMD_HEARTBEAT 0x02）。"""
    head = b"\xAA\x55" + struct.pack(">H", len(payload)) + bytes([cmd])
    crc = crc16(head[2:] + payload)
    sock.sendall(head + payload + struct.pack(">H", crc))


def port_free(port: int) -> bool:
    try:
        with socket.create_connection((HOST, port), timeout=0.3):
            return False
    except OSError:
        return True


def wait_port(port: int, timeout: float = 10.0, proc=None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def fd_count(pid: int) -> int:
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return -1


def main() -> int:
    failures = []

    def check(name: str, cond: bool, detail: str = "") -> None:
        status = "PASS" if cond else "FAIL"
        print(f"[{status}] {name} {detail}")
        if not cond:
            failures.append(name)

    # --- 环境检查 ---
    if not os.path.isfile(GW_BIN):
        print("[SKIP] 网关未构建（先执行 cmake --build build）")
        return 77
    try:
        probe = socket.socket()
        probe.bind((HOST, 0))
        probe.close()
    except OSError:
        print("[SKIP] 当前环境无法创建 socket")
        return 77
    if not port_free(PORT):
        print(f"[SKIP] 端口 {PORT} 已被占用")
        return 77

    # --- 准备临时 cwd（短心跳超时配置，不改仓库 config/） ---
    tmp = tempfile.mkdtemp(prefix="iotgw_lifecycle_")
    cfg_dir = os.path.join(tmp, "config")
    os.makedirs(cfg_dir)
    with open(os.path.join(cfg_dir, "gateway.ini"), "w") as f:
        f.write(CONFIG)

    gw = subprocess.Popen([GW_BIN], cwd=tmp,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_port(PORT, proc=gw):
            print("[FAIL] 网关未就绪")
            return 1
        time.sleep(1.0)  # 让 MQTT 管理线程稳定（避免 fd 计数抖动）
        baseline = fd_count(gw.pid)

        # --- A. 心跳超时断开 ---
        sock = socket.create_connection((HOST, PORT), timeout=1.0)
        send_frame(sock)  # 注册活跃后静默
        closed_at = None
        deadline = time.time() + 9  # interval 1s + timeout 3s + 余量
        sock.settimeout(1.0)
        while time.time() < deadline:
            try:
                data = sock.recv(1)
                if data == b"":
                    closed_at = time.time()
                    break
            except socket.timeout:
                continue
            except OSError:
                closed_at = time.time()
                break
        sock.close()
        check("A. 心跳超时后连接被关闭", closed_at is not None,
              f"(窗口 9s)")

        # --- B. 半包后断开：网关存活且可继续 accept ---
        s1 = socket.create_connection((HOST, PORT), timeout=1.0)
        s1.sendall(b"\xAA\x55\x00")  # 不完整帧
        s1.close()
        time.sleep(0.5)
        check("B. 半包断开后网关进程存活", gw.poll() is None)
        s2 = socket.create_connection((HOST, PORT), timeout=1.0)
        s2.close()
        check("B. 半包断开后仍可接受新连接", True)

        # --- C. 多次连接/断开后 fd 稳定（无泄漏） ---
        for _ in range(3):
            sc = socket.create_connection((HOST, PORT), timeout=1.0)
            send_frame(sc)
            time.sleep(0.2)
            sc.close()
        time.sleep(1.0)
        after = fd_count(gw.pid)
        check("C. 连接清理后 fd 数量稳定",
              baseline >= 0 and after >= 0 and after <= baseline + 5,
              f"(baseline={baseline}, after={after})")

        # --- D. SIGUSR1 实时指标（via gateway_stats.sh） + SIGTERM 优雅退出 ---
        log_path = os.path.join(tmp, "logs", "gateway.log")
        stats_script = os.path.join(ROOT, "tools", "gateway_stats.sh")
        stats_res = subprocess.run(
            ["bash", stats_script, str(gw.pid), log_path],
            capture_output=True, text=True, timeout=5,
        )
        check("D1. gateway_stats.sh 触发 SIGUSR1 并采集快照且进程存活",
              stats_res.returncode == 0
              and "stats[sigusr1]:" in stats_res.stdout
              and gw.poll() is None,
              f"(rc={stats_res.returncode})")

        # 停机信号后：信号处理器置位停机标志 → 唤醒 epoll_wait → 关闭全部连接
        # → main 走完逆序清理（心跳/线程池/MQTT/conn_manager/logger/config）→ exit 0。
        gw.send_signal(signal.SIGTERM)
        try:
            exit_code = gw.wait(timeout=15)
        except subprocess.TimeoutExpired:
            exit_code = None
        check("D2. SIGTERM 后网关优雅退出 (exit=0)",
              exit_code == 0, f"(exit={exit_code})")

        try:
            with open(log_path) as lf:
                log_text = lf.read()
        except OSError:
            log_text = ""
        check("D3. 日志落盘 SIGUSR1 与停机统计快照",
              "stats[sigusr1]:" in log_text and "stats[shutdown]:" in log_text)
    finally:
        if gw.poll() is None:
            gw.terminate()
            try:
                gw.wait(timeout=5)
            except subprocess.TimeoutExpired:
                gw.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"FAIL: {len(failures)} 项失败")
        return 1
    print("connection_lifecycle: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
