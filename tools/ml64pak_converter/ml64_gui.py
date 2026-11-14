"""
ModLoader64 -> Ship-of-Harkinian / 2Ship converter, with a plain Visual-Basic
styled desktop UI for end users.

Drop in a ModLoader64 `.pak`; the tool tells you what it is (OoT / MM / both;
adult / child / equipment / multiple) and writes clean `.o2r` files - one per
model or equipment item - into an output folder. Replacement resources are
emitted under the engine's `alt/` asset tree.

Runs as a script (`python ml64_gui.py`) or as a frozen `.exe` (see build_exe.ps1).
Only depends on the bundled `ml64_to_soh` package + Pillow (for the preview).
"""
from __future__ import annotations
import os
import sys
import threading
import traceback

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ml64_to_soh import pak, manifest, equipment, convert           # noqa: E402
from ml64_to_soh import equipment_convert as ec, o2r as _o2r        # noqa: E402
from ml64_to_soh import model as _model                             # noqa: E402

try:
    from PIL import Image, ImageTk
    _HAVE_PIL = True
except Exception:
    _HAVE_PIL = False

APP_TITLE = "ModLoader64  ->  SoH / 2Ship Converter"
VB_FONT = ("Segoe UI", 9)
VB_BOLD = ("Segoe UI", 9, "bold")
PREVIEW_PX = 260         # fixed preview box size, in PIXELS

# Window icon: the Kokiri Tunic inventory sprite, read from YOUR Ship of Harkinian
# `oot.o2r` at runtime. No game asset is bundled with this repo, so if that file
# isn't found the app simply runs without a custom icon. See tools/make_icon.py to
# bake an .ico for a frozen build.
_ICON_RESOURCE = "textures/icon_item_static/gItemIconTunicKokiriTex"


def _load_icon_png_bytes():
    """PNG bytes of the tunic icon from a local oot.o2r, or None if unavailable."""
    try:
        import io
        import zipfile
        import numpy as np
        from ml64_to_soh import convert as _cv, o2r as _o2rmod
        path = _cv._oot_o2r_path()
        if not path:
            return None
        with zipfile.ZipFile(path) as z:
            _tt, w, h, body = _o2rmod.read_texture(z.read(_ICON_RESOURCE))
        rgba = np.frombuffer(body, np.uint8).reshape(h, w, 4).copy()   # RGBA32
        img = Image.fromarray(rgba, "RGBA")
        box = img.getbbox()
        if box:
            img = img.crop(box)
        buf = io.BytesIO()
        img.resize((48, 48), Image.NEAREST).save(buf, "PNG")
        return buf.getvalue()
    except Exception:
        return None

# We draw with ttk's self-contained "clam" theme and an explicit palette rather
# than the native theme, so window skinners (WindowBlinds, OS dark mode) can't
# half-skin the controls into an inconsistent light/dark mix. The palette follows
# the user's Windows light/dark setting so it still feels at home either way.
PALETTES = {
    "light": dict(bg="#f2f2f4", panel="#ffffff", inset="#ececf0", text="#1b1b1f",
                  sub="#6a6a72", accent="#2f6fb0", btn="#e4e4ea", btn_active="#d3d3db",
                  border="#c2c2cc", sel="#2f6fb0", sel_fg="#ffffff"),
    "dark":  dict(bg="#2b2b2e", panel="#1e1e21", inset="#232326", text="#e7e7ea",
                  sub="#9a9aa2", accent="#5aa0e6", btn="#3a3a3e", btn_active="#4a4a50",
                  border="#46464c", sel="#3d6ea5", sel_fg="#ffffff"),
}


def _is_dark_mode() -> bool:
    """True if Windows is set to a dark apps theme (best-effort)."""
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                           r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
        val, _ = winreg.QueryValueEx(k, "AppsUseLightTheme")
        return val == 0
    except Exception:
        return False


# ----------------------------------------------------------------------------
# Conversion model: one "job" = one .o2r we can produce from the pak.
# ----------------------------------------------------------------------------
class Job:
    """A single convertible output (a player model form, or an equipment item)."""

    def __init__(self, kind, label, stem, produce, preview, games=(), produce_for_game=None):
        self.kind = kind          # "player" | "equipment"
        self.label = label        # human list label
        self.stem = stem          # output filename stem
        self.produce = produce    # (out_path) -> stats dict  (writes the .o2r)
        self.preview = preview    # (out_path) -> PIL.Image | None
        self.games = tuple(games)
        self.produce_for_game = produce_for_game


