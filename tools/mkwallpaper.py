#!/usr/bin/env python3
"""Turn a photograph into something leahOS can actually read.

Two things stand in the way of using assets/wallpaper.jpg directly.

It is a progressive JPEG, and leahOS has no JPEG decoder - writing one, and a
progressive one at that, would be a large amount of work to display a picture.
The image is a build asset like the icons, so it gets converted here instead,
once, by the machine doing the building.

And the OS reads PNGs with stored deflate blocks only: img_read_png walks the
blocks straight into the pixel array and gives up on anything compressed. Its
input buffer is 2 MB, which a 1024x768 image would exceed on its own, so this
downscales. wserver samples the wallpaper rather than requiring it at screen
size - a nearest-neighbour stretch, honest about being one - and for a
soft-focus photograph the difference does not show.

    mkwallpaper.py <in.jpg> <out.png> [width] [height]

sips does the decode because it is on every Mac and already used elsewhere in
this build; the rest is here because pure Python can unfilter a PNG in about
thirty lines and cannot decode a progressive JPEG in any number of them.
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib


def decode_png(path):
    """Read a real (compressed) PNG into (width, height, RGBA bytes)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('%s: not a PNG' % path)

    at = 8
    width = height = depth = colour = None
    idat = b''
    while at + 8 <= len(data):
        length, tag = struct.unpack_from('>I4s', data, at)
        body = data[at + 8:at + 8 + length]
        at += 12 + length                      # length, tag, body, crc
        if tag == b'IHDR':
            width, height, depth, colour = struct.unpack_from('>IIBB', body, 0)
        elif tag == b'IDAT':
            idat += body
        elif tag == b'IEND':
            break

    if depth != 8 or colour not in (2, 6):
        raise SystemExit('%s: expected 8-bit RGB or RGBA, got depth %s type %s'
                         % (path, depth, colour))

    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels

    # Undo the per-scanline filters. Each row is preceded by its filter type,
    # and every filter refers to the pixel to the left and the row above.
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            x = line[i]
            if ftype == 1:   x += a
            elif ftype == 2: x += b
            elif ftype == 3: x += (a + b) >> 1
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                x += a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = x & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line

    # Normalise to RGBA, which is the shape img_read_png hands back.
    if channels == 4:
        return width, height, bytes(out)
    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        rgba[i * 4 + 0] = out[i * 3 + 0]
        rgba[i * 4 + 1] = out[i * 3 + 1]
        rgba[i * 4 + 2] = out[i * 3 + 2]
        rgba[i * 4 + 3] = 255
    return width, height, bytes(rgba)


def write_stored_png(path, width, height, rgba):
    """A real PNG whose deflate stream is stored blocks - what the OS reads.

    Three channels, not four: img_read_png accepts colour type 2 and nothing
    else, and refuses the file outright otherwise. It also costs a quarter of
    the size, which matters against a 2 MB input buffer."""
    body = bytearray()
    for y in range(height):
        body.append(0)                          # filter: none, the only one read
        row = rgba[y * width * 4:(y + 1) * width * 4]
        for x in range(width):
            body += row[x * 4:x * 4 + 3]

    stream = bytearray(b'\x78\x01')             # zlib header, no compression
    at = 0
    while at < len(body):
        chunk = body[at:at + 65535]
        at += len(chunk)
        stream.append(1 if at >= len(body) else 0)
        stream += struct.pack('<HH', len(chunk), len(chunk) ^ 0xFFFF)
        stream += chunk
    stream += struct.pack('>I', zlib.adler32(bytes(body)) & 0xFFFFFFFF)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR',
                      struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', bytes(stream)))
        f.write(chunk(b'IEND', b''))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit('usage: mkwallpaper.py <in.jpg> <out.png> [w] [h]')
    source, target = sys.argv[1], sys.argv[2]
    want_w = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    want_h = int(sys.argv[4]) if len(sys.argv) > 4 else 480

    tmp = tempfile.mkdtemp()
    staged = os.path.join(tmp, 'staged.png')
    subprocess.run(['sips', '-s', 'format', 'png',
                    '-z', str(want_h), str(want_w), source, '--out', staged],
                   check=True, capture_output=True)
    w, h, rgba = decode_png(staged)
    write_stored_png(target, w, h, rgba)
    os.remove(staged); os.rmdir(tmp)
    print('wallpaper: %s, %dx%d, %d bytes' % (target, w, h,
                                              os.path.getsize(target)))
