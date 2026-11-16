#!/usr/bin/env python3
# Writes the image files of test/img. They are committed, so nothing needs
# this script to build or run the tests; it is here to say how each file was
# made and to make another. Standard library only.
#
#   python test/img/gen.py [outdir]
#
# imgtest.c checks pixel values against what the files below were designed
# to show, not against another decoder, so keep the two in step.

import base64
import math
import os
import random
import struct
import sys
import zlib

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def save(name, data):
    with open(os.path.join(out, name), "wb") as f:
        f.write(data)


# PNG

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def png_chunk(kind, data):
    c = kind + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))


def png(w, h, ctype, depth, rows, plte=None, trns=None):
    """rows: a list of bytes, one per row, already packed."""
    raw = b"".join(b"\0" + r for r in rows)
    d = PNG_SIG
    d += png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 0))
    if plte:
        d += png_chunk(b"PLTE", plte)
    if trns:
        d += png_chunk(b"tRNS", trns)
    d += png_chunk(b"IDAT", zlib.compress(raw))
    return d + png_chunk(b"IEND", b"")


# 4x2 RGBA. Straight alpha: opaque red, half-transparent green, fully
# transparent (with colour that must vanish), opaque white; half blue,
# quarter red, opaque black, opaque (10, 20, 30).
alpha_rows = [
    bytes([255, 0, 0, 255,  0, 255, 0, 128,  200, 100, 50, 0,  255, 255, 255, 255]),
    bytes([0, 0, 255, 128,  255, 0, 0, 64,  0, 0, 0, 255,  10, 20, 30, 255]),
]
alpha_png = png(4, 2, 6, 8, alpha_rows)
save("alpha.png", alpha_png)

# 3x3 RGB
save("rgb.png", png(3, 3, 2, 8, [
    bytes([255, 0, 0,  0, 255, 0,  0, 0, 255]),
    bytes([255, 255, 0,  0, 255, 255,  255, 0, 255]),
    bytes([0, 0, 0,  128, 128, 128,  255, 255, 255]),
]))

# 4x1 palette image, one entry transparent through tRNS
save("palette.png", png(4, 1, 3, 8, [bytes([0, 1, 2, 3])],
    plte=bytes([255, 0, 0,  0, 255, 0,  0, 0, 255,  255, 255, 0]),
    trns=bytes([255, 255, 0, 255])))

# 2x2 gray
save("gray.png", png(2, 2, 0, 8, [bytes([0, 255]), bytes([128, 64])]))

# 16-bit RGB, 1x1
save("rgb16.png", png(1, 1, 2, 16, [bytes([0xff, 0xff, 0x80, 0x00, 0x00, 0x00])]))


# BMP

def bmp(w, h, bpp, pixels, palette=None):
    """pixels: rows from the top, each a bytes object of w pixels (indices
    for 8 bpp, BGR triples for 24)."""
    stride = (w*bpp + 31)//32*4
    body = b""
    for r in reversed(pixels):
        body += r + b"\0"*(stride - len(r))
    pal = b""
    if palette:
        pal = b"".join(bytes([b, g, r, 0]) for r, g, b in palette)
    off = 14 + 40 + len(pal)
    d = b"BM" + struct.pack("<IHHI", off + len(body), 0, 0, off)
    d += struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, 0, len(body), 2835, 2835,
                     len(palette) if palette else 0, 0)
    return d + pal + body


# 3x2 24-bit (rows of 9 bytes padded to 12): red green blue / white black yellow
save("bmp24.bmp", bmp(3, 2, 24, [
    bytes([0, 0, 255,  0, 255, 0,  255, 0, 0]),
    bytes([255, 255, 255,  0, 0, 0,  0, 255, 255]),
]))
# 4x2 8-bit
save("bmp8.bmp", bmp(4, 2, 8, [bytes([0, 1, 2, 3]), bytes([3, 2, 1, 0])],
    palette=[(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]))


# ICO with two PNG images: 16x16 red and 32x32 blue

def solid_png(w, h, rgba):
    return png(w, h, 6, 8, [bytes(rgba)*w]*h)

ico_imgs = [(16, solid_png(16, 16, (255, 0, 0, 255))), (32, solid_png(32, 32, (0, 0, 255, 255)))]
ico = struct.pack("<HHH", 0, 1, len(ico_imgs))
off = 6 + 16*len(ico_imgs)
for size, data in ico_imgs:
    ico += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(data), off)
    off += len(data)
