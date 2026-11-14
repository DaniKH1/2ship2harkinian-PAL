"""
Convert a ModLoader64 player-model zobj into SoH/2Ship `.o2r` resources.

Approach (matches how fast64-authored custom Link models load): for each vanilla
limb we regenerate a clean binary DisplayList that binds a re-encoded RGBA16
texture, loads its vertices (via the OTR CRC64-hash vertex opcode) and draws the
triangles, replaying the source limb's render state (combiner/othermode/geometry
mode) so shading matches. All emitted under the vanilla limb-DL symbol paths so
they shadow the base game's geometry.
"""
from __future__ import annotations
import struct
import numpy as np
from typing import Dict, List, Tuple

from . import gbi, model as _model, aliases, o2r as _o2r, n64texture
from .render import decode_tile

# Replacement assets live under SoH/2Ship's alt-asset tree: with Alt Assets ON,
# a request for "objects/X" is resolved to "alt/objects/X" (ResourceManagerHelpers
# prepends the prefix). Storing our overrides here is the mechanism the engine
# expects for replacing vanilla player/equipment resources, and every internal
# cross-reference (marker/vtx/tex/DL hash, skeleton limb paths) is computed from
# these same alt paths so the archive is fully self-consistent.
ALT = "alt/objects/"


# ------------------------------------------------------------------ helpers
def _rgba8_to_rgba16(rgba: np.ndarray) -> bytes:
    r = (rgba[..., 0].astype(np.uint16) >> 3) & 0x1F
    g = (rgba[..., 1].astype(np.uint16) >> 3) & 0x1F
    b = (rgba[..., 2].astype(np.uint16) >> 3) & 0x1F
    a = (rgba[..., 3] >= 128).astype(np.uint16)
    px = (r << 11) | (g << 6) | (b << 1) | a
    return px.astype(">u2").tobytes()


def _emit_rgba16_load(dl: "_o2r.DLBuilder", tex_path: str, w: int, h: int, cms: int, cmt: int,
                      texcmd: Tuple[int, int] = (0, 0)):
    masks = (w - 1).bit_length() if w > 1 else 0
    maskt = (h - 1).bit_length() if h > 1 else 0
    line = ((w * 2) + 7) >> 3
    lb_line = line if line else 1
    dxt = ((1 << 11) + lb_line - 1) // lb_line
    # G_TEXTURE: enable texturing on render tile 0. Preserve the SOURCE S/T scale
    # (texcmd w1) when present - texture-gen (reflective/env-mapped) materials rely
    # on a non-unity scale to map the reflection, so forcing 1.0 warps their shine.
    # Fall back to 1.0 (0xFFFF) when the source set no scale, so UVs don't smear.
    tscale = texcmd[1] if texcmd != (0, 0) else 0xFFFFFFFF
    dl.raw((0xD7 << 24) | (0 << 11) | (0 << 8) | (1 << 1), tscale)
    dl.settimg_hash(0, 2, w, tex_path)                       # SETTIMG_OTR_HASH (RGBA16)
    dl.raw((0xF5 << 24) | (0 << 21) | (2 << 19), (7 << 24) | (cmt << 18) | (maskt << 14) | (cms << 8) | (masks << 4))
    dl.raw(0xE6 << 24, 0)                                    # RDPLoadSync
    dl.raw(0xF3 << 24, (7 << 24) | ((w * h - 1) << 12) | dxt)  # LoadBlock (LOADTILE 7)
    dl.raw(0xE7 << 24, 0)                                    # RDPPipeSync
    dl.raw((0xF5 << 24) | (0 << 21) | (2 << 19) | (line << 9),
           (0 << 24) | (cmt << 18) | (maskt << 14) | (cms << 8) | (masks << 4))  # render tile 0
    dl.raw((0xF2 << 24), (0 << 24) | (((w - 1) << 2) << 12) | ((h - 1) << 2))    # SetTileSize


# a plain textured-decal combiner as a safe default when the source did not set one
_DEFAULT_COMBINE = (0xFCFFFFFF, 0xFFFCF279)   # gsDPSetCombineMode(G_CC_DECALRGBA, G_CC_DECALRGBA)


def _emit_tex_load(dl: "_o2r.DLBuilder", tex_path: str, w: int, h: int, ts: "gbi.TileState"):
    """Bind a decoded RGBA16 texture, honouring the source render tile's wrap
    (cms/cmt), mask and UV-shift so texel addressing matches the game exactly.
    Does NOT emit G_TEXTURE - the transcoder passes the source's own through."""
    masks = ts.masks or ((w - 1).bit_length() if w > 1 else 0)
    maskt = ts.maskt or ((h - 1).bit_length() if h > 1 else 0)
    line = ((w * 2) + 7) >> 3
    lb_line = line if line else 1
    dxt = ((1 << 11) + lb_line - 1) // lb_line
    dl.settimg_hash(0, 2, w, tex_path)                       # SETTIMG_OTR_HASH (RGBA16)
    dl.raw((0xF5 << 24) | (2 << 19), (7 << 24) | (ts.cmt << 18) | (maskt << 14) | (ts.cms << 8) | (masks << 4))
    dl.raw(0xE6 << 24, 0)                                    # RDPLoadSync
    dl.raw(0xF3 << 24, (7 << 24) | ((w * h - 1) << 12) | dxt)  # LoadBlock (LOADTILE 7)
    dl.raw(0xE7 << 24, 0)                                    # RDPPipeSync
    dl.raw((0xF5 << 24) | (2 << 19) | (line << 9),          # render tile 0
           (0 << 24) | (ts.cmt << 18) | (maskt << 14) | ((ts.shift_t & 0xF) << 10)
           | (ts.cms << 8) | (masks << 4) | (ts.shift_s & 0xF))
    dl.raw((0xF2 << 24), (((w - 1) << 2) << 12) | ((h - 1) << 2))  # SetTileSize


