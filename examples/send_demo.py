#!/usr/bin/env python3
"""Minimal raw-HID-over-UDP sender using only Python's standard library."""

import argparse
import math
import socket
import struct
import time

MAGIC = 0x32424856  # VHB2
VERSION = 2
HID_DEVICE_ADD = 2
HID_DEVICE_REMOVE = 3
HID_INPUT_REPORT = 4
DEVICE_ID = 1

# 18 buttons followed by four signed 16-bit axes. The six-byte vendor output
# report is included to demonstrate the bidirectional raw-HID contract.
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


def message(message_type: int, payload: bytes, sequence: int) -> bytes:
    header = struct.pack(
        "<IBBHIIIQ",
        MAGIC,
        VERSION,
        message_type,
        0,
        len(payload),
        DEVICE_ID,
        sequence,
        time.monotonic_ns() // 1000,
    )
    return header + payload


def device_add() -> bytes:
    product = b"Raw HID Demo Controller"
    manufacturer = b"Virtual HID Bridge"
    serial = b"demo-1"
    fixed = struct.pack(
        "<HHHBBHBBBB",
        0x1209,
        0x5342,
        1,
        4,  # network transport
        1,  # explicitly allow transparent diagnostic publication
        len(REPORT_DESCRIPTOR),
        len(product),
        len(manufacturer),
        len(serial),
        0,
    )
    return fixed + REPORT_DESCRIPTOR + product + manufacturer + serial


def input_report(angle: float, pressed: bool) -> bytes:
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
    # HidReportHeader: input type, unnumbered report ID, payload length.
    return struct.pack("<BBH", 0, 0, len(report)) + report


def send(sock: socket.socket, destination, message_type, payload, sequence):
    sock.sendto(message(message_type, payload, sequence), destination)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=48660)
    parser.add_argument(
        "--duration", type=float, default=0.0,
        help="stop after N seconds; zero runs until Ctrl-C"
    )
    args = parser.parse_args()
    destination = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sequence = 0
    send(sock, destination, HID_DEVICE_ADD, device_add(), sequence)
    try:
        started = time.monotonic()
        while True:
            sequence += 1
            elapsed = time.monotonic() - started
            if args.duration and elapsed >= args.duration:
                break
            send(
                sock,
                destination,
                HID_INPUT_REPORT,
                input_report(elapsed * 2.0, int(elapsed) % 2 == 0),
                sequence,
            )
            time.sleep(1 / 120)
    except KeyboardInterrupt:
        pass
    sequence += 1
    send(sock, destination, HID_DEVICE_REMOVE, b"", sequence)


if __name__ == "__main__":
    main()
