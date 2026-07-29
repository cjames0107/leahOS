#!/usr/bin/env python3
"""Write a 32x32 PNG icon for an application bundle.

Generated rather than drawn: this system has no icon editor, and a bundle that
declares an icon it does not have is worse than one that declares none. Each is
a bevelled tile in a colour derived from the name, with the initial on it - so
they are distinguishable at a glance and stable across builds.

The output is a real PNG with stored deflate blocks, matching what
user/libc/image.c writes and what the viewer reads back.
"""
import sys, zlib, struct

FONT = {                                # 5x7, only what the initials need
    'F': ["11111","10000","11110","10000","10000","10000","10000"],
    'T': ["11111","00100","00100","00100","00100","00100","00100"],
    'E': ["11111","10000","11110","10000","10000","10000","11111"],
    'P': ["11110","10001","10001","11110","10000","10000","10000"],
    'I': ["11111","00100","00100","00100","00100","00100","11111"],
    'C': ["01110","10001","10000","10000","10000","10001","01110"],
    'S': ["01111","10000","10000","01110","00001","00001","11110"],
}

def main():
    out, name = sys.argv[1], sys.argv[2]
    W = H = 32
    # A hue from the name, so two applications are unlikely to share a colour.
    h = 0
    for ch in name:
        h = (h * 131 + ord(ch)) & 0xFFFFFF
    base = (0x40 + (h & 0x7F), 0x40 + ((h >> 8) & 0x7F), 0x40 + ((h >> 16) & 0x7F))
    light = tuple(min(255, c + 0x50) for c in base)
    dark = tuple(max(0, c - 0x30) for c in base)

    px = [[base for _ in range(W)] for _ in range(H)]
    for i in range(W):                  # the bevel
        px[0][i] = light; px[H-1][i] = dark
    for i in range(H):
        px[i][0] = light; px[i][W-1] = dark

    glyph = FONT.get(name[0].upper())
    if glyph:                           # the initial, doubled to 10x14
        for r, row in enumerate(glyph):
            for c, on in enumerate(row):
                if on == '1':
                    for dy in range(2):
                        for dx in range(2):
                            px[9 + r*2 + dy][11 + c*2 + dx] = (255, 255, 255)

    raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in px)
    # Stored deflate, the same shape the OS's own encoder produces.
    body = b'\x78\x01'
    i, a, b = 0, 1, 0
    for byte in raw:
        a = (a + byte) % 65521; b = (b + a) % 65521
    while i < len(raw):
        chunk = raw[i:i+65535]
        i += len(chunk)
        body += bytes([1 if i >= len(raw) else 0])
        body += struct.pack('<HH', len(chunk), len(chunk) ^ 0xFFFF)
        body += chunk
    body += struct.pack('>I', (b << 16) | a)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', body) + chunk(b'IEND', b''))
    with open(out, 'wb') as f:
        f.write(png)

main()
