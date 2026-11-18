# 2 Ship 2 Harkinian PAL

Unofficial Windows x64 fork of [2 Ship 2 Harkinian](https://github.com/2ship2harkinian/2ship2harkinian), based on the PAL-supporting [2Ship2Harkinian PAL fork](https://github.com/alexbcz/2ship2harkinian-PAL). It combines European Majora's Mask support with multiplayer, graphics, interface, and mod-management improvements.

## Supported Platform

- Windows x64

## Supported ROMs

- Majora's Mask US / NTSC-U
- Majora's Mask European / PAL 1.1 (Rev 1): English, French, German, and Spanish

For PAL 1.1, the game language can be changed from **File Select > Options**.

## Installation

Download a Windows release from the [releases page](https://github.com/DaniKH1/2ship2harkinian-PAL/releases), extract it, and launch `2ship.exe`. On first launch, select your supported Majora's Mask ROM when prompted.

No game ROM is included in this repository or its releases.

## Features and Changes

### PAL, language, and startup menu

- Adds European / PAL 1.1 ROM support and multilingual game assets while preserving US / NTSC-U compatibility.
- Adds a custom startup screen with **Start Game**, **Settings**, **Mods**, and **Exit**. The Majora's Mask artwork is embedded in the Windows executable rather than loaded as a loose image file.
- Adds an English / Spanish interface-language selector. The menu and commonly used settings have Spanish translations; untranslated text remains in English.
- The settings window starts closed on each launch instead of restoring its previous open state.
- Targets Windows x64; release packaging and CI are configured for that platform.

### Anchor multiplayer

The **Anchor** client is available under **Settings > Network > Anchor**. Set a player name and room ID, then enable Anchor to connect. Players who should share items and flags must use the same team ID. Room hosts can configure PvP, location visibility, and teleporting; the Anchor panel also provides setup instructions and a public global room.

Anchor is beta software. If using Anchor with Archipelago, disable item syncing in the Anchor room settings to avoid conflicts.

### Graphics, camera, and controls

- Adds flat-shading and rain cosmetic options, dungeon-specific 3D keys, a screen-distortion toggle, and a fix for circular-shadow streaks. Related graphics options are grouped under **Graphics Overhaul Settings**; 3D keys appear automatically in Randomizer.
- Sets the default target frame rate to 60 FPS.
- Adjusts the default camera options to fix targeting-camera snap and disable auto-centering, with both camera-axis inversion options off by default.
- Uses the D-pad for the C-button controls by default.
- Opening Settings during offline gameplay pauses gameplay and mutes game audio; online Anchor play is not paused by that offline behavior.

### Mod library and catalog

The redesigned Mod Menu organizes installed mods into active and inactive lists, supports searching, and lets you change load priority; higher entries override lower entries. Files from the same mod folder are grouped together, so enabling, disabling, deleting, or reordering a mod applies to its archives together. Mods can be removed with a confirmation prompt.

Each mod's **Mod Settings** can expose optional content folders and variant folders as controls. Variant folders become categories with dropdowns; a folder named with terms such as Vanilla, Default, Base, Original, or OG is preferred as the default when available. Loose supported archives are organized into individual mod folders when the library is initialized.

The **Download Mods** catalog includes:

- GameBanana Majora's Mask mods.
- Majora's Mask-compatible ModLoader64 models, filterable by Adult, Child, Combined, Emotes, and Equipment.
- ModLoader64 community mods.
- A curated selection of Majora's Mask texture packs from Emulation King.

Searches and thumbnail loading run asynchronously, with thumbnail aspect ratios preserved. Downloads show transfer and installation progress, and catalog thumbnails use a temporary cache at `mods/.mod-browser-cache` that is cleared when the application closes. ZIP mod downloads are installed in a folder named after the mod. Extraction accepts supported `.o2r` and `.pak` archives and preserves their nested folder layout, including when different variant folders contain archives with the same filename.

Texture packs are downloaded separately into `mods/Texture Packs` and retained as ZIP files. Rice, Glide64 `.dat`, and GlideN64 `.htc` options are offered for download only; **this build does not apply those texture-pack formats**.

### ModLoader64 and Z64Online packages

Supported ModLoader64 / Z64Online `.pak` player-model and equipment mods are converted to `.o2r` when you select **Start Game**. Conversion is limited to content targeting Majora's Mask; OoT-only assets are not generated. Converted archives use the asset names from the package, without a generated `pakconv_<hash>` prefix. The temporary package-specific conversion cache is removed after archives are moved, and an original `.pak` is removed only after its converted archives load successfully. Alternate assets are enabled because converted resources use 2Ship's `alt/` asset paths.

Other `.pak` formats and unsupported package contents are not loadable. Windows builds generate the standalone converter automatically; install `tools/ml64pak_converter/requirements-build.txt` in the Python environment used by CMake, or build it manually with `tools/build_ml64pak_converter.ps1`.

### Windows build and release cleanup

Windows builds keep intermediate libraries, symbols, and the ZAPD tool in the build directory rather than alongside the release executable. Each game build removes generated development files and temporary `.pak` conversion cache from `x64/Release`, while preserving configuration, mods, saves, and other runtime data.

<<<<<<< HEAD
To publish a Windows release, run **Actions > Manual Windows Release > Run workflow** on the commit to release. The workflow builds and validates the distributable ZIP, then publishes it as a GitHub Release after approval. Set up a GitHub environment named `release` with required reviewers under **Settings > Environments** to enable that approval gate. You can provide a tag beginning with `v` (for example, `v1.0.0`); leaving it blank creates a date-based tag.

The PAL-specific `libultraship` changes are stored as a patch in this repository so CI can build from the publicly available submodule commit. For local builds, initialize the submodules and run `.\tools\apply_libultraship_patch.ps1` once before configuring CMake.

=======
>>>>>>> parent of 0ca7fb853 (Add manual Windows release workflow)
## Compatibility Notes

Zelda 64: Recompiled `.nrm` mods are not compatible. They are ZIP-based packages containing a manifest and compiled patches that target Zelda 64: Recompiled's runtime and mod API, not resource archives that 2Ship can mount. Supporting them would require a separate compatible patch runtime or porting each mod to 2Ship.

## Known Issues

- Texture-pack downloads are supported, but applying Rice, Glide64, or GlideN64 texture packs is not.
- Near-plane clipping may occur when the camera is very close to walls.

## References and Credits

- Official upstream project: [2 Ship 2 Harkinian](https://github.com/2ship2harkinian/2ship2harkinian)
- PAL base: [2Ship2Harkinian PAL](https://github.com/alexbcz/2ship2harkinian-PAL)
- Multiplayer reference: [2Ship2Harkinian Anchor branch](https://github.com/garrettjoecox/2ship2harkinian/tree/anchor)
- Graphics reference: [2Ship2Harkinian Graphics Overhaul Project](https://github.com/whitesun046/2ship2harkinian-Graphics-Overhaul-Project)
- GameBanana: [Majora's Mask mods](https://gamebanana.com/games/20371)
- ModLoader64: [models and community mods](https://modloader64.com/)
- Texture-pack catalog: [Emulation King Majora's Mask texture packs](https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/)
- Mod-format reference: [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) and its [Majora's Mask mod template](https://github.com/Zelda64Recomp/MMRecompModTemplate)
- `.pak` conversion reference: [ModLoader64 to Ship of Harkinian / 2Ship converter](https://github.com/AgitationSkeleton/soh_modloaderconverter_public)

## AI Assistance Disclosure

Generative AI was used as an assistant for portions of this fork's code changes and documentation. AI-assisted changes should be reviewed and tested by contributors.