# opcodes passed through the transcoder verbatim (render state that must reach SoH
# unchanged: flex matrices, combiner, colours, geometry/other modes, texture scale)
_PASSTHRU = {0xDA, 0xFC, 0xFA, 0xFB, 0xD9, 0xE3, 0xE2, 0xDB, 0xDC, 0xE1, 0xF1, 0xD7, 0x03}
# opcodes whose w1 is an address resolved through SoH's Interpreter::SegAddr, which
# only treats it as SEGMENTED when (w1 & 1) is set (libultraship fast/interpreter.cpp).
# Raw N64 segmented addresses are even (e.g. G_MTX 0x0D000440); pass them through as-is
# and SoH derefs the raw 0x0D000000 base -> access violation. ZAPD sets that bit when it
# extracts vanilla DLs; we must too. Applies to G_MTX(0xDA), G_DL(0xDE), G_MOVEMEM(0xDC).
_SEGADDR_OPS = {0xDA, 0xDE, 0xDC}


def _seg1(w1: int) -> int:
    """Set the SoH 'this is a segmented address' low bit on a real seg (1..15) w1."""
    return (w1 | 1) if 1 <= ((w1 >> 24) & 0xFF) <= 0x0F else w1
# texture-image setup we intercept (re-emitted as a decoded RGBA16 load instead)
_TEXOPS = {0xFD, 0xF5, 0xF2, 0xF3, 0xF4, 0xF0, 0xE6, 0xE7, 0xE8}


def _new_tracker():
    """Persistent RDP state carried across a model's limbs in draw order, so a
    limb that draws without re-setting texture/combiner/mode inherits the previous
    limb's state - and each emitted limb DL re-states it up front to stay
    self-contained (robust regardless of SoH's draw order)."""
    return {"ts": gbi.TileState(), "pend": 0, "pfmt": 0, "psiz": 0,
            "geom": None, "oth_h": None, "oth_l": None, "combine": None,
            "prim": None, "env": None, "tex": None, "bound": None,
            "face": (None, None)}     # baked vanilla (eye, mouth) for seg-8/9 refs


