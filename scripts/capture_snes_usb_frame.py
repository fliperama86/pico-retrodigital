#!/usr/bin/env python3
"""Request one or more SNES RGB565 frames over the firmware USB CDC port."""

from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"PRDF"
ERROR_MAGIC = b"PRDE"
# Version 1 is the original 24-byte header (no brightness telemetry).
# Version 2 adds a 28-byte header (version 1's fields plus
# brightness/flags/reserved) with no trailer. Version 3 keeps the same
# 28-byte header and appends a fixed trailer after the RGB payload: 224
# bytes of per-line brightness, 7168 bytes of mask plane, then a u32 CRC-32
# over those 7392 bytes. The neopico_hd in-firmware USB capture path still
# sends version 1 unchanged, so all three must be accepted.
VERSION_FIELD = struct.Struct("<H")
HEADER_V1 = struct.Struct("<4sHHHHIII")
HEADER_V2 = struct.Struct("<4sHHHHIIIBBH")
ERROR_HEADER = struct.Struct("<4sHHIIIIII")
TRAILER_CRC_FIELD = struct.Struct("<I")
SUPPORTED_PROTOCOL_VERSIONS = (1, 2, 3)
PIXEL_FORMAT_RGB565_LE = 1
RASPBERRY_PI_USB_VID = 0x2E8A


@dataclass(frozen=True)
class FrameResult:
    source_frame: int
    width: int
    height: int
    payload: bytes
    brightness: int | None
    pixel_valid: bool | None
    gated: bool | None
    line_brightness: list[int] | None
    mask: bytes | None


def load_serial_module():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise SystemExit(
            "pyserial is required; run with: "
            "uv run --with pyserial scripts/capture_snes_usb_frame.py"
        ) from exc
    return serial, list_ports


def choose_port(list_ports, requested: str | None) -> str:
    if requested:
        return requested

    ports = list(list_ports.comports())
    candidates = [
        port
        for port in ports
        if port.vid == RASPBERRY_PI_USB_VID
        or "snes frame capture" in (port.description or "").lower()
    ]
    if not candidates:
        candidates = [port for port in ports if "usbmodem" in port.device.lower()]
    if len(candidates) == 1:
        return candidates[0].device

    available = "\n".join(
        f"  {port.device}: {port.description}"
        for port in ports
    ) or "  none"
    if not candidates:
        raise SystemExit(f"No Pico USB CDC port found. Available ports:\n{available}")
    raise SystemExit(f"Multiple Pico USB CDC ports found; pass --port. Available ports:\n{available}")


