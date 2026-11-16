# Regenerates res/md.ico: "md_" in white pixel letters on black, drawn
# per size so every pixel lands on the grid. Not part of the build; run it
# after changing the design and commit the .ico:
#   $ python res/icongen.py res/md.ico
import struct, sys, zlib

# 8x16 VGA-style lowercase, cropped to the inked rows.
M = """
###.##.
#######
##.#.##
##.#.##
##.#.##
##.#.##
##...##
"""
D = """
...###.
....##.
....##.
..####.
.##.##.
##..##.
##..##.
##..##.
##..##.
.###.##
"""
# Simpler letters for the sizes too small for the ones above.
SMALL_M = """
##.#.
#.#.#
#.#.#
#.#.#
#.#.#
"""
SMALL_D = """
...#
...#
.###
#..#
#..#
#..#
.###
"""
# Pixel scale of the big letters per size: whole numbers keep every pixel
# crisp, and each is picked to leave a margin. 16 and 20 use the small ones.
SCALE = {24: 1, 32: 1, 48: 2, 64: 2, 256: 10}
SIZES = (16, 20) + tuple(SCALE)
FG, BG = (255, 255, 255, 255), (0, 0, 0, 255)

def glyph(s):
    return [[c == "#" for c in row] for row in s.strip("\n").split("\n")]

def draw(n):
    px = [[BG] * n for _ in range(n)]
    def rect(x, y, w, h):
        for j in range(y, y + h):
            for i in range(x, x + w):
                px[j][i] = FG
    def blit(g, x, y, s):
        for j, row in enumerate(g):
            for i, on in enumerate(row):
                if on:
                    rect(x + i*s, y + j*s, s, s)
    if n < 24:
        # m (5 wide), gap, d (4), gap, cursor (3), on a shared baseline.
        x   = (n - 14) // 2
        top = (n - 7) // 2
        blit(glyph(SMALL_M), x, top + 2, 1)
        blit(glyph(SMALL_D), x + 6, top, 1)
        rect(x + 11, top + 6, 3, 1)
    else:
        # m (7), gap, d (7), gap, cursor (6): 22 units, scaled to fit.
        s   = SCALE[n]
        x   = (n - 22*s) // 2
        top = (n - 10*s) // 2
        blit(glyph(M), x, top + 3*s, s)
        blit(glyph(D), x + 8*s, top, s)
        rect(x + 16*s, top + 8*s, 6*s, 2*s)
    return px

def png(px):
    n = len(px)
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)
    def chunk(t, d):
        crc = zlib.crc32(t + d) & 0xffffffff
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", crc)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))

def ico(images):
    # ICONDIR, then one ICONDIRENTRY per image, then the PNG data.
    out    = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16*len(images)
    for n, data in images:
        dim = 0 if n >= 256 else n
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b"".join(data for _, data in images)

if __name__ == "__main__":
    images = [(n, png(draw(n))) for n in SIZES]
    open(sys.argv[1], "wb").write(ico(images))