def _transcode_limb(o: "_o2r.O2R", folder: str, dl_names: List[str],
                    seg: Dict[int, bytes], start_addr: int, tag: str, tr: dict) -> int:
    """Faithfully transcode a source F3DEX2 limb DL into an o2r ODLT.

    Unlike the old regenerate path this PRESERVES the flex skeleton: G_MTX loads
    from segment 0x0D (which blend joint vertices under a neighbouring limb's
    matrix) are passed through verbatim, exactly as vanilla SoH limb DLs do -
    SkelAnime_DrawFlex sets up segment 0x0D identically at runtime. Vertices,
    triangles and render state are emitted 1:1; only seg-6 vertex/texture refs are
    rewritten to o2r resources. `tr` carries RDP state across limbs (drawn in
    order): inherited combiner/mode/texture are re-emitted at the top of each limb
    DL so every limb is self-contained. Returns the triangle count emitted.
    """
    ts = tr["ts"]
    primary = f"{folder}/{dl_names[0]}"
    dl = _o2r.DLBuilder()
    dl.marker(primary)
    # Re-state inherited render state so this limb DL stands alone. NOTE: do NOT
    # re-emit the geometry mode here - G_GEOMETRYMODE is a CLEAR-then-SET command,
    # and the base bits (G_ZBUFFER, G_SHADE, G_CULL_BACK) are set by SoH's player
    # draw BEFORE the skeleton, then tweaked incrementally by the limb DLs (which we
    # pass through). A full clear-and-set here wipes that base (e.g. ZBUFFER -> no
    # depth test -> back faces overdraw front -> see-through model). The mode is RSP
    # state that carries over, exactly like vanilla. combine/prim/env/othermode/
    # texture are plain SET (or masked) commands, so re-emitting them is safe.
    for k in ("oth_h", "oth_l", "combine", "tex", "prim", "env"):
        if tr[k] is not None:
            dl.raw(*tr[k])
    tr["bound"] = None                     # force this limb to re-bind its texture
    st = {"dirty": bool(ts.timg_addr), "texn": 0, "vtxn": 0, "tris": 0}

    def _read(addr, length):
        s, off = (addr >> 24) & 0xFF, addr & 0xFFFFFF
        buf = seg.get(s)
        return buf[off:off + length] if buf else b""

    def flush_tex():
        if not st["dirty"]:
            return
        st["dirty"] = False
        if not ts.timg_addr:
            return
        key = ts.key()
        if key == tr["bound"]:
            return
        tr["bound"] = key
        st["texn"] += 1
        path = f"{folder}/{tag}_tex{st['texn']}"
        tex = decode_tile(seg, ts)
        if tex is not None:
            o.add(path, _o2r.build_texture_rgba16(_rgba8_to_rgba16(tex.rgba), tex.w, tex.h))
            _emit_tex_load(dl, path, tex.w, tex.h, ts)
        else:
            fill = ts.env if ts.env != (255, 255, 255, 255) else ts.prim
            px = np.array([[list(fill)]], np.uint8)
            o.add(path, _o2r.build_texture_rgba16(_rgba8_to_rgba16(px), 1, 1))
            _emit_tex_load(dl, path, 1, 1, ts)

    def emit_vtx(w0, w1):
        n = (w0 >> 12) & 0xFF
        dst = ((w0 >> 1) & 0x7F) - n
        raw = _read(w1, n * 16)
        verts = []
        for i in range(n):
            b = i * 16
            if b + 16 > len(raw):
                break
            x, y, z, flag, s, t, r, g, bb, a = struct.unpack_from(">hhhHhhBBBB", raw, b)
            verts.append((x, y, z, flag, s, t, r, g, bb, a))
        if not verts:
            return
        st["vtxn"] += 1
        vp = f"{folder}/{tag}_v{st['vtxn']}"
        o.add(vp, _o2r.build_vertex_array(verts))
        dl.vtx_hash(len(verts), dst, 0, vp)

    def track_texop(op, w0, w1):
        if op == 0xFD:                                   # SETTIMG
            tr["pend"] = w1; tr["pfmt"] = (w0 >> 21) & 0x7; tr["psiz"] = (w0 >> 19) & 0x3
        elif op in (0xF3, 0xF4):                         # LOADBLOCK / LOADTILE
            ts.timg_addr = tr["pend"]; ts.fmt = tr["pfmt"]; ts.siz = tr["psiz"]; st["dirty"] = True
        elif op == 0xF0:                                 # LOADTLUT
            ts.tlut_addr = tr["pend"]; ts.tlut_count = ((w1 >> 14) & 0x3FF) + 1; st["dirty"] = True
        elif op == 0xF5:                                 # SETTILE (render tile 0)
            if ((w1 >> 24) & 0x7) == 0:
                ts.fmt = (w0 >> 21) & 0x7; ts.siz = (w0 >> 19) & 0x3; ts.line = (w0 >> 9) & 0x1FF
                ts.cmt = (w1 >> 18) & 0x3; ts.cms = (w1 >> 8) & 0x3
                ts.maskt = (w1 >> 14) & 0xF; ts.masks = (w1 >> 4) & 0xF
                ts.shift_t = (w1 >> 10) & 0xF; ts.shift_s = w1 & 0xF; st["dirty"] = True
        elif op == 0xF2:                                 # SETTILESIZE (render tile 0)
            if ((w1 >> 24) & 0x7) == 0:
                uls = (w0 >> 12) & 0xFFF; ult = w0 & 0xFFF
                lrs = (w1 >> 12) & 0xFFF; lrt = w1 & 0xFFF
                ts.uls, ts.ult = uls, ult
                ts.width = ((lrs - uls) >> 2) + 1
                ts.height = ((lrt - ult) >> 2) + 1
                mw, mh = ts.masks, ts.maskt
                if 1 <= mw <= 8 and (1 << mw) < ts.width and not (ts.cms & 0x2):
                    ts.width = 1 << mw
                if 1 <= mh <= 8 and (1 << mh) < ts.height and not (ts.cmt & 0x2):
                    ts.height = 1 << mh
                st["dirty"] = True

    def walk(addr, depth, stack):
        # `stack` guards only against a DL re-entering itself (a cycle); shared
        # sub-DLs are legitimately CALLED many times per limb, so we must NOT
        # permanently blacklist a visited address - only ancestors on the path.
        if depth > 40 or addr in stack:
            return
        stack = stack | {addr}
        s, p = (addr >> 24) & 0xFF, addr & 0xFFFFFF
        buf = seg.get(s)
        if buf is None:
            return
        while p + 8 <= len(buf):
            w0, w1 = struct.unpack_from(">II", buf, p)
            op = (w0 >> 24) & 0xFF
            p += 8
            if op == 0xDF:                               # ENDDL
                return
            if op in (0x05, 0x06, 0x07):                 # TRI1 / TRI2 / QUAD
                flush_tex(); dl.raw(w0, w1); st["tris"] += 2 if op == 0x06 else 1
            elif op == 0x01:                             # G_VTX
                emit_vtx(w0, w1)
            elif op in _TEXOPS:                          # texture setup (intercepted)
                track_texop(op, w0, w1)
            elif op == 0xFA:                             # PRIM: track + persist + emit
                ts.prim = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
                tr["prim"] = (w0, w1); dl.raw(w0, w1)
            elif op == 0xFB:                             # ENV
                ts.env = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
                tr["env"] = (w0, w1); dl.raw(w0, w1)
            elif op == 0xD9:                             # GEOMETRYMODE (accumulate effective)
                base = tr["geom"] if tr["geom"] is not None else 0
                tr["geom"] = (base & (w0 & 0x00FFFFFF)) | w1
                dl.raw(w0, w1)
            elif op == 0xFC:                             # COMBINE
                tr["combine"] = (w0, w1); dl.raw(w0, w1)
            elif op == 0xE3:                             # OTHERMODE_H
                tr["oth_h"] = (w0, w1); dl.raw(w0, w1)
            elif op == 0xE2:                             # OTHERMODE_L
                tr["oth_l"] = (w0, w1); dl.raw(w0, w1)
            elif op == 0xD7:                             # TEXTURE (scale/enable)
                tr["tex"] = (w0, w1); dl.raw(w0, w1)
            elif op == 0xDE:                             # G_DL
                tgt_seg = (w1 >> 24) & 0xFF
                push = ((w0 >> 16) & 0xFF) == 0
                if tgt_seg == s:                         # same object -> inline transcode
                    walk(w1, depth + 1, stack)
                    if not push:
                        return
                else:                                    # runtime dispatch (seg 0x0C etc.)
                    dl.raw(w0, _seg1(w1))
            elif op in _PASSTHRU:                        # flex MTX, moveword/mem, rdphalf
                dl.raw(w0, _seg1(w1) if op in _SEGADDR_OPS else w1)
        return

    walk(start_addr, 0, frozenset())
    dl.end()
    res = dl.to_resource()
    for name in dl_names:
        o.add(f"{folder}/{name}", res)
    return st["tris"]


