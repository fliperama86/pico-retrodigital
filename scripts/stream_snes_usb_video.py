#!/usr/bin/env python3
"""Display a live SNES capture from the Pico USB CDC interface using SDL."""

from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass

from capture_snes_usb_frame import (
    choose_port,
    load_serial_module,
    request_frame,
    rgb565_to_rgb888,
)


@dataclass(frozen=True)
class VideoFrame:
    source_frame: int
    width: int
    height: int
    rgb888: bytes
    received_at: float


def load_pygame_module():
    try:
        import pygame
    except ImportError as exc:
        raise SystemExit(
            "pygame and pyserial are required; run with: "
            "uv run --with pygame --with pyserial "
            "scripts/stream_snes_usb_video.py"
        ) from exc
    return pygame


def replace_queued_item(destination: queue.Queue, item: object) -> None:
    try:
        destination.get_nowait()
    except queue.Empty:
        pass
    destination.put_nowait(item)


def capture_worker(
    serial,
    device: str,
    timeout: float,
    frames: queue.Queue,
    errors: queue.Queue,
    stop: threading.Event,
    port_holder: list,
) -> None:
    try:
        with serial.Serial(device, 115200, timeout=0.05, write_timeout=timeout) as port:
            port_holder.append(port)
            time.sleep(0.1)
            while not stop.is_set():
                try:
                    source_frame, width, height, payload = request_frame(port, timeout)
                    frame = VideoFrame(
                        source_frame=source_frame,
                        width=width,
                        height=height,
                        rgb888=bytes(rgb565_to_rgb888(payload)),
                        received_at=time.monotonic(),
                    )
                    replace_queued_item(frames, frame)
                except (TimeoutError, ValueError) as exc:
                    replace_queued_item(errors, str(exc))
                    stop.wait(0.1)
    except (OSError, serial.SerialException) as exc:
        if not stop.is_set():
            replace_queued_item(errors, f"USB capture stopped: {exc}")
    finally:
        port_holder.clear()


def fit_rect(pygame, container, aspect_ratio: float):
    width, height = container.get_size()
    fitted_width = width
    fitted_height = round(width / aspect_ratio)
    if fitted_height > height:
        fitted_height = height
        fitted_width = round(height * aspect_ratio)
    return pygame.Rect(
        (width - fitted_width) // 2,
        (height - fitted_height) // 2,
        fitted_width,
        fitted_height,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB CDC device, auto-detected when exactly one Pico is connected")
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds allowed for each frame read")
    parser.add_argument("--scale", type=int, default=3, help="initial integer window scale")
    parser.add_argument(
        "--square-pixels",
        action="store_true",
        help="show the raw 256:224 aspect instead of stretching to 4:3",
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.scale <= 0:
        parser.error("--scale must be greater than zero")
    return args


def main() -> int:
    args = parse_args()
    serial, list_ports = load_serial_module()
    pygame = load_pygame_module()
    device = choose_port(list_ports, args.port)

    pygame.init()
    pygame.display.set_caption("Pico RetroDigital SNES USB")
    raw_size = (256, 224)
    base_size = raw_size if args.square_pixels else (round(raw_size[1] * 4 / 3), raw_size[1])
    windowed_size = (base_size[0] * args.scale, base_size[1] * args.scale)
    screen = pygame.display.set_mode(windowed_size, pygame.RESIZABLE)
    clock = pygame.time.Clock()

    frames: queue.Queue[VideoFrame] = queue.Queue(maxsize=1)
    errors: queue.Queue[str] = queue.Queue(maxsize=1)
    stop = threading.Event()
    port_holder: list = []
    worker = threading.Thread(
        target=capture_worker,
        args=(serial, device, args.timeout, frames, errors, stop, port_holder),
        name="snes-usb-capture",
        daemon=True,
    )
    worker.start()

    current_surface = None
    current_frame = None
    frame_times: deque[float] = deque(maxlen=30)
    four_by_three = not args.square_pixels
    fullscreen = False
    status = f"Opening {device}"
    running = True

    try:
        while running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False
                elif event.type == pygame.KEYDOWN:
                    if event.key in (pygame.K_ESCAPE, pygame.K_q):
                        running = False
                    elif event.key == pygame.K_a:
                        four_by_three = not four_by_three
                    elif event.key == pygame.K_f:
                        fullscreen = not fullscreen
                        flags = pygame.FULLSCREEN if fullscreen else pygame.RESIZABLE
                        size = (0, 0) if fullscreen else windowed_size
                        screen = pygame.display.set_mode(size, flags)

            try:
                while True:
                    current_frame = frames.get_nowait()
                    current_surface = pygame.image.frombuffer(
                        current_frame.rgb888,
                        (current_frame.width, current_frame.height),
                        "RGB",
                    )
                    frame_times.append(current_frame.received_at)
                    status = ""
            except queue.Empty:
                pass

            try:
                status = errors.get_nowait()
            except queue.Empty:
                pass

            fps = 0.0
            if len(frame_times) >= 2:
                elapsed = frame_times[-1] - frame_times[0]
                if elapsed > 0:
                    fps = (len(frame_times) - 1) / elapsed

            if current_frame is not None:
                aspect_name = "4:3" if four_by_three else "square pixels"
                title = (
                    f"Pico RetroDigital SNES USB | {fps:.1f} fps | "
                    f"frame {current_frame.source_frame} | {aspect_name}"
                )
            else:
                title = f"Pico RetroDigital SNES USB | {status}"
            if status and current_frame is not None:
                title += f" | {status}"
            pygame.display.set_caption(title)

            screen.fill("black")
            if current_surface is not None and current_frame is not None:
                aspect = 4 / 3 if four_by_three else current_frame.width / current_frame.height
                destination = fit_rect(pygame, screen, aspect)
                scaled = pygame.transform.scale(current_surface, destination.size)
                screen.blit(scaled, destination)
            pygame.display.flip()
            clock.tick(60)
    finally:
        stop.set()
        if port_holder:
            try:
                port_holder[0].close()
            except (OSError, serial.SerialException):
                pass
        worker.join(timeout=0.5)
        pygame.quit()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
