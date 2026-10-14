#!/usr/bin/env python3
"""Capture ESP32-S3 A401 pressure UDP CSV packets to a local file."""

from __future__ import annotations

import argparse
import csv
import socket
import sys
from datetime import datetime
from pathlib import Path


CSV_HEADER = [
    "uptime_ms",
    "pressure_vout_raw",
    "pressure_vout_mv",
    "pressure_vref_raw",
    "pressure_vref_mv",
    "pressure_delta_mv",
]
CSV_HEADER_LINE = ",".join(CSV_HEADER)


def default_output_path() -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return Path("data") / f"pressure_udp_{stamp}.csv"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture ESP32-S3 A401 pressure UDP CSV samples to a file."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="Local address to bind.")
    parser.add_argument("--port", type=int, default=5005, help="Local UDP port.")
    parser.add_argument("--out", type=Path, default=default_output_path(), help="Output CSV path.")
    parser.add_argument("--buffer-bytes", type=int, default=2048, help="UDP receive buffer size.")
    parser.add_argument("--quiet", action="store_true", help="Do not print every sample.")
    return parser.parse_args()


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


def capture(bind: str, port: int, output_path: Path, buffer_bytes: int, quiet: bool) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Listening on {bind}:{port} UDP.")
    print(f"Writing samples to {output_path.resolve()}")
    print("Press Ctrl+C to stop.")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, port))

    with output_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(CSV_HEADER)
        csv_file.flush()

        while True:
            packet, address = sock.recvfrom(buffer_bytes)
            text = packet.decode("utf-8", errors="replace").strip()
            if not text:
                continue

            for line in text.splitlines():
                line = line.strip()
                if not line or line.startswith("#") or line == CSV_HEADER_LINE:
                    if not quiet:
                        print(line)
                    continue

                row = next(csv.reader([line]))
                if not is_sample_row(row):
                    print(f"Skipping non-sample UDP payload from {address}: {line}", file=sys.stderr)
                    continue

                writer.writerow(row)
                csv_file.flush()
                if not quiet:
                    print(f"{address[0]}:{address[1]} {line}")


def main() -> int:
    args = parse_args()
    try:
        capture(args.bind, args.port, args.out, args.buffer_bytes, args.quiet)
    except KeyboardInterrupt:
        print("\nCapture stopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