save("icon.ico", ico + b"".join(d for _, d in ico_imgs))


# TIFF: uncompressed RGB 3x2, little endian

def tiff():
    w, h = 3, 2
    px = bytes([255, 0, 0,  0, 255, 0,  0, 0, 255,  255, 255, 255,  0, 0, 0,  255, 255, 0])
    entries = [
        (256, 3, 1, w), (257, 3, 1, h), (258, 3, 3, None), (259, 3, 1, 1),
        (262, 3, 1, 2), (273, 4, 1, None), (277, 3, 1, 3), (278, 3, 1, h),
        (279, 4, 1, len(px)),
    ]
    ifd_off = 8
    ifd_len = 2 + 12*len(entries) + 4
    bps_off = ifd_off + ifd_len
    data_off = bps_off + 6
    d = b"II*\0" + struct.pack("<I", ifd_off) + struct.pack("<H", len(entries))
    for tag, typ, cnt, val in entries:
        if tag == 258:
            d += struct.pack("<HHII", tag, typ, cnt, bps_off)
        elif tag == 273:
            d += struct.pack("<HHII", tag, typ, cnt, data_off)
        else:
            d += struct.pack("<HHII", tag, typ, cnt, val)
    d += struct.pack("<I", 0)
    d += struct.pack("<HHH", 8, 8, 8) + px
    return d

save("rgb.tif", tiff())


# WebP: the smallest lossless and lossy files in circulation, 1x1. They
# decode when the system has a WebP codec.
save("lossless.webp", base64.b64decode("UklGRhoAAABXRUJQVlA4TA0AAAAvAAAAEAcQERGIiP4HAA=="))
save("lossy.webp", base64.b64decode("UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEADsD+JaQAA3AAAAAA"))


# JPEG, written by hand: baseline, three components without subsampling,
# every 8x8 block one flat colour. The quantisation table is all ones and a
# flat block is only a DC coefficient, so every decoder reproduces the colour
# to within a rounding step or two. The Huffman tables are made up: twelve
# 4-bit codes for the DC categories, one 1-bit code for end of block.

