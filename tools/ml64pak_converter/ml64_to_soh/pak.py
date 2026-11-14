"""
ModLoader64 .pak container reader.

Binary layout (big-endian), reverse-engineered from the reference paks:

    0x00  char[12]   "ModLoader64\0"
    0x0C  u32        header field (low byte = entry count; exact meaning of the
                     upper bytes is a version/flags word - we don't rely on it and
                     instead stop parsing when a record tag is not DEFL/UNCO)
    0x10  record[N]  16 bytes each:
                        char[4] tag         "DEFL" (zlib) | "UNCO" (stored)
                        u32     nameOffset  absolute offset into the string table
                        u32     dataStart   absolute offset of the (compressed) blob
                        u32     dataEnd      absolute end offset (exclusive)
    ...   string table   names, each terminated by a 0xFF separator byte.
                         Every name is prefixed with "<PakName>/" e.g.
                         "Child_OoTLink/object_link_child.zobj".
    ...   data blobs      zlib streams (DEFL) or raw bytes (UNCO).

`load()` returns a list of PakEntry(name, tag, data) with `data` already
decompressed.
"""
from __future__ import annotations
import struct
import zlib
from dataclasses import dataclass
from typing import List


@dataclass
class PakEntry:
    name: str          # full internal path, e.g. "Child_OoTLink/object_link_child.zobj"
    tag: str           # "DEFL" or "UNCO"
    data: bytes        # decompressed bytes

    @property
    def basename(self) -> str:
        return self.name.replace("\\", "/").rsplit("/", 1)[-1]

    @property
    def relpath(self) -> str:
        """Path without the leading '<PakName>/' component."""
        p = self.name.replace("\\", "/")
        return p.split("/", 1)[1] if "/" in p else p


MAGIC = b"ModLoader64"


def is_pak(path: str) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(11) == MAGIC
    except OSError:
        return False


def load(path: str) -> List[PakEntry]:
    with open(path, "rb") as f:
        d = f.read()
    if d[:11] != MAGIC:
        raise ValueError("not a ModLoader64 pak (bad magic)")

    off = 0x10
    records = []
    while off + 16 <= len(d):
        tag = d[off:off + 4]
        if tag not in (b"DEFL", b"UNCO"):
            break
        name_off, start, end = struct.unpack_from(">III", d, off + 4)
        records.append((tag, name_off, start, end))
        off += 16

    entries: List[PakEntry] = []
    for tag, name_off, start, end in records:
        # names are terminated by a 0xFF separator byte
        j = name_off
        while j < len(d) and d[j] != 0xFF:
            j += 1
        name = d[name_off:j].decode("latin1")
        blob = d[start:end]
        if tag == b"DEFL":
            try:
                data = zlib.decompress(blob)
            except zlib.error:
                data = zlib.decompress(blob, -15)  # raw deflate fallback
        else:
            data = blob
        entries.append(PakEntry(name=name, tag=tag.decode("ascii"), data=data))
    return entries


def load_map(path: str) -> dict:
    """Return {relpath: bytes} keyed by path without the pak-name prefix."""
    return {e.relpath: e.data for e in load(path)}
