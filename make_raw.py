#!/usr/bin/env python3
"""
make_raw.py — Convert a GIF to a .raw file for the JC2432W328 video player.

Usage:
    python3 make_raw.py <input.gif> <output.raw> [--width W] [--height H]

The output file format (8-byte header + raw frames):
    uint16_t width          — frame width  in pixels (little-endian)
    uint16_t height         — frame height in pixels (little-endian)
    uint16_t frame_delay_ms — milliseconds between frames (little-endian)
                              Uses the first frame's delay for the whole file.
    uint16_t frame_count    — total number of frames (little-endian)
    Then: frame_count * (width * height * 2) bytes of big-endian RGB565 pixels,
          one frame after another, top-to-bottom, left-to-right.

Requirements:
    pip install Pillow

Example:
    python3 make_raw.py gif_a.gif gif_a.raw
    python3 make_raw.py gif_b.gif gif_b.raw --width 240 --height 320
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageSequence
except ImportError:
    sys.exit("ERROR: Pillow is required.  Run: pip install Pillow")


def rgb888_to_rgb565be(r: int, g: int, b: int) -> bytes:
    """Convert 8-bit RGB to big-endian RGB565 (2 bytes)."""
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack(">H", v)   # big-endian 16-bit


def convert(src: Path, dst: Path, out_w: int, out_h: int) -> None:
    img = Image.open(src)

    frames = []
    delays = []

    for frame in ImageSequence.Iterator(img):
        # Convert every frame to RGBA so we can composite transparency
        rgba = frame.convert("RGBA")
        # Resize to target dimensions using high-quality Lanczos
        if rgba.size != (out_w, out_h):
            rgba = rgba.resize((out_w, out_h), Image.LANCZOS)
        # Flatten alpha onto black background
        bg = Image.new("RGBA", (out_w, out_h), (0, 0, 0, 255))
        bg.paste(rgba, mask=rgba.split()[3])
        rgb = bg.convert("RGB")
        frames.append(rgb)

        # GIF delay is stored in centiseconds
        d = frame.info.get("duration", 100)   # default 100 ms
        if d < 16:
            d = 16
        delays.append(d)

    if not frames:
        sys.exit(f"ERROR: no frames found in {src}")

    # Use the first frame's delay for the whole file (simplest; both
    # reference GIFs have constant 100 ms per frame).
    avg_delay = 33 # delays[0]

    frame_count = len(frames)
    print(f"{src}: {out_w}x{out_h}  {frame_count} frames  "
          f"{avg_delay} ms/frame  "
          f"→ {out_w * out_h * 2 * frame_count // 1024} KB raw data")

    with open(dst, "wb") as f:
        # 8-byte header
        f.write(struct.pack("<HHHH", out_w, out_h, avg_delay, frame_count))

        # Frames
        for i, rgb in enumerate(frames):
            pixels = rgb.load()
            row_data = bytearray()
            for y in range(out_h):
                for x in range(out_w):
                    r, g, b = pixels[x, y]
                    row_data += rgb888_to_rgb565be(b, g, r)
            f.write(row_data)
            if (i + 1) % 5 == 0 or (i + 1) == frame_count:
                print(f"  frame {i+1}/{frame_count}", end="\r")

    print(f"\nWrote {dst}  ({dst.stat().st_size // 1024} KB)")


def main():
    ap = argparse.ArgumentParser(
        description="Convert GIF to .raw for JC2432W328 video player")
    ap.add_argument("input",  type=Path, help="Source GIF file")
    ap.add_argument("output", type=Path, help="Destination .raw file")
    ap.add_argument("--width",  type=int, default=240,
                    help="Output frame width  (default: 240)")
    ap.add_argument("--height", type=int, default=320,
                    help="Output frame height (default: 320)")
    args = ap.parse_args()

    if not args.input.exists():
        sys.exit(f"ERROR: {args.input} not found")

    convert(args.input, args.output, args.width, args.height)


if __name__ == "__main__":
    main()