def _sanitize(s):
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in s).strip("_")


def analyze(pak_path):
    """Return (summary_dict, [Job]). Raises on an unreadable pak."""
    files = pak.load_map(pak_path)
    man = manifest.from_pak_files(files)
    if man is None or man.kind == "unknown":
        return {"name": os.path.basename(pak_path), "kind": "Unknown",
                "game": "-", "type": "Not a recognized ModLoader64 pak"}, []

    games_lbl = _games_label(man.games)
    games = set(man.games)
    jobs = []

    # A pak can carry player models AND equipment (e.g. a character with a custom
    # weapon). Scan for BOTH rather than branching on a single kind, so neither half
    # is dropped. man.models is empty for a pure-equipment pak; equipment.parse only
    # yields items with a real EQUIPMENTCAT, so player zobjs are naturally excluded.
    forms = set()
    for game in man.games:
        for form in ("adult", "child"):
            for mref in man.models_for(game, form):
                zdata = files.get(mref.file) or convert._find(files, mref.file)
                if not zdata:
                    continue
                forms.add(form)
                jobs.append(_player_job(zdata, mref, game, form))

    n_eq = 0
    ep = equipment.parse(files, man.raw)
    for it in ep.items:
        if not it.category:                 # not an equipment zobj (a player model)
            continue
        age = ec.item_age(it)
        for g in it.games:
            games.add(g)
        if ec.dl_symbol_for(it.category, age) is None:
            jobs.append(_equip_job(it, age, supported=False))
        else:
            n_eq += 1
            jobs.append(_equip_job(it, age, supported=True))

    n_models = sum(1 for j in jobs if j.kind == "player")
    parts = []
    if n_models:
        parts.append(_type_label(sorted(forms)) + (f" x{n_models}" if n_models > 1 else ""))
    if n_eq:
        parts.append(f"Equipment ({n_eq} item{'s' if n_eq != 1 else ''})")
    kind_lbl = " + ".join(k for k in (
        "Player model" if n_models else "", "Equipment" if n_eq else "") if k) or man.kind.title()
    summary = {"name": man.name or os.path.basename(pak_path), "kind": kind_lbl,
               "game": _games_label(games), "type": "  +  ".join(parts) or "-",
               "author": man.author}
    return summary, jobs


def _games_label(games):
    has = set(games)
    if "OOT" in has and "MM" in has:
        return "OoT + Majora's Mask"
    if "OOT" in has:
        return "Ocarina of Time"
    if "MM" in has:
        return "Majora's Mask"
    return "-"


def _type_label(forms):
    if "adult" in forms and "child" in forms:
        return "Adult + Child"
    if "adult" in forms:
        return "Adult"
    if "child" in forms:
        return "Child"
    return "Player"


def _player_job(zdata, mref, game, form):
    stem = _sanitize(f"{os.path.splitext(mref.file)[0]}_{game}_{form}")
    gl = {"OOT": "OoT", "MM": "MM"}[game]
    label = f"[Model]  {gl} {form.capitalize()}  —  {mref.name or mref.file}"

    def produce(out_path):
        pm = _model.load(zdata)
        o = _o2r.O2R()
        stats = convert.convert_model(pm, game, form, o, ns=stem)
        if not stats.get("ok"):
            raise RuntimeError(stats.get("reason", "conversion failed"))
        o.write(out_path)
        return stats

    def preview(out_path):
        if not _HAVE_PIL:
            return None
        from batch_out import _player_after
        pm = _model.load(zdata)
        return _player_after(pm, out_path, game, form, size=PREVIEW_PX)

    return Job("player", label, stem, produce, preview, games=(game,))