def _build_limb(o: "_o2r.O2R", folder: str, dl_names: List[str],
                triangles, seg: Dict[int, bytes], tag: str):
    """Emit one ODLT (+ its OTEX/OARR) and register it under every dl_names path.
    Sub-resources (OTEX/OARR) are namespaced by `tag` (which MUST be unique per
    item), not by the DL name: two different items can legitimately target the
    same SoH slot (e.g. two Kokiri Swords -> gCustomKokiriSwordDL), and if their
    vtx/tex resources shared the slot name they would collide in the archive when
    loaded together -> a DL binding another item's shorter vertex array -> OOB read
    -> crash."""
    # group triangles by texture/tile
    groups: Dict[tuple, List[gbi.Triangle]] = {}
    order: List[tuple] = []
    for tri in triangles:
        k = tri.tile.key()
        if k not in groups:
            groups[k] = []
            order.append(k)
        groups[k].append(tri)

    primary_path = f"{folder}/{dl_names[0]}"
    dl = _o2r.DLBuilder()
    dl.marker(primary_path)

    gi = 0
    for k in order:
        tris = groups[k]
        ts = tris[0].tile
        # ---- texture ----
        tex = decode_tile(seg, ts)
        gi += 1
        if tex is not None:
            w, h = tex.w, tex.h
            tex_path = f"{folder}/{tag}_tex_{gi}"
            o.add(tex_path, _o2r.build_texture_rgba16(_rgba8_to_rgba16(tex.rgba), w, h))
            # ---- render state (replay source) ----
            for gm in ts.geometrymode:
                dl.raw(*gm)
            if ts.othermode_h != (0, 0):
                dl.raw(*ts.othermode_h)
            if ts.othermode_l != (0, 0):
                dl.raw(*ts.othermode_l)
            dl.raw(*(ts.combine if ts.combine != (0, 0) else _DEFAULT_COMBINE))
            pr, pg, pb, pa = ts.prim
            dl.raw(0xFA000000, (pr << 24) | (pg << 16) | (pb << 8) | pa)
            er, eg, eb, ea = ts.env
            dl.raw(0xFB000000, (er << 24) | (eg << 16) | (eb << 8) | ea)
            _emit_rgba16_load(dl, tex_path, w, h, ts.cms, ts.cmt, ts.texture)
        else:
            # unresolved texture -> flat 1x1 fill from prim/env
            fill = ts.env if ts.env != (255, 255, 255, 255) else ts.prim
            px = np.array([[list(fill)]], np.uint8)
            tex_path = f"{folder}/{tag}_tex_{gi}"
            o.add(tex_path, _o2r.build_texture_rgba16(_rgba8_to_rgba16(px), 1, 1))
            dl.raw(*_DEFAULT_COMBINE)
            _emit_rgba16_load(dl, tex_path, 1, 1, 0, 0)

        # ---- geometry: greedy <=32-vertex batches ----
        _emit_batches(o, dl, folder, tag, gi, tris)

    dl.end()
    res = dl.to_resource()
    for name in dl_names:
        o.add(f"{folder}/{name}", res)


def _emit_batches(o, dl, folder, base, gi, tris):
    batch: List = []                # unique vertices in current batch
    index: Dict[int, int] = {}      # id(Vertex) -> slot
    pending: List[Tuple[int, int, int]] = []
    bno = 0

    def flush():
        nonlocal batch, index, pending, bno
        if not batch:
            return
        bno += 1
        vpath = f"{folder}/{base}_g{gi}_vtx{bno}"
        o.add(vpath, _o2r.build_vertex_array(
            [(v.x, v.y, v.z, 0, v.s, v.t, v.r, v.g, v.b, v.a) for v in batch]))
        dl.vtx_hash(len(batch), 0, 0, vpath)
        for (a, b, c) in pending:
            dl.tri1(a, b, c)
        batch = []
        index = {}
        pending = []

    for tri in tris:
        vs = tri.v
        need = [v for v in vs if id(v) not in index]
        if len(batch) + len(need) > 32:
            flush()
            need = list(vs)
        for v in need:
            if id(v) not in index:
                index[id(v)] = len(batch)
                batch.append(v)
        pending.append((index[id(vs[0])], index[id(vs[1])], index[id(vs[2])]))
    flush()


