"""
Load and render a Link model straight out of a SoH/2Ship `.o2r` archive
(skeleton OSKL -> limbs OSLB -> display lists ODLT), so the base game's vanilla
`object_link_boy` / `object_link_child` can be rendered as a ground-truth
reference alongside converted models. Handles CI textures (+TLUT) and the
segment-0x0D flex skeleton, matching the in-game standing pose.
"""
from __future__ import annotations
import struct
import zipfile
from typing import Dict, List, Optional

import numpy as np

from . import skeleton as skel, n64texture, model as _model
from .o2r import crc64
from .gbi import Triangle, TileState, Vertex
from .render import Tex
from .model import PlayerModel, KOKIRI_GREEN


def _rd_str(d, p):
    n = struct.unpack_from("<I", d, p)[0]
    return d[p + 4:p + 4 + n].decode("latin1"), p + 4 + n


def _parse_oslb(d):
    p = 0x40
    p += 2                          # limbType, skinSegmentType
    _, p = _rd_str(d, p)            # skinDList
    p += 2 + 4                      # skinVtxCnt, skinLimbModifCount
    _, p = _rd_str(d, p)            # skinDList2
    p += 12 + 6                     # legTrans(3f), rot(3u16)
    _, p = _rd_str(d, p)            # childPtr
    _, p = _rd_str(d, p)            # siblingPtr
    near, p = _rd_str(d, p)         # dListPtr
    far, p = _rd_str(d, p)          # dList2Ptr
    tx, ty, tz = struct.unpack_from("<hhh", d, p); p += 6
    ci = d[p]; si = d[p + 1]
    return (tx, ty, tz), ci, si, near.replace("__OTR__", ""), far.replace("__OTR__", "")


def load(o2r_path: str, object_folder: str, skel_name: str) -> "PlayerModel":
    z = zipfile.ZipFile(o2r_path)
    names = set(z.namelist())
    by_hash = {crc64(n): n for n in names}
    base = f"objects/{object_folder}"

    sd = z.read(f"{base}/{skel_name}")
    p = 0x40
    p += 2                                    # skelType, limbType
    limb_count, dlist_count = struct.unpack_from("<II", sd, p); p += 8
    p += 1                                     # limbTableType
    table_count = struct.unpack_from("<I", sd, p)[0]; p += 4
    limb_paths = []
    for _ in range(table_count):
        s, p = _rd_str(sd, p)
        limb_paths.append(s.replace("__OTR__", ""))

    limbs = []
    dls = []
    for i, lp in enumerate(limb_paths):
        (x, y, zz), ci, si, near, far = _parse_oslb(z.read(lp))
        lb = skel.Limb(i, x, y, zz, ci, si, 1 if near else 0)
        limbs.append(lb)
        dls.append(near)

    sk = skel.Skeleton(limbs=limbs, limb_count=len(limbs), dlist_count=dlist_count)
    skel._pose(sk, skel.PLAYER_IDLE_POSE)

    # default face textures the game binds to segments 8 (eyes) / 9 (mouth)
    from .o2r import read_texture
    prefix = "gLinkAdult" if "boy" in object_folder else "gLinkChild"

    def read_opt(name):
        p = f"{base}/{name}"
        return read_texture(z.read(p)) if p in names else None

    face = (read_opt(prefix + "EyesOpenTex"), read_opt(prefix + "Mouth1Tex"))

    # The Master Sword hilt (guard/pommel/crystal) is drawn with a palette the
    # player C code loads before the sword DL; the DL itself only carries the
    # scabbard palette, so those CI8 textures come out blue. Bind the correct
    # gold sword-hilt palette by name for the reference render.
    sword_pal = None
    if "boy" in object_folder:
        sp = f"{base}/object_link_boyTLUT_00CF50"
        if sp in names:
            sword_pal = read_texture(z.read(sp))[3]

    pm = PlayerModel(data=b"")
    pm.skeleton = sk
    pm._o2r = (z, by_hash, dls, face, sword_pal)
    return pm


def limb_triangles(pm, tunic=KOKIRI_GREEN):
    z, by_hash, dls, face, sword_pal = pm._o2r
    sk = pm.skeleton
    eye = np.eye(3)

    def eff(v, lb):
        s = v.flex_slot
        if 0 <= s < len(sk.slot_to_limb):
            return sk.limbs[sk.slot_to_limb[s]]
        return lb

    out = []
    for lb in sk.limbs:
        if not lb.dlist or not dls[lb.index]:
            continue
        for tri in _decode(z.read(dls[lb.index]), z, by_hash, face, sword_pal):
            va, vb, vc = tri.v
            effs = [eff(va, lb), eff(vb, lb), eff(vc, lb)]
            if tri.tile.lit and _model.back_facing(tri.v, effs):
                continue
            wvs = [e.apply(v.x, v.y, v.z) for v, e in zip(tri.v, effs)]
            cols = [PlayerModel._vtx_color(v, tri.tile, e.M if e.M is not None else eye, tunic)
                    for v, e in zip(tri.v, effs)]
            out.append((tri, wvs, cols, getattr(tri, "_tex", None)))
    return out


