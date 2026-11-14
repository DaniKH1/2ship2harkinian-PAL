"""
High-level loader: a ModLoader64 player-model zobj -> skeleton + posed geometry.

Provides everything the renderer and the converter need from a single zobj:
the form/age byte, the skeleton, the per-limb display lists, and (if present)
the PlayAsManifest role table.
"""
from __future__ import annotations
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import numpy as np

from . import gbi, skeleton as skel

# form/age byte at header+0x0B
FORM_NAMES = {0: "adult/fd", 1: "child/goron", 2: "zora", 3: "deku", 4: "human"}

# ObjectModelReader parity: the game's diffuse light direction, and the default
# Kokiri tunic colour the player's C code seeds into the env register before
# drawing the cloth limbs (grayscale texel x env -> green tunic).
_LIGHT_DIR = np.array([0.4, 0.85, 0.3], np.float64)
_LIGHT_DIR /= np.linalg.norm(_LIGHT_DIR)
KOKIRI_GREEN = (30 / 255.0, 105 / 255.0, 27 / 255.0)
_WHITE = (255, 255, 255, 255)


@dataclass
class PlayerModel:
    data: bytes
    form_byte: int = 0
    skeleton: Optional[skel.Skeleton] = None
    manifest: List[Tuple[str, int]] = field(default_factory=list)

    # ModLoader/Z64Online zzplayas models store the face textures at the very
    # START of the zobj (before the MODLOADER64 block @0x5000): the eye frames at
    # offset 0, the mouth frames at 0x4000. At runtime Z64Online points segment 8
    # (eyes) at offset 0 and segment 9 (mouth) at 0x4000 (see Z64Online
    # UniversalAliasTable: TEX_EYES->0x06000000, TEX_MOUTH->0x06004000). Mapping
    # the segments the same way lets the head DL's seg-8/9 SETTIMGs resolve to the
    # model's OWN eyes/mouth (open eye / neutral mouth = frame 0).
    FACE_MOUTH_OFF = 0x4000

    def seg_map(self) -> Dict[int, bytes]:
        d = self.data
        mouth = d[self.FACE_MOUTH_OFF:] if len(d) > self.FACE_MOUTH_OFF else d
        m = {6: d, 8: d, 9: mouth, 0xA: d}
        # Segment 4 = the engine's shared gameplay_keep. Vanilla-Link re-exports draw
        # hair/tunic detail from it; map it so those tiles resolve (OoT forms only -
        # form_byte 4 is MM, which has its own keep). No-op when no ROM is available.
        if self.form_byte != 4:
            from .keep import oot_gameplay_keep
            gk = oot_gameplay_keep()
            if gk:
                m[4] = gk
        return m

    # local vertex coords sit within ~2000 units of the limb origin; anything
    # far larger is a stray load from a non-geometry segment -> reject.
    LOCAL_LIMIT = 6000

    def limb_triangles(self, tunic=KOKIRI_GREEN):
        """
        Return [(Triangle, worldVerts[3], vertexColors[3])] for every limb DL,
        faithfully reproducing the in-game look (ObjectModelReader parity):
          - lit vertices are shaded by their world normal . light direction
          - the RDP combiner's prim/env colour tints the (often grayscale) texel
          - env defaults to the tunic colour on cloth limbs
        vertexColors are (r,g,b) floats in 0..1 to multiply the sampled texel by.
        """
        out = []
        if not self.skeleton:
            return out
        geo = {6: self.data}
        sk = self.skeleton
        eye = np.eye(3)

        def eff_limb(v, lb):
            # flex skeleton: a vertex loaded under a seg-0x0D matrix belongs to
            # another limb (elbow/shoulder blend), not the limb being drawn.
            s = v.flex_slot
            if 0 <= s < len(sk.slot_to_limb):
                return sk.limbs[sk.slot_to_limb[s]]
            return lb

        # Draw limbs in SkelAnime draw order (slot_to_limb) with a persistent
        # interpreter so RDP texture state carries across limbs (some limbs draw
        # untextured and inherit the previous limb's bound texture).
        order = sk.slot_to_limb or [l.index for l in sk.limbs if l.dlist]
        interp = gbi.Interp(geo)
        for limb_idx in order:
            lb = sk.limbs[limb_idx]
            if not lb.dlist:
                continue
            for tri in interp.run_limb(lb.dlist):
                if any(abs(v.x) > self.LOCAL_LIMIT or abs(v.y) > self.LOCAL_LIMIT
                       or abs(v.z) > self.LOCAL_LIMIT for v in tri.v):
                    continue
                effs = [eff_limb(v, lb) for v in tri.v]
                if tri.tile.lit and back_facing(tri.v, effs):
                    continue
                wvs = [e.apply(v.x, v.y, v.z) for v, e in zip(tri.v, effs)]
                cols = [self._vtx_color(v, tri.tile, e.M if e.M is not None else eye, tunic)
                        for v, e in zip(tri.v, effs)]
                out.append((tri, wvs, cols))
        return out

    @staticmethod
    def _vtx_color(v, ts, M, tunic):
        if ts.lit:
            n = np.array([_sb(v.r), _sb(v.g), _sb(v.b)], np.float64)
            nw = M @ n
            m = np.linalg.norm(nw)
            if m > 1e-3:
                nw /= m
            # We render two-sided (no backface cull, since model winding varies),
            # so orient the normal toward the +Z camera before lighting - a
            # back-facing surface then shades like a front-facing one.
            if nw[2] < 0:
                nw = -nw
            shade = float(np.clip(np.dot(nw, _LIGHT_DIR) * 0.4 + 0.68, 0.55, 1.0))
            base = (shade, shade, shade)
        else:
            base = (v.r / 255.0, v.g / 255.0, v.b / 255.0)
        # combiner tint: prim, else env (defaulting to tunic on cloth), else none
        if ts.combine_uses_prim:
            tint = tuple(c / 255.0 for c in ts.prim[:3])
        elif ts.combine_uses_env:
            tint = tunic if ts.env[:4] == _WHITE else tuple(c / 255.0 for c in ts.env[:3])
        else:
            tint = (1.0, 1.0, 1.0)
        return (base[0] * tint[0], base[1] * tint[1], base[2] * tint[2])


