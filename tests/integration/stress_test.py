#!/usr/bin/env python3
"""
TCP stress test for iot-gateway — asyncio high-concurrency device simulator.

Generates realistic sensor traffic to measure gateway throughput, latency,
and stability under load.

Protocol frame layout (big-endian, must match protocol.h exactly):
    Offset  Size  Field
    ------  ----  -----
       0     2    Header: 0xAA 0x55
       2     2    Payload length (big-endian, max 1024)
       4     1    Command (0x01 = sensor data)
       5     N    Payload (JSON string)
     5+N     2    CRC16-CCITT-FALSE over [length + command + payload]

Usage:
    python3 tests/stress_test.py                          # defaults: 100 conn, 10 fps, 60s
    python3 tests/stress_test.py --connections 500 --fps 20 --duration 120
    python3 tests/stress_test.py --host 192.168.1.100 --port 8080 --connections 50
"""

import argparse
import asyncio
import json
import os
import random
import signal
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# CRC16-CCITT-FALSE (polynomial 0x1021, initial 0xFFFF)
# Must match protocol.c exactly.
# ---------------------------------------------------------------------------


def _build_crc_table() -> list:
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc = crc << 1
        table.append(crc & 0xFFFF)
    return table


_CRC_TABLE = _build_crc_table()


def crc16(data: bytes) -> int:
    """Compute CRC16-CCITT-FALSE over *data*."""
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) ^ _CRC_TABLE[((crc >> 8) ^ byte) & 0xFF]) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# Frame builder
# ---------------------------------------------------------------------------

HEADER = b'\xAA\x55'
CMD_SENSOR_DATA = 0x01
CMD_CONFIG = 0x03


def build_frame(device_id: str) -> bytes:
    """Build a valid protocol frame with random sensor data."""
    payload = json.dumps({
        "device_id": device_id,
        "temp": round(random.uniform(20.0, 35.0), 1),
        "hum": round(random.uniform(30.0, 80.0), 1),
        "timestamp": int(time.time()),
    }).encode('utf-8')

    length_field = struct.pack('>H', len(payload))
    cmd_field = bytes([CMD_SENSOR_DATA])
    crc = crc16(length_field + cmd_field + payload)
    crc_field = struct.pack('>H', crc)

    return HEADER + length_field + cmd_field + payload + crc_field


def build_config_probe_frame() -> bytes:
    """Build a CMD_CONFIG (0x03) request frame with empty payload."""
    length_field = struct.pack('>H', 0)
    cmd_field = bytes([CMD_CONFIG])
    crc = crc16(length_field + cmd_field)
    return HEADER + length_field + cmd_field + struct.pack('>H', crc)


async def read_config_response(reader: asyncio.StreamReader) -> tuple:
    """Read and validate one complete CMD_CONFIG response frame from reader.

    Returns (cmd, payload_bytes).
    """
    hdr = await reader.readexactly(2)
    if hdr != HEADER:
        raise ValueError(f"Invalid frame header: {hdr.hex()}")
    len_bytes = await reader.readexactly(2)
    (plen,) = struct.unpack('>H', len_bytes)
    cmd_bytes = await reader.readexactly(1)
    cmd = cmd_bytes[0]
    payload = await reader.readexactly(plen)
    crc_bytes = await reader.readexactly(2)
    (got_crc,) = struct.unpack('>H', crc_bytes)
    exp_crc = crc16(len_bytes + cmd_bytes + payload)
    if exp_crc != got_crc:
        raise ValueError(f"CRC mismatch: exp={exp_crc:#x} got={got_crc:#x}")
    return cmd, payload


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------


@dataclass
class ConnMetrics:
    """Per-connection counters."""

    sent: int = 0
    connect_errors: int = 0
    send_errors: int = 0
    latencies: list = field(default_factory=list)  # list[float] in seconds


@dataclass
class GlobalMetrics:
    """Aggregated test metrics."""

    start_time: float = 0.0
    end_time: float = 0.0
    total_sent: int = 0
    total_connect_errors: int = 0
    total_send_errors: int = 0
    all_latencies: list = field(default_factory=list)

    @property
    def duration(self) -> float:
        return self.end_time - self.start_time

    @property
    def qps(self) -> float:
        return self.total_sent / self.duration if self.duration > 0 else 0.0

    @property
    def avg_latency(self) -> Optional[float]:
        if not self.all_latencies:
            return None
        return sum(self.all_latencies) / len(self.all_latencies)

    @property
    def p50_latency(self) -> Optional[float]:
        return self._percentile(50)

    @property
    def p99_latency(self) -> Optional[float]:
        return self._percentile(99)

    @property
    def error_rate(self) -> float:
        total = self.total_sent + self.total_send_errors
        if total == 0:
            return 0.0
        return self.total_send_errors / total * 100.0

    def _percentile(self, pct: float) -> Optional[float]:
        if not self.all_latencies:
            return None
        latencies = sorted(self.all_latencies)
        idx = int(len(latencies) * pct / 100.0)
        if idx >= len(latencies):
            idx = len(latencies) - 1
        return latencies[idx]


