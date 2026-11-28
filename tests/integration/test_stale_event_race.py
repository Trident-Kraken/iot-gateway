#!/usr/bin/env python3
"""
心跳超时关闭竞态回归测试（自带网关实例）：

缺陷背景（修复前）：
  epoll_wait 返回的 events[] 是快照。同一批次内，pipe 消息（心跳超时关闭）
  先关闭连接后，批内残留的该连接陈旧 EPOLLIN/EPOLLHUP 事件会对已释放的
  连接上下文二次 connection_close → 对象池节点双重归还 → 空闲链自环 →
  后续所有新连接共享同一连接上下文（连接跟踪整体腐坏）。

本测试构造该竞态的高压场景：
  循环多轮「连接 → 短暂活跃 → 在心跳超时判定边界（interval=1/timeout=1，
  约 t≈2s）附近突发数据 → 立即断连」，让设备末帧/FIN 与网关的
  超时关闭请求反复落入同一事件批次的竞争窗口。

验证 canary（任一腐坏即失败）：
  1. 每轮断连后，新连接发送 CMD_CONFIG(0x03) 请求必须收到下行响应帧
     （下行经 self-pipe + conn_list_find(fd, generation) 发送，连接池/
     连接链表一旦腐坏则查不到连接、无响应）；
  2. 末尾双连接并发请求均收到响应；
  3. 全程网关存活、fd 数量稳定。

说明：竞态窗口为微秒级，本测试是概率性 churn canary 而非确定性复现；
其价值在于覆盖高并发场景下「超时关闭 × 在途数据」churn 竞态窗口，
并守护对象池/连接链表不变量。

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
HOST = "127.0.0.1"
PORT = 8083  # 与 E2E(8080)/生命周期(8081)/MQTT缓存(8082)错开

ROUNDS = 10          # churn 轮数：每轮约 2.5s
BURST_FRAMES = 12    # 超时边界附近的突发帧数
CANARY_TIMEOUT = 2.0

CONFIG = """[server]
server_port=%d
[thread_pool]
thread_pool_size=2
[heartbeat]
heartbeat_interval=1
heartbeat_timeout=1
[log]
log_level=ERROR
[mqtt]
mqtt_broker=127.0.0.1
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


def build_frame(cmd: int = 0x02, payload: bytes = b"") -> bytes:
    head = b"\xAA\x55" + struct.pack(">H", len(payload)) + bytes([cmd])
    crc = crc16(head[2:] + payload)
    return head + payload + struct.pack(">H", crc)


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


def config_echo_canary(name: str) -> bool:
    """新连接发送 CMD_CONFIG 请求，必须收到 0x03 下行响应帧。

    下行链路 = 工作线程 self-pipe 委托 → epoll 线程 conn_list_find(fd,
    generation) 匹配 → send。连接池/连接链表腐坏时 find 失败、无响应，
    因此收到响应即可证明连接跟踪仍然完整。
    """
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CANARY_TIMEOUT)
    except OSError:
        return False
    try:
        sock.settimeout(CANARY_TIMEOUT)
        sock.sendall(build_frame(0x03))
        buf = b""
        # 读完整帧：帧头(2) + 长度(2) + 命令字(1) + payload + CRC(2)
        while len(buf) < 7:
            chunk = sock.recv(64)
            if not chunk:
                return False
            buf += chunk
        if buf[0] != 0xAA or buf[1] != 0x55 or buf[4] != 0x03:
            return False
        total = 7 + struct.unpack(">H", buf[2:4])[0]
        while len(buf) < total:
            chunk = sock.recv(64)
            if not chunk:
                return False
            buf += chunk
        return len(buf) >= total
    except OSError:
        return False
    finally:
        sock.close()


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

    # --- 临时 cwd：最短心跳配置（interval=1/timeout=1），不改仓库 config/ ---
    tmp = tempfile.mkdtemp(prefix="iotgw_race_")
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
        time.sleep(1.0)  # 让 MQTT 管理线程/日志稳定，避免 fd 计数抖动
        baseline = fd_count(gw.pid)

        ok_rounds = 0
        for r in range(ROUNDS):
            # 1) 连接并激活（last_active = t0）
            sock = socket.create_connection((HOST, PORT), timeout=1.0)
            sock.sendall(build_frame(0x02))  # 心跳帧注册活跃

            # 2) 静默至超时判定边界：interval=1/timeout=1 时，
            #    心跳线程在 t≈1 与 t≈2 扫描，t≈2 时判定过期并发关闭请求
            time.sleep(1.95)

            # 3) 边界突发数据：跨越 t≈2.0 的扫描/pipe 写入时刻，
            #    与超时关闭竞争（连接已被网关关闭时会收到 RST，忽略）
            try:
                for _ in range(BURST_FRAMES):
                    sock.sendall(build_frame(0x02))
                    time.sleep(0.007)  # 12 帧 × 7ms ≈ 覆盖 84ms 窗口
            except OSError:
                pass  # 对端已关（RST）——正是竞态发生的表现之一

            # 4) 立即断连：FIN/HUP 同样与关闭请求竞争
            sock.close()
            time.sleep(0.3)

            # 5) canary：连接跟踪必须仍然完整
            if gw.poll() is not None:
                check(f"第 {r + 1} 轮网关存活", False)
                return 1
            if config_echo_canary(f"round-{r + 1}"):
                ok_rounds += 1
            else:
                check(f"第 {r + 1} 轮 config 下行回显", False)
                return 1  # 连接跟踪已腐坏，后续轮无意义

        check(f"{ROUNDS} 轮超时边界 churn 后网关存活", gw.poll() is None)
        check(f"每轮 config 下行回显均成功", ok_rounds == ROUNDS,
              f"({ok_rounds}/{ROUNDS})")

        # 6) 双连接并发回显：对象池若自环，两个连接会共享同一上下文，
        #    至少一个连接的下行查不到
        s1 = socket.create_connection((HOST, PORT), timeout=1.0)
        s2 = socket.create_connection((HOST, PORT), timeout=1.0)
        for s in (s1, s2):
            s.sendall(build_frame(0x02))  # 各自注册活跃
        time.sleep(0.2)
        for s in (s1, s2):
            s.sendall(build_frame(0x03))
        dual_ok = True
        for s in (s1, s2):
            try:
                s.settimeout(CANARY_TIMEOUT)
                buf = b""
                while len(buf) < 7:
                    chunk = s.recv(64)
                    if not chunk:
                        dual_ok = False
                        break
                    buf += chunk
                if len(buf) < 7 or buf[4] != 0x03:
                    dual_ok = False
            except OSError:
                dual_ok = False
            s.close()
        check("双连接并发 config 下行回显", dual_ok)

        # 7) fd 稳定（无连接/解析器上下文泄漏）
        after = fd_count(gw.pid)
        check("churn 后 fd 数量稳定",
              baseline >= 0 and after >= 0 and after <= baseline + 5,
              f"(baseline={baseline}, after={after})")
    finally:
        gw.terminate()
        try:
            gw.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gw.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"FAIL: {len(failures)} 项失败")
        return 1
    print("stale_event_race: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