def _sb(b):
    """interpret a 0..255 byte as a signed int8 (normal component)."""
    return b - 256 if b > 127 else b


_EYE3 = np.eye(3)


def back_facing(verts, effs) -> bool:
    """True if a lit triangle's averaged world vertex-normal points away from the
    +Z camera. Uses the outward vertex NORMALS (not screen winding), so it culls
    correctly regardless of a model's triangle winding order."""
    nz = 0.0
    for v, e in zip(verts, effs):
        n = np.array([_sb(v.r), _sb(v.g), _sb(v.b)], np.float64)
        M = e.M if e.M is not None else _EYE3
        nw = M @ n
        nz += nw[2] / (np.linalg.norm(nw) or 1.0)
    return nz < 0.0


def _u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def parse_manifest(d: bytes) -> List[Tuple[str, int]]:
    i = d.find(b"!PlayAsManifest0")
    if i < 0:
        return []
    p = i + 16
    count = struct.unpack_from(">H", d, p)[0]
    p += 2
    recs = []
    try:
        for _ in range(count):
            j = d.index(b"\0", p)
            name = d[p:j].decode("latin1")
            p = j + 1
            off = struct.unpack_from(">I", d, p)[0]
            p += 4
            recs.append((name, off))
    except Exception:
        pass
    return recs


def load(data: bytes) -> PlayerModel:
    pm = PlayerModel(data=data)
    m = data.find(b"MODLOADER64")
    if m >= 0 and m + 16 <= len(data):
        pm.form_byte = data[m + 0x0B]
    hdr = skel.find_skeleton_header(data)
    if hdr is not None:
        try:
            pm.skeleton = skel.parse(data, hdr)
        except Exception:
            pm.skeleton = None
    pm.manifest = parse_manifest(data)
    return pm
