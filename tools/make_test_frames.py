
#!/usr/bin/env python3
"""Generate a synthetic Tesserae-format test frame for the Inky Frame self-test.

test.bin  WxH, 4 bpp palette indices (pico_bin layout, high nibble = left pixel):
          eight equal bars of palette index 0..7 across the top quarter, a black
          border, and a solid black triangle whose apex points at the LEFT edge
          (x = 0) of the native landscape framebuffer.

Usage: make_test_frames.py [out_dir] [--size 800x480|640x400|600x448]
The default size is the 7.3" (800x480); the 4.0" is 640x400, the 5.7" 600x448.

On a landscape mount the bars should be along the top and the triangle should
point LEFT. Serve with `python3 -m http.server 8000` from this folder.
"""
import argparse, pathlib

ap = argparse.ArgumentParser()
ap.add_argument("out", nargs="?", default=".")
ap.add_argument("--size", default="800x480", help="WxH: 800x480 (7.3\"), 640x400 (4.0\") or 600x448 (5.7\")")
args = ap.parse_args()
W, H = (int(v) for v in args.size.lower().split("x"))
BLACK, WHITE = 0, 1
out = pathlib.Path(args.out)

# The 7.3" geometry, scaled to the panel.
sx, sy = W / 800, H / 480

def px(x, y):
    if x < 8 or x >= W - 8 or y < 8 or y >= H - 8:
        return BLACK                                   # border
    if y < 120 * sy:
        return min(x * 8 // W, 7)                      # palette bars 0..7
    # Triangle: apex at (40, 300), base at x = 420 from y = 170..430, on the 7.3".
    ax, bx, cy, half_h = 40 * sx, 420 * sx, 300 * sy, 130 * sy
    if ax <= x <= bx:
        half = (x - ax) * half_h / (bx - ax)
        if abs(y - cy) <= half:
            return BLACK
    return WHITE

buf = bytearray(W * H // 2)
i = 0
for y in range(H):
    for x in range(0, W, 2):
        buf[i] = (px(x, y) << 4) | px(x + 1, y)
        i += 1
(out / "test.bin").write_bytes(buf)
print("wrote", out / "test.bin", f"{W}x{H},", len(buf), "bytes")