# ------------------------------------------------------------------ public API
def convert_model(pm: "_model.PlayerModel", game: str, form_kind: str,
                  o: "_o2r.O2R", ns: str = "") -> dict:
    """Convert one player model into resources added to `o`. Returns a stats dict.

    The limb DLs and skeleton keep their vanilla symbol names (SoH override
    targets), but the OTEX/OARR/OSLB SUB-resources are namespaced by `ns` (a token
    unique to this model). Two models sharing an object folder - e.g. an OoT-child
    and an MM-child both under object_link_child, or the 10 Ganondorf variants -
    would otherwise emit identically-named mhLimb##/_tex/_v resources that collide
    if both mods are loaded together, binding another model's shorter vertex array
    (OOB -> crash). ns defaults to '' (single model; still self-consistent)."""
    tgt = aliases.target_for(game, pm.form_byte, form_kind)
    if tgt is None or pm.skeleton is None:
        return {"ok": False, "reason": "unsupported form or no skeleton"}
    seg = pm.seg_map()
    stats = {"ok": True, "object": tgt.object_folder, "limbs": 0,
             "resources_before": len(o.files)}
    # Walk limbs in SkelAnime draw order so RDP state inherited across limbs (a
    # limb with no SETTIMG uses the previous limb's texture) is tracked correctly.
    sk = pm.skeleton
    order = sk.slot_to_limb or [l.index for l in sk.limbs if l.dlist]
    seen = set()
    order = [i for i in order if not (i in seen or seen.add(i))]
    for i in [l.index for l in sk.limbs if l.dlist]:
        if i not in seen:
            order.append(i); seen.add(i)
    tr = _new_tracker()
    for limb_idx in order:
        lb = sk.limbs[limb_idx]
        if not lb.dlist or lb.index not in tgt.limb_dls:
            continue
        # Faithful transcode: preserve flex matrices, exact verts/tris/state so the
        # limb rigs and textures in SoH exactly as in ModLoader (not regenerated).
        tag = f"{ns}_limb{lb.index}" if ns else f"limb{lb.index}"
        ntris = _transcode_limb(o, ALT + tgt.object_folder, tgt.limb_dls[lb.index],
                                seg, lb.dlist, tag, tr)
        if ntris:
            stats["limbs"] += 1

    # The skeleton's hand limb DLs are only the OPEN hands. When Link holds an item
    # SoH draws a CLOSED-fist hand DL instead (the custom-equip patch jumps to it),
    # which we don't override from the skeleton -> vanilla Link hands show while
    # holding. Emit the model's own fist DLs over those names too.
    if game == "OOT":
        _emit_hand_dls(o, tgt, pm, form_kind, ns, seg, stats)

    # Emit the skeleton + limb structs. SoH only takes the custom-model draw path
    # when ResourceGetIsCustomByName(gLink*Skel) is true (z_player_lib.c), so the
    # skeleton MUST be present and IsCustom - overriding limb DLs alone is ignored.
    _build_skeleton(o, tgt, pm, ns)
    stats["resources"] = len(o.files) - stats["resources_before"]
    return stats


# In a zzplayas model's DL pointer table (@0x5000) the CLOSED-fist DL is stored in
# the entry immediately AFTER the limb's OPEN-hand entry - true for BOTH the older
# (fixed-offset, PlayAsManifest0) layout AND the newer contiguous one, so we find
# the open hand by its skeleton dlist and take the next table entry. Each entry is a
# G_DL branch: `de010000`<ptr>. Emitted over the closed-hand symbols SoH draws while
# holding an item (names per customequipment.cpp leftHandClosed/rightHandClosed).
_HAND_DLS = {   # L_HAND=limb 15, R_HAND=limb 18
    "adult": {15: ["gLinkAdultLeftHandClosedNearDL", "gLinkAdultLeftHandClosedFarDL"],
              18: ["gLinkAdultRightHandClosedNearDL", "gLinkAdultRightHandClosedFarDL"]},
    "child": {15: ["gLinkChildLeftFistNearDL", "gLinkChildLeftFistFarDL"],
              18: ["gLinkChildRightHandClosedNearDL", "gLinkChildRightHandClosedFarDL"]},
}


def _fist_after_open_hand(d: bytes, open_addr: int):
    """The closed fist = the pointer-table G_DL branch right after the open hand's."""
    tgt = open_addr & 0xFFFFFF
    i = 0x5000
    while i + 16 <= len(d):
        if struct.unpack_from(">I", d, i)[0] == 0xDE010000:
            p = struct.unpack_from(">I", d, i + 4)[0]
            if (p >> 24) == 0x06 and (p & 0xFFFFFF) == tgt \
               and struct.unpack_from(">I", d, i + 8)[0] == 0xDE010000:
                nxt = struct.unpack_from(">I", d, i + 12)[0]
                if (nxt >> 24) == 0x06 and (nxt & 0xFFFFFF) < len(d):
                    return nxt
        i += 4
    return None


def _emit_hand_dls(o, tgt, pm, form_kind, ns, seg, stats):
    hand_map = _HAND_DLS.get(form_kind)
    if not hand_map:
        return
    folder = ALT + tgt.object_folder
    for limb_idx, dl_names in hand_map.items():
        if limb_idx >= len(pm.skeleton.limbs):
            continue
        lb = pm.skeleton.limbs[limb_idx]
        if not lb.dlist:
            continue
        fist = _fist_after_open_hand(pm.data, lb.dlist)     # closed hand for held items
        if fist is None:
            continue
        tag = f"{ns}_hand{limb_idx}" if ns else f"hand{limb_idx}"
        if _transcode_limb(o, folder, dl_names, seg, fist, tag, _new_tracker()):
            stats["hands"] = stats.get("hands", 0) + 1


