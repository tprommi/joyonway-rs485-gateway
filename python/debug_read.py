#!/usr/bin/env python3
"""Capture, replay, and analyse timestamped Joyonway RS485 diagnostics."""

from __future__ import annotations

import argparse
import csv
import socket
import struct
import sys
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

MAGIC = b"JWDB"
VERSION = 1
HEADER = struct.Struct("!4sBBIQH")
SYNC_FRAME = bytes.fromhex("1a0120083caa1000006b73e4b91d")
EVENT_NAMES = {
    1: "RX_READ",
    2: "TX_START",
    3: "TX_QUEUED",
    4: "TX_COMPLETE",
    5: "COMMAND_ACCEPTED",
    6: "SYNC_ARMED",
    7: "COMMAND_DROPPED",
    8: "POLL_21_ARMED",
}


@dataclass(frozen=True)
class DebugRecord:
    """One record received from the gateway debug stream."""

    event: int
    sequence: int
    timestamp_us: int
    payload: bytes

    def encode(self) -> bytes:
        """Return the gateway wire representation."""
        return (
            HEADER.pack(
                MAGIC,
                VERSION,
                self.event,
                self.sequence,
                self.timestamp_us,
                len(self.payload),
            )
            + self.payload
        )


@dataclass(frozen=True)
class Frame:
    """One reconstructed Joyonway frame."""

    sequence: int
    timestamp_us: int
    direction: str
    data: bytes


class FrameAssembler:
    """Reassemble delimiter-framed Joyonway packets from UART chunks."""

    def __init__(self, direction: str) -> None:
        self.direction = direction
        self.buffer = bytearray()
        self.discarded_bytes = 0

    def feed(self, record: DebugRecord) -> list[Frame]:
        """Consume one payload and return all completed frames."""
        self.buffer.extend(record.payload)
        frames: list[Frame] = []
        while self.buffer:
            try:
                start = self.buffer.index(0x1A)
            except ValueError:
                self.discarded_bytes += len(self.buffer)
                self.buffer.clear()
                break
            if start:
                self.discarded_bytes += start
                del self.buffer[:start]
            try:
                end = self.buffer.index(0x1D, 1)
            except ValueError:
                break
            data = bytes(self.buffer[: end + 1])
            del self.buffer[: end + 1]
            frames.append(
                Frame(record.sequence, record.timestamp_us, self.direction, data)
            )
        return frames


def receive_exact(stream: BinaryIO | socket.socket, size: int) -> bytes:
    """Receive exactly size bytes or raise EOFError."""
    data = bytearray()
    while len(data) < size:
        if isinstance(stream, socket.socket):
            chunk = stream.recv(size - len(data))
        else:
            chunk = stream.read(size - len(data))
        if not chunk:
            raise EOFError("debug stream closed")
        data.extend(chunk)
    return bytes(data)


def read_records(
    stream: BinaryIO | socket.socket,
) -> Iterator[tuple[DebugRecord, bytes]]:
    """Decode records while retaining their exact wire representation."""
    while True:
        try:
            header = receive_exact(stream, HEADER.size)
        except EOFError:
            return
        magic, version, event, sequence, timestamp_us, payload_length = HEADER.unpack(
            header
        )
        if magic != MAGIC:
            raise ValueError(f"invalid magic at record {sequence}: {magic!r}")
        if version != VERSION:
            raise ValueError(f"unsupported version at record {sequence}: {version}")
        payload = receive_exact(stream, payload_length)
        record = DebugRecord(event, sequence, timestamp_us, payload)
        yield record, header + payload


def classify_frame(frame: Frame) -> str:
    """Return a concise packet classification."""
    if frame.data == SYNC_FRAME:
        return "SYNC"
    if frame.direction == "TX":
        return "COMMAND"
    if frame.data.startswith(b"\x1a\xff\x01\x3c\xd2\xb4\xff"):
        return "BROADCAST"
    if len(frame.data) > 1:
        return f"UNICAST_0x{frame.data[1]:02X}"
    return "UNKNOWN"