@dataclass
class ProbeMetrics:
    """Aggregated metrics for CMD_CONFIG (0x03) request/response RTT probe."""

    total_probes: int = 0
    successful_probes: int = 0
    timeouts: int = 0
    errors: int = 0
    rtt_latencies: list = field(default_factory=list)  # list[float] in seconds

    @property
    def avg_rtt(self) -> Optional[float]:
        if not self.rtt_latencies:
            return None
        return sum(self.rtt_latencies) / len(self.rtt_latencies)

    @property
    def p50_rtt(self) -> Optional[float]:
        return self._percentile(50)

    @property
    def p99_rtt(self) -> Optional[float]:
        return self._percentile(99)

    @property
    def min_rtt(self) -> Optional[float]:
        return min(self.rtt_latencies) if self.rtt_latencies else None

    @property
    def max_rtt(self) -> Optional[float]:
        return max(self.rtt_latencies) if self.rtt_latencies else None

    def _percentile(self, pct: float) -> Optional[float]:
        if not self.rtt_latencies:
            return None
        lat = sorted(self.rtt_latencies)
        idx = int(len(lat) * pct / 100.0)
        if idx >= len(lat):
            idx = len(lat) - 1
        return lat[idx]


async def run_config_probe(
    host: str,
    port: int,
    interval: float,
    timeout: float,
    stop_event: asyncio.Event,
    probe_metrics: ProbeMetrics,
):
    """Periodically send CMD_CONFIG (0x03) to measure true gateway RTT under load."""
    req_frame = build_config_probe_frame()

    # Small warmup delay to let background load establish
    try:
        await asyncio.wait_for(stop_event.wait(), timeout=min(0.2, interval))
        return
    except asyncio.TimeoutError:
        pass

    reader: Optional[asyncio.StreamReader] = None
    writer: Optional[asyncio.StreamWriter] = None

    async def cleanup_conn():
        nonlocal reader, writer
        if writer is not None:
            try:
                writer.close()
                await writer.wait_closed()
            except OSError:
                pass
        reader = None
        writer = None

    while not stop_event.is_set():
        if writer is None:
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(host, port),
                    timeout=timeout,
                )
            except asyncio.TimeoutError:
                probe_metrics.total_probes += 1
                probe_metrics.timeouts += 1
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=interval)
                    break
                except asyncio.TimeoutError:
                    continue
            except Exception:
                probe_metrics.total_probes += 1
                probe_metrics.errors += 1
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=interval)
                    break
                except asyncio.TimeoutError:
                    continue

        probe_metrics.total_probes += 1
        t0 = time.perf_counter()
        try:
            writer.write(req_frame)
            await writer.drain()

            cmd, payload = await asyncio.wait_for(
                read_config_response(reader),
                timeout=timeout,
            )
            if cmd != CMD_CONFIG:
                probe_metrics.errors += 1
                await cleanup_conn()
            else:
                elapsed = time.perf_counter() - t0
                probe_metrics.successful_probes += 1
                probe_metrics.rtt_latencies.append(elapsed)
        except asyncio.TimeoutError:
            probe_metrics.timeouts += 1
            await cleanup_conn()
        except Exception:
            probe_metrics.errors += 1
            await cleanup_conn()

        try:
            await asyncio.wait_for(stop_event.wait(), timeout=interval)
            break
        except asyncio.TimeoutError:
            pass

    await cleanup_conn()


# ---------------------------------------------------------------------------
# Single connection coroutine
# ---------------------------------------------------------------------------

PROGRESS_INTERVAL = 5.0  # seconds between live status lines


