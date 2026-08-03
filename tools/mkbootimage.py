#!/usr/bin/env python3
"""Turn an ELF into something the kernel can map without understanding ELF.

The kernel carries three programs - the disk driver, the filesystem and init -
because they have to run before there is anything to load them from. That was
the last reason for an ELF parser in ring 0: everything else is exec'd by a
process that can read the file itself.

So the parsing happens here instead, once, at build time. What comes out is a
small header and the segment bytes laid end to end:

    u64 magic       'LEAHIMG1'
    u64 entry       where to start
    u32 count       how many segments
    u32 reserved
    segments[count] { u64 vaddr, u64 offset, u64 filesz, u64 memsz,
                      u32 flags, u32 pad }
    ... bytes, at the offsets the segments name ...

flags are the ELF program-header flags, unchanged: 1 execute, 2 write, 4 read.
The kernel maps from this and never sees a program header.
"""

import struct
import sys

MAGIC = b'LEAHIMG1'
PT_LOAD = 1


def read_elf(path):
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF':
        raise SystemExit('%s: not an ELF' % path)
    if data[4] != 2 or data[5] != 1:
        raise SystemExit('%s: not 64-bit little-endian' % path)

    entry = struct.unpack_from('<Q', data, 24)[0]
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize = struct.unpack_from('<H', data, 54)[0]
    phnum = struct.unpack_from('<H', data, 56)[0]

    segments = []
    for i in range(phnum):
        at = phoff + i * phentsize
        p_type, p_flags = struct.unpack_from('<II', data, at)
        p_offset, p_vaddr = struct.unpack_from('<QQ', data, at + 8)
        p_filesz, p_memsz = struct.unpack_from('<QQ', data, at + 32)
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        if p_filesz > p_memsz:
            raise SystemExit('%s: segment %d has more file than memory' % (path, i))
        segments.append({
            'vaddr': p_vaddr,
            'memsz': p_memsz,
            'flags': p_flags,
            'bytes': data[p_offset:p_offset + p_filesz],
        })
    if not segments:
        raise SystemExit('%s: nothing to load' % path)
    return entry, segments


def write_image(out_path, entry, segments):
    header_size = 8 + 8 + 4 + 4
    table_size = len(segments) * 40
    body_at = header_size + table_size

    table = b''
    body = b''
    for s in segments:
        offset = body_at + len(body)
        table += struct.pack('<QQQQII', s['vaddr'], offset, len(s['bytes']),
                             s['memsz'], s['flags'], 0)
        body += s['bytes']

    with open(out_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<QII', entry, len(segments), 0))
        f.write(table)
        f.write(body)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: mkbootimage.py <in.elf> <out.img>')
    entry, segments = read_elf(sys.argv[1])
    write_image(sys.argv[2], entry, segments)
    total = sum(len(s['bytes']) for s in segments)
    print('boot image: %s, %d segment(s), %d bytes of contents'
          % (sys.argv[2], len(segments), total))
