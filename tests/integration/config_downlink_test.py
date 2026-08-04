#!/usr/bin/env python3
"""
E2E verification for CMD_CONFIG (0x03) request/response downlink.

Prerequisite: gateway already listening on 127.0.0.1:8080.

Flow:
  1. Connect to the gateway.
  2. Send a CMD_CONFIG frame with empty payload (= "request my config").
  3. Read the response frame; assert header/CRC/cmd=0x03 and config JSON keys.

Frame layout (big-endian): AA 55 | len(2) | cmd(1) | payload | CRC16-CCITT-FALSE
"""
import json
import socket
import struct
import sys
import time


HEADER = b'\xAA\x55'
CMD_CONFIG = 0x03
DEFAULT_HOST = '127.0.0.1'
DEFAULT_PORT = 8080
EXPECTED_KEYS = ('sampling_interval', 'upload_freq', 'heartbeat_interval')


def _build_crc_table() -> list:
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        table.append(crc & 0xFFFF)
    return table


_CRC_TABLE = _build_crc_table()


def crc16(data: bytes) -> int:
    """CRC16-CCITT-FALSE, must match src/protocol.c."""
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) ^ _CRC_TABLE[((crc >> 8) ^ byte) & 0xFF]) & 0xFFFF
    return crc


def build_frame(cmd: int, payload: bytes) -> bytes:
    length_field = struct.pack('>H', len(payload))
    cmd_field = bytes([cmd])
    crc = crc16(length_field + cmd_field + payload)
    return HEADER + length_field + cmd_field + payload + struct.pack('>H', crc)


def parse_frame(data: bytes) -> tuple:
    """Validate one complete frame; return (cmd, payload)."""
    assert data[:2] == HEADER, f"bad header {data[:2].hex()}"
    (plen,) = struct.unpack('>H', data[2:4])
    total = 7 + plen
    assert len(data) >= total, f"short frame {len(data)} < {total}"
    exp = crc16(data[2:5 + plen])
    (got,) = struct.unpack('>H', data[5 + plen:7 + plen])
    assert exp == got, f"CRC mismatch exp={exp:#x} got={got:#x}"
    return data[4], data[5:5 + plen]


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    sock = socket.create_connection((host, port), timeout=5)
    try:
        sock.sendall(build_frame(CMD_CONFIG, b''))
        sock.settimeout(5)

        buf = b''
        deadline = time.time() + 5
        while time.time() < deadline:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            if len(buf) >= 7:
                (plen,) = struct.unpack('>H', buf[2:4])
                if len(buf) >= 7 + plen:
                    break

        if len(buf) < 7:
            print(f"FAIL: no config downlink response from {host}:{port}")
            return 1

        cmd, payload = parse_frame(buf)
        if cmd != CMD_CONFIG:
            print(f"FAIL: expected cmd=0x{CMD_CONFIG:02x}, got 0x{cmd:02x}")
            return 1

        cfg = json.loads(payload)
        missing = [k for k in EXPECTED_KEYS if k not in cfg]
        if missing:
            print(f"FAIL: missing config keys {missing}")
            return 1

        print(f"OK: got CMD_CONFIG downlink: {payload.decode()}")
        return 0
    finally:
        sock.close()


if __name__ == '__main__':
    sys.exit(main())