async def run_connection(
    conn_id: int,
    host: str,
    port: int,
    fps: float,
    duration: float,
    metrics: list,  # out param: list of ConnMetrics
    stop_event: asyncio.Event,
    progress_lock: asyncio.Lock,
    global_metrics: GlobalMetrics,
):
    """One async TCP connection sending frames at *fps* Hz."""
    cm = ConnMetrics()
    metrics.append(cm)
    device_id = f"stress_{conn_id:05d}"
    interval = 1.0 / fps

    # Connect with retry
    for attempt in range(3):
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(host, port),
                timeout=5.0,
            )
            break
        except (OSError, asyncio.TimeoutError) as e:
            if attempt == 2:
                cm.connect_errors += 1
                return
            await asyncio.sleep(0.5 * (attempt + 1))
    else:
        return

    deadline = time.monotonic() + duration
    try:
        while not stop_event.is_set():
            now = time.monotonic()
            if now >= deadline:
                break

            t0 = time.perf_counter()
            frame = build_frame(device_id)

            try:
                writer.write(frame)
                await writer.drain()
                elapsed = time.perf_counter() - t0
                cm.sent += 1
                cm.latencies.append(elapsed)
            except OSError:
                cm.send_errors += 1

            # Rate-limiting sleep (same perf_counter clock base as t0)
            next_tick = t0 + interval
            sleep_for = next_tick - time.perf_counter()
            if sleep_for > 0:
                await asyncio.sleep(sleep_for)
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Live status printer
# ---------------------------------------------------------------------------


async def progress_reporter(
    metrics: list,
    global_metrics: GlobalMetrics,
    probe_metrics: Optional[ProbeMetrics],
    stop_event: asyncio.Event,
    lock: asyncio.Lock,
):
    """Periodically print live stats."""
    last_total = 0
    last_time = time.monotonic()

    while not stop_event.is_set():
        await asyncio.sleep(PROGRESS_INTERVAL)
        async with lock:
            if metrics:
                total_sent = sum(m.sent for m in metrics)
                total_errs = sum(m.send_errors for m in metrics)
            else:
                total_sent = 0
                total_errs = 0

        now = time.monotonic()
        elapsed = now - last_time
        instant_rate = (total_sent - last_total) / elapsed if elapsed > 0 else 0

        probe_info = ""
        if probe_metrics and probe_metrics.total_probes > 0:
            last_rtt = (
                f"{probe_metrics.rtt_latencies[-1] * 1000:.2f}ms"
                if probe_metrics.rtt_latencies
                else "N/A"
            )
            probe_info = (
                f"  probe={probe_metrics.successful_probes}/{probe_metrics.total_probes} "
                f"(last_rtt={last_rtt})"
            )

        print(
            f"  [live] sent={total_sent}  send_errors={total_errs}  "
            f"send_rate={instant_rate:.1f}fps  elapsed={now - global_metrics.start_time:.0f}s{probe_info}",
            flush=True,
        )
        last_total = total_sent
        last_time = now


# ---------------------------------------------------------------------------
# Report generator
# ---------------------------------------------------------------------------


