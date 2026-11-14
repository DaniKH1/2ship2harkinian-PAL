"""
Parse a ModLoader64 pak's package.json and classify what it contains.

There are three manifest schemas seen in the wild (all under package.json):

1. zzplayas - NEW array form (e.g. Child_OoTLink):
     "zzplayas": {
       "OOT": { "adult_model": [ {"file","name"} ], "child_model": [...],
                "anim_file": {"file","name"}, "colors": {...}, "options": {...} },
       "MM":  { "child_model": [ {"file","name"} ] }
     }

2. zzplayas - OLD string form (e.g. Adult_Zelda3DS_OOT, Child_ToonLink_Combined):
     "zzplayas": {
       "OcarinaOfTime": { "adult_model":"x.zobj", "child_model":"y.zobj",
                          "anim_file":"", "adult_icon":"", "child_icon":"" },
       "MajorasMask":   { "child_model":"z.zobj", "anim_file":"" }
     }

3. zzequipment - equipment gear packs (e.g. Equip_Four_Sword, 3DS_Equipment):
     "zzequipment": {
       "OcarinaOfTime": { "equipment": [ "sword.zobj", "sword ZZENV.zobj", ... ] },
       "MajorasMask":   { "equipment": [ "" ] }
     }

This module normalises all of them into a Manifest dataclass.
"""
from __future__ import annotations
import json
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# game keys used across the two schema variants
_OOT_KEYS = ("OOT", "OcarinaOfTime", "OcarinaofTime", "oot")
_MM_KEYS = ("MM", "MajorasMask", "mm")


@dataclass
class ModelRef:
    file: str
    name: str = ""
    form: str = ""      # "adult" | "child"
    game: str = ""      # "OOT" | "MM"


@dataclass
class Manifest:
    raw: dict
    name: str = ""
    author: str = ""
    kind: str = "unknown"          # "player" | "equipment" | "unknown"
    games: List[str] = field(default_factory=list)   # subset of ["OOT","MM"]
    models: List[ModelRef] = field(default_factory=list)     # player models
    equipment: Dict[str, List[str]] = field(default_factory=dict)  # game -> [zobj,...]
    anim_files: List[ModelRef] = field(default_factory=list)
    colors: Dict[str, str] = field(default_factory=dict)

    # ---- convenience queries -------------------------------------------
    @property
    def has_adult(self) -> bool:
        return any(m.form == "adult" for m in self.models)

    @property
    def has_child(self) -> bool:
        return any(m.form == "child" for m in self.models)

    @property
    def is_oot(self) -> bool:
        return "OOT" in self.games

    @property
    def is_mm(self) -> bool:
        return "MM" in self.games

    def models_for(self, game: str, form: Optional[str] = None) -> List[ModelRef]:
        out = [m for m in self.models if m.game == game]
        if form:
            out = [m for m in out if m.form == form]
        return out

    def summary(self) -> str:
        lines = [f"{self.name or '(unnamed)'}  by {self.author or '?'}",
                 f"  kind: {self.kind}",
                 f"  games: {', '.join(self.games) or '(none detected)'}"]
        if self.models:
            lines.append("  player models:")
            for m in self.models:
                lines.append(f"    [{m.game:3s} {m.form:5s}] {m.file}"
                             + (f'  ("{m.name}")' if m.name else ""))
        if self.anim_files:
            for a in self.anim_files:
                lines.append(f"  anim bank [{a.game}]: {a.file}")
        for game, eq in self.equipment.items():
            real = [z for z in eq if z]
            if real:
                lines.append(f"  equipment [{game}]: {len(real)} piece(s)")
                for z in real:
                    lines.append(f"    - {z}")
        if self.colors:
            cols = {k: v for k, v in self.colors.items() if v}
            if cols:
                lines.append(f"  tunic color palettes: {', '.join(cols)}")
        return "\n".join(lines)


def _iter_game_blocks(section: dict):
    """Yield ("OOT"|"MM", block) for the game keys present in a section."""
    for k, v in section.items():
        if k in _OOT_KEYS:
            yield "OOT", v
        elif k in _MM_KEYS:
            yield "MM", v


def _model_entries(value) -> List[dict]:
    """Normalise a model field that may be a string or a list of {file,name}."""
    if not value:
        return []
    if isinstance(value, str):
        return [{"file": value, "name": ""}]
    if isinstance(value, dict):
        return [value]
    out = []
    for e in value:
        if isinstance(e, str):
            out.append({"file": e, "name": ""})
        elif isinstance(e, dict):
            out.append(e)
    return out


def parse(pkg: dict) -> Manifest:
    m = Manifest(raw=pkg)
    m.name = str(pkg.get("name", ""))
    m.author = str(pkg.get("author", ""))

    zzp = pkg.get("zzplayas")
    zze = pkg.get("zzequipment")

    games = set()

    if isinstance(zzp, dict):
        m.kind = "player"
        for game, block in _iter_game_blocks(zzp):
            if not isinstance(block, dict):
                continue
            saw = False
            for e in _model_entries(block.get("adult_model")):
                if e.get("file"):
                    m.models.append(ModelRef(e["file"], e.get("name", ""), "adult", game))
                    saw = True
            for e in _model_entries(block.get("child_model")):
                if e.get("file"):
                    m.models.append(ModelRef(e["file"], e.get("name", ""), "child", game))
                    saw = True
            for e in _model_entries(block.get("anim_file")):
                if e.get("file"):
                    m.anim_files.append(ModelRef(e["file"], e.get("name", ""), "", game))
                    saw = True
            cols = block.get("colors")
            if isinstance(cols, dict):
                m.colors.update({k: v for k, v in cols.items() if v})
            if saw:
                games.add(game)

    if isinstance(zze, dict):
        if m.kind == "unknown":
            m.kind = "equipment"
        for game, block in _iter_game_blocks(zze):
            if not isinstance(block, dict):
                continue
            eq = [z for z in (block.get("equipment") or []) if z]
            if eq:
                m.equipment[game] = eq
                games.add(game)

    # Preserve OOT-before-MM ordering
    m.games = [g for g in ("OOT", "MM") if g in games]
    return m


def from_pak_files(files: dict) -> Optional[Manifest]:
    """`files` = {relpath: bytes} from pak.load_map(). Returns Manifest or None."""
    for k, v in files.items():
        if k.rsplit("/", 1)[-1] == "package.json":
            try:
                return parse(json.loads(v.decode("latin1")))
            except Exception:
                return None
    return None
