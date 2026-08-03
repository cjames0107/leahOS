#!/usr/bin/env python3
"""Convert the media in media/ into formats leahOS can actually read.

Three things stand between the files a person has and the files this system can
open, and all three are decoders it does not have.

JPEG is not implemented. It needs a discrete cosine transform, dequantisation
and Huffman tables, and the photographs here are *progressive* JPEGs on top of
that, which is the harder variant. MP3 is further still: subband synthesis and
its own Huffman coding. Writing either to show a picture or play a tune would
be a large piece of work aimed at the wrong target.

So the machine doing the build converts them, once, into what the system reads:
PNG for images and 16-bit PCM for sound. This is the same trade the old
mkwallpaper.py made, generalised - and one part of it is no longer needed, now
that img_read_png inflates a real deflate stream. That tool had to emit
*stored* blocks and downscale hard to keep the result inside a 2 MB buffer. A
compressed PNG at screen size is fine now, so the photographs arrive as
photographs.

    mkmedia.py <out-dir>

sips and afconvert do the decoding. Both ship with macOS and sips is already
used elsewhere in this build. Everything is cached against source mtime,
because converting eighty megabytes of audio on every make would be intolerable
and almost always pointless.
"""

import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIA = os.path.join(HERE, "media")

# The screen is 1024x768. A wallpaper larger than that is sampled down by the
# window server anyway, and sampling a 6700-pixel-wide photograph down to 1024
# throws away most of every pixel it reads.
WALLPAPER_LONG_EDGE = 1024

# Demo photographs keep more detail than a wallpaper needs, because the viewer
# pans around them - but not the 4784 pixels they arrive with, which would make
# a PNG bigger than the picture is worth.
DEMO_LONG_EDGE = 1400


def newer(src, dst):
    """True when dst is missing or older than src."""
    if not os.path.exists(dst):
        return True
    return os.path.getmtime(src) > os.path.getmtime(dst)


def run(args):
    result = subprocess.run(args, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode(errors="replace"))
        raise SystemExit("failed: %s" % " ".join(args[:2]))
    return result


# --- images ----------------------------------------------------------------

def to_png(src, dst, long_edge):
    """Decode with sips, then re-encode properly.

    sips writes a PNG, but it will happily emit 16-bit channels or a colour
    profile, and img_read_png reads 8-bit and ignores profiles. Rather than
    hope, the pixels are pulled back out and written again in exactly the two
    forms the reader accepts: RGB, or RGBA when the source had transparency.
    """
    tmp = dst + ".sips.png"
    run(["sips", "-s", "format", "png",
         "-Z", str(long_edge), src, "--out", tmp])
    width, height, has_alpha, rows = read_png(tmp)
    os.remove(tmp)
    write_png(dst, width, height, has_alpha, rows)
    return width, height


def read_png(path):
    """Enough of a PNG reader for what sips writes: 8-bit RGB or RGBA."""
    data = open(path, "rb").read()
    at, idat = 8, b""
    width = height = depth = colour = 0
    while at < len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack_from(">IIBB", data, at + 8)
        elif kind == b"IDAT":
            idat += data[at + 8:at + 8 + length]
        at += 12 + length
    if depth != 8 or colour not in (2, 6):
        raise SystemExit("%s: sips produced depth %d type %d" % (path, depth, colour))

    bpp = 4 if colour == 6 else 3
    stride = width * bpp
    raw = zlib.decompress(idat)
    rows, previous, at = [], bytearray(stride), 0
    for _ in range(height):
        filt = raw[at]
        at += 1
        line = bytearray(raw[at:at + stride])
        at += stride
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = previous[i]
            upleft = previous[i - bpp] if i >= bpp else 0
            if filt == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif filt == 4:
                estimate = left + up - upleft
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - upleft))
                nearest = left if (da <= db and da <= dc) else (up if db <= dc else upleft)
                line[i] = (line[i] + nearest) & 0xFF
        rows.append(bytes(line))
        previous = line
    return width, height, colour == 6, rows


def chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def write_png(path, width, height, has_alpha, rows):
    """Filter type 1 (sub) on every row, then deflate.

    Photographs compress poorly under 'none' and well under a filter, and
    'sub' is the cheapest one that helps. The reader handles all five, so this
    could choose per row; the gain over always-sub is small and the code to
    decide is not.
    """
    bpp = 4 if has_alpha else 3
    body = bytearray()
    for row in rows:
        body.append(1)
        filtered = bytearray(row)
        for i in range(len(row) - 1, bpp - 1, -1):
            filtered[i] = (row[i] - row[i - bpp]) & 0xFF
        body += filtered
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8,
                                             6 if has_alpha else 2, 0, 0, 0)))
        out.write(chunk(b"IDAT", zlib.compress(bytes(body), 9)))
        out.write(chunk(b"IEND", b""))


# --- sound ------------------------------------------------------------------

def to_wav(src, dst):
    """48 kHz, 16-bit, stereo - the one format audio.h accepts.

    Converting to exactly what the hardware wants means nothing in the running
    system has to resample, which is the reason that header says there is only
    one format at all.
    """
    run(["afconvert", "-f", "WAVE", "-d", "LEI16@48000", "-c", "2", src, dst])


# --- staging ----------------------------------------------------------------

def stage(out_dir):
    made = []

    # Icons are already PNGs of the kind the system reads, so they are copied
    # rather than converted - see mkext.sh, which places them directly.

    papers = os.path.join(out_dir, "wallpapers")
    os.makedirs(papers, exist_ok=True)
    src_dir = os.path.join(MEDIA, "wallpapers")
    if os.path.isdir(src_dir):
        for name in sorted(os.listdir(src_dir)):
            if not name.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            src = os.path.join(src_dir, name)
            dst = os.path.join(papers, upper_stem(name) + ".PNG")
            if newer(src, dst):
                to_png(src, dst, WALLPAPER_LONG_EDGE)
                made.append(dst)

    images = os.path.join(out_dir, "demos", "images")
    os.makedirs(images, exist_ok=True)
    src_dir = os.path.join(MEDIA, "demo media", "images")
    if os.path.isdir(src_dir):
        for name in sorted(os.listdir(src_dir)):
            if not name.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            src = os.path.join(src_dir, name)
            dst = os.path.join(images, upper_stem(name) + ".PNG")
            if newer(src, dst):
                to_png(src, dst, DEMO_LONG_EDGE)
                made.append(dst)

    audio = os.path.join(out_dir, "demos", "audio")
    os.makedirs(audio, exist_ok=True)
    src_dir = os.path.join(MEDIA, "demo media", "audio")
    if os.path.isdir(src_dir):
        for name in sorted(os.listdir(src_dir)):
            if not name.lower().endswith(".mp3"):
                continue
            src = os.path.join(src_dir, name)
            dst = os.path.join(audio, upper_stem(name) + ".WAV")
            if newer(src, dst):
                to_wav(src, dst)
                made.append(dst)

    return made


def upper_stem(name):
    """Upper case, and spaces to underscores.

    Not cosmetic: the shell resolves commands by upper-casing, /BIN is upper
    case throughout, and a space in a path is a second word to anything that
    splits on them. These names came from a Mac and have both problems.
    """
    stem = name.rsplit(".", 1)[0]
    return "".join(c if c.isalnum() else "_" for c in stem).upper()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: mkmedia.py <out-dir>")
    target = sys.argv[1]
    os.makedirs(target, exist_ok=True)
    converted = stage(target)
    if converted:
        total = sum(os.path.getsize(p) for p in converted)
        print("media:  %d file(s) converted, %.1f MiB"
              % (len(converted), total / 1048576.0))
    else:
        print("media:  up to date")
