"""
ModLoader64 equipment-pak pipeline: detect game (OoT/MM) and which items a pak
contains, extract each item's geometry for rendering, and expose per-item export.

Equipment zobjs (unlike player models) have NO skeleton - they are single meshes
dispatched through a run of G_DL (0xDE) commands near the MODLOADER64 footer.
Each zobj embeds its own metadata:
  EQUIPMENTNAME  - internal name
  EQUIPMENTCAT   - the item type (e.g. "Master Sword", "Hylian Shield")
  EQUIPMANIFEST  - JSON {game: {age: {slot: dlName}}} mapping slots to DL names
                   (a game/age with a non-empty dict => this item targets it)
"""
from __future__ import annotations
import json
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from . import gbi

_GAMES = {"OOT": "OOT", "MM": "MM"}


def _embed(z: bytes, tag: bytes) -> Optional[str]:
    i = z.find(tag)
    if i < 0:
        return None
    p = i + len(tag)
    while p < len(z) and z[p] == 0:
        p += 1
    j = p
    while j < len(z) and z[j] not in (0, 0xFF):
        j += 1
    return z[p:j].decode("latin1", errors="replace")


def dispatch_dls(z: bytes) -> List[int]:
    """All G_DL(0xDE)->seg6 targets in the zobj. Equipment DLs are reached through
    these (a dispatch entry may chain: dispatch -> jump DL -> geometry DL), so we
    walk every target and dedup the resulting triangles - robust to whichever
    dispatch layout a pak uses. Returns unique seg-6 offsets, in file order."""
    seen, offs = set(), []
    for o in range(0, len(z) - 8, 4):
        if z[o] == 0xDE:
            w1 = struct.unpack_from(">I", z, o + 4)[0]
            tgt = w1 & 0xFFFFFF
            if (w1 >> 24) == 0x06 and tgt < len(z) and tgt not in seen:
                seen.add(tgt)
                offs.append(tgt)
    return offs


@dataclass
class EquipItem:
    file: str
    name: str = ""
    category: str = ""
    manifest: dict = field(default_factory=dict)
    dl_offsets: List[int] = field(default_factory=list)
    zdata: bytes = b""

    @property
    def games(self) -> List[str]:
        out = []
        for g in ("OOT", "MM"):
            block = self.manifest.get(g) or {}
            if any(v for v in block.values()):   # some age has slots
                out.append(g)
        return out

    @property
    def ages(self) -> List[str]:
        out = []
        for g, block in self.manifest.items():
            for age, slots in (block or {}).items():
                if slots:
                    out.append(f"{g}:{age}")
        return out

    def geometry(self):
        """gbi.Triangle for the equipment mesh (object space). Walks every DL
        target and dedups identical triangles (dispatch entries can chain into
        the same geometry DL)."""
        seg = {6: self.zdata}
        seen, tris = set(), []
        for off in (self.dl_offsets or dispatch_dls(self.zdata)):
            for t in gbi.walk(seg, 0x06000000 | off).triangles:
                key = tuple((v.x, v.y, v.z, v.s, v.t) for v in t.v)
                if key in seen:
                    continue
                seen.add(key)
                tris.append(t)
        return tris

    def _ordered_slot_names(self, game=None):
        """Manifest slot names in numeric slot order (e.g. sword1_blade,
        sword1_hilt, sword1_sheath) for the requested game, or the first target."""
        for g in ((game,) if game else ("OOT", "MM")):
            for age, slots in (self.manifest.get(g) or {}).items():
                if slots:
                    return [slots[k] for k in sorted(slots, key=lambda x: int(x))]
        return []

    def pieces(self, game=None):
        """[(slot_name, [Triangle])] per dispatch DL. The dispatch table lists the
        piece DLs in the SAME order as the numbered manifest slots (verified: the
        contiguous G_DL run maps 1:1 to slots 0,1,2 = blade,hilt,sheath), so we
        zip them. Lets the converter split a sword's blade/hilt from its scabbard
        into SoH's separate custom-equip slots instead of baking the sheath onto
        the held sword."""
        seg = {6: self.zdata}
        names = self._ordered_slot_names(game)
        out = []
        for i, off in enumerate(self.dl_offsets or dispatch_dls(self.zdata)):
            nm = names[i] if i < len(names) else "slot%d" % i
            out.append((nm, gbi.walk(seg, 0x06000000 | off).triangles))
        return out

    def seg_map(self):
        return {6: self.zdata, 8: self.zdata, 9: self.zdata}


@dataclass
class EquipPak:
    kind: str = "equipment"
    name: str = ""
    author: str = ""
    items: List[EquipItem] = field(default_factory=list)
    icon: bytes = b""            # pak thumbnail (icon.png)

    @property
    def games(self) -> List[str]:
        s = set()
        for it in self.items:
            s.update(it.games)
        return [g for g in ("OOT", "MM") if g in s]

    def summary(self) -> str:
        lines = [f"{self.name or '(unnamed)'}  by {self.author or '?'}",
                 "  kind: equipment",
                 f"  games: {', '.join(self.games) or '(none)'}",
                 f"  items: {len(self.items)}" + ("  [icon present]" if self.icon else "")]
        for it in self.items:
            lines.append(f"    - {it.category or it.name:22s} "
                         f"[{','.join(it.ages) or '?'}]  {it.file}")
        return "\n".join(lines)


def parse(files: Dict[str, bytes], pkg: Optional[dict] = None) -> EquipPak:
    """`files` = {relpath: bytes} from pak.load_map()."""
    ep = EquipPak()
    if pkg:
        ep.name = str(pkg.get("name", ""))
        ep.author = str(pkg.get("author", ""))
    for k, v in files.items():
        base = k.rsplit("/", 1)[-1]
        if base == "icon.png":
            ep.icon = v
    for k, v in sorted(files.items()):
        if not k.lower().endswith(".zobj"):
            continue
        it = EquipItem(file=k.rsplit("/", 1)[-1], zdata=v)
        it.name = _embed(v, b"EQUIPMENTNAME") or ""
        it.category = _embed(v, b"EQUIPMENTCAT") or ""
        man = _embed(v, b"EQUIPMANIFEST")
        if man:
            try:
                it.manifest = json.loads(man)
            except Exception:
                it.manifest = {}
        it.dl_offsets = dispatch_dls(v)
        ep.items.append(it)
    return ep
