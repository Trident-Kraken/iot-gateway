#!/usr/bin/env python3
"""
TCP device simulator for testing the iot-gateway (C language industrial IoT gateway).

Simulates temperature/humidity sensors that connect to the gateway and send data
in the custom binary protocol.

Protocol frame layout (big-endian):
    Offset  Size  Field
    ------  ----  -----
       0     2    Header: 0xAA 0x55
       2     2    Payload length (big-endian, max 1024)
       4     1    Command (0x01 = sensor data)
       5     N    Payload (JSON string)
     5+N     2    CRC16-CCITT-FALSE over [length + command + payload]
"""

import argparse
import json
import random
import socket
import struct
import sys
import threading
import time

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
# Frame builders
# ---------------------------------------------------------------------------

HEADER = b'\xAA\x55'
CMD_SENSOR_DATA = 0x01


def build_normal_frame(device_id: str, temp: float, hum: float,
                       timestamp: int) -> bytes:
    """Build a valid protocol frame carrying a sensor-data JSON payload."""
    payload = json.dumps({
        "device_id": device_id,
        "temp": round(temp, 1),
        "hum": round(hum, 1),
        "timestamp": timestamp,
    }).encode('utf-8')

    if len(payload) > 1024:
        raise ValueError(f"Payload too long ({len(payload)} bytes, max 1024)")

    # Field to CRC: length(2) + cmd(1) + payload(N)
    length_field = struct.pack('>H', len(payload))
    cmd_field = bytes([CMD_SENSOR_DATA])
    crc = crc16(length_field + cmd_field + payload)
    crc_field = struct.pack('>H', crc)

    return HEADER + length_field + cmd_field + payload + crc_field


def build_crc_error_frame(device_id: str, temp: float, hum: float,
                          timestamp: int) -> bytes:
    """Build a frame with an intentionally wrong CRC."""
    frame = build_normal_frame(device_id, temp, hum, timestamp)
    # Flip some bits in the last two bytes (CRC field).
    payload = bytearray(frame)
    payload[-1] ^= 0xFF
    return bytes(payload)


def build_length_overflow_frame(*_args) -> bytes:
    """Build a frame with payload_len > PROTOCOL_MAX_PAYLOAD_LEN (1024)."""
    length_field = struct.pack('>H', 2048)        # illegal length (> 1024)
    cmd_field = bytes([CMD_SENSOR_DATA])
    garbage = b'A' * 2048
    crc = crc16(length_field + cmd_field + garbage)
    crc_field = struct.pack('>H', crc)
    return HEADER + length_field + cmd_field + garbage + crc_field


def build_bad_header_frame(device_id: str, temp: float, hum: float,
                           timestamp: int) -> bytes:
    """Build a frame with an invalid header (0x11 0x22)."""
    frame = build_normal_frame(device_id, temp, hum, timestamp)
    return b'\x11\x22' + frame[2:]


def build_half_frame(device_id: str, temp: float, hum: float,
                     timestamp: int) -> bytes:
    """Build an incomplete frame: header + length only (no payload/CRC)."""
    frame = build_normal_frame(device_id, temp, hum, timestamp)
    # Only send header (2) + length (2) = 4 bytes.
    return frame[:4]


# ---------------------------------------------------------------------------
# Anomaly injection helpers
# ---------------------------------------------------------------------------

ANOMALY_CRC_ERROR = 'crc_error'
ANOMALY_LENGTH_OVERFLOW = 'length_overflow'
ANOMALY_BAD_HEADER = 'bad_header'
ANOMALY_HALF_FRAME = 'half_frame'

ANOMALY_BUILDERS = {
    ANOMALY_CRC_ERROR: build_crc_error_frame,
    ANOMALY_LENGTH_OVERFLOW: build_length_overflow_frame,
    ANOMALY_BAD_HEADER: build_bad_header_frame,
    ANOMALY_HALF_FRAME: build_half_frame,
}


def random_sensor_data(device_id: str, timestamp: int) -> tuple:
    """Generate plausible temperature / humidity values."""
    temp = round(random.uniform(20.0, 35.0), 1)
    hum = round(random.uniform(30.0, 80.0), 1)
    return (device_id, temp, hum, timestamp)


# ---------------------------------------------------------------------------
# Device thread
# ---------------------------------------------------------------------------

