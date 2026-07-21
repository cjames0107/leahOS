#!/usr/bin/env python3
"""Format a FAT32 partition inside the leahOS disk image and populate it.

This exists so the kernel's filesystem code has something real to read. It
writes a genuine FAT32 volume - correct BPB, two FATs, FSInfo, backup boot
sector, long filename entries - rather than a simplified layout the kernel
could accidentally be written to match.

It also fills in the MBR partition entry, so the image's geometry has exactly
one definition rather than being duplicated between here and the bootloader.

    mkfs_fat32.py <image> <start-lba> [--label NAME]
"""

import argparse
import struct
import sys

SECTOR = 512

# Directory entry attributes
ATTR_READ_ONLY = 0x01
ATTR_HIDDEN    = 0x02
ATTR_SYSTEM    = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE   = 0x20
ATTR_LFN       = ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID

FAT_EOC  = 0x0FFFFFFF
FAT_FREE = 0x00000000

# Fixed timestamp so rebuilding the image twice produces identical bytes.
FIXED_TIME = 0x6000        # 12:00:00
FIXED_DATE = 0x5AE1        # 2025-07-01


def short_name_checksum(short_name: bytes) -> int:
    """The checksum an LFN entry carries to bind it to its 8.3 entry."""
    total = 0
    for byte in short_name:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def is_valid_short(name: str) -> bool:
    """True when the name fits 8.3 exactly and needs no LFN entries."""
    if name != name.upper():
        return False
    stem, _, ext = name.partition(".")
    if "." in ext or not stem or len(stem) > 8 or len(ext) > 3:
        return False
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^{}")
    return all(c in allowed for c in stem + ext)


def to_short(name: str, taken: set) -> bytes:
    """Build the 11-byte 8.3 field, mangling to NAME~n when necessary."""
    stem, _, ext = name.upper().partition(".")
    keep = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^{}"
    stem = "".join(c for c in stem if c in keep) or "FILE"
    ext = "".join(c for c in ext if c in keep)

    if len(stem) <= 8 and name.upper() == name and len(ext) <= 3:
        candidate = f"{stem:<8.8}{ext:<3.3}"
        if candidate not in taken:
            taken.add(candidate)
            return candidate.encode("ascii")

    for index in range(1, 1000):
        suffix = f"~{index}"
        candidate = f"{stem[:8 - len(suffix)]}{suffix:<{len(suffix)}}"
        candidate = f"{candidate:<8.8}{ext:<3.3}"
        if candidate not in taken:
            taken.add(candidate)
            return candidate.encode("ascii")
    raise RuntimeError(f"cannot generate a short name for {name}")


def lfn_entries(name: str, short: bytes) -> bytes:
    """Long filename entries, stored in reverse order ahead of the 8.3 entry."""
    checksum = short_name_checksum(short)
    utf16 = name.encode("utf-16-le")

    # Terminate with NUL, then pad with 0xFFFF to fill the last entry.
    chars = [utf16[i:i + 2] for i in range(0, len(utf16), 2)]
    chars.append(b"\x00\x00")
    while len(chars) % 13:
        chars.append(b"\xFF\xFF")

    out = b""
    total = len(chars) // 13
    for slot in range(total):
        piece = chars[slot * 13:(slot + 1) * 13]
        sequence = slot + 1
        if slot == total - 1:
            sequence |= 0x40                     # last physical entry
        entry = struct.pack("<B", sequence)
        entry += b"".join(piece[0:5])
        entry += struct.pack("<BBB", ATTR_LFN, 0, checksum)
        entry += b"".join(piece[5:11])
        entry += struct.pack("<H", 0)
        entry += b"".join(piece[11:13])
        out = entry + out                        # reverse order
    return out


def dir_entry(short: bytes, attr: int, cluster: int, size: int) -> bytes:
    return (
        short
        + struct.pack("<BBB", attr, 0, 0)
        + struct.pack("<HH", FIXED_TIME, FIXED_DATE)   # create
        + struct.pack("<H", FIXED_DATE)                # last access
        + struct.pack("<H", (cluster >> 16) & 0xFFFF)
        + struct.pack("<HH", FIXED_TIME, FIXED_DATE)   # write
        + struct.pack("<H", cluster & 0xFFFF)
        + struct.pack("<I", size)
    )