class CaptureWriter:
    """Write lossless records, reconstructed frames, and timing analysis."""

    CSV_FIELDS = (
        "sequence",
        "timestamp_us",
        "elapsed_ms",
        "direction",
        "frame_type",
        "length",
        "delta_previous_frame_ms",
        "delta_previous_direction_ms",
        "delta_last_sync_ms",
        "raw_hex",
    )
    EVENT_FIELDS = (
        "sequence",
        "timestamp_us",
        "elapsed_ms",
        "event",
        "length",
        "delta_previous_event_ms",
        "payload_hex",
    )

    def __init__(self, output: Path | None, save_raw: bool) -> None:
        self.output = output
        self.raw_file: BinaryIO | None = None
        self.csv_file = None
        self.csv_writer: csv.DictWriter | None = None
        self.event_file = None
        self.event_writer: csv.DictWriter | None = None
        if output is not None:
            output.parent.mkdir(parents=True, exist_ok=True)
            if save_raw:
                self.raw_file = output.with_suffix(".jwdb").open("wb")
            self.csv_file = output.with_suffix(".csv").open(
                "w", encoding="utf-8", newline=""
            )
            self.csv_writer = csv.DictWriter(self.csv_file, fieldnames=self.CSV_FIELDS)
            self.csv_writer.writeheader()
            self.event_file = output.with_name(f"{output.name}_events.csv").open(
                "w", encoding="utf-8", newline=""
            )
            self.event_writer = csv.DictWriter(
                self.event_file, fieldnames=self.EVENT_FIELDS
            )
            self.event_writer.writeheader()

        self.first_timestamp_us: int | None = None
        self.previous_frame_us: int | None = None
        self.previous_direction_us: dict[str, int] = {}
        self.last_sync_us: int | None = None
        self.frames = 0
        self.rx_frames = 0
        self.tx_frames = 0
        self.sequence_gaps = 0
        self.previous_sequence: int | None = None
        self.first_record_us: int | None = None
        self.previous_record_us: int | None = None
        self.tx_rows: list[dict[str, str | int | float]] = []

    def write_record(self, record: DebugRecord, encoded: bytes) -> None:
        """Persist one original record and check sequence continuity."""
        if self.raw_file is not None:
            self.raw_file.write(encoded)
            self.raw_file.flush()
        if self.first_record_us is None:
            self.first_record_us = record.timestamp_us
        if self.event_writer is not None:
            self.event_writer.writerow(
                {
                    "sequence": record.sequence,
                    "timestamp_us": record.timestamp_us,
                    "elapsed_ms": (
                        f"{(record.timestamp_us - self.first_record_us) / 1000:.3f}"
                    ),
                    "event": EVENT_NAMES.get(record.event, f"UNKNOWN_{record.event}"),
                    "length": len(record.payload),
                    "delta_previous_event_ms": self._format_delta(
                        self._delta(record.timestamp_us, self.previous_record_us)
                    ),
                    "payload_hex": record.payload.hex(),
                }
            )
            self.event_file.flush()
        self.previous_record_us = record.timestamp_us
        if self.previous_sequence is not None:
            expected = (self.previous_sequence + 1) & 0xFFFFFFFF
            if record.sequence != expected:
                self.sequence_gaps += 1
                print(
                    f"sequence gap: expected={expected} received={record.sequence}",
                    file=sys.stderr,
                )
        self.previous_sequence = record.sequence

    def write_frame(self, frame: Frame) -> None:
        """Print and persist one reconstructed frame."""
        if self.first_timestamp_us is None:
            self.first_timestamp_us = frame.timestamp_us
        elapsed_ms = (frame.timestamp_us - self.first_timestamp_us) / 1000
        delta_previous = self._delta(frame.timestamp_us, self.previous_frame_us)
        delta_direction = self._delta(
            frame.timestamp_us, self.previous_direction_us.get(frame.direction)
        )
        frame_type = classify_frame(frame)
        delta_sync = self._delta(frame.timestamp_us, self.last_sync_us)
        if frame_type == "SYNC" and frame.direction == "RX":
            self.last_sync_us = frame.timestamp_us
            delta_sync = 0.0

        row: dict[str, str | int | float] = {
            "sequence": frame.sequence,
            "timestamp_us": frame.timestamp_us,
            "elapsed_ms": f"{elapsed_ms:.3f}",
            "direction": frame.direction,
            "frame_type": frame_type,
            "length": len(frame.data),
            "delta_previous_frame_ms": self._format_delta(delta_previous),
            "delta_previous_direction_ms": self._format_delta(delta_direction),
            "delta_last_sync_ms": self._format_delta(delta_sync),
            "raw_hex": frame.data.hex(),
        }
        if self.csv_writer is not None:
            self.csv_writer.writerow(row)
            self.csv_file.flush()
        if frame.direction == "TX":
            self.tx_rows.append(row)
            self.tx_frames += 1
        else:
            self.rx_frames += 1
        self.frames += 1
        self.previous_frame_us = frame.timestamp_us
        self.previous_direction_us[frame.direction] = frame.timestamp_us

        sync_text = self._format_delta(delta_sync) or "n/a"
        print(
            f"{frame.sequence:10d} {elapsed_ms:12.3f} ms {frame.direction:2s} "
            f"{frame_type:14s} sync={sync_text:>9s} ms {frame.data.hex()}"
        )

    def close(self, rx_assembler: FrameAssembler, tx_assembler: FrameAssembler) -> None:
        """Close outputs and write the human-readable summary."""
        if self.raw_file is not None:
            self.raw_file.close()
        if self.csv_file is not None:
            self.csv_file.close()
        if self.event_file is not None:
            self.event_file.close()
        if self.output is None:
            return

        summary = self.output.with_name(f"{self.output.name}_summary.md")
        lines = [
            "# Joyonway debug capture summary",
            "",
            f"- Reconstructed frames: {self.frames}",
            f"- RX frames: {self.rx_frames}",
            f"- TX command frames: {self.tx_frames}",
            f"- Debug sequence gaps: {self.sequence_gaps}",
            f"- Discarded RX bytes outside frames: {rx_assembler.discarded_bytes}",
            f"- Discarded TX bytes outside frames: {tx_assembler.discarded_bytes}",
            f"- Incomplete RX bytes at end: {len(rx_assembler.buffer)}",
            f"- Incomplete TX bytes at end: {len(tx_assembler.buffer)}",
            "",
            (
                "RX timestamps mark the UART read that completed a frame. A frame split "
                "across reads therefore uses the final chunk timestamp."
            ),
            "",
            "## Transmitted commands",
            "",
        ]
        if self.tx_rows:
            lines.extend(
                [
                    (
                        "| Sequence | ESP time (us) | Since previous frame (ms) | "
                        "Since last RX sync (ms) | Bytes |"
                    ),
                    "| ---: | ---: | ---: | ---: | :--- |",
                ]
            )
            for row in self.tx_rows:
                lines.append(
                    f"| {row['sequence']} | {row['timestamp_us']} | "
                    f"{row['delta_previous_frame_ms'] or 'n/a'} | "
                    f"{row['delta_last_sync_ms'] or 'n/a'} | `{row['raw_hex']}` |"
                )
        else:
            lines.append("No TX command frames were captured.")
        summary.write_text("\n".join(lines) + "\n", encoding="utf-8")

    @staticmethod
    def _delta(current: int, previous: int | None) -> float | None:
        return None if previous is None else (current - previous) / 1000

    @staticmethod
    def _format_delta(value: float | None) -> str:
        return "" if value is None else f"{value:.3f}"


