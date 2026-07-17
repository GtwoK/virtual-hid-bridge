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
HID_OUTPUT_REPORT = 33
HID_REPORT_INPUT = 0
HID_REPORT_OUTPUT = 1
TRANSPORT_NETWORK = 4
ALLOW_TRANSPARENT_OUTPUT = 1
SOURCE_INPUT_INFER = 0
SOURCE_OUTPUT_DEFAULT = 0
SOURCE_INPUT_GENERIC_HID = 0xFF
SOURCE_OUTPUT_NONE = 0xFF
SOURCE_OUTPUT_SWITCH_PRO = 2
SOURCE_OUTPUT_SWITCH2_PRO = 3
DEVICE_ID = 1
KEEPALIVE_US = 5_000_000
TIMEOUT_US = 15_000_000

# 18 buttons, four signed 16-bit axes, six signed 16-bit HID Sensors-page
# motion fields.
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
        0x06, 0x20, 0x00,  # Sensors page
        0x0A, 0x53, 0x04,  # Acceleration X
        0x0A, 0x54, 0x04,  # Acceleration Y
        0x0A, 0x55, 0x04,  # Acceleration Z
        0x0A, 0x57, 0x04,  # Angular velocity X
        0x0A, 0x58, 0x04,  # Angular velocity Y
        0x0A, 0x59, 0x04,  # Angular velocity Z
        0x16, 0x00, 0x80,
        0x26, 0xFF, 0x7F,
        0x55, 0x0E,  # values are centi-G and centi-degrees/sec
        0x75, 0x10,
        0x95, 0x06,
        0x81, 0x02,
        0x55, 0x00,
        0xC0,
    ]
)


class HidUdpSender:
    def __init__(self, host: str, port: int, device_id: int):
        self.destination = (host, port)
        self.device_id = device_id
        self.session_id = random.getrandbits(32) or 1
        self.sequence = 0
        self.last_output_report: bytes | None = None
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
                self.sock.setblocking(False)
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
        source_input_profile: int = SOURCE_INPUT_GENERIC_HID,
        source_output_profile: int = SOURCE_OUTPUT_DEFAULT,
    ) -> None:
        product_bytes = product.encode("utf-8")
        manufacturer_bytes = manufacturer.encode("utf-8")
        serial_bytes = serial.encode("utf-8")
        payload = struct.pack(
            "<HHHBBHBBBBBB",
            vendor_id,
            product_id,
            version,
            TRANSPORT_NETWORK,
            ALLOW_TRANSPARENT_OUTPUT if allow_transparent_output else 0,
            len(descriptor),
            len(product_bytes),
            len(manufacturer_bytes),
            len(serial_bytes),
            source_input_profile,
            source_output_profile,
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

    def poll_output_reports(self) -> None:
        while True:
            try:
                packet, _ = self.sock.recvfrom(1400)
            except BlockingIOError:
                return
            parsed = self._parse_output_report(packet)
            if parsed is None:
                continue
            report_id, report = parsed
            if report == self.last_output_report:
                continue
            self.last_output_report = report
            print(
                f"output report: id=0x{report_id:02x} "
                f"bytes={report.hex(' ')}"
            )

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

    def _parse_output_report(self, packet: bytes) -> tuple[int, bytes] | None:
        if len(packet) < 32:
            return None
        magic, version, message_type, _, payload_size, device_id, _, _ = (
            struct.unpack_from("<IBBHIIIQ", packet)
        )
        if (
            magic != MAGIC or
            version != VERSION or
            message_type != HID_OUTPUT_REPORT or
            device_id != self.device_id or
            len(packet) < 28 + payload_size or
            payload_size < 4
        ):
            return None
        report_type, report_id, data_size = struct.unpack_from(
            "<BBH", packet, 28
        )
        if (
            report_type != HID_REPORT_OUTPUT or
            data_size + 4 != payload_size
        ):
            return None
        start = 32
        return report_id, packet[start:start + data_size]

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
        "<3B4h6h",
        buttons & 0xFF,
        (buttons >> 8) & 0xFF,
        (buttons >> 16) & 0x03,
        round(math.cos(angle) * 32767),
        round(math.sin(angle) * 32767),
        0,
        0,
        round(math.sin(angle * 0.5) * 25),
        round(math.cos(angle * 0.5) * 25),
        100,
        round(math.sin(angle) * 9000),
        0,
        round(math.cos(angle) * 4500),
    )
    return report


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=48660)
    parser.add_argument(
        "--source-input-profile",
        choices=("infer", "generic-hid"),
        default="generic-hid",
    )
    parser.add_argument(
        "--source-output-profile",
        choices=("default", "infer", "none", "switch-pro", "switch2-pro"),
        default="default",
    )
    parser.add_argument(
        "--duration", type=float, default=0.0,
        help="stop after N seconds; zero runs until Ctrl-C"
    )
    args = parser.parse_args()
    sender = HidUdpSender(args.host, args.port, DEVICE_ID)
    sender.open_session()
    source_input_profile = {
        "infer": SOURCE_INPUT_INFER,
        "generic-hid": SOURCE_INPUT_GENERIC_HID,
    }[args.source_input_profile]
    source_output_profile = {
        "default": SOURCE_OUTPUT_DEFAULT,
        "infer": SOURCE_OUTPUT_DEFAULT,
        "none": SOURCE_OUTPUT_NONE,
        "switch-pro": SOURCE_OUTPUT_SWITCH_PRO,
        "switch2-pro": SOURCE_OUTPUT_SWITCH2_PRO,
    }[args.source_output_profile]
    sender.add_controller(
        descriptor=REPORT_DESCRIPTOR,
        vendor_id=0x1209,
        product_id=0x5342,
        version=1,
        product="Raw HID Demo Controller",
        manufacturer="Virtual HID Bridge",
        serial="demo-1",
        source_input_profile=source_input_profile,
        source_output_profile=source_output_profile,
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
            sender.poll_output_reports()
            time.sleep(1 / 120)
    except KeyboardInterrupt:
        pass
    sender.remove_controller()
    sender.close_session()


if __name__ == "__main__":
    main()