class Fat32Volume:
    def __init__(self, total_sectors, start_lba, sectors_per_cluster, label):
        self.total_sectors = total_sectors
        self.start_lba = start_lba
        self.spc = sectors_per_cluster
        self.reserved = 32
        self.num_fats = 2
        self.label = label

        # Microsoft's own sizing formula. Solving it directly rather than
        # iterating avoids the off-by-one that makes the last cluster
        # unaddressable.
        tmp1 = total_sectors - self.reserved
        tmp2 = ((256 * self.spc) + self.num_fats) // 2
        self.fat_sectors = (tmp1 + tmp2 - 1) // tmp2

        self.data_start = self.reserved + self.num_fats * self.fat_sectors
        self.cluster_count = (total_sectors - self.data_start) // self.spc

        if self.cluster_count < 65525:
            raise SystemExit(
                f"error: {self.cluster_count} clusters is below the FAT32 minimum "
                f"of 65525 - grow the partition or shrink sectors-per-cluster"
            )

        self.fat = [FAT_FREE] * (self.cluster_count + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = FAT_EOC
        self.next_free = 2
        self.cluster_data = {}

    @property
    def cluster_bytes(self):
        return self.spc * SECTOR

    def allocate(self):
        if self.next_free >= self.cluster_count + 2:
            raise SystemExit("error: filesystem is full")
        cluster = self.next_free
        self.next_free += 1
        self.fat[cluster] = FAT_EOC
        return cluster

    def write_chain(self, payload: bytes) -> int:
        """Store payload across as many clusters as it needs; return the first."""
        if not payload:
            return 0                                  # empty files own no cluster

        chunks = [payload[i:i + self.cluster_bytes]
                  for i in range(0, len(payload), self.cluster_bytes)]
        clusters = [self.allocate() for _ in chunks]

        for index, (cluster, chunk) in enumerate(zip(clusters, chunks)):
            self.cluster_data[cluster] = chunk.ljust(self.cluster_bytes, b"\x00")
            self.fat[cluster] = clusters[index + 1] if index + 1 < len(clusters) else FAT_EOC
        return clusters[0]

    def build_directory(self, tree: dict, cluster: int, parent_cluster: int, is_root: bool):
        """Fill an already-allocated directory cluster chain from a dict."""
        entries = b""
        taken = set()

        if is_root and self.label:
            entries += dir_entry(f"{self.label:<11.11}".encode("ascii"),
                                 ATTR_VOLUME_ID, 0, 0)
        if not is_root:
            entries += dir_entry(b".          ", ATTR_DIRECTORY, cluster, 0)
            # A root parent is recorded as cluster 0, not as 2.
            entries += dir_entry(b"..         ", ATTR_DIRECTORY,
                                 0 if parent_cluster == 2 else parent_cluster, 0)

        for name, content in tree.items():
            short = to_short(name, taken)
            if isinstance(content, dict):
                child = self.allocate()
                self.cluster_data[child] = b"\x00" * self.cluster_bytes
                if not is_valid_short(name):
                    entries += lfn_entries(name, short)
                entries += dir_entry(short, ATTR_DIRECTORY, child, 0)
                self.build_directory(content, child, cluster, False)
            else:
                first = self.write_chain(content)
                if not is_valid_short(name):
                    entries += lfn_entries(name, short)
                entries += dir_entry(short, ATTR_ARCHIVE, first, len(content))

        if len(entries) > self.cluster_bytes:
            raise SystemExit("error: directory needs more than one cluster "
                             "(not implemented in this tool)")
        self.cluster_data[cluster] = entries.ljust(self.cluster_bytes, b"\x00")

    def boot_sector(self) -> bytes:
        sector = bytearray(SECTOR)
        sector[0:3] = b"\xEB\x58\x90"                 # jmp short +0x58; nop
        sector[3:11] = b"LEAHOS  "

        struct.pack_into("<H", sector, 11, SECTOR)
        sector[13] = self.spc
        struct.pack_into("<H", sector, 14, self.reserved)
        sector[16] = self.num_fats
        struct.pack_into("<H", sector, 17, 0)          # root entries: FAT32 has none
        struct.pack_into("<H", sector, 19, 0)          # 16-bit total: too small
        sector[21] = 0xF8                              # fixed disk
        struct.pack_into("<H", sector, 22, 0)          # 16-bit FAT size: FAT32 has none
        struct.pack_into("<H", sector, 24, 63)
        struct.pack_into("<H", sector, 26, 255)
        struct.pack_into("<I", sector, 28, self.start_lba)
        struct.pack_into("<I", sector, 32, self.total_sectors)

        struct.pack_into("<I", sector, 36, self.fat_sectors)
        struct.pack_into("<H", sector, 40, 0)          # mirror all FATs
        struct.pack_into("<H", sector, 42, 0)          # version 0.0
        struct.pack_into("<I", sector, 44, 2)          # root directory cluster
        struct.pack_into("<H", sector, 48, 1)          # FSInfo sector
        struct.pack_into("<H", sector, 50, 6)          # backup boot sector

        sector[64] = 0x80
        sector[66] = 0x29                              # extended boot signature
        struct.pack_into("<I", sector, 67, 0x1EA405)
        sector[71:82] = f"{self.label:<11.11}".encode("ascii")
        sector[82:90] = b"FAT32   "

        sector[510:512] = b"\x55\xAA"
        return bytes(sector)

    def fsinfo_sector(self) -> bytes:
        sector = bytearray(SECTOR)
        struct.pack_into("<I", sector, 0, 0x41615252)
        struct.pack_into("<I", sector, 484, 0x61417272)
        free = self.cluster_count - (self.next_free - 2)
        struct.pack_into("<I", sector, 488, free)
        struct.pack_into("<I", sector, 492, self.next_free)
        sector[508:512] = b"\x00\x00\x55\xAA"
        return bytes(sector)

    def serialise(self) -> bytes:
        out = bytearray(self.total_sectors * SECTOR)

        out[0:SECTOR] = self.boot_sector()
        out[SECTOR:2 * SECTOR] = self.fsinfo_sector()
        # The backup at sector 6 is what a repair tool reaches for when the
        # primary is damaged, so it has to be a real copy.
        out[6 * SECTOR:7 * SECTOR] = self.boot_sector()
        out[7 * SECTOR:8 * SECTOR] = self.fsinfo_sector()

        packed_fat = b"".join(struct.pack("<I", e) for e in self.fat)
        packed_fat = packed_fat.ljust(self.fat_sectors * SECTOR, b"\x00")
        for copy in range(self.num_fats):
            start = (self.reserved + copy * self.fat_sectors) * SECTOR
            out[start:start + len(packed_fat)] = packed_fat

        for cluster, data in self.cluster_data.items():
            offset = (self.data_start + (cluster - 2) * self.spc) * SECTOR
            out[offset:offset + len(data)] = data

        return bytes(out)


def write_partition_entry(image: bytearray, start_lba: int, sectors: int):
    """Fill MBR partition slot 0. CHS fields are the LBA-only escape value."""
    entry = struct.pack(
        "<B3sB3sII",
        0x00,                 # not the active partition; we boot from the MBR
        b"\xFE\xFF\xFF",      # CHS start: "too large, use LBA"
        0x0C,                 # FAT32 with LBA access
        b"\xFE\xFF\xFF",      # CHS end
        start_lba,
        sectors,
    )
    image[446:446 + 16] = entry


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("start_lba", type=int)
    parser.add_argument("--label", default="LEAHOS")
    parser.add_argument("--sectors-per-cluster", type=int, default=1)
    parser.add_argument("--add", action="append", default=[], metavar="PATH=FILE",
                        help="place FILE in the image at PATH (one directory deep)")
    args = parser.parse_args()

    with open(args.image, "rb") as handle:
        image = bytearray(handle.read())

    total_sectors = len(image) // SECTOR
    if args.start_lba >= total_sectors:
        raise SystemExit("error: partition starts past the end of the image")
    partition_sectors = total_sectors - args.start_lba

    volume = Fat32Volume(partition_sectors, args.start_lba,
                         args.sectors_per_cluster, args.label)

    # The tree the kernel's tests expect. README.MD is deliberately larger than
    # one cluster so reading it has to follow a FAT chain rather than getting
    # lucky with a single-cluster file.
    long_line = ("leahOS reads this file by following a cluster chain through "
                 "the FAT, which is the whole point of it being this long. ")
    tree = {
        "HELLO.TXT": b"Hello from FAT32.\n",
        "README.MD": (("# leahOS\n\n" + long_line * 24).encode("ascii")),
        "DOCS": {
            "NOTES.TXT": b"Notes live in a subdirectory.\n",
        },
        "a-long-file-name-for-leahos.txt":
            b"This file needs long filename entries to be named correctly.\n",
    }

    for spec in args.add:
        target, _, source = spec.partition("=")
        if not source:
            raise SystemExit(f"error: --add expects PATH=FILE, got {spec!r}")
        with open(source, "rb") as handle:
            payload = handle.read()

        parts = [p for p in target.split("/") if p]
        node = tree
        for directory in parts[:-1]:
            node = node.setdefault(directory, {})
            if not isinstance(node, dict):
                raise SystemExit(f"error: {directory} is a file, not a directory")
        node[parts[-1]] = payload

    root = 2
    volume.allocate()                     # cluster 2 is the root directory
    volume.build_directory(tree, root, root, True)

    filesystem = volume.serialise()
    offset = args.start_lba * SECTOR
    image[offset:offset + len(filesystem)] = filesystem

    write_partition_entry(image, args.start_lba, partition_sectors)

    with open(args.image, "wb") as handle:
        handle.write(image)

    print(f"fat32:  {partition_sectors} sectors at LBA {args.start_lba}, "
          f"{volume.cluster_count} clusters of {volume.cluster_bytes} B, "
          f"FAT {volume.fat_sectors} sectors")


if __name__ == "__main__":
    sys.exit(main())