def capture_stream(
    stream: BinaryIO | socket.socket, output: Path | None, save_raw: bool
) -> None:
    """Process a live or recorded debug stream."""
    writer = CaptureWriter(output, save_raw)
    rx_assembler = FrameAssembler("RX")
    tx_assembler = FrameAssembler("TX")
    try:
        for record, encoded in read_records(stream):
            writer.write_record(record, encoded)
            assembler = (
                rx_assembler
                if record.event == 1
                else tx_assembler
                if record.event == 2
                else None
            )
            if assembler is not None:
                for frame in assembler.feed(record):
                    writer.write_frame(frame)
    finally:
        writer.close(rx_assembler, tx_assembler)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture or replay the ESP32 Joyonway debug stream."
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--host", help="gateway address for a live capture")
    source.add_argument("--input", type=Path, help="existing .jwdb capture to replay")
    parser.add_argument("--port", type=int, default=8082)
    parser.add_argument(
        "--output",
        type=Path,
        help="output basename; writes .jwdb, .csv, and _summary.md",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.host:
        output = args.output or Path("captures/joyonway_debug")
        with socket.create_connection((args.host, args.port)) as connection:
            capture_stream(connection, output, save_raw=True)
    else:
        with args.input.open("rb") as stream:
            capture_stream(stream, args.output, save_raw=False)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (EOFError, OSError, ValueError) as err:
        print(f"error: {err}", file=sys.stderr)
        raise SystemExit(1) from err