def _equip_job(it, age, supported):
    stem = _sanitize(os.path.splitext(it.file)[0])
    if supported:
        label = f"[Equip]  {it.category or it.name}  ({age})"
    else:
        label = f"[Equip]  {it.category or it.name}  —  no SoH slot (skipped)"

    def produce(out_path):
        if not supported:
            raise RuntimeError(f"'{it.category}' has no SoH custom-equipment slot")
        o = _o2r.O2R()
        stats = ec.convert_equipment_item(it, o, age)
        if not stats.get("ok"):
            raise RuntimeError(stats.get("reason", "conversion failed"))
        o.write(out_path)
        return stats

    def produce_for_game(out_path, game):
        game_age = ec.item_age(it, game)
        if ec.dl_symbol_for(it.category, game_age) is None:
            raise RuntimeError(f"'{it.category}' has no SoH custom-equipment slot for {game}")
        o = _o2r.O2R()
        stats = ec.convert_equipment_item(it, o, game_age, game=game)
        if not stats.get("ok"):
            raise RuntimeError(stats.get("reason", "conversion failed"))
        o.write(out_path)
        return stats

    def preview(out_path):
        if not _HAVE_PIL or not supported:
            return None
        from batch_out import _equip_after
        return _equip_after(out_path, it.category, size=PREVIEW_PX)

    return Job("equipment", label, stem, produce, preview, games=it.games, produce_for_game=produce_for_game)