def _decode(d, z, by_hash, face=(None, None), sword_pal=None) -> List[Triangle]:
    """Decode one vanilla ODLT into Triangles (CI textures + flex slots)."""
    eye_raw, mouth_raw = face
    out = []
    p = 0x48 if d[0x40] == 0x04 else 0x40
    cache: List[Optional[Vertex]] = [None] * 32
    ts = TileState(lit=True)
    cur_flex = -1
    pend_tex = None            # (type, w, h, body) from last SETTIMG
    pend_name = ""
    cur_raw = None             # texture bound by LOADBLOCK (indices, still needs palette)
    cur_name = ""
    cur_tlut = b""             # palette bound by LOADTLUT
    cur_tlut_name = ""
    _tex_cache = {}

    def cur_tex():
        # decode lazily: a CI texture may be LOADBLOCK'd before its palette is
        # LOADTLUT'd, so only decode once both are known (at draw time). Cache by
        # the stable resource NAMES - id()-keyed caching is unsafe because these
        # texture tuples are transient and Python reuses freed object ids.
        if cur_raw is None:
            return None
        key = (cur_name, cur_tlut_name)
        if key not in _tex_cache:
            _tex_cache[key] = _decode_tex(*cur_raw, cur_tlut)
        return _tex_cache[key]

    while p + 8 <= len(d):
        w0, w1 = struct.unpack_from("<II", d, p)
        op = (w0 >> 24) & 0xFF
        if op in (0x33, 0x32, 0x31, 0x20, 0x35, 0x36):   # expanded 128-bit
            hi, lo = struct.unpack_from("<II", d, p + 8)
            h = (hi << 32) | lo
            p += 16
            if op == 0x32:                    # VTX hash
                numv = (w0 >> 12) & 0xFF
                dst = ((w0 >> 1) & 0x7F) - numv
                start = w1 // 16              # byte offset into a shared vtx buffer
                path = by_hash.get(h)
                if path:
                    from .o2r import read_vertex_array
                    arr = read_vertex_array(z.read(path))
                    for i in range(numv):
                        if start + i < len(arr) and 0 <= dst + i < 32:
                            x, y, zz, fl, s, t, c0, c1, c2, c3 = arr[start + i]
                            cache[dst + i] = Vertex(x, y, zz, s, t, c0, c1, c2, c3, cur_flex)
            elif op == 0x20:                  # SETTIMG hash -> pending texture
                path = by_hash.get(h)
                pend_tex = None
                pend_name = path or ""
                if path:
                    from .o2r import read_texture
                    pend_tex = read_texture(z.read(path))   # (type,w,h,body)
            continue
        p += 8
        if op == 0xDF:
            break
        if op == 0xFD:                                    # raw SETTIMG (runtime seg)
            seg = (w1 >> 24) & 0xFF
            pend_tex = eye_raw if seg == 8 else mouth_raw if seg == 9 else None
            pend_name = "__eye__" if seg == 8 else "__mouth__" if seg == 9 else "__seg%d__" % seg
        elif op == 0xDA and (w1 >> 24) & 0xFF == 0x0D:    # flex matrix
            cur_flex = (w1 & 0xFFFFFF) // 0x40
        elif op == 0xF0 and pend_tex:                     # LOADTLUT -> palette
            cur_tlut = pend_tex[3]
            cur_tlut_name = pend_name
        elif op == 0xF3 and pend_tex:                     # LOADBLOCK -> texture
            cur_raw = pend_tex
            cur_name = pend_name
        elif op == 0xF5 and ((w1 >> 24) & 0x7) == 0:      # SETTILE render tile
            ts.cms = (w1 >> 8) & 0x3; ts.cmt = (w1 >> 18) & 0x3
            ts.masks = (w1 >> 4) & 0xF; ts.maskt = (w1 >> 14) & 0xF
            ts.shift_s = w1 & 0xF; ts.shift_t = (w1 >> 10) & 0xF
        elif op == 0xF2 and ((w1 >> 24) & 0x7) == 0:      # SETTILESIZE render tile
            ts.uls = (w0 >> 12) & 0xFFF; ts.ult = w0 & 0xFFF
        elif op == 0xFA:
            ts.prim = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
        elif op == 0xFB:
            ts.env = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
        elif op == 0xFC:
            a0 = (w0 >> 20) & 0xF; c0 = (w0 >> 15) & 0x1F
            a1 = (w0 >> 5) & 0xF; c1 = w0 & 0x1F
            dd0 = (w1 >> 15) & 0x7; dd1 = (w1 >> 6) & 0x7
            ts.combine_uses_prim = 3 in (a0, c0, a1, c1, dd0, dd1)
            ts.combine_uses_env = 5 in (a0, c0, a1, c1, dd0, dd1)
        elif op == 0xD9:
            if w1 & 0x20000:
                ts.lit = True
            elif not (w0 & 0x20000):
                ts.lit = False
        elif op in (0x05, 0x06, 0x07):                    # TRI1/TRI2/QUAD
            idx = [((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2]
            if op != 0x05:
                idx += [((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2]
            groups = [idx[0:3]] + ([idx[3:6]] if len(idx) > 3 else [])
            tex = cur_tex()
            for g in groups:
                a, b, c = g
                if a < 32 and b < 32 and c < 32 and cache[a] and cache[b] and cache[c]:
                    snap = TileState(**{k: getattr(ts, k) for k in ts.__dataclass_fields__})
                    snap.timg_addr = 1 if tex is not None else 0
                    t = Triangle((cache[a], cache[b], cache[c]), snap)
                    t._tex = tex
                    out.append(t)
    return out


def _decode_tex(ttype, w, h, body, tlut) -> Optional[Tex]:
    # SoH TextureType -> (fmt, siz)
    tmap = {1: (0, 3), 2: (0, 2), 3: (2, 0), 4: (2, 1),
            5: (4, 0), 6: (4, 1), 7: (3, 0), 8: (3, 1), 9: (3, 2)}
    if ttype not in tmap or w <= 0 or h <= 0:
        return None
    fmt, siz = tmap[ttype]
    try:
        rgba = n64texture.decode(body, fmt, siz, w, h, tlut)
    except Exception:
        return None
    return Tex(rgba, 0, 0)
