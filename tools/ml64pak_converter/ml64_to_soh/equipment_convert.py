"""
Convert a ModLoader64 equipment zobj into a SoH/2Ship `.o2r` that plugs into
Ship-of-Harkinian's *custom-equipment* system.

How SoH consumes it (soh/Enhancements/customequipment.cpp):
  At runtime SoH looks for a *custom* Gfx resource named `gCustom<Item>DL` under
  `objects/object_custom_equip/`. If one exists (ResourceGetIsCustomByName) and
  Alt-Assets are enabled, it live-patches the vanilla held/sheathed player DLs to
  CALL that custom DL instead of drawing the built-in weapon. The custom DL is
  invoked in the very same hand-matrix space the vanilla weapon used, and the
  ModLoader equipment geometry is authored to sit in exactly that space - so we
  can emit the zobj's mesh verbatim and it lands in Link's hand correctly.

We therefore emit, per item:
  objects/object_custom_equip/gCustom<Item>DL      (+ its _tex_/_vtx OTEX/OARR)
and, when the pak ships one, the inventory icon:
  textures/icon_item_static/<iconSymbol>            (RGBA16 OTEX)

Every resource header carries IsCustom=1 (o2r._header), which is what makes
ResourceGetIsCustomByName fire.
"""
from __future__ import annotations
import os
from typing import Optional

from . import o2r as _o2r
from .convert import _build_limb

CUSTOM_EQUIP_FOLDER = "alt/objects/object_custom_equip"

# ModLoader EQUIPMENTCAT -> SoH custom-equip DL symbol (held / in-hand model).
# Names verified against Shipwright customequipment.cpp ApplyCommonEquipmentPatches.
_CATEGORY_DL = {
    "Master Sword": "gCustomMasterSwordDL",
    "Kokiri Sword": "gCustomKokiriSwordDL",
    "Biggoron Sword": "gCustomLongswordDL",   # Biggoron / Giant's Knife share the longsword slot
    "Hylian Shield": "gCustomHylianShieldDL",
    "Mirror Shield": "gCustomMirrorShieldDL",
    "Deku Shield": "gCustomDekuShieldDL",
    "Bow": "gCustomBowDL",
    "Hookshot": "gCustomHookshotDL",
    "Longshot": "gCustomLongshotDL",
    "Boomerang": "gCustomBoomerangDL",
    "Slingshot": "gCustomSlingshotDL",
    "Megaton Hammer": "gCustomHammerDL",
}
# categories whose in-hand DL differs by age (adult vs child)
_CATEGORY_DL_BY_AGE = {
    "Ocarina of Time": {"adult": "gCustomOcarinaOfTimeAdultDL", "child": "gCustomOcarinaOfTimeDL"},
    "Fairy Ocarina":  {"adult": "gCustomFairyOcarinaAdultDL", "child": "gCustomFairyOcarinaDL"},
}
# The Deku Stick has NO gCustom* custom-equip slot; instead SoH's held stick is the
# real player resource gLinkChildLinkDekuStickDL (object_link_child), so we replace it
# directly + mirror our texture into all three stick texture paths, because SoH's
# runtime OOB-fix patch can repoint the held DL's SETTIMG to gDekuStickOverflowTex
# (mechanism reverse-engineered in WorkFolders/soh_mmdekustick).
_DEKU_STICK_DL = "gLinkChildLinkDekuStickDL"
_DEKU_STICK_FOLDER = "alt/objects/object_link_child"
_DEKU_STICK_TEX_PATHS = (
    "alt/objects/gameplay_keep/gDekuStickTex",
    "alt/objects/gameplay_keep/gDekuStickOverflowTex",
    "alt/objects/object_link_child/gLinkChildDekuStickMMBakedTex",
)


def dl_symbol_for(category: str, age: str) -> Optional[str]:
    """Return the SoH DL symbol an item category maps to (+age), or None if the
    category has no replaceable slot in Ship-of-Harkinian."""
    if category == "Deku Stick":
        return _DEKU_STICK_DL                       # direct player-DL replacement
    if category in _CATEGORY_DL_BY_AGE:
        return _CATEGORY_DL_BY_AGE[category].get(age)
    return _CATEGORY_DL.get(category)


def item_age(item, game: Optional[str] = None) -> str:
    """Best age for the item from its manifest ('adult' unless it only targets
    child)."""
    ages = [age for age in item.ages if game is None or age.startswith(game + ":")]
    if any(a.endswith("adult") for a in ages):
        return "adult"
    if any(a.endswith("child") for a in ages):
        return "child"
    return "adult"


# categories whose custom-equip slot is a SWORD -> also has *InSheathDL / *SheathDL
# variants (held blade+hilt / sword-on-back / scabbard-on-back). The manifest splits
# them into blade/hilt/sheath pieces; we must NOT bake the sheath onto the held sword.
_SWORD_CATS = {"Master Sword", "Kokiri Sword", "Biggoron Sword"}