# ----------------------------------------------------------------------------
# UI
# ----------------------------------------------------------------------------
class ConverterApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self._set_window_icon()
        self.pal = PALETTES["dark" if _is_dark_mode() else "light"]
        self.configure(bg=self.pal["bg"])
        self.resizable(True, True)
        self.minsize(560, 470)
        self.jobs = []
        self.pak_path = None
        self._preview_imgtk = None
        self._build()

    def _set_window_icon(self):
        """Kokiri-tunic title-bar icon, read from a local oot.o2r if one is present.
        Silently skipped when it isn't - no game asset ships with this repo."""
        png = _load_icon_png_bytes()
        if not png:
            return
        try:
            import base64 as _b64
            self._winicon = tk.PhotoImage(data=_b64.b64encode(png).decode())
            self.iconphoto(True, self._winicon)
        except Exception:
            try:
                import io as _io
                self._winicon = ImageTk.PhotoImage(Image.open(_io.BytesIO(png)))
                self.iconphoto(True, self._winicon)
            except Exception:
                pass

    # ---- layout ----
    def _build(self):
        p = self.pal
        style = ttk.Style(self)
        try:
            style.theme_use("clam")        # self-drawn: consistent under any skin
        except Exception:
            pass
        style.configure(".", background=p["bg"], foreground=p["text"],
                        fieldbackground=p["panel"], font=VB_FONT, bordercolor=p["border"])
        style.configure("TLabel", background=p["bg"], foreground=p["text"])
        style.configure("Head.TLabel", font=VB_BOLD, background=p["bg"], foreground=p["text"])
        style.configure("Val.TLabel", background=p["bg"], foreground=p["accent"])
        style.configure("TLabelframe", background=p["bg"], bordercolor=p["border"])
        style.configure("TLabelframe.Label", background=p["bg"], foreground=p["sub"], font=VB_BOLD)
        style.configure("TEntry", fieldbackground=p["panel"], foreground=p["text"],
                        bordercolor=p["border"], insertcolor=p["text"])
        style.map("TEntry", fieldbackground=[("readonly", p["inset"])])
        style.configure("TButton", background=p["btn"], foreground=p["text"],
                        bordercolor=p["border"], focuscolor=p["accent"], padding=(10, 4))
        style.map("TButton",
                  background=[("active", p["btn_active"]), ("disabled", p["inset"])],
                  foreground=[("disabled", p["sub"])])

        pad = dict(padx=6, pady=4)

        # --- source group ---
        g1 = ttk.LabelFrame(self, text=" ModLoader64 source (.pak) ")
        g1.grid(row=0, column=0, columnspan=2, sticky="ew", **pad)
        g1.grid_columnconfigure(0, weight=1)
        self.src_var = tk.StringVar(value="(no file selected)")
        ttk.Entry(g1, textvariable=self.src_var, width=64, state="readonly"
                  ).grid(row=0, column=0, padx=6, pady=6, sticky="ew")
        ttk.Button(g1, text="Browse…", command=self.on_browse
                   ).grid(row=0, column=1, padx=6, pady=6)

        # --- detected group ---
        g2 = ttk.LabelFrame(self, text=" Detected ")
        g2.grid(row=1, column=0, sticky="nsew", **pad)
        self.lbl_name = self._kv(g2, "Name:", 0)
        self.lbl_kind = self._kv(g2, "Kind:", 1)
        self.lbl_game = self._kv(g2, "Game:", 2)
        self.lbl_type = self._kv(g2, "Type:", 3)
        ttk.Label(g2, text="Contents:", style="Head.TLabel"
                  ).grid(row=4, column=0, sticky="w", padx=6, pady=(6, 0))
        self.listbox = tk.Listbox(g2, width=52, height=9, font=VB_FONT,
                                  activestyle="none", exportselection=False,
                                  bg=p["panel"], fg=p["text"], bd=0, relief="flat",
                                  highlightthickness=1, highlightbackground=p["border"],
                                  selectbackground=p["sel"], selectforeground=p["sel_fg"])
        self.listbox.grid(row=5, column=0, columnspan=2, sticky="nsew", padx=6, pady=4)
        self.listbox.bind("<<ListboxSelect>>", self.on_select)
        g2.grid_columnconfigure(0, weight=1)   # list widens with the window
        g2.grid_rowconfigure(5, weight=1)       # list grows taller with the window

        # --- preview group (fixed PIXEL box: width/height on a Label are text units,
        #     so we pin a Frame's pixel size and disable geometry propagation) ---
        g3 = ttk.LabelFrame(self, text=" Preview (post-conversion) ")
        g3.grid(row=1, column=1, sticky="n", **pad)
        holder = tk.Frame(g3, width=PREVIEW_PX, height=PREVIEW_PX, bg=p["inset"],
                          highlightbackground=p["border"], highlightthickness=1)
        holder.grid(row=0, column=0, padx=6, pady=6)
        holder.grid_propagate(False)
        self.preview_lbl = tk.Label(holder, bg=p["inset"], fg=p["sub"], font=VB_FONT,
                                    text="Select an item\nto preview the\nconverted model")
        self.preview_lbl.place(relx=0.5, rely=0.5, anchor="center")

        # --- output group ---
        g4 = ttk.LabelFrame(self, text=" Output ")
        g4.grid(row=2, column=0, columnspan=2, sticky="ew", **pad)
        g4.grid_columnconfigure(1, weight=1)
        self.out_var = tk.StringVar(value=os.path.join(os.getcwd(), "out"))
        ttk.Label(g4, text="Folder:").grid(row=0, column=0, sticky="w", padx=6, pady=6)
        ttk.Entry(g4, textvariable=self.out_var, width=52).grid(row=0, column=1, padx=2, pady=6, sticky="ew")
        ttk.Button(g4, text="Browse…", command=self.on_out_browse
                   ).grid(row=0, column=2, padx=6, pady=6)
        self.btn_convert = ttk.Button(g4, text="Convert to .o2r", command=self.on_convert,
                                      state="disabled")
        self.btn_convert.grid(row=1, column=1, sticky="e", padx=2, pady=(0, 8))

        # --- status bar ---
        self.status = tk.StringVar(value="Ready.  Choose a ModLoader64 .pak to begin.")
        sb = tk.Label(self, textvariable=self.status, bd=0, anchor="w", padx=6, pady=3,
                      font=VB_FONT, bg=p["inset"], fg=p["sub"])
        sb.grid(row=3, column=0, columnspan=2, sticky="ew")
        # Row 1 (Detected list + Preview) absorbs vertical stretch and pushes Output
        # down; column 0 (Detected) absorbs horizontal stretch, preview stays fixed.
        self.grid_columnconfigure(0, weight=1)
        self.grid_columnconfigure(1, weight=0)
        self.grid_rowconfigure(1, weight=1)

    def _kv(self, parent, key, row):
        ttk.Label(parent, text=key, style="Head.TLabel").grid(
            row=row, column=0, sticky="w", padx=6, pady=1)
        var = tk.StringVar(value="-")
        ttk.Label(parent, textvariable=var, style="Val.TLabel").grid(
            row=row, column=1, sticky="w", padx=4, pady=1)
        return var

    # ---- actions ----
    def on_browse(self):
        p = filedialog.askopenfilename(title="Select a ModLoader64 .pak",
                                       filetypes=[("ModLoader64 pak", "*.pak"), ("All files", "*.*")])
        if not p:
            return
        self.pak_path = p
        self.src_var.set(p)
        self.status.set("Analyzing…")
        self.update_idletasks()
        try:
            summary, jobs = analyze(p)
        except Exception as e:
            messagebox.showerror("Analyze failed", f"{e}\n\n{traceback.format_exc()}")
            self.status.set("Analyze failed.")
            return
        self.jobs = jobs
        self.lbl_name.set(summary.get("name", "-"))
        self.lbl_kind.set(summary.get("kind", "-"))
        self.lbl_game.set(summary.get("game", "-"))
        self.lbl_type.set(summary.get("type", "-"))
        self.listbox.delete(0, tk.END)
        for j in jobs:
            self.listbox.insert(tk.END, j.label)
        self._set_preview_text("Rendering preview…")
        convertible = [j for j in jobs if "skipped" not in j.label]
        self.btn_convert.config(state=("normal" if convertible else "disabled"))
        self.status.set(f"Detected {summary.get('kind','?')} — "
                        f"{len(convertible)} convertible item(s).")
        # Auto-select the first convertible item so its preview renders immediately.
        first = next((i for i, j in enumerate(jobs) if "skipped" not in j.label), None)
        if first is not None:
            self.listbox.selection_clear(0, tk.END)
            self.listbox.selection_set(first)
            self.listbox.see(first)
            self.on_select(None)
        else:
            self._set_preview_text("Select an item\nto preview")

    def on_out_browse(self):
        d = filedialog.askdirectory(title="Choose output folder",
                                    initialdir=self.out_var.get() or os.getcwd())
        if d:
            self.out_var.set(d)

    def on_select(self, _evt):
        sel = self.listbox.curselection()
        if not sel:
            return
        job = self.jobs[sel[0]]
        if "skipped" in job.label:
            self._set_preview_text("\n\n(no SoH slot\nfor this item)")
            return
        self.status.set(f"Rendering preview: {job.stem}…")
        self.update_idletasks()
        threading.Thread(target=self._preview_worker, args=(job,), daemon=True).start()

    def _preview_worker(self, job):
        try:
            import tempfile
            tmp = os.path.join(tempfile.gettempdir(), "ml64gui_preview.o2r")
            job.produce(tmp)
            img = job.preview(tmp)
            self.after(0, lambda: self._show_preview(img, job))
        except Exception as e:
            self.after(0, lambda: (self._set_preview_text("\n\n(preview\nunavailable)"),
                                   self.status.set(f"Preview error: {e}")))

    def _show_preview(self, img, job):
        if img is None:
            self._set_preview_text("\n\n(preview\nunavailable)")
            self.status.set("Preview unavailable (geometry empty).")
            return
        self._preview_imgtk = ImageTk.PhotoImage(img.convert("RGB"))
        self.preview_lbl.config(image=self._preview_imgtk, text="")
        self.status.set(f"Preview: {job.label}")

    def _set_preview_text(self, txt):
        self.preview_lbl.config(image="", text=txt)

    def on_convert(self):
        if not self.jobs:
            return
        outdir = self.out_var.get().strip()
        if not outdir:
            messagebox.showwarning("No output folder", "Choose an output folder first.")
            return
        self.btn_convert.config(state="disabled")
        threading.Thread(target=self._convert_worker, args=(outdir,), daemon=True).start()

    def _convert_worker(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        ok, fail = 0, 0
        log = []
        for j in self.jobs:
            if "skipped" in j.label:
                continue
            out = os.path.join(outdir, j.stem + ".o2r")
            try:
                self.after(0, lambda s=j.stem: self.status.set(f"Converting {s}…"))
                stats = j.produce(out)
                ok += 1
                extra = ""
                if j.kind == "player":
                    extra = f"{stats.get('limbs',0)} limbs, {stats.get('hands',0)} hands"
                else:
                    extra = stats.get("symbol", "")
                log.append(f"OK    {os.path.basename(out)}   {extra}")
            except Exception as e:
                fail += 1
                log.append(f"FAIL  {j.stem}: {e}")
        msg = f"Converted {ok} file(s)" + (f", {fail} failed" if fail else "") + f"\n\ninto {outdir}"
        self.after(0, lambda: self._convert_done(ok, fail, outdir, log, msg))

    def _convert_done(self, ok, fail, outdir, log, msg):
        self.btn_convert.config(state="normal")
        self.status.set(f"Done — {ok} converted, {fail} failed.  → {outdir}")
        messagebox.showinfo("Conversion complete", msg + "\n\n" + "\n".join(log[:20]))


def main():
    app = ConverterApp()
    app.mainloop()


if __name__ == "__main__":
    main()
