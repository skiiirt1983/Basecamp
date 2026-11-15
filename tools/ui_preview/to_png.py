import struct, sys, zlib, glob, os
W = 466
def png(path, rgb):
    raw = b"".join(b"\x00" + rgb[y * W * 3:(y + 1) * W * 3] for y in range(W))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, W, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
for f in sorted(glob.glob(os.path.join(sys.argv[1], "*.rgb565"))):
    d = open(f, "rb").read(); out = bytearray()
    for y in range(W):
        for x in range(W):
            v = struct.unpack_from("<H", d, (y * W + x) * 2)[0]
            r, g, b = ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
            if (x - 232.5) ** 2 + (y - 232.5) ** 2 > 233 ** 2: r, g, b = 40, 40, 48     # outside the round panel
            out += bytes((r, g, b))
    png(f[:-7] + ".png", bytes(out)); os.remove(f)
print("PNGs in", sys.argv[1])