def jpeg(w, h, color, orientation=None):
    def seg(marker, data):
        return b"\xff" + bytes([marker]) + struct.pack(">H", len(data) + 2) + data

    d = b"\xff\xd8"
    d += seg(0xe0, b"JFIF\0\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    if orientation:
        tiff_hdr = (b"II*\0" + struct.pack("<I", 8) + struct.pack("<H", 1) +
                    struct.pack("<HHIHH", 0x112, 3, 1, orientation, 0) + struct.pack("<I", 0))
        d += seg(0xe1, b"Exif\0\0" + tiff_hdr)
    d += seg(0xdb, b"\x00" + bytes([1])*64)
    d += seg(0xc0, struct.pack(">BHHB", 8, h, w, 3) + b"\x01\x11\x00\x02\x11\x00\x03\x11\x00")
    d += seg(0xc4, b"\x00" + bytes([0, 0, 0, 12] + [0]*12) + bytes(range(12)))
    d += seg(0xc4, b"\x10" + bytes([1] + [0]*15) + b"\x00")
    d += seg(0xda, b"\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00")

    bits = []

    def put(value, n):
        for i in range(n - 1, -1, -1):
            bits.append((value >> i) & 1)

    prev = [0, 0, 0]
    for by in range((h + 7)//8):
        for bx in range((w + 7)//8):
            r, g, b = color(bx, by)
            y  = 0.299*r + 0.587*g + 0.114*b
            cb = -0.168736*r - 0.331264*g + 0.5*b + 128
            cr = 0.5*r - 0.418688*g - 0.081312*b + 128
            for c, v in enumerate((y, cb, cr)):
                dc = int(round((v - 128)*8))
                diff = dc - prev[c]
                prev[c] = dc
                cat = abs(diff).bit_length()
                put(cat, 4)
                if cat:
                    put(diff if diff >= 0 else diff + (1 << cat) - 1, cat)
                put(0, 1)       # end of block
    while len(bits) % 8:
        bits.append(1)
    scan = bytearray()
    for i in range(0, len(bits), 8):
        v = int("".join(map(str, bits[i:i + 8])), 2)
        scan.append(v)
        if v == 0xff:
            scan.append(0)
    return d + bytes(scan) + b"\xff\xd9"


# 24x16, 3x2 blocks. The corners are red (top left), green (top right), blue
# (bottom left) and yellow (bottom right); the blocks between are gray.
corner = {(0, 0): (255, 0, 0), (2, 0): (0, 255, 0), (0, 1): (0, 0, 255), (2, 1): (255, 255, 0)}

def corners(bx, by):
    return corner.get((bx, by), (128, 128, 128))

for o in range(1, 9):
    save("orient%d.jpg" % o, jpeg(24, 16, corners, o))
save("color.jpg", jpeg(24, 16, corners))
save("orient9.jpg", jpeg(24, 16, corners, 9))     # not an orientation
# 10x6: not a multiple of the block size
save("odd.jpg", jpeg(10, 6, lambda bx, by: (255, 0, 0) if bx == 0 else (0, 0, 255)))


# GIF

PALETTE = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255),
           (255, 255, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]


def lzw(data, minsize):
    clear = 1 << minsize
    eoi = clear + 1
    out = bytearray()
    acc = 0
    nacc = 0

    def emit(code, size):
        nonlocal acc, nacc
        acc |= code << nacc
        nacc += size
        while nacc >= 8:
            out.append(acc & 255)
            acc >>= 8
            nacc -= 8

    table = {}
    size = minsize + 1
    nxt = eoi + 1
    emit(clear, size)
    cur = None
    for b in data:
        if cur is None:
            cur = b
            continue
        key = (cur, b)
        if key in table:
            cur = table[key]
            continue
        emit(cur, size)
        if nxt < 4096:
            table[key] = nxt
            nxt += 1
            if nxt > (1 << size) and size < 12:
                size += 1
        else:
            emit(clear, size)
            table = {}
            size = minsize + 1
            nxt = eoi + 1
        cur = b
    if cur is not None:
        emit(cur, size)
    emit(eoi, size)
    if nacc:
        out.append(acc & 255)
    return bytes(out)


def blocks(data):
    r = b""
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        r += bytes([len(chunk)]) + chunk
    return r + b"\0"


class F:
    """One GIF frame. fill is a palette index for the whole rectangle, or
    pixels a list of rows (each a list of indices), in file order."""
    def __init__(self, left, top, w, h, fill=1, pixels=None, disposal=None, delay=None,
                 transp=None, interlace=False):
        self.left, self.top, self.w, self.h = left, top, w, h
        self.pixels = pixels or [[fill]*w for _ in range(h)]
        self.disposal, self.delay, self.transp = disposal, delay, transp
        self.interlace = interlace


def gif(w, h, frames, loop=True, palette=PALETTE):
    bits = (len(palette) - 1).bit_length()          # 2**bits colours
    minsize = max(bits, 2)
    d = b"GIF89a" + struct.pack("<HHBBB", w, h, 0xf0 | (bits - 1), 0, 0)
    d += b"".join(bytes(c) for c in palette)
    if loop and len(frames) > 1:
        d += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
    for f in frames:
        if f.disposal is not None or f.delay is not None or f.transp is not None:
            packed = ((f.disposal or 0) << 2) | (1 if f.transp is not None else 0)
            d += b"\x21\xf9\x04" + struct.pack("<BHBB", packed, f.delay or 0, f.transp or 0, 0)
        d += b"\x2c" + struct.pack("<HHHHB", f.left, f.top, f.w, f.h, 0x40 if f.interlace else 0)
        flat = bytes(v for row in f.pixels for v in row)
        d += bytes([minsize]) + blocks(lzw(flat, minsize))
    return d + b"\x3b"


# 4x3, one frame with a delay, which a still image does not report
save("still.gif", gif(4, 3, [F(0, 0, 4, 3, delay=50, pixels=[
    [1, 2, 3, 4], [5, 6, 7, 0], [1, 1, 2, 2]])]))

# Frames that keep what the earlier ones drew
save("keep.gif", gif(8, 8, [
    F(0, 0, 8, 8, 1, delay=5, disposal=1),
    F(2, 2, 4, 4, 2, delay=10, disposal=1),
    F(4, 4, 4, 4, 3, delay=20, disposal=0),
]))

# Frames that clear their own rectangle afterwards
save("background.gif", gif(8, 8, [
    F(0, 0, 8, 8, 1, delay=10, disposal=2),
    F(2, 2, 4, 4, 2, delay=10, disposal=2),
    F(0, 0, 2, 2, 3, delay=10, disposal=0),
]))

# Frames that put back what was there before them
save("previous.gif", gif(8, 8, [
    F(0, 0, 8, 8, 1, delay=10, disposal=1),
    F(2, 2, 4, 4, 2, delay=10, disposal=3),
    F(4, 0, 4, 4, 3, delay=10, disposal=1),
]))
save("previous-first.gif", gif(8, 8, [
    F(0, 0, 4, 4, 1, delay=10, disposal=3),
    F(4, 4, 4, 4, 2, delay=10, disposal=1),
]))

# A transparent index: the frame below shows through
save("transparent.gif", gif(8, 8, [
    F(0, 0, 8, 8, 1, delay=10, disposal=1),
    F(0, 0, 4, 4, pixels=[[0 if (x + y) % 2 == 0 else 2 for x in range(4)] for y in range(4)],
      delay=10, disposal=1, transp=0),
]))

# A first frame smaller than the screen, and a transparent pixel over nothing
save("offsets.gif", gif(8, 8, [
    F(1, 1, 2, 2, 1, delay=10, disposal=1),
    F(5, 5, 2, 2, pixels=[[0, 3], [3, 3]], delay=10, disposal=1, transp=0),
]))

# Frames partly or wholly outside the screen
save("outside.gif", gif(4, 4, [
    F(2, 2, 4, 4, 1, delay=10, disposal=1),
    F(10, 10, 2, 2, 2, delay=10, disposal=2),
    F(0, 0, 2, 2, 3, delay=10, disposal=1),
]))

# Delays of 0, 1, 2, 5 and 100 hundredths, and none at all
save("delays.gif", gif(2, 2, [
    F(0, 0, 2, 2, 1, delay=0, disposal=1),
    F(0, 0, 2, 2, 2, delay=1, disposal=1),
    F(0, 0, 2, 2, 3, delay=2, disposal=1),
    F(0, 0, 2, 2, 4, delay=5, disposal=1),
    F(0, 0, 2, 2, 5, delay=100, disposal=1),
    F(0, 0, 2, 2, 6),
]))

# Interlaced: the rows are stored in the order 0, 8, 4, 2, 6, 1, 3, 5, 7 of
# an 8-row image where displayed row r has the colour index r % 7 + 1
def inter_rows(h):
    order = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
    return [[r % 7 + 1]*2 for r in order]

save("interlaced.gif", gif(2, 8, [F(0, 0, 2, 8, pixels=inter_rows(8), interlace=True)]))

# A logical screen of 0x0 (as some encoders write): the first frame sizes it
save("lsd0.gif", gif(0, 0, [F(1, 1, 2, 2, 1, delay=10)]))

# Dimensions beyond what we decode
save("huge.gif", gif(20000, 20000, [F(0, 0, 1, 1, 1, delay=10)]))

# Everything at once, for comparing in-order playback with random access
rng = random.Random(1)
frames = []
for i in range(10):
    w, h = rng.randint(2, 12), rng.randint(2, 12)
    frames.append(F(rng.randint(0, 12), rng.randint(0, 12), w, h,
        pixels=[[rng.randint(0, 3) for _ in range(w)] for _ in range(h)],
        delay=rng.choice([0, 1, 3, 7]), disposal=rng.choice([0, 1, 2, 3]),
        transp=rng.choice([None, None, 0])))
save("mixed.gif", gif(16, 16, frames))


# Damaged files

save("empty.bin", b"")
rng = random.Random(2)
save("garbage.bin", bytes(rng.randint(0, 255) for _ in range(200)))
save("trunc.png", alpha_png[:40])
save("header.png", alpha_png[:33])
# A broken compressed stream, which WIC shows as a blank picture
save("corrupt.png", PNG_SIG + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
     + png_chunk(b"IDAT", bytes(rng.randint(0, 255) for _ in range(40))) + png_chunk(b"IEND", b""))
# A bit depth that does not exist
save("badhdr.png", PNG_SIG + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 3, 6, 0, 0, 0))
     + png_chunk(b"IDAT", zlib.compress(b"\0"*68)) + png_chunk(b"IEND", b""))
