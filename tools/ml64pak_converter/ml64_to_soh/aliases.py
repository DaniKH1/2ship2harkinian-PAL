"""
Maps a Zelda64 Link skeleton limb index to the vanilla SoH/2Ship resource
symbol(s) it must be emitted as, per game/age/form.

Skeleton limb index order (shared by all Link forms):
 0 Root  1 Waist  2 LowerCtrl  3 RThigh  4 RShin/Leg  5 RFoot  6 LThigh
 7 LShin/Leg  8 LFoot  9 UpperCtrl  10 Head  11 Hat  12 Collar  13 LShoulder
 14 LForearm/Arm  15 LHand  16 RShoulder  17 RForearm/Arm  18 RHand
 19 Sheath/SwordAndSheath  20 Torso

Names & object folders verified from decomp asset XML (work/RESEARCH_o2r.md §11).
"""
from __future__ import annotations
from typing import Dict, List, Optional

# target descriptor for one conversion
class Target:
    def __init__(self, object_folder: str, limb_dls: Dict[int, List[str]], lod: bool,
                 skel_name: str = ""):
        self.object_folder = object_folder
        self.limb_dls = limb_dls     # limb index -> [resource symbol names to emit]
        self.lod = lod
        self.skel_name = skel_name   # vanilla skeleton symbol to override (IsCustom gate)


def _pairs(body_by_limb: Dict[int, str], prefix: str) -> Dict[int, List[str]]:
    """OoT: each limb has Near+Far DL resources."""
    return {i: [f"{prefix}{name}NearDL", f"{prefix}{name}FarDL"]
            for i, name in body_by_limb.items()}


def _single(body_by_limb: Dict[int, str], fmt) -> Dict[int, List[str]]:
    return {i: [fmt(name)] for i, name in body_by_limb.items()}


# limb index -> body-part name fragment
_CHILD_BODY = {
    1: "Waist", 3: "RightThigh", 4: "RightShin", 5: "RightFoot",
    6: "LeftThigh", 7: "LeftShin", 8: "LeftFoot", 10: "Head", 11: "Hat",
    12: "Collar", 13: "LeftShoulder", 14: "LeftForearm", 15: "LeftHand",
    16: "RightShoulder", 17: "RightForearm", 18: "RightHand",
    19: "SwordAndSheath", 20: "Torso",
}
_ADULT_BODY = {
    1: "Waist", 3: "RightThigh", 4: "RightLeg", 5: "RightFoot",
    6: "LeftThigh", 7: "LeftLeg", 8: "LeftFoot", 10: "Head", 11: "Hat",
    12: "Collar", 13: "LeftShoulder", 14: "LeftArm", 15: "LeftHand",
    16: "RightShoulder", 17: "RightArm", 18: "RightHand",
    20: "Torso",
}
_ADULT_SHEATH = {19: "Sheath"}   # gLinkAdultSheathNearDL/FarDL
_HUMAN_BODY = {
    1: "Waist", 3: "RightThigh", 4: "RightShin", 5: "RightFoot",
    6: "LeftThigh", 7: "LeftShin", 8: "LeftFoot", 10: "Head", 11: "Hat",
    12: "Collar", 20: "Torso", 13: "LeftShoulder", 14: "LeftForearm",
    16: "RightShoulder", 17: "RightForearm",
}
_HUMAN_HANDS = {15: "LeftHandOpen", 18: "RightHandOpen"}


def target_for(game: str, form_byte: int, form_kind: str) -> Optional[Target]:
    """
    game: "OOT" | "MM".  form_kind: "adult" | "child" (from the manifest).
    form_byte: header 0x500B (OoT child=1/adult=0; MM human=4).
    """
    if game == "OOT" and form_kind == "adult":
        dls = _pairs(_ADULT_BODY, "gLinkAdult")
        dls.update(_pairs(_ADULT_SHEATH, "gLinkAdult"))
        return Target("object_link_boy", dls, lod=True, skel_name="gLinkAdultSkel")

    if game == "OOT" and form_kind == "child":
        return Target("object_link_child", _pairs(_CHILD_BODY, "gLinkChild"), lod=True,
                      skel_name="gLinkChildSkel")

    if game == "MM":
        # MM human form (single-resolution DLs, gLinkHuman*)
        dls = _single(_HUMAN_BODY, lambda n: f"gLinkHuman{n}DL")
        dls.update(_single(_HUMAN_HANDS, lambda n: f"gLinkHuman{n}DL"))
        return Target("object_link_child", dls, lod=False, skel_name="gLinkHumanSkel")

    return None