def _build_skeleton(o: "_o2r.O2R", tgt, pm, ns: str = ""):
    folder = ALT + tgt.object_folder
    limb_type = _o2r.LIMB_LOD if tgt.lod else _o2r.LIMB_STANDARD
    pfx = f"{ns}_" if ns else ""            # namespace limb structs per model
    limb_paths = []
    dlist_count = 0
    for lb in pm.skeleton.limbs:
        limb_res = f"{folder}/{pfx}mhLimb{lb.index:02d}"
        limb_paths.append(limb_res)
        near = far = ""
        names = tgt.limb_dls.get(lb.index)
        if names and lb.dlist:
            near = f"{folder}/{names[0]}"
            far = f"{folder}/{names[1]}" if len(names) > 1 else ""
            dlist_count += 1
        o.add(limb_res, _o2r.build_limb(
            (lb.x, lb.y, lb.z), lb.child, lb.sibling, near, far, limb_type))
    # dListCount sizes the runtime flex-matrix array (Mtx[dListCount], seg 0x0D).
    # SoH's player draw fills it via SkelAnime + Player_OverrideLimbDrawGameplay,
    # and the override ADDS dLists (sheath, held items) at limbs our skeleton may
    # not have one on - each advancing the matrix pointer. Sizing to our own
    # dlist-limb count (as vanilla does for its exact skeleton) underflows when a
    # model is missing a limb DL the override still draws -> the fill runs past the
    # array -> access violation in the G_MTX handler. Each limb contributes at most
    # one matrix, so sizing to limbCount is always large enough and never overflows.
    o.add(f"{folder}/{tgt.skel_name}",
          _o2r.build_skeleton(limb_paths, limb_type=limb_type,
                              skel_type=_o2r.SKEL_FLEX,
                              dlist_count=max(dlist_count, len(pm.skeleton.limbs))))


# ------------------------------------------------------------------ CLI wiring
def register(sub):
    p = sub.add_parser("convert", help="convert a ModLoader64 pak to a .o2r")
    p.add_argument("pak")
    p.add_argument("-o", "--out", help="output .o2r path")
    p.add_argument("--render", action="store_true",
                   help="also write before/after comparison PNGs")
    p.set_defaults(func=cmd_convert)


def cmd_convert(args):
    import os
    from . import pak, manifest
    files = pak.load_map(args.pak)
    man = manifest.from_pak_files(files)
    if man is None:
        print("no package.json; cannot classify pak")
        return 1
    base = os.path.splitext(os.path.basename(args.pak))[0]
    outdir = os.path.dirname(os.path.abspath(args.out)) if args.out else os.getcwd()
    print(f"Converting {base}: kind={man.kind} games={man.games}")

    produced = []
    for game in man.games:
        for form_kind in ("adult", "child"):
            for mref in man.models_for(game, form_kind):
                zdata = files.get(mref.file) or _find(files, mref.file)
                if not zdata:
                    print(f"  [skip] {game} {form_kind}: {mref.file} not in pak")
                    continue
                pm = _model.load(zdata)
                o = _o2r.O2R()
                _ns = "".join(c if c.isalnum() else "_"
                              for c in f"{mref.file.rsplit('.',1)[0]}_{game}_{form_kind}")
                stats = convert_model(pm, game, form_kind, o, ns=_ns)
                if not stats.get("ok"):
                    print(f"  [skip] {game} {form_kind} {mref.file}: {stats.get('reason')}")
                    continue
                out = args.out or os.path.join(
                    outdir, f"{base}_{game}_{form_kind}.o2r")
                if len(produced):
                    root, ext = os.path.splitext(out)
                    out = f"{root}_{game}_{form_kind}{ext}"
                o.write(out)
                print(f"  [ok] {game} {form_kind}: {stats['limbs']} limbs, "
                      f"{stats['resources']} resources -> {os.path.basename(out)}")
                produced.append((pm, out))
                if args.render:
                    _render_pair(pm, out, os.path.splitext(out)[0], game, form_kind)
    if not produced:
        print("  nothing converted (no supported player models)")
        return 1
    return 0


def _find(files, name):
    b = name.rsplit("/", 1)[-1]
    for k, v in files.items():
        if k.rsplit("/", 1)[-1] == b:
            return v
    return None


def _render_pair(pm, o2r_path, out_base, game, form_kind):
    """before = source zobj; after = geometry read back out of the .o2r."""
    from . import render
    before = render.render(pm.limb_triangles(), pm.seg_map(), size=384, bg=(28, 28, 36, 255))
    before.save(out_base + "_before.png")
    # map each emitted limb-DL resource name -> that limb (for full affine posing)
    tgt = aliases.target_for(game, pm.form_byte, form_kind)
    limb_by_name = {}
    if tgt and pm.skeleton:
        wl = {lb.index: lb for lb in pm.skeleton.limbs}
        for idx, names in tgt.limb_dls.items():
            for nm in names:
                if idx in wl:
                    limb_by_name[nm] = wl[idx]
    after_tris = _readback_triangles(o2r_path, limb_by_name, pm.skeleton)
    after = render.render(after_tris, {6: b""}, size=384, bg=(28, 28, 36, 255))
    after.save(out_base + "_after.png")
    # side by side
    from PIL import Image
    combo = Image.new("RGBA", (before.width * 2 + 12, before.height), (18, 18, 24, 255))
    combo.paste(before, (0, 0))
    combo.paste(after, (before.width + 12, 0))
    combo.save(out_base + "_compare.png")
    print(f"       wrote {out_base}_compare.png")


_FACE_CACHE = {}


