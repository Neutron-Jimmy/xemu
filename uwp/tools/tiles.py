"""tiles.py DIR -- the package's tile and splash PNGs: a plain green square on black."""
import os
import struct
import sys
import zlib


def png(path, w, h, fg=(16, 124, 16), bg=(0, 0, 0)):
    rows = []
    for y in range(h):
        row = bytearray(b"\0")
        for x in range(w):
            inside = w // 5 <= x < w - w // 5 and h // 5 <= y < h - h // 5
            row += bytes(fg if inside else bg)
        rows.append(bytes(row))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
    open(path, "wb").write(data)


for name, w, h in [("Square150x150Logo", 150, 150), ("Square44x44Logo", 44, 44), ("Wide310x150Logo", 310, 150),
                   ("StoreLogo", 50, 50), ("SplashScreen", 620, 300)]:
    png(os.path.join(sys.argv[1], name + ".png"), w, h)
