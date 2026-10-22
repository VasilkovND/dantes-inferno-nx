#!/usr/bin/env python3
"""Make the SD card folder for dantes-inferno-nx from your own disc and title update.

    python3 stage_sd.py --disc "Dante's Inferno.iso" --tu <TU2 package or its default.xexp> -o <out>/dantes-inferno-nx

Writes, beside where the NRO goes (src/dante_app.h reads this layout):
    dantes-inferno-nx/game/   the extracted disc (bigfile0.viv 3.2 GB and bigfile1.viv 2.7 GB: under FAT32's 4 GB)
                              and Title Update 2's default.xexp next to its default.xex

The same game/ folder is what tools/build_mac.sh takes to generate the code. Every input is checked against the
hashes the port was made and tested with: the code is TU2, so any other executable or patch would not match it.
Supported: the Europe disc (Media ID 27C36C6C) with its Title Update 2 package (TU_12K226F_000000C000000.00000000000G3),
and the disc of Media ID 49028C6A with its own (TU_12K226F_0000004000000.00000000000G1). A Games on Demand install
needs no staging: copy it to god/ (dante/src/dante_god.cpp).
Only the standard library (the title update package is read by the small STFS reader below).
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

TITLE_ID = 0x454108CF
# TU2 default.xexp hash -> (edition, Media ID, disc default.xex hash)
EDITIONS = {
    "f49c87b8d16ef0eed693c134458ccb2bd05481ef24f38b7c6d60d532163d2290":
        ("Europe", "27C36C6C", "3840af42c628d0d8d6d98965057893b8bfc7c197479f485fe6f57db8b014b0aa"),
    # The edition whose Games on Demand package carries this executable already updated (default.xex 0.0.2.1, the
    # disc's beside it as default.dll). After TU2 its image is byte for byte the Europe one's: the same build runs it.
    "0842733820a27e0c902e6a652b3cd71d2c4a05f36db5121c1d342416a6b7ab09":
        ("Media ID 49028C6A", "49028C6A", "abfef19fa03a247ab17d5c011e34ca70a0338f13c15b7fd8932c7ff7d9e2c2e9"),
}
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Disc files the game does not need: the console's system update.
SKIP = ("$systemupdate/",)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- XDVDFS (Xbox 360 disc) -------------------------------------------------------------------------------
def partition(f) -> int:
    for base in (0x0, 0x2080000, 0xFD90000, 0x18300000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    sys.exit("Not an Xbox 360 disc image (no XDVDFS volume found)")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    pending, seen = [0], set()
    while pending:
        o = pending.pop()
        p = o * 4
        if o in seen or p + 14 > len(table):
            continue
        seen.add(o)
        left, right, start, length, attr, nlen = struct.unpack("<HHIIBB", table[p:p + 14])
        if left == 0xFFFF:
            continue
        name = table[p + 14:p + 14 + nlen].decode("latin1")
        if left:
            pending.append(left)
        if right:
            pending.append(right)
        if "/" in name or "\\" in name or name in ("", ".", ".."):
            sys.exit(f"Unsafe name on disc: {name!r}")
        path = f"{prefix}{name}"
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, path + "/")
        else:
            yield path, start, length


def extract(iso: Path, out: Path) -> None:
    with iso.open("rb") as f:
        base = partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root, root_size = struct.unpack("<II", f.read(8))
        files = sorted(e for e in walk(f, base, root, root_size) if not e[0].lower().startswith(SKIP))
        total = sum(n for _, _, n in files)
        done = 0
        for path, start, length in files:
            dest = out / path
            if dest.is_file() and dest.stat().st_size == length:
                done += length
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            f.seek(base + start * SECTOR)
            left = length
            with dest.open("wb") as o:
                while left:
                    chunk = f.read(min(8 << 20, left))
                    if not chunk:
                        sys.exit(f"{iso}: image ends inside {path}")
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    print(f"\r  {iso.name}: {done * 100 // max(total, 1)}%", end="", flush=True)
        print(f"\r  {iso.name}: {len(files)} files, {total / 2**30:.1f} GiB -> {out}")


# --- Title update ------------------------------------------------------------------------------------------
class Stfs:
    """The parts of an Xbox 360 STFS package (CON/LIVE/PIRS) needed to read one file from it."""

    BLOCK = 0x1000

    def __init__(self, path: Path):
        self.f = open(path, "rb")
        head = self.read(0, 0x3A0)
        if head[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            sys.exit(f"{path} is not an Xbox 360 package (STFS)")
        self.title_id = struct.unpack_from(">I", head, 0x360)[0]
        header_size = struct.unpack_from(">I", head, 0x340)[0]
        self.data = (header_size + 0xFFF) & ~0xFFF
        volume = head[0x379:0x379 + 0x24]
        # Read-only packages keep one hash table per level, the others two.
        self.tables = 1 if volume[2] & 1 else 2
        self.table_blocks = struct.unpack_from("<H", volume, 3)[0]
        self.table_start = int.from_bytes(volume[5:8], "little")

    def read(self, offset: int, n: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(n)

    def block(self, b: int) -> bytes:
        # Data blocks are numbered without the hash table blocks between them: one level 0 table every 0xAA blocks,
        # one level 1 every 0x70E4, one level 2 every 0x4AF768.
        backing = b + (b + 0xAA) // 0xAA * self.tables
        if b >= 0xAA:
            backing += (b + 0x70E4) // 0x70E4 * self.tables
            if b >= 0x70E4:
                backing += (b + 0x4AF768) // 0x4AF768 * self.tables
        return self.read(self.data + backing * self.BLOCK, self.BLOCK)

    def entries(self) -> list:
        out = []
        for k in range(self.table_blocks):
            table = self.block(self.table_start + k)
            for i in range(0, self.BLOCK, 0x40):
                e = table[i:i + 0x40]
                if not e[0]:
                    continue
                flags = e[0x28]
                out.append({"name": e[:flags & 0x3F].decode("latin-1"), "dir": bool(flags & 0x80),
                            "contig": bool(flags & 0x40), "blocks": int.from_bytes(e[0x29:0x2C], "little"),
                            "start": int.from_bytes(e[0x2F:0x32], "little"),
                            "parent": struct.unpack_from(">h", e, 0x32)[0],
                            "size": struct.unpack_from(">I", e, 0x34)[0]})
        return out


def read_xexp(path: Path) -> bytes:
    if path.stat().st_size < 16 << 20:
        data = path.read_bytes()
        if sha256(data) in EDITIONS:
            return data
    pkg = Stfs(path)
    try:
        if pkg.title_id != TITLE_ID:
            sys.exit(f"{path} is not a Dante's Inferno package")
        entries = pkg.entries()
        for e in entries:
            parent = entries[e["parent"]]["name"] if 0 <= e["parent"] < len(entries) else ""
            if e["dir"] or e["name"].casefold() != "default.xexp":
                continue
            if not e["contig"]:
                sys.exit("default.xexp is fragmented in this package: extract it with another tool and pass "
                         "the .xexp file to --tu")
            data = b"".join(pkg.block(e["start"] + k) for k in range(e["blocks"]))[:e["size"]]
            if sha256(data) not in EDITIONS:
                sys.exit("default.xexp is not a Title Update 2 this port supports (hash mismatch): use the Title "
                         "Update 2 package for your disc's Media ID (Europe 27C36C6C: TU_12K226F_000000C000000.00000000000G3; "
                         "49028C6A: TU_12K226F_0000004000000.00000000000G1)")
            return data
        sys.exit(f"{path} has no default.xexp")
    finally:
        pkg.f.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", type=Path, required=True, help="the disc image (ISO)")
    ap.add_argument("--tu", type=Path, required=True, help="Title Update 2 package, or its default.xexp")
    ap.add_argument("-o", "--out", type=Path, required=True)
    a = ap.parse_args()
    for name, path in (("--disc", a.disc), ("--tu", a.tu)):
        if not path.is_file():
            sys.exit(f"{name}: {path} not found")

    xexp = read_xexp(a.tu)
    edition, media_id, xex_sha = EDITIONS[sha256(xexp)]
    print(f"  TU2 default.xexp: ok ({edition}, Media ID {media_id})")
    game = a.out / "game"
    extract(a.disc, game)
    xex = (game / "default.xex").read_bytes()
    if sha256(xex) != xex_sha:
        other = [e for e in EDITIONS.values() if e[2] == sha256(xex)]
        sys.exit(f"The disc's default.xex is from the {other[0][0]} edition, but the TU2 is for {edition}: use the TU2 "
                 f"package with Media ID {other[0][1]}" if other else
                 f"The disc's default.xex is not one this port supports (Dante's Inferno, {edition}, Media ID {media_id})")
    (game / "default.xexp").write_bytes(xexp)
    print(f"Ready: copy {a.out} to sdmc:/switch/ (the NRO and the shader files go in it, beside game/).")


if __name__ == "__main__":
    main()
