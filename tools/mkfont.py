#!/usr/bin/env python3
"""Strip a TrueType font down to the tables that draw glyphs.

Google Sans Flex is four megabytes, and all but forty kilobytes of that is
things this system has no use for: `gvar` alone is 3.6 MB of variable-axis
deltas, and GPOS/GDEF/GSUB are the OpenType layout tables that drive kerning,
ligatures and script shaping. A renderer that draws one glyph at a time at one
weight reads none of them.

What is left is the default instance of the font - which is what `glyf` holds
for a variable font when nobody applies the deltas - at about one per cent of
the size. Small enough to read whole at startup and keep in memory, which makes
the runtime side much simpler than seeking around a four megabyte file.

This is table stripping, not subsetting. Every glyph survives and every glyph
id keeps its number, so `cmap` and `loca` need no rewriting and there is
nothing here that can silently drop a character.
"""

import struct
import sys

# Everything needed to turn a character into an outline and know how far to
# advance afterwards. Anything not here describes typography this system does
# not do yet.
KEEP = [b"head", b"hhea", b"maxp", b"cmap", b"hmtx", b"loca", b"glyf"]


def strip(source, destination):
    data = open(source, "rb").read()

    version, count = struct.unpack(">IH", data[:6])
    if version not in (0x00010000, 0x74727565):
        raise SystemExit(f"{source}: not a TrueType file with glyf outlines")

    tables = {}
    for i in range(count):
        at = 12 + 16 * i
        tag = data[at:at + 4]
        _checksum, offset, length = struct.unpack(">III", data[at + 4:at + 16])
        tables[tag] = data[offset:offset + length]

    missing = [t.decode() for t in KEEP if t not in tables]
    if missing:
        raise SystemExit(f"{source}: no {', '.join(missing)} table")

    kept = [(tag, tables[tag]) for tag in KEEP]

    # The directory has to be sorted by tag, and every table four-byte aligned.
    kept.sort(key=lambda entry: entry[0])
    n = len(kept)

    # searchRange and friends: a binary-search hint nothing here reads, written
    # correctly anyway because a malformed one is the kind of thing that works
    # until something else opens the file.
    power = 1
    while power * 2 <= n:
        power *= 2
    search_range = power * 16
    entry_selector = power.bit_length() - 1
    range_shift = n * 16 - search_range

    header = struct.pack(">IHHHH", 0x00010000, n, search_range,
                         entry_selector, range_shift)

    offset = 12 + 16 * n
    directory = b""
    body = b""
    for tag, payload in kept:
        checksum = 0
        padded = payload + b"\0" * (-len(payload) % 4)
        for k in range(0, len(padded), 4):
            checksum = (checksum + struct.unpack(">I", padded[k:k + 4])[0]) & 0xFFFFFFFF
        directory += struct.pack(">4sIII", tag, checksum, offset, len(payload))
        body += padded
        offset += len(padded)

    open(destination, "wb").write(header + directory + body)

    before, after = len(data), 12 + 16 * n + len(body)
    print(f"font:   {destination} ({after} bytes, was {before}, "
          f"{n} tables of {count})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: mkfont.py SOURCE.ttf DESTINATION.ttf")
    strip(sys.argv[1], sys.argv[2])
