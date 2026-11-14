"""
Extract OoT's `gameplay_keep` (segment 4) from a retail ROM.

Some ModLoader64 / Z64Online models - typically vanilla-Link re-exports - draw
parts of themselves (hair, tunic detail) from the engine-provided shared object
`gameplay_keep`, referenced as N64 segment 0x04. That data lives in the ROM, not
in the pak, so without it those tiles have no texture (hair renders white) AND the
produced .o2r can't resolve them either (SoH addresses gameplay_keep by resource
name, not by the raw VROM offset the model uses) - so we bake the referenced
textures in. If no ROM is found we degrade gracefully (segment 4 stays unmapped).
"""
from __future__ import annotations
import glob
import os
import struct
from typing import Optional

_CACHE: dict = {}
_ENV_VARS = ("ML64_OOT_ROM", "OOT_ROM")


def _rom_candidates():
    """OoT ROM search paths. You supply your own ROM - none is bundled. Preferred
    way is the ML64_OOT_ROM env var; otherwise we look in the working directory and
    a `roms/` folder beside it for an Ocarina of Time (USA) image."""
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = [os.getcwd(), os.path.join(os.getcwd(), "roms"),
            os.path.join(here, "..", "roms"), os.path.join(here, "..", "..", "roms")]
    hits = []
    for d in dirs:
        d = os.path.normpath(d)
        if os.path.isdir(d):
            for pat in ("*Ocarina of Time*.z64", "*Ocarina of Time*.n64",
                        "*ocarina*.z64", "*oot*.z64"):
                hits += sorted(glob.glob(os.path.join(d, pat)))
    # de-dup, preserve order
    seen, out = set(), []
    for h in hits:
        if h.lower() not in seen:
            seen.add(h.lower()); out.append(h)
    return out


def _yaz0(d: bytes) -> bytes:
    size = struct.unpack_from(">I", d, 4)[0]
    out = bytearray()
    src = 16
    while len(out) < size:
        group = d[src]; src += 1
        for b in range(8):
            if len(out) >= size:
                break
            if group & (0x80 >> b):
                out.append(d[src]); src += 1
            else:
                b1, b2 = d[src], d[src + 1]; src += 2
                dist = ((b1 & 0xF) << 8) | b2
                n = b1 >> 4
                if n == 0:
                    n = d[src] + 0x12; src += 1
                else:
                    n += 2
                start = len(out) - dist - 1
                for i in range(n):
                    out.append(out[start + i])
    return bytes(out)


def _find_rom() -> Optional[str]:
    for v in _ENV_VARS:
        p = os.environ.get(v)
        if p and os.path.exists(p):
            return p
    for p in _rom_candidates():
        if os.path.exists(p):
            return p
    return None


def oot_gameplay_keep() -> Optional[bytes]:
    """The decompressed gameplay_keep blob (segment 4 base), or None if no ROM.
    Cached; the Yaz0 decompress runs at most once per process."""
    if "oot" in _CACHE:
        return _CACHE["oot"]
    _CACHE["oot"] = None
    rom = _find_rom()
    if not rom:
        return None
    try:
        data = open(rom, "rb").read()
        # dmadata file table (USA 1.0 at ROM 0x7430): 16-byte entries
        # [vromStart, vromEnd, romStart, romEnd].
        p = 0x7430
        for _ in range(1600):
            vs, ve, rs, re = struct.unpack_from(">IIII", data, p); p += 16
            if vs == 0 and ve == 0 and rs == 0 and re == 0:
                break
            size = ve - vs
            # gameplay_keep: the pre-scene (VROM < 0x03000000) Yaz0 file whose
            # decompressed size is ~0x5C000 (much larger than field/dangeon keep).
            if vs < 0x03000000 and 0x50000 <= size <= 0x70000:
                raw = data[rs:re] if re and re > rs else data[rs:rs + size]
                if raw[:4] == b"Yaz0":
                    _CACHE["oot"] = _yaz0(raw)
                    break
    except Exception:
        _CACHE["oot"] = None
    return _CACHE["oot"]