jpg = jpeg(24, 16, corners, 1)
save("trunc.jpg", jpg[:len(jpg)*6//10])
with open(os.path.join(out, "keep.gif"), "rb") as f:
    save("trunc.gif", f.read()[:80])
# 20000x1 and 16384x16384: refused by size, before any pixel is read
save("wide.png", png(20000, 1, 6, 8, [bytes(4)*20000]))
save("big.png", PNG_SIG + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 16384, 16384, 8, 6, 0, 0, 0))
     + png_chunk(b"IDAT", zlib.compress(b"\0"*100)) + png_chunk(b"IEND", b""))


# The pictures of test/sample.md, in test/images (photo.jpg comes from
# test/images/photo.ps1, since a smooth JPEG needs a real encoder)

images = os.path.join(out, "..", "images")
os.makedirs(images, exist_ok=True)


def save_image(name, data):
    with open(os.path.join(images, name), "wb") as f:
        f.write(data)


def inside(poly, x, y):
    n = len(poly)
    c = False
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1)*(y - y1)/(y2 - y1) + x1:
            c = not c
    return c


def logo(size=240):
    """A rounded square with an M and a down arrow, antialiased by 4x4
    supersampling, with transparent corners."""
    m = [(40, 170), (40, 70), (70, 70), (100, 115), (130, 70), (160, 70), (160, 170),
         (135, 170), (135, 110), (105, 155), (95, 155), (65, 110), (65, 170)]
    arrow = [(183, 70), (203, 70), (203, 115), (218, 115), (193, 162), (168, 115), (183, 115)]
    margin, radius = 8, 48

    def box(x, y):
        # distance outside a rounded rectangle, negative inside
        cx = min(max(x, margin + radius), size - margin - radius)
        cy = min(max(y, margin + radius), size - margin - radius)
        d = ((x - cx)**2 + (y - cy)**2)**0.5 - radius
        return d if margin <= x <= size - margin and margin <= y <= size - margin else max(d, 1)

    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            cover = white = 0
            for sy in range(4):
                for sx in range(4):
                    x, y = px + (sx + .5)/4, py + (sy + .5)/4
                    if box(x, y) <= 0:
                        cover += 1
                        if inside(m, x, y) or inside(arrow, x, y):
                            white += 1
            t = (px + py)/(2*size)
            base = [79 + (20 - 79)*t, 70 + (184 - 70)*t, 229 + (166 - 229)*t]
            if cover:
                k = white/cover
                rgb = [int(round(b*(1 - k) + 255*k)) for b in base]
                row += bytes(rgb) + bytes([int(round(255*cover/16))])
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return png(size, size, 6, 8, rows)


