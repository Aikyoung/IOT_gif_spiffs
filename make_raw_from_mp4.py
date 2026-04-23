#!/usr/bin/env python3
"""
make_raw_from_mp4.py — Convert MP4 to .raw for JC2432W328

Usage:
    python3 make_raw_from_mp4.py input.mp4 output.raw --width 240 --height 320
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    import cv2
except ImportError:
    sys.exit("ERROR: OpenCV required. Install with: pip install opencv-python")

    
def rgb888_to_rgb565be(r: int, g: int, b: int) -> bytes:
    """Convert 8-bit RGB to big-endian RGB565 (2 bytes)."""
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack(">H", v)   # big-endian 16-bit

def convert(src: Path, dst: Path, out_w: int, out_h: int):
    cap = cv2.VideoCapture(str(src))
    if not cap.isOpened():
        sys.exit(f"ERROR: Cannot open {src}")

    fps = cap.get(cv2.CAP_PROP_FPS)
    if fps <= 0:
        fps = 30  # fallback

    delay_ms = int(1000 / fps)
    if delay_ms < 16:
        delay_ms = 16

    frames = []
    frame_count = 0

    print(f"Reading video: {src}")

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        # Resize to LCD resolution
        frame = cv2.resize(frame, (out_w, out_h), interpolation=cv2.INTER_AREA)

        frames.append(frame)
        frame_count += 1

        if frame_count % 10 == 0:
            print(f"  frame {frame_count}", end="\r")

    cap.release()

    if frame_count == 0:
        sys.exit("ERROR: No frames decoded")

    print(f"\n{out_w}x{out_h}, {frame_count} frames, {delay_ms} ms/frame")

    with open(dst, "wb") as f:
        # Header
        f.write(struct.pack("<HHHH", out_w, out_h, delay_ms, frame_count))

        # Frames
        for i, frame in enumerate(frames):
            row_data = bytearray()

        for y in range(out_h):
            for x in range(out_w):
                b, g, r = frame[y, x]  # OpenCV = BGR
                row_data += rgb888_to_rgb565be(b, g, r)

            f.write(row_data)

            if (i + 1) % 5 == 0 or (i + 1) == frame_count:
                print(f"  writing frame {i+1}/{frame_count}", end="\r")

    print(f"\nWrote {dst} ({dst.stat().st_size // 1024} KB)")
    

def main():
    ap = argparse.ArgumentParser(description="Convert MP4 to RAW RGB565")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--width", type=int, default=240)
    ap.add_argument("--height", type=int, default=320)
    args = ap.parse_args()

    if not args.input.exists():
        sys.exit("Input file not found")

    convert(args.input, args.output, args.width, args.height)


if __name__ == "__main__":
    main()