# Shields also mount on Link's back when not held: SoH patches the on-back player DLs
# to draw a separate gCustom*OnBack DL (customequipment.cpp). ModLoader ships one
# shield mesh (held/arm space) which Z64Online draws at both the arm and the back
# matrix, so we reuse the same geometry for the on-back slot(s). Deku is a child
# shield; Mirror is adult-only; Hylian mounts on both child and adult backs.
_SHIELD_ONBACK = {
    "Hylian Shield": ["gCustomHylianShieldOnBackDL", "gCustomHylianShieldOnChildBackDL"],
    "Mirror Shield": ["gCustomMirrorShieldOnBackDL"],
    "Deku Shield":   ["gCustomDekuShieldOnBackDL"],
}


def convert_equipment_item(item, o: "_o2r.O2R", age: Optional[str] = None,
                            game: Optional[str] = None) -> dict:
    """Emit one equipment item's custom-equip DL(s) (+textures/verts) into `o`.
    Returns a stats dict {ok, reason?, symbol?, tris?}."""
    age = age or item_age(item)
    sym = dl_symbol_for(item.category, age)
    if sym is None:
        return {"ok": False, "reason": f"no SoH custom-equip slot for '{item.category}'"}
    geo = item.geometry()
    if not geo:
        return {"ok": False, "reason": "no geometry"}
    before = len(o.files)
    seg = item.seg_map()
    # Namespace this item's OTEX/OARR by a unique tag (its file stem): two items can
    # target the same gCustom* slot (e.g. two Kokiri Swords), and shared sub-resource
    # names would collide in the archive when both mods load -> OOB read -> crash.
    tag = _sanitize(os.path.splitext(item.file)[0]) or "eq"

    # Deku Stick: replace the held player DL directly (no custom-equip slot) and mirror
    # our texture into every stick texture path the runtime OOB-patch might sample.
    if item.category == "Deku Stick":
        _build_limb(o, _DEKU_STICK_FOLDER, [_DEKU_STICK_DL], geo, seg, tag)
        tex = next((o.files[k] for k in o.files
                    if k.startswith(_DEKU_STICK_FOLDER) and f"{tag}_tex_" in k), None)
        if tex is not None:
            for path in _DEKU_STICK_TEX_PATHS:
                o.add(path, tex)
        return {"ok": True, "symbol": _DEKU_STICK_DL, "age": "child", "tris": len(geo),
                "resources": len(o.files) - before}

    # Swords: split blade/hilt (held) from the scabbard so the sheath mounts on the
    # back instead of riding along with the drawn sword.
    if item.category in _SWORD_CATS:
        pieces = item.pieces(game=game)
        sheath = [t for nm, tris in pieces if "sheath" in nm.lower() for t in tris]
        sword = [t for nm, tris in pieces if "sheath" not in nm.lower() for t in tris]
        if sheath and sword:
            base = sym[:-2]                       # strip trailing "DL"
            _build_limb(o, CUSTOM_EQUIP_FOLDER, [sym], sword, seg, tag)                 # held
            _build_limb(o, CUSTOM_EQUIP_FOLDER, [base + "InSheathDL"], sword, seg, tag + "_is")  # on back
            _build_limb(o, CUSTOM_EQUIP_FOLDER, [base + "SheathDL"], sheath, seg, tag + "_sh")   # scabbard
            return {"ok": True, "symbol": sym, "age": age, "tris": len(geo),
                    "split": True, "resources": len(o.files) - before}

    _build_limb(o, CUSTOM_EQUIP_FOLDER, [sym], geo, seg, tag)
    # Shields: also emit the on-back slot(s) so the shield stays custom when slung on
    # Link's back (not just when held). Same mesh; SoH draws it at the back matrix.
    onback = _SHIELD_ONBACK.get(item.category)
    if onback:
        _build_limb(o, CUSTOM_EQUIP_FOLDER, onback, geo, seg, tag + "_ob")
    return {"ok": True, "symbol": sym, "age": age, "tris": len(geo),
            "onback": bool(onback), "resources": len(o.files) - before}


def convert_equipment_pak(ep, outdir: str, base: str) -> list:
    """Convert every supported item in an EquipPak to its own `.o2r`.
    Returns [(item, age, symbol, out_path, stats), ...]."""
    os.makedirs(outdir, exist_ok=True)
    produced = []
    seen = set()
    for it in ep.items:
        age = item_age(it)
        sym = dl_symbol_for(it.category, age)
        if sym is None:
            continue
        # one .o2r per item; if two items map to the same slot, keep the first
        # (SoH can only hold one custom model per slot at a time).
        o = _o2r.O2R()
        stats = convert_equipment_item(it, o, age)
        if not stats.get("ok"):
            continue
        stem = _sanitize(os.path.splitext(it.file)[0])
        out = os.path.join(outdir, f"{stem}.o2r")
        o.write(out)
        produced.append((it, age, sym, out, stats))
        seen.add(sym)
    return produced


def _sanitize(s: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in s).strip("_")
