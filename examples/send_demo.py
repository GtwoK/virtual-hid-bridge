#!/usr/bin/env python3
"""Minimal HID-over-UDP source using only Python's standard library."""

import argparse
import math
import random
import socket
import struct
import time

MAGIC = 0x32424856  # Versioned HID-over-UDP envelope.
VERSION = 2
SESSION_OPEN = 1
SESSION_ACCEPT = 2
SESSION_CLOSE = 3
SESSION_PING = 4
SESSION_PONG = 5
HID_DEVICE_ADD = 16
HID_DEVICE_REMOVE = 17
HID_INPUT_REPORT = 32
HID_REPORT_INPUT = 0
TRANSPORT_NETWORK = 4
ALLOW_TRANSPARENT_OUTPUT = 1
DEVICE_ID = 1
KEEPALIVE_US = 5_000_000
TIMEOUT_US = 15_000_000

# 18 buttons followed by four signed 16-bit axes. The six-byte vendor output
# report demonstrates the host output-report return path.
REPORT_DESCRIPTOR = bytes(
    [
        0x05, 0x01,  # Generic Desktop
        0x09, 0x05,  # Game Pad
        0xA1, 0x01,  # Application
        0x05, 0x09,  # Button page
        0x19, 0x01,
        0x29, 0x12,  # Buttons 1..18
        0x15, 0x00,
        0x25, 0x01,
        0x75, 0x01,
        0x95, 0x12,
        0x81, 0x02,
        0x75, 0x01,
        0x95, 0x06,  # six padding bits
        0x81, 0x03,
        0x05, 0x01,
        0x09, 0x30,  # X
        0x09, 0x31,  # Y
        0x09, 0x32,  # Z
        0x09, 0x35,  # Rz
        0x16, 0x00, 0x80,
        0x26, 0xFF, 0x7F,
        0x75, 0x10,
        0x95, 0x04,
        0x81, 0x02,
        0x06, 0x00, 0xFF,
        0x09, 0x10,
        0x15, 0x00,
        0x26, 0xFF, 0x00,
        0x75, 0x08,
        0x95, 0x06,
        0x91, 0x02,
        0xC0,
    ]
)


class HidUdpSender:
    def __init__(self, host: str, port: int, device_id: int):
        self.destination = (host, port)
        self.device_id = device_id
        self.session_id = random.getrandbits(32) or 1
        self.sequence = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def open_session(self) -> None:
        payload = struct.pack(
            "<IIIII32s",
            self.session_id,
            2,
            0,
            KEEPALIVE_US,
            TIMEOUT_US,
            b"vhid-demo",
        )
        self.sock.settimeout(0.25)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            self._send(SESSION_OPEN, payload, device_id=0)
            try:
                packet, _ = self.sock.recvfrom(1400)
            except socket.timeout:
                continue
            if self._accepted_session(packet):
                self.sock.settimeout(None)
                return
        raise TimeoutError("bridge did not accept the UDP session")

    def add_controller(
        self,
        *,
        descriptor: bytes,
        vendor_id: int,
        product_id: int,
        version: int,
        product: str,
        manufacturer: str,
        serial: str,
        allow_transparent_output: bool = False,
    ) -> None:
        product_bytes = product.encode("utf-8")
        manufacturer_bytes = manufacturer.encode("utf-8")
        serial_bytes = serial.encode("utf-8")
        payload = struct.pack(
            "<HHHBBHBBBB",
            vendor_id,
            product_id,
            version,
            TRANSPORT_NETWORK,
            ALLOW_TRANSPARENT_OUTPUT if allow_transparent_output else 0,
            len(descriptor),
            len(product_bytes),
            len(manufacturer_bytes),
            len(serial_bytes),
            0,
        )
        self._send(
            HID_DEVICE_ADD,
            payload + descriptor + product_bytes + manufacturer_bytes +
            serial_bytes,
        )

    def send_input_report(self, report: bytes, report_id: int = 0) -> None:
        payload = struct.pack(
            "<BBH", HID_REPORT_INPUT, report_id, len(report)
        ) + report
        self._send(HID_INPUT_REPORT, payload)

    def remove_controller(self) -> None:
        self._send(HID_DEVICE_REMOVE, b"")

    def close_session(self) -> None:
        self._send(SESSION_CLOSE, b"", device_id=0)

    def _accepted_session(self, packet: bytes) -> bool:
        if len(packet) < 28:
            return False
        magic, version, message_type, _, payload_size, device_id, _, _ = (
            struct.unpack_from("<IBBHIIIQ", packet)
        )
        if (
            magic != MAGIC or
            version != VERSION or
            message_type != SESSION_ACCEPT or
            device_id != 0 or
            payload_size != 52 or
            len(packet) < 80
        ):
            return False
        session_id, = struct.unpack_from("<I", packet, 28)
        return session_id == self.session_id

    def _send(self, message_type: int, payload: bytes,
              device_id: int | None = None) -> None:
        header = struct.pack(
            "<IBBHIIIQ",
            MAGIC,
            VERSION,
            message_type,
            0,
            len(payload),
            self.device_id if device_id is None else device_id,
            self.sequence,
            time.monotonic_ns() // 1000,
        )
        self.sequence += 1
        self.sock.sendto(header + payload, self.destination)


def make_input_report(angle: float, pressed: bool) -> bytes:
    buttons = 1 if pressed else 0
    report = struct.pack(
        "<3B4h",
        buttons & 0xFF,
        (buttons >> 8) & 0xFF,
        (buttons >> 16) & 0x03,
        round(math.cos(angle) * 32767),
        round(math.sin(angle) * 32767),
        0,
        0,
    )
    return report


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=48660)
    parser.add_argument(
        "--duration", type=float, default=0.0,
        help="stop after N seconds; zero runs until Ctrl-C"
    )
    args = parser.parse_args()
    sender = HidUdpSender(args.host, args.port, DEVICE_ID)
    sender.open_session()
    sender.add_controller(
        descriptor=REPORT_DESCRIPTOR,
        vendor_id=0x1209,
        product_id=0x5342,
        version=1,
        product="Raw HID Demo Controller",
        manufacturer="Virtual HID Bridge",
        serial="demo-1",
    )
    try:
        started = time.monotonic()
        while True:
            elapsed = time.monotonic() - started
            if args.duration and elapsed >= args.duration:
                break
            sender.send_input_report(
                make_input_report(elapsed * 2.0, int(elapsed) % 2 == 0)
            )
            time.sleep(1 / 120)
    except KeyboardInterrupt:
        pass
    sender.remove_controller()
    sender.close_session()


if __name__ == "__main__":
    main()
