#!/usr/bin/env python3
"""
配置校验集成测试：非法配置应 fail-fast 拒绝启动（非零退出 + 错误日志），
合法配置应正常监听（正对照）。通过临时 cwd 加载不同 config/gateway.ini，
不修改仓库 config/。

退出码：0 通过；1 失败；77 环境不满足（跳过）。
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GW_BIN = os.path.join(ROOT, "build", "src", "iot-gateway")
HOST = "127.0.0.1"


def free_port() -> int:
    """探测一个空闲端口（用于合法配置的正对照）。"""
    for port in range(8100, 8200):
        try:
            with socket.create_connection((HOST, port), timeout=0.3):
                continue
        except OSError:
            return port
    return 8083


def base_config(port: int) -> str:
    return (
        "[server]\n"
        "server_port=%d\n"
        "[thread_pool]\n"
        "thread_pool_size=4\n"
        "[heartbeat]\n"
        "heartbeat_interval=5\n"
        "heartbeat_timeout=15\n"
        "[log]\n"
        "log_level=ERROR\n"
        "[mqtt]\n"
        "mqtt_broker=127.0.0.1\n"
        "mqtt_port=1883\n"
        "mqtt_qos=0\n" % port)


def main() -> int:
    if not os.path.isfile(GW_BIN):
        print("[SKIP] 网关未构建")
        return 77
    try:
        probe = socket.socket()
        probe.bind((HOST, 0))
        probe.close()
    except OSError:
        print("[SKIP] 当前环境无法创建 socket")
        return 77

    port = free_port()
    failures = []

    # (名称, 覆盖的配置行或 None=缺失文件, 期望退出码 0=保持运行)
    cases = [
        ("missing_file", None, 1),
        ("bad_port_low",  "server_port=0", 1),
        ("bad_port_high", "server_port=99999", 1),
        ("bad_threads",   "thread_pool_size=0", 1),
        ("bad_qos",       "mqtt_qos=3", 1),
        ("valid",         None, 0),
    ]

    for name, override, expect_code in cases:
        tmp = tempfile.mkdtemp(prefix=f"iotgw_cfgtest_{name}_")
        try:
            if name == "missing_file":
                pass  # 不创建 config/ → config_load 失败
            else:
                os.makedirs(os.path.join(tmp, "config"))
                cfg = base_config(port)
                if override:
                    key = override.split("=")[0]
                    lines = [ln for ln in cfg.splitlines()
                             if not ln.startswith(key)]
                    cfg = "\n".join(lines) + "\n" + override + "\n"
                with open(os.path.join(tmp, "config", "gateway.ini"), "w") as f:
                    f.write(cfg)

            proc = subprocess.Popen(
                [GW_BIN], cwd=tmp,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

            if expect_code == 0:
                # 正对照：应保持运行并监听端口。
                ok = False
                deadline = time.time() + 6
                while time.time() < deadline:
                    if proc.poll() is not None:
                        break
                    try:
                        with socket.create_connection((HOST, port), timeout=0.3):
                            ok = True
                            break
                    except OSError:
                        time.sleep(0.2)
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                status = "PASS" if ok else "FAIL"
                print(f"[{status}] {name}: 合法配置正常监听")
                if not ok:
                    failures.append(name)
            else:
                # 非法配置：应在数秒内以非零码退出。
                try:
                    rc = proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    rc = None
                ok = rc is not None and rc != 0
                status = "PASS" if ok else "FAIL"
                print(f"[{status}] {name}: 退出码={rc}（期望非零）")
                if not ok:
                    failures.append(name)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"FAIL: {len(failures)} 项失败: {failures}")
        return 1
    print("config_validation: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