def _oot_o2r_path():
    """Locate a Ship of Harkinian `oot.o2r` (used only to preview vanilla face
    textures). Set SOH_OOT_O2R / MH_OOT_O2R to point at your own install; otherwise
    we probe the working directory. Entirely optional - conversion works without it."""
    import glob
    import os
    for var in ("SOH_OOT_O2R", "MH_OOT_O2R"):
        p = os.environ.get(var)
        if p and os.path.exists(p):
            return p
    for pat in ("oot.o2r", os.path.join("soh", "oot.o2r"), os.path.join("*", "oot.o2r")):
        hits = glob.glob(os.path.join(os.getcwd(), pat))
        if hits:
            return hits[0]
    return None


def _face_textures(adult: bool):
    """Vanilla eye (seg 8) + mouth (seg 9) textures SoH's face system binds at
    runtime, decoded from oot.o2r for a representative preview. Cached."""
    key = bool(adult)
    if key in _FACE_CACHE:
        return _FACE_CACHE[key]
    from .o2r import read_texture
    from .o2r_model import _decode_tex
    import zipfile
    res = (None, None)
    path = _oot_o2r_path()
    if path:
        try:
            z = zipfile.ZipFile(path)
            names = set(z.namelist())
            folder = "objects/object_link_boy" if adult else "objects/object_link_child"
            pre = "gLinkAdult" if adult else "gLinkChild"
            tlut_name = f"{folder}/{pre}HeadTLUT" if adult else f"{folder}/gLinkChildSkinTLUT"
            tlut = read_texture(z.read(tlut_name))[3] if tlut_name in names else b""

            def dec(texname):
                p = f"{folder}/{texname}"
                if p not in names:
                    return None
                tt, w, h, body = read_texture(z.read(p))
                return _decode_tex(tt, w, h, body, tlut)
            res = (dec(pre + "EyesOpenTex"), dec(pre + "Mouth1Tex"))
        except Exception:
            res = (None, None)
    _FACE_CACHE[key] = res
    return res


def _readback_triangles(o2r_path, limb_by_name=None, sk=None):
    """Parse our own .o2r back into posed triangles for the 'after' render. When a
    skeleton is supplied, flex (seg-0x0D) vertices are posed under their blend
    limb, exactly like the in-game/source render - so 'after' matches 'before'."""
    import zipfile
    limb_by_name = limb_by_name or {}
    tris_out = []
    with zipfile.ZipFile(o2r_path) as z:
        names = set(z.namelist())
        by_hash = {_o2r.crc64(n): n for n in names}
        face = _face_textures(any("object_link_boy" in n for n in names))
        import struct as _st
        for n in names:
            base = n.rsplit("/", 1)[-1]
            if "_tex" in n or "_v" in base:
                continue
            # Near and Far LOD DLs hold the same limb (only one draws in-game); the
            # readback would overlay both, so render only the Near LOD.
            if base.endswith("FarDL"):
                continue
            # The closed-fist hand DLs (custom-equip held-item hands) are NOT part of
            # the skeleton draw - they'd render un-posed at the origin, a blob between
            # the feet. Skip them for the preview.
            if "HandClosed" in base or "Fist" in base:
                continue
            data = z.read(n)
            if len(data) < 0x48 or _st.unpack_from("<I", data, 0x04)[0] != _o2r.T_ODLT:
                continue
            lb = limb_by_name.get(base)
            tris_out.extend(_decode_dl(data, z, by_hash, lb, sk, face))
    return tris_out