def read_exact(port, length: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    result = bytearray()
    while len(result) < length:
        chunk = port.read(length - len(result))
        if chunk:
            result.extend(chunk)
            continue
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Timed out after receiving {len(result)} of {length} bytes")
    return bytes(result)


def find_magic(port, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    window = bytearray()
    while time.monotonic() < deadline:
        byte = port.read(1)
        if not byte:
            continue
        window.extend(byte)
        if len(window) > len(MAGIC):
            del window[0]
        if window == MAGIC or window == ERROR_MAGIC:
            return bytes(window)
    raise TimeoutError("Timed out waiting for a PRDF frame or PRDE error header")


def describe_levels(levels: int) -> str:
    return f"HBLANK={(levels >> 0) & 1}, VBLANK={(levels >> 1) & 1}, PCLK={(levels >> 2) & 1}"


def read_capture_error(port, timeout: float) -> None:
    header = ERROR_MAGIC + read_exact(port, ERROR_HEADER.size - len(ERROR_MAGIC), timeout)
    _, version, error, levels, hblank_edges, vblank_edges, pclk_edges, line, _ = ERROR_HEADER.unpack(header)
    if version not in SUPPORTED_PROTOCOL_VERSIONS:
        raise ValueError(f"Unsupported error protocol version {version}")
    stages = {
        5: f"waiting for frame completion ({line} line(s) captured before timeout)",
    }
    stage = stages.get(error, f"at unknown stage {error}")
    raise ValueError(
        f"device timed out {stage}; {describe_levels(levels)}; "
        f"transitions/50ms: HBLANK={hblank_edges}, VBLANK={vblank_edges}, PCLK={pclk_edges}"
    )


def unpack_mask(mask_bytes: bytes, width: int, height: int) -> bytes:
    """Unpack the wire mask plane (u32 words, LSB-first pixels within each
    word) into one byte per pixel (0 or 1), row-major."""
    words_per_line = width // 32
    result = bytearray(width * height)
    offset = 0
    for line in range(height):
        base = line * width
        for word_index in range(words_per_line):
            word = int.from_bytes(mask_bytes[offset:offset + 4], "little")
            offset += 4
            pixel_base = base + word_index * 32
            for bit in range(32):
                result[pixel_base + bit] = (word >> bit) & 1
    return bytes(result)


def request_frame(port, timeout: float) -> FrameResult:
    """Request one frame. brightness/pixel_valid/gated are None for a
    version 1 header (no telemetry fields); brightness is the CMOD nibble
    (0-15), pixel_valid is the PIXEL_VALID level, and gated is whether
    PIXEL_VALID gating was applied to this frame's RGB payload (flags bit1),
    for version 2 and 3. line_brightness (per-line CMOD nibbles) and mask
    (one byte per pixel, 0 or 1, row-major) are populated only for a version
    3 reply; both are None otherwise."""
    port.reset_input_buffer()
    port.write(b"C")
    port.flush()

    magic = find_magic(port, timeout)
    if magic == ERROR_MAGIC:
        read_capture_error(port, timeout)

    version_bytes = read_exact(port, VERSION_FIELD.size, timeout)
    (version,) = VERSION_FIELD.unpack(version_bytes)
    if version not in SUPPORTED_PROTOCOL_VERSIONS:
        raise ValueError(f"Unsupported frame protocol version {version}")

    if version == 1:
        rest = read_exact(port, HEADER_V1.size - len(MAGIC) - VERSION_FIELD.size, timeout)
        (_, _, pixel_format, width, height, payload_bytes, source_frame, expected_crc) = HEADER_V1.unpack(
            MAGIC + version_bytes + rest
        )
        brightness = None
        pixel_valid = None
        gated = None
    else:
        rest = read_exact(port, HEADER_V2.size - len(MAGIC) - VERSION_FIELD.size, timeout)
        (_, _, pixel_format, width, height, payload_bytes, source_frame, expected_crc, brightness, flags,
         _reserved) = HEADER_V2.unpack(MAGIC + version_bytes + rest)
        pixel_valid = bool(flags & 0x01)
        gated = bool(flags & 0x02)

    if pixel_format != PIXEL_FORMAT_RGB565_LE:
        raise ValueError(f"Unsupported pixel format {pixel_format}")
    expected_bytes = width * height * 2
    if payload_bytes != expected_bytes:
        raise ValueError(f"Invalid payload size {payload_bytes}; expected {expected_bytes}")

    payload = read_exact(port, payload_bytes, timeout)
    actual_crc = zlib.crc32(payload)
    if actual_crc != expected_crc:
        raise ValueError(f"Frame CRC mismatch: received {actual_crc:08x}, expected {expected_crc:08x}")

    line_brightness = None
    mask = None
    if version == 3:
        line_brightness_bytes = read_exact(port, height, timeout)
        mask_bytes_len = (width // 32) * height * 4
        mask_raw = read_exact(port, mask_bytes_len, timeout)
        trailer_crc_bytes = read_exact(port, TRAILER_CRC_FIELD.size, timeout)
        (expected_trailer_crc,) = TRAILER_CRC_FIELD.unpack(trailer_crc_bytes)
        actual_trailer_crc = zlib.crc32(line_brightness_bytes + mask_raw)
        if actual_trailer_crc != expected_trailer_crc:
            raise ValueError(
                f"Trailer CRC mismatch: received {actual_trailer_crc:08x}, "
                f"expected {expected_trailer_crc:08x}"
            )
        line_brightness = list(line_brightness_bytes)
        mask = unpack_mask(mask_raw, width, height)

    return FrameResult(
        source_frame, width, height, payload, brightness, pixel_valid, gated, line_brightness, mask
    )


def rgb565_to_rgb888(payload: bytes) -> bytearray:
    rgb888 = bytearray(len(payload) // 2 * 3)
    output = 0
    for offset in range(0, len(payload), 2):
        pixel = payload[offset] | (payload[offset + 1] << 8)
        red = (pixel >> 11) & 0x1F
        green = (pixel >> 5) & 0x3F
        blue = pixel & 0x1F
        rgb888[output] = (red << 3) | (red >> 2)
        rgb888[output + 1] = (green << 2) | (green >> 4)
        rgb888[output + 2] = (blue << 3) | (blue >> 2)
        output += 3
    return rgb888


def write_ppm(path: Path, width: int, height: int, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        output.write(rgb565_to_rgb888(payload))


def write_pgm_mask(path: Path, width: int, height: int, mask: bytes) -> None:
    """Writes the unpacked mask (one byte per pixel, 0 or 1) as a binary PGM
    (P5), 255 for a valid pixel and 0 for invalid."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(f"P5\n{width} {height}\n255\n".encode("ascii"))
        output.write(bytes(255 if bit else 0 for bit in mask))


def mask_path_for(ppm_path: Path) -> Path:
    return ppm_path.with_name(f"{ppm_path.stem}-mask.pgm")


def output_path(base: Path, index: int, count: int) -> Path:
    if count == 1:
        return base
    suffix = base.suffix or ".ppm"
    return base.with_name(f"{base.stem}-{index:04d}{suffix}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB CDC device, auto-detected when exactly one Pico is connected")
    parser.add_argument("--output", type=Path, default=Path("snes-frame.ppm"), help="output PPM path")
    parser.add_argument("--count", type=int, default=1, help="number of frames; 0 captures until interrupted")
    parser.add_argument("--interval", type=float, default=0.0, help="minimum seconds between frame requests")
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds allowed for each protocol read")
    parser.add_argument(
        "--save-mask",
        action="store_true",
        help="also write a 256x224 PGM PIXEL_VALID mask next to each PPM (version 3 replies only)",
    )
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be zero or greater")
    if args.interval < 0:
        parser.error("--interval must be zero or greater")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return args


def main() -> int:
    args = parse_args()
    serial, list_ports = load_serial_module()
    device = choose_port(list_ports, args.port)
    print(f"Opening {device}", file=sys.stderr)

    captured = 0
    last_started = 0.0
    try:
        with serial.Serial(device, 115200, timeout=0.05, write_timeout=args.timeout) as port:
            time.sleep(0.1)
            while args.count == 0 or captured < args.count:
                delay = args.interval - (time.monotonic() - last_started)
                if delay > 0:
                    time.sleep(delay)
                last_started = time.monotonic()

                result = request_frame(port, args.timeout)
                path = output_path(args.output, captured, args.count)
                write_ppm(path, result.width, result.height, result.payload)
                if args.save_mask and result.mask is not None:
                    write_pgm_mask(mask_path_for(path), result.width, result.height, result.mask)
                captured += 1
                elapsed = time.monotonic() - last_started

                telemetry = ""
                if result.brightness is not None:
                    telemetry = f" bright={result.brightness} valid={1 if result.pixel_valid else 0}"
                if result.gated:
                    telemetry += " gated"
                if result.line_brightness is not None:
                    telemetry += f" lines {min(result.line_brightness)}-{max(result.line_brightness)}"
                if result.mask is not None:
                    valid_percent = sum(result.mask) / len(result.mask) * 100.0
                    telemetry += f" valid {valid_percent:.1f}%"

                print(
                    f"frame {result.source_frame}: {result.width}x{result.height}, "
                    f"{len(result.payload)} bytes, {elapsed:.3f}s -> {path}{telemetry}"
                )
    except KeyboardInterrupt:
        print(f"\nCaptured {captured} frame(s)", file=sys.stderr)
        return 130
    except (OSError, TimeoutError, ValueError, serial.SerialException) as exc:
        print(f"capture failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