save_image("logo.png", logo())

# A ball bouncing over a landscape: the first frame is the whole picture and
# stays, every other is a small sprite that is taken away again afterwards
# (disposal 3), so playing it needs all of GIF's rules.
BALL = [(0, 0, 0), (120, 180, 235), (140, 195, 240), (160, 208, 245), (180, 220, 248),
        (200, 232, 250), (96, 170, 80), (64, 130, 60), (220, 50, 40), (255, 150, 130),
        (150, 25, 20), (255, 255, 255), (255, 214, 64), (0, 0, 0), (0, 0, 0), (0, 0, 0)]
W, H = 160, 90
scene = []
for y in range(H):
    row = []
    for x in range(W):
        if y >= 72:
            row.append(7 if (x // 8 + y // 6) % 2 else 6)
        elif (x - 132)**2 + (y - 20)**2 <= 100:
            row.append(12)
        else:
            row.append(1 + y * 5 // 72)
    scene.append(row)
SPRITE = 20
sprite = []
for y in range(SPRITE):
    row = []
    for x in range(SPRITE):
        d = ((x - 9.5)**2 + (y - 9.5)**2)**0.5
        if d > 9.5:
            row.append(0)
        elif (x - 6)**2 + (y - 6)**2 <= 6:
            row.append(9)
        elif d > 7.5:
            row.append(10)
        else:
            row.append(8)
    sprite.append(row)
ball = [F(0, 0, W, H, pixels=scene, delay=4, disposal=1)]
N = 28
for i in range(N):
    t = i / N
    x = int(8 + (W - 16 - SPRITE) * (0.5 - 0.5*math.cos(2*math.pi*t)))
    y = int(72 - SPRITE + 2 - abs(math.sin(4*math.pi*t)) * 50)
    ball.append(F(x, y, SPRITE, SPRITE, pixels=sprite, delay=4, disposal=3, transp=0))
save_image("ball.gif", gif(W, H, ball, palette=BALL))
