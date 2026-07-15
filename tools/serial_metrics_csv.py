#!/usr/bin/env python3
"""Capture PQC_CSV serial lines from the root and write them to CSV."""

import argparse
import csv
import sys
from pathlib import Path

try:
    import serial
except ImportError:  # pragma: no cover - user environment dependency
    serial = None


DEFAULT_HEADER = [
    "record",
    "root_seconds",
    "node_id",
    "event",
    "value0",
    "value1",
    "value2",
    "value3",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Read PQC_CSV lines from a serial port and append them to a CSV file."
    )
    parser.add_argument("--port", required=True, help="Serial port, for example COM5 or /dev/ttyUSB0.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate.")
    parser.add_argument("--out", required=True, help="Output CSV path.")
    parser.add_argument(
        "--echo",
        action="store_true",
        help="Also print captured CSV rows to stdout.",
    )
    return parser.parse_args()


def open_serial(port, baud):
    if serial is None:
        raise SystemExit("pyserial is required: pip install pyserial")

    return serial.Serial(port=port, baudrate=baud, timeout=1)


def parse_pqc_line(raw_line):
    line = raw_line.strip()
    if not line:
        return None, None

    if line.startswith("PQC_CSV_HEADER,"):
        return "header", line.split(",")[1:]

    if line.startswith("PQC_CSV,"):
        return "row", line.split(",")[1:]

    return None, None


def main():
    args = parse_args()
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    header = DEFAULT_HEADER
    write_header = not out_path.exists() or out_path.stat().st_size == 0

    with open_serial(args.port, args.baud) as ser, out_path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)

        if write_header:
            writer.writerow(header)
            handle.flush()

        while True:
            raw = ser.readline()
            if not raw:
                continue

            text = raw.decode("utf-8", errors="replace")
            kind, values = parse_pqc_line(text)

            if kind == "header":
                header = values
                continue

            if kind != "row":
                continue

            writer.writerow(values)
            handle.flush()

            if args.echo:
                print(",".join(values))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