def _decode_dl(data, z, by_hash, lb=None, sk=None, face=(None, None)):
    """Re-parse an emitted ODLT into faithfully-shaded posed triangles.

    Tracks combiner/prim/env/lighting so shading matches the source, AND the
    seg-0x0D flex matrix (G_MTX) so joint-blended vertices pose under the correct
    neighbouring limb - reproducing the in-game flex rig.
    """
    import struct
    import numpy as np
    from .gbi import Triangle, TileState, Vertex
    from . import n64texture, model as _model
    from .render import Tex
    _EYE = np.eye(3)
    out = []
    p = 0x40
    p = (p + 7) & ~7 if data[p] == 0x04 else p
    p += 8  # ucode word
    cur_vtx: List = [None] * 32
    cur_tex = None
    cur_flex = -1
    ts = TileState(lit=True)

    def eff(v):
        if sk is not None and 0 <= v.flex_slot < len(sk.slot_to_limb):
            return sk.limbs[sk.slot_to_limb[v.flex_slot]]
        return lb

    def emit(a, b, c):
        if not (a < 32 and b < 32 and c < 32):
            return
        va, vb, vc = cur_vtx[a], cur_vtx[b], cur_vtx[c]
        if not (va and vb and vc):
            return
        verts = (va, vb, vc)
        effs = [eff(v) for v in verts]
        tri = Triangle(verts, TileState(**{k: getattr(ts, k) for k in ts.__dataclass_fields__}))
        tri.tile.timg_addr = 1 if cur_tex else 0
        if tri.tile.lit and all(e is not None for e in effs) and _model.back_facing(verts, effs):
            return
        wvs = [(e.apply(v.x, v.y, v.z) if e is not None else (v.x, v.y, v.z))
               for v, e in zip(verts, effs)]
        cols = [_model.PlayerModel._vtx_color(
                    v, tri.tile, (e.M if (e is not None and e.M is not None) else _EYE),
                    _model.KOKIRI_GREEN)
                for v, e in zip(verts, effs)]
        # A texture merely left bound by a previous limb must be ignored when this
        # tile's combiner doesn't sample TEXEL0/1 (prim/shade-only skin) - otherwise
        # e.g. the pale face inherits the last limb's dark texture and goes brown.
        tex_for_tri = cur_tex if ts.combine_uses_tex else None
        out.append((tri, wvs, cols, tex_for_tri))

    while p + 8 <= len(data):
        w0, w1 = struct.unpack_from("<II", data, p)
        op = (w0 >> 24) & 0xFF
        if op in (0x33, 0x32, 0x31, 0x20):     # expanded 128-bit
            hi, lo = struct.unpack_from("<II", data, p + 8)
            h = (hi << 32) | lo
            p += 16
            if op == 0x32:                     # VTX hash
                numv = (w0 >> 12) & 0xFF
                dst = ((w0 >> 1) & 0x7F) - numv
                path = by_hash.get(h)
                if path:
                    for i, rec in enumerate(_o2r.read_vertex_array(z.read(path))):
                        x, y, zz, fl, s, t, c0, c1, c2, c3 = rec
                        if 0 <= dst + i < 32:
                            cur_vtx[dst + i] = Vertex(x, y, zz, s, t, c0, c1, c2, c3, cur_flex)
            elif op == 0x20:                   # SETTIMG hash -> texture
                path = by_hash.get(h)
                cur_tex = None
                if path:
                    _, tw, th, body = _o2r.read_texture(z.read(path))
                    rgba = n64texture.decode(body, 0, 2, tw, th)  # RGBA16
                    cur_tex = Tex(rgba, 0, 0)
                    ts.width, ts.height = tw, th
            continue
        p += 8
        if op == 0xDF:
            break
        if op == 0xFD:                          # raw SETTIMG -> runtime face segment
            seg = (w1 >> 24) & 0xFF
            cur_tex = face[0] if seg == 8 else face[1] if seg == 9 else None
            if cur_tex is not None:
                ts.width, ts.height = cur_tex.w, cur_tex.h
        elif op == 0xDA:                        # G_MTX: seg-0x0D flex matrix switch
            if (w1 >> 24) & 0xFF == 0x0D:
                cur_flex = (w1 & 0xFFFFFF) // 0x40
        elif op == 0xFA:                        # PRIM
            ts.prim = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
        elif op == 0xFB:                        # ENV
            ts.env = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
        elif op == 0xFC:                        # COMBINE
            a0 = (w0 >> 20) & 0xF; c0 = (w0 >> 15) & 0x1F
            b0 = (w1 >> 28) & 0xF; dd0 = (w1 >> 15) & 0x7
            a1 = (w0 >> 5) & 0xF; c1 = w0 & 0x1F
            b1 = (w1 >> 24) & 0xF; dd1 = (w1 >> 6) & 0x7
            ts.combine_uses_prim = 3 in (a0, c0, a1, c1, dd0, dd1)
            ts.combine_uses_env = 5 in (a0, c0, a1, c1, dd0, dd1)
            ts.combine_uses_tex = any(x in (1, 2) for x in (a0, b0, c0, dd0, a1, b1, c1, dd1))
            Aa0 = (w0 >> 12) & 0x7; Ac0 = (w0 >> 9) & 0x7
            Ab0 = (w1 >> 12) & 0x7; Ad0 = (w1 >> 9) & 0x7
            Aa1 = (w1 >> 21) & 0x7; Ac1 = (w1 >> 18) & 0x7
            Ab1 = (w1 >> 3) & 0x7; Ad1 = w1 & 0x7
            asrc = (Aa0, Ab0, Ac0, Ad0, Aa1, Ab1, Ac1, Ad1)
            ts.combine_alpha_uses_tex = any(x in (1, 2) for x in asrc)
            ts.combine_alpha_uses_prim = 3 in asrc
            ts.combine_alpha_uses_env = 5 in asrc
        elif op == 0xE2:                        # SETOTHERMODE_L (render mode)
            ts.othermode_l = (w0, w1)           # w1 bit 0x4000 = FORCE_BL (alpha blend)
        elif op == 0xF5:                        # SETTILE (render tile 0): wrap mode
            if ((w1 >> 24) & 0x7) == 0:
                # Without this the tile's clamp/mirror/wrap and shift default to 0
                # (WRAP) in the readback, so a CLAMP texture whose UVs run past its
                # edges TILES instead of clamping - e.g. the face texture repeats
                # its eyes across the head. Mirrors what _emit_rgba16_load wrote.
                ts.cmt = (w1 >> 18) & 0x3; ts.cms = (w1 >> 8) & 0x3
                ts.maskt = (w1 >> 14) & 0xF; ts.masks = (w1 >> 4) & 0xF
                ts.shift_t = (w1 >> 10) & 0xF; ts.shift_s = w1 & 0xF
        elif op == 0xD9:                        # GEOMETRYMODE (lighting bit)
            if w1 & 0x00020000:
                ts.lit = True
            elif not (w0 & 0x00020000):
                ts.lit = False
        elif op == 0x05:                        # TRI1
            emit(((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2)
        elif op == 0x06:                        # TRI2
            emit(((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2)
            emit(((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2)
        elif op == 0x07:                        # QUAD
            emit(((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2)
            emit(((w0 >> 16) & 0xFF) // 2, (w0 & 0xFF) // 2, (w1 & 0xFF) // 2)
    return out