class SensorDevice(threading.Thread):
    """Simulates one sensor device continuously sending frames."""

    def __init__(self, device_id: str, host: str, port: int,
                 interval: float, anomaly: str | None,
                 anomaly_interval: float | None,
                 print_lock: threading.Lock):
        super().__init__(daemon=True)
        self.device_id = device_id
        self.host = host
        self.port = port
        self.interval = interval
        self.anomaly = anomaly
        self.anomaly_interval = anomaly_interval
        self.print_lock = print_lock
        self._stop_event = threading.Event()

        # Stats
        self.sent_count = 0
        self.anomaly_count = 0
        self.connect_failures = 0
        self._last_anomaly_time = 0.0

    def stop(self):
        self._stop_event.set()

    def log(self, msg: str):
        with self.print_lock:
            print(f"[{self.device_id}] {msg}", flush=True)

    def send_frame(self, sock: socket.socket, frame: bytes):
        sock.sendall(frame)
        hex_bytes = ' '.join(f'{b:02X}' for b in frame)
        self.log(f"TX ({len(frame)} bytes): {hex_bytes}")

    def _connect(self) -> socket.socket | None:
        while not self._stop_event.is_set():
            try:
                sock = socket.create_connection((self.host, self.port),
                                                timeout=5)
                self.log(f"connected to {self.host}:{self.port}")
                return sock
            except OSError as e:
                self.connect_failures += 1
                self.log(f"connect failed ({e}), retry in 2s")
                self._stop_event.wait(2)
        return None

    def run(self):
        sock = self._connect()
        if sock is None:
            return

        try:
            while not self._stop_event.is_set():
                ts = int(time.time())

                # Decide whether to inject an anomaly.
                now = time.monotonic()
                if (self.anomaly and self.anomaly_interval
                        and now - self._last_anomaly_time >= self.anomaly_interval):
                    builder = ANOMALY_BUILDERS.get(self.anomaly)
                    if builder:
                        args = random_sensor_data(self.device_id, ts)
                        frame = builder(*args)
                        try:
                            self.send_frame(sock, frame)
                            self.anomaly_count += 1
                        except OSError as e:
                            self.log(f"send error ({e}), reconnecting")
                            sock.close()
                            sock = self._connect()
                            if sock is None:
                                return
                        self._last_anomaly_time = now
                        self._stop_event.wait(self.interval)
                        continue

                # Normal frame.
                args = random_sensor_data(self.device_id, ts)
                frame = build_normal_frame(*args)
                try:
                    self.send_frame(sock, frame)
                    self.sent_count += 1
                except OSError as e:
                    self.log(f"send error ({e}), reconnecting")
                    sock.close()
                    sock = self._connect()
                    if sock is None:
                        return

                self._stop_event.wait(self.interval)

        finally:
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description="TCP device simulator for iot-gateway testing")

    parser.add_argument('--host', default='127.0.0.1',
                        help='Gateway host (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=8080,
                        help='Gateway TCP port (default: 8080)')

    rate = parser.add_mutually_exclusive_group()
    rate.add_argument('--freq', type=float, default=1,
                      help='Frames per second per device (default: 1)')
    rate.add_argument('--interval', type=float,
                      help='Seconds between frames (overrides --freq)')

    parser.add_argument('--devices', type=int, default=1,
                        help='Number of simulated devices (default: 1)')
    parser.add_argument('--device-prefix', default='dev',
                        help='Device ID prefix (default: dev)')

    parser.add_argument('--anomaly', choices=list(ANOMALY_BUILDERS),
                        help='Type of anomaly frame to inject periodically')
    parser.add_argument('--anomaly-interval', type=float, default=10,
                        help='Seconds between anomaly injections (default: 10)')

    parser.add_argument('--duration', type=float,
                        help='Test duration in seconds (default: run forever)')
    parser.add_argument('--stats', action='store_true',
                        help='Print per-device statistics on exit')

    return parser.parse_args()


def main():
    args = parse_args()
    interval = args.interval if args.interval is not None else (1.0 / args.freq)
    print_lock = threading.Lock()

    devices = []
    for i in range(1, args.devices + 1):
        dev_id = f"{args.device_prefix}_{i:03d}"
        ani = args.anomaly_interval if args.anomaly else None
        dev = SensorDevice(
            device_id=dev_id,
            host=args.host,
            port=args.port,
            interval=interval,
            anomaly=args.anomaly,
            anomaly_interval=ani,
            print_lock=print_lock,
        )
        devices.append(dev)

    print(f"=== Device Simulator ===")
    print(f"  Target:     {args.host}:{args.port}")
    print(f"  Devices:    {args.devices}")
    print(f"  Interval:   {interval:.3f}s ({1/interval:.1f} Hz)")
    if args.anomaly:
        print(f"  Anomaly:    {args.anomaly} every {args.anomaly_interval}s")
    if args.duration:
        print(f"  Duration:   {args.duration}s")
    print()

    for dev in devices:
        dev.start()

    try:
        if args.duration:
            time.sleep(args.duration)
        else:
            # Run until Ctrl-C.
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        print("\n--- Shutting down ---")
    finally:
        for dev in devices:
            dev.stop()
        for dev in devices:
            dev.join(timeout=3)

        total_sent = sum(d.sent_count for d in devices)
        total_anomaly = sum(d.anomaly_count for d in devices)
        total_fail = sum(d.connect_failures for d in devices)

        print("\n=== Summary ===")
        print(f"  Normal frames sent:  {total_sent}")
        print(f"  Anomaly frames sent: {total_anomaly}")
        print(f"  Connect failures:    {total_fail}")

        if args.stats:
            print()
            for dev in devices:
                print(f"  [{dev.device_id}] "
                      f"normal={dev.sent_count}  "
                      f"anomaly={dev.anomaly_count}  "
                      f"connect_fail={dev.connect_failures}")


if __name__ == '__main__':
    main()
