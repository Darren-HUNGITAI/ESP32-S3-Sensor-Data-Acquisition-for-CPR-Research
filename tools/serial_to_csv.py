#!/usr/bin/env python3
"""Capture ESP32-S3 A401 pressure USB serial CSV output to a local file."""

from __future__ import annotations

import argparse
import csv
import sys
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


CSV_HEADER = [
    "uptime_ms",
    "pressure_vout_raw",
    "pressure_vout_mv",
    "pressure_vref_raw",
    "pressure_vref_mv",
    "pressure_delta_mv",
]


def default_output_path() -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return Path("data") / f"pressure_{stamp}.csv"


def list_serial_ports() -> None:
    ensure_pyserial()
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return

    for port in ports:
        print(f"{port.device}\t{port.description}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture ESP32-S3 A401 pressure serial CSV samples to a file."
    )
    parser.add_argument("--port", help="Serial port, for example COM6 on Windows.")
    parser.add_argument("--baud", type=int, default=921600, help="Serial baud rate.")
    parser.add_argument("--out", type=Path, default=default_output_path(), help="Output CSV path.")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit.")
    return parser.parse_args()


def ensure_pyserial() -> None:
    if serial is None or list_ports is None:
        raise RuntimeError(
            "Missing pyserial. Install it with: python -m pip install -r tools\\requirements.txt"
        )


def is_sample_row(row: list[str]) -> bool:
    if len(row) != len(CSV_HEADER):
        return False
    try:
        int(row[0])
        for value in row[1:]:
            float(value)
    except ValueError:
        return False
    return True


def capture(port: str, baud: int, output_path: Path) -> None:
    ensure_pyserial()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Opening {port} at {baud} baud.")
    print(f"Writing samples to {output_path.resolve()}")
    print("Press Ctrl+C to stop.")

    with serial.Serial(port, baudrate=baud, timeout=1) as ser, output_path.open(
        "w", newline="", encoding="utf-8"
    ) as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(CSV_HEADER)
        csv_file.flush()

        while True:
            raw_line = ser.readline()
            if not raw_line:
                continue

            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line or line.startswith("#") or line == ",".join(CSV_HEADER):
                print(line)
                continue

            row = next(csv.reader([line]))
            if not is_sample_row(row):
                print(f"Skipping non-sample line: {line}", file=sys.stderr)
                continue

            writer.writerow(row)
            csv_file.flush()
            print(line)


def main() -> int:
    args = parse_args()

    try:
        if args.list_ports:
            list_serial_ports()
            return 0

        if not args.port:
            print("Missing --port. Use --list-ports to see available ports.", file=sys.stderr)
            return 2

        capture(args.port, args.baud, args.out)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nCapture stopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
