# 2 Ship 2 Harkinian PAL

Unofficial fork of 2 Ship 2 Harkinian adding support for the European / PAL 1.1 release of The Legend of Zelda: Majora's Mask.

This fork supports the PAL 1.1 ROM on Windows, Linux, macOS and Android, including English, French, German and Spanish.

## Supported Platforms

- Windows x64
- Linux x86_64
- macOS
- Android ARM64

## Supported ROMs

- Majora's Mask US / NTSC-U
- Majora's Mask European / PAL 1.1 (Rev 1): English, French, German and Spanish

For PAL 1.1, the language can be changed from **File Select > Options**.

## Installation

Download the appropriate build from:

https://github.com/alexbcz/2ship2harkinian-PAL/releases

### Windows

1. Download the Windows ZIP and extract it to a folder.
2. Launch `2ship.exe`.
3. On first launch, select your supported Majora's Mask ROM when prompted.

### Linux

The Linux release is distributed as an **AppImage**: a portable executable that does not require a traditional installation or extracting an archive.

1. Download the Linux `.AppImage` file.
2. Make it executable and launch it.
3. On first launch, provide your supported Majora's Mask ROM when prompted.

### macOS

1. Download the macOS `.dmg` file.
2. Open the DMG to mount the disk image.
3. Open the application (`.app`) inside it, or copy it to your **Applications** folder first.
4. On first launch, select your supported Majora's Mask ROM when prompted.

If macOS blocks the application because it is from an unidentified developer, review the warning and, if you trust the download, allow the app through **System Settings > Privacy & Security**.

### Android

**Requirements:** Android 7 or later, an ARM64 device, and OpenGL ES 3.0 or later.

1. Download and install the Android APK.
2. Launch the app.
3. If game assets have not already been extracted, select your supported Majora's Mask ROM when prompted. The app will generate `mm.o2r`.
4. Subsequent launches should start directly into the game.

To open the menu, tap the on-screen **Back** button. On a physical controller, use the **Back** or **minus (-)** button, depending on its mapping.

#### Important: Switching from an existing US Android installation

If you previously installed another Android port of 2 Ship 2 Harkinian and extracted a **US / NTSC-U ROM**, this PAL fork may reuse the existing `mm.o2r` in your shared `2S2H` data folder. Since US ROMs are still supported, the game then starts in the **US version**, even when using the PAL APK.

To switch to PAL 1.1:

1. Open the PAL app and check the selected data folder under **Settings > General**.
2. Close the app completely.
3. In that data folder, **remove only `mm.o2r`**. You can move it outside the folder first if you want to keep a backup of your US extraction.
4. Launch the PAL app again. When prompted, select your **European PAL 1.1 (Rev 1)** ROM and let the app generate a new `mm.o2r`.
5. Select your preferred language under **File Select > Options**.

**Do not delete the entire `2S2H` folder** or your save files and settings. Do not remove `2ship.o2r`: it is a different resource archive.

Existing installations of this fork's **PAL v1.0.0** can be updated directly to v1.1.0 without uninstalling the app.

## Android Data Folder

The app stores extracted resources, settings, saves, and other user files in the selected `2S2H` data folder. You can view or change its location under **Settings > General**, including using an SD card where supported.

Place mods and user preset files in the relevant directories inside that data folder.

## FAQs

### What is different about this fork?

It adds European / PAL 1.1 ROM support and multilingual assets while preserving upstream US / NTSC-U compatibility.

The Android version also includes mobile-friendly menus, improved touch navigation, options for touch controls and menu scaling, and memory-management improvements.

### Why does the Android app still launch the US version?

An existing `mm.o2r` extracted from a US ROM may be reused. Follow the **Switching from an existing US Android installation** instructions above to regenerate it from your PAL 1.1 ROM.

### Why does the game crash immediately?

If the extracted game assets are corrupted, back up or remove `mm.o2r` from the active data folder, then relaunch the app and extract it again from your own supported ROM. Keep your save files and settings.

### My controller is not responding. What should I do?

Open the menu and check **Settings > Controls** to confirm that the controller is detected and mapped.

### Can I hide the on-screen touch controls on Android?

Yes. Tap the **eye icon** in the bottom-right corner of the screen.

### Can I resize the menu on Android?

Yes. Use **Settings > General > Menu Scale**.

## Known Issues

- Near-plane clipping may occur when the camera is very close to walls.

## Credits

- [Original 2 Ship 2 Harkinian project](https://github.com/2ship2harkinian/2ship2harkinian)
- [Original Android port by Waterdish](https://github.com/Waterdish/2ship2harkinian-Android)
- [Android fork by linkzenic, used as the base for this port](https://github.com/linkzenic/2ship2harkinian-Android)