def generate_report(
    global_metrics: GlobalMetrics,
    conn_metrics: list,
    probe_metrics: Optional[ProbeMetrics],
    args: argparse.Namespace,
    report_path: str,
):
    """Generate a plain-text stress report with explicit terminology and caliber."""
    lines = []
    sep = "=" * 60

    lines.append(sep)
    lines.append("  IoT Gateway Stress Test Report")
    lines.append(sep)
    lines.append("")
    lines.append(f"  Target:         {args.host}:{args.port}")
    lines.append(f"  Connections:    {args.connections}")
    lines.append(f"  Target FPS:     {args.fps} per connection")
    lines.append(f"  Target Duration:{args.duration}s")
    lines.append(f"  Actual Duration:{global_metrics.duration:.2f}s")
    lines.append("")

    lines.append("--- 客户端发送速率 (Client Send Rate) ---")
    lines.append(f"  Total frames sent:     {global_metrics.total_sent}")
    lines.append(f"  Send errors:           {global_metrics.total_send_errors}")
    lines.append(f"  Connect failures:      {global_metrics.total_connect_errors}")
    lines.append(f"  Target send rate:      {args.connections * args.fps:,.1f} frames/s")
    lines.append(f"  Client send rate:      {global_metrics.qps:,.1f} frames/s")
    target_total = args.connections * args.fps * args.duration
    if target_total > 0:
        achievement = (global_metrics.total_sent / target_total) * 100.0
        lines.append(f"  Rate achievement:      {achievement:.1f}% (理论目标 {target_total:,.0f} 帧)")
    lines.append(f"  发送错误率 (Send error rate): {global_metrics.error_rate:.2f}% (仅统计客户端 socket 异常)")
    lines.append("")

    lines.append("--- 客户端构帧及发送耗时 (Client Frame Build & Drain Time) ---")
    lines.append("  [口径说明: 测定区间为 Python 组包至写入本地内核发送缓冲区，非网关端到端处理延迟]")
    if global_metrics.avg_latency is not None:
        lines.append(f"  Avg latency:           {global_metrics.avg_latency * 1000:.3f} ms")
    if global_metrics.p50_latency is not None:
        lines.append(f"  P50 latency:           {global_metrics.p50_latency * 1000:.3f} ms")
    if global_metrics.p99_latency is not None:
        lines.append(f"  P99 latency:           {global_metrics.p99_latency * 1000:.3f} ms")
    if global_metrics.all_latencies:
        lines.append(f"  Min latency:           {min(global_metrics.all_latencies) * 1000:.3f} ms")
        lines.append(f"  Max latency:           {max(global_metrics.all_latencies) * 1000:.3f} ms")
    lines.append("")

    if probe_metrics is not None and probe_metrics.total_probes > 0:
        lines.append("--- 配置请求往返时延探针 (0x03 RTT Probe under Load) ---")
        lines.append("  [口径说明: 在背景 0x01 负载下定时向网关发起 CMD_CONFIG 请求，经 epoll->parser->queue->worker->self-pipe->send 全链路返回]")
        lines.append(f"  Total probes:          {probe_metrics.total_probes}")
        lines.append(f"  Successful probes:     {probe_metrics.successful_probes}")
        lines.append(f"  Timeouts:              {probe_metrics.timeouts}")
        lines.append(f"  Errors:                {probe_metrics.errors}")
        succ_rate = (
            (probe_metrics.successful_probes / probe_metrics.total_probes * 100.0)
            if probe_metrics.total_probes > 0
            else 0.0
        )
        lines.append(f"  Probe success rate:    {succ_rate:.1f}%")
        if probe_metrics.avg_rtt is not None:
            lines.append(f"  Avg RTT:               {probe_metrics.avg_rtt * 1000:.3f} ms")
        if probe_metrics.p50_rtt is not None:
            lines.append(f"  P50 RTT:               {probe_metrics.p50_rtt * 1000:.3f} ms")
        if probe_metrics.p99_rtt is not None:
            lines.append(f"  P99 RTT:               {probe_metrics.p99_rtt * 1000:.3f} ms")
        if probe_metrics.min_rtt is not None:
            lines.append(f"  Min RTT:               {probe_metrics.min_rtt * 1000:.3f} ms")
        if probe_metrics.max_rtt is not None:
            lines.append(f"  Max RTT:               {probe_metrics.max_rtt * 1000:.3f} ms")
        lines.append("")

    lines.append("--- Connections ---")
    successful_conns = sum(1 for m in conn_metrics if m.connect_errors == 0)
    failed_conns = sum(1 for m in conn_metrics if m.connect_errors > 0)
    lines.append(f"  Successful connections:{successful_conns}")
    lines.append(f"  Failed connections:    {failed_conns}")

    if successful_conns > 0:
        avg_frames_per_conn = global_metrics.total_sent / successful_conns
        lines.append(f"  Avg frames/conn:       {avg_frames_per_conn:.1f}")
    lines.append("")

    lines.append("--- Test Parameters ---")
    lines.append(f"  Host:           {args.host}")
    lines.append(f"  Port:           {args.port}")
    lines.append(f"  Connections:    {args.connections}")
    lines.append(f"  FPS per conn:   {args.fps}")
    lines.append(f"  Duration:       {args.duration}s")
    if getattr(args, "probe_interval", 0) > 0:
        lines.append(f"  Probe Interval: {args.probe_interval}s (timeout {args.probe_timeout}s)")
    lines.append("")
    lines.append(sep)

    report = "\n".join(lines)
    print(report)

    report_dir = os.path.dirname(os.path.abspath(report_path))
    os.makedirs(report_dir, exist_ok=True)
    with open(report_path, "w") as f:
        f.write(report + "\n")

    print(f"\nReport saved to: {os.path.abspath(report_path)}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def parse_args():
    parser = argparse.ArgumentParser(
        description="IoT Gateway TCP Stress Test (asyncio)"
    )

    parser.add_argument("--host", default="127.0.0.1", help="Gateway host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Gateway TCP port (default: 8080)")
    parser.add_argument(
        "--connections", "-c", type=int, default=100,
        help="Number of concurrent connections (default: 100)",
    )
    parser.add_argument(
        "--fps", "-f", type=float, default=10,
        help="Frames per second per connection (default: 10)",
    )
    parser.add_argument(
        "--duration", "-d", type=float, default=60,
        help="Test duration in seconds (default: 60)",
    )
    parser.add_argument(
        "--report", "-r", default="tests/stress_report.txt",
        help="Report output path (default: tests/stress_report.txt)",
    )
    parser.add_argument(
        "--probe-interval", type=float, default=1.0,
        help="Interval between CMD_CONFIG (0x03) RTT probes in seconds (0 to disable, default: 1.0)",
    )
    parser.add_argument(
        "--probe-timeout", type=float, default=2.0,
        help="Timeout for each CMD_CONFIG RTT probe in seconds (default: 2.0)",
    )

    return parser.parse_args()


async def main_async(args):
    stop_event = asyncio.Event()
    progress_lock = asyncio.Lock()
    metrics: list = []
    global_metrics = GlobalMetrics()
    probe_metrics = ProbeMetrics()

    print("=" * 60)
    print("  IoT Gateway Stress Test")
    print("=" * 60)
    print(f"  Target:          {args.host}:{args.port}")
    print(f"  Connections:     {args.connections}")
    print(f"  FPS/conn:        {args.fps}")
    print(f"  Duration:        {args.duration}s")
    print(f"  Target send rate:{args.connections * args.fps:,.1f} frames/s")
    if args.probe_interval > 0:
        print(f"  RTT Probe (0x03): every {args.probe_interval}s (timeout {args.probe_timeout}s)")
    print("=" * 60)
    print()

    global_metrics.start_time = time.monotonic()

    # Launch reporter
    reporter_task = asyncio.create_task(
        progress_reporter(metrics, global_metrics, probe_metrics, stop_event, progress_lock)
    )

    # Launch probe task if enabled
    probe_task = None
    if args.probe_interval > 0:
        probe_task = asyncio.create_task(
            run_config_probe(
                host=args.host,
                port=args.port,
                interval=args.probe_interval,
                timeout=args.probe_timeout,
                stop_event=stop_event,
                probe_metrics=probe_metrics,
            )
        )

    # Launch all connections
    tasks = []
    for i in range(args.connections):
        task = asyncio.create_task(
            run_connection(
                conn_id=i + 1,
                host=args.host,
                port=args.port,
                fps=args.fps,
                duration=args.duration,
                metrics=metrics,
                stop_event=stop_event,
                progress_lock=progress_lock,
                global_metrics=global_metrics,
            )
        )
        tasks.append(task)

        # Stagger connections to avoid SYN flood
        if (i + 1) % 100 == 0:
            print(f"  Launched {i + 1}/{args.connections} connections...", flush=True)
            await asyncio.sleep(0.02)

    print(f"  All {args.connections} connections launched.")
    print(f"  Running for {args.duration}s... (Ctrl+C to stop early)")
    print()

    # Wait for duration or until all tasks complete
    try:
        connection_tasks = tasks
        await asyncio.wait_for(
            asyncio.gather(*connection_tasks),
            timeout=args.duration,
        )
    except asyncio.TimeoutError:
        pass

    # Signal reporter and probe to stop
    stop_event.set()
    reporter_task.cancel()
    try:
        await reporter_task
    except asyncio.CancelledError:
        pass

    if probe_task is not None:
        try:
            await probe_task
        except asyncio.CancelledError:
            pass

    # Ensure all connection coroutines are done (cancel any stragglers)
    for t in connection_tasks:
        if not t.done():
            t.cancel()
    # Gather with return_exceptions to swallow CancelledError
    await asyncio.gather(*connection_tasks, return_exceptions=True)

    global_metrics.end_time = time.monotonic()

    # Aggregate metrics
    global_metrics.total_sent = sum(m.sent for m in metrics)
    global_metrics.total_connect_errors = sum(m.connect_errors for m in metrics)
    global_metrics.total_send_errors = sum(m.send_errors for m in metrics)
    for m in metrics:
        global_metrics.all_latencies.extend(m.latencies)

    # Print final live line
    probe_summary = ""
    if probe_metrics and probe_metrics.total_probes > 0:
        rtt_p99 = f"{probe_metrics.p99_rtt * 1000:.2f}ms" if probe_metrics.p99_rtt else "N/A"
        probe_summary = f"  probe_success={probe_metrics.successful_probes}/{probe_metrics.total_probes}  rtt_p99={rtt_p99}"
    print(
        f"  [final] sent={global_metrics.total_sent}  "
        f"send_errors={global_metrics.total_send_errors}  "
        f"send_rate={global_metrics.qps:,.1f}fps"
        f"{probe_summary}",
        flush=True,
    )

    generate_report(global_metrics, metrics, probe_metrics, args, args.report)


def main():
    args = parse_args()
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("\nInterrupted.", flush=True)


if __name__ == "__main__":
    main()

