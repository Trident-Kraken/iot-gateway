#!/usr/bin/env python3
"""
fd 耗尽（EMFILE）处理集成测试（Day4 F2 回归，自带低 ulimit 网关实例）：

  通过 preexec_fn 把网关的 RLIMIT_NOFILE 压到 96，再发起 120 个并发连接：
  1. fd 耗尽期间网关必须存活；
  2. 耗尽期间的新连接必须被"及时拒绝"（reserve fd 排空 backlog），
     而不是无声挂起（修复前：backlog 不清空，ET 模式下监听 fd 永不再
     触发事件，新连接无限期挂起）；
  3. 客户端全部释放后，网关必须完全恢复（新连接 + 0x03 配置回显可用）。

退出码：0 通过；1 失败；77 环境不满足（跳过）。
"""
import os
import resource
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GW_BIN = os.path.join(ROOT, "build", "src", "iot-gateway")
HOST = "127.0.0.1"
PORT = 8084  # 与 8080/8081/8082/8083 错开

NOFILE_LIMIT = 96   # 网关进程 fd 上限（含 stdio/listen/epoll/pipe/log 等开销）
CLIENTS = 120       # > NOFILE_LIMIT，必然触发 EMFILE

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


def build_frame(cmd: int = 0x03, payload: bytes = b"") -> bytes:
    head = b"\xAA\x55" + struct.pack(">H", len(payload)) + bytes([cmd])
    crc = crc16(head[2:] + payload)
    return head + payload + struct.pack(">H", crc)


def wait_port(port: int, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def low_nofile():
    """preexec_fn：在 fork 后、exec 前压缩子进程 fd 上限。"""
    resource.setrlimit(resource.RLIMIT_NOFILE,
                       (NOFILE_LIMIT, NOFILE_LIMIT))


def config_echo(sock: socket.socket, timeout: float = 5.0) -> bool:
    """发送 CMD_CONFIG 请求并等待默认配置 JSON 回显。"""
    sock.settimeout(timeout)
    sock.sendall(build_frame(0x03, b""))
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            break
        except OSError:
            break
        if not data:
            break
        buf += data
        if b"sampling_interval" in buf:
            return True
    return b"sampling_interval" in buf


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
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft != resource.RLIM_INFINITY and soft < NOFILE_LIMIT:
        print(f"[SKIP] 当前环境 RLIMIT_NOFILE={soft} 低于测试所需 {NOFILE_LIMIT}")
        return 77

    # --- 临时 cwd + 低 ulimit 网关 ---
    tmp = tempfile.mkdtemp(prefix="iotgw_fdex_")
    os.makedirs(os.path.join(tmp, "config"))
    with open(os.path.join(tmp, "config", "gateway.ini"), "w") as f:
        f.write(CONFIG)

    gw = subprocess.Popen([GW_BIN], cwd=tmp, preexec_fn=low_nofile,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    clients = []
    try:
        if not wait_port(PORT):
            print("[FAIL] 网关未就绪（低 ulimit 下启动失败？）")
            return 1
        time.sleep(0.5)

        # --- 1. 120 连接打满 fd：网关存活 ---
        for _ in range(CLIENTS):
            try:
                c = socket.create_connection((HOST, PORT), timeout=2.0)
                clients.append(c)
            except OSError:
                pass
        time.sleep(1.0)
        check("A. fd 耗尽风暴后网关进程存活", gw.poll() is None,
              f"(connected={len(clients)}/{CLIENTS})")

        # --- 2. 耗尽期间新连接被及时拒绝（而非无声挂起） ---
        # 修复前：backlog 残留 → ET 不再通知 → 该 recv 永远超时。
        probe = socket.create_connection((HOST, PORT), timeout=2.0)
        probe.settimeout(3.0)
        rejected = False
        try:
            probe.sendall(build_frame(0x03, b""))
            data = probe.recv(4096)
            rejected = (data == b"")  # 对端关闭 = 明确拒绝
        except (ConnectionResetError, BrokenPipeError):
            rejected = True  # RST = 明确拒绝
        except socket.timeout:
            rejected = False  # 无声挂起 = 修复缺失
        except OSError:
            rejected = True
        probe.close()
        check("B. fd 耗尽期间新连接被及时拒绝", rejected)
        check("B2. 拒绝期间网关进程存活", gw.poll() is None)

        # --- 3. 释放全部连接后完全恢复 ---
        for c in clients:
            try:
                c.close()
            except OSError:
                pass
        clients = []
        time.sleep(2.0)

        recovered = False
        for _ in range(3):
            try:
                c = socket.create_connection((HOST, PORT), timeout=2.0)
                if config_echo(c, timeout=5.0):
                    recovered = True
                c.close()
            except OSError:
                pass
            if recovered:
                break
            time.sleep(1.0)
        check("C. 连接释放后网关完全恢复（0x03 回显可用）", recovered)
    finally:
        for c in clients:
            try:
                c.close()
            except OSError:
                pass
        if gw.poll() is None:
            gw.terminate()
            try:
                gw.wait(timeout=10)
            except subprocess.TimeoutExpired:
                gw.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"FAIL: {len(failures)} 项失败")
        return 1
    print("fd_exhaustion: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
