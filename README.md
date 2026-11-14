# FateREVita

A native PS Vita port and PC asset pipeline for **Fate/stay night REMASTERED** (2024 Steam) combined with **Fate/stay night [Réalta Nua] Ultimate Edition** restoration patches.

> **Disclaimer:** This repository contains **only** the custom C++17 / `vitaGL` Vita engine (`vita_engine/`) and Python asset-conversion scripts (`pipeline/`). No copyrighted game assets, scripts, audio, keys, or third-party binaries are included.

---

## Quick Start — How to Install on PS Vita

1. **Download & Install `FateReVita.vpk` on your PS Vita**
   - Download `FateReVita.vpk` from the [Releases](https://github.com/SEMSEM66/FateREVita/releases) page.
   - Copy it to your PS Vita (`ux0:`) and install it via **VitaShell**.
   - *(Standard `vitaGL` requirement: ensure `libshacccg.suprx` is installed in `ur0:data/` or `ux0:data/`.)*
2. **Generate the Vita Data Files on PC (1 Command)**
   - Ensure the [Prerequisites](#prerequisites) below (Python 3.10+ with Pillow, `ffmpeg`, external decryption tools in `tools/`, and your legally owned Steam + Ultimate Edition game folders) are ready.
   - Run the single pipeline command:
     ```bash
     python pipeline/build_full.py --steam "<path-to-Blade>" --ue "<path-to-Ultimate-Edition>"
     ```
   - This converts all scripts, 960x544 graphics, soundtracks, 28k+ voices, gallery thumbnails, and opening movies (`op01`–`op03`), then automatically packs `dist/FateReVita_base.tar` and `dist/FateReVita_voice.tar`.
3. **Transfer Data to PS Vita & Play**
   - Open **VitaShell** -> connect USB (or FTP) and copy both `dist/FateReVita_base.tar` and `dist/FateReVita_voice.tar` directly into `ux0:data/` on your Vita.
   - Launch **Fate/stay night REMASTERED** from LiveArea. On first launch it automatically unpacks `FateReVita_base.tar` into `ux0:data/FateReVita/` and indexes `FateReVita_voice.tar` in-place (cleaning up the `.tar` installer step smoothly).

---

## Features

- **Full Game Support:** Prologue (Days 1–3), **Fate**, **Unlimited Blade Works**, **Heaven's Feel**, **Last Episode**, and **Tiger Dojo** retry prompts.
- **Selectable Soundtrack (`OST Mode`):**
  - `New (Remastered)` — 2024 Steam soundtrack
  - `Old (2004 / PS2)` — Original soundtrack from Ultimate Edition with `.sli` seamless sample-accurate loop points
- **Selectable Content Mode:**
  - `All-Ages (Remastered)` — 2024 Steam official English translation & 16:9 HD visuals
  - `18+ (Original Mosaic)` — Restored adult scenes & original mosaic CGs
  - `18+ (Uncensored)` — Restored adult scenes & demosaic-patched CGs (`decensor_config.dic`)
- **In-Game Menus & QoL:**
  - Authentic *Fate/stay night REMASTERED* New Game progression (starts straight into Prologue -> Fate -> Unlimited Blade Works -> Heaven's Feel with persistent global route clear flags)
  - Interactive **Story Flowchart** screen accessible from both the Title Screen (`FLOWCHART`) and In-Game `START` Pause Menu (`Open Story Flowchart`): browse all routes (`Prologue`, `Fate`, `UBW`, `Heaven's Feel`, and `Special` bonus episodes: `Last Episode`, `Special Tiger Dojo`, `Mini Theater 1–3`), switch days, inspect choices/Tiger Dojos/endings in reading order with `[CURRENT]` and `[SEEN]` tracking persisted in `extras.json`, toggle `Unlock All` (`Select`), and jump directly into any unlocked scene or choice
  - Startup & Title configuration panel + persistent settings (`ux0:data/FateReVita/config.json`)
  - Quick-Save / `Continue` (`ux0:data/FateReVita/save0.json`)
  - `START` Pause Menu to open the Story Flowchart, switch Soundtrack or Content Mode on the fly, Quick-Save, view Backlog, or return to Title
  - Region-aware Circle / Cross confirm button detection (`sceAppUtilSystemParamGetInt`)
  - Built-in USTAR `.tar` installer on boot: extracts `ux0:data/FateReVita_base.tar` and indexes `ux0:data/FateReVita_voice.tar` in-place (`audio/voice.tar` + `audio/voice.idx`) so 28k+ voice lines stream without extracting tens of thousands of loose files onto FAT32/exFAT
- **Opening Video Playback (`playmovie`):**
  - Hardware-accelerated H.264 Baseline (`960x544`, 23.976 fps via `SceVideodec`) paired with a separate Vorbis audio track (`.h264` + `.vidx` frame index + `.ogg`) and synced to the audio clock
  - Plays `op01` (after Prologue Day 3), `op02` (Unlimited Blade Works Day 3), and `op03` (Heaven's Feel Day 3), automatically stopping and restoring BGM afterward — unlocked videos can also be replayed directly from the **Movies** tab in **CG Gallery** (or all 3 via **Unlock All**)
  - Skippable via `Cross`, `Circle`, `Start`, or touchscreen tap (`canskip`), and falls back to skipping cleanly if video files are missing or decoder initialization/allocation fails
- **Music Room & CG Gallery Extras (`ux0:data/FateReVita/extras.json`):**
  - Accessed directly from the Title Screen (`MUSIC ROOM` and `CG GALLERY`), with persistent unlock tracking across routes/sessions in `ux0:data/FateReVita/extras.json` (plus an **Extras Unlock** setting in Config — `Normal (Unlock in Game)` vs `Unlock All (Music & CGs)` — and a live `Select` shortcut inside either screen)
  - **Music Room:** Scrollable 72-track list generated in `audio/bgm_index.json`, current-track status, Play/Pause, Previous/Next unlocked track, Loop toggle, and live `New (Remastered)` / `Old (2004 / PS2)` OST switching honoring `.sli` seamless sample-accurate loops through the game's audio engine, stopping cleanly on exit
  - **CG Gallery:** 4x3 thumbnail grid (`gallery/cg_index.json` + lazy-cached `160x90` thumbnails) with `Movies` + route category tabs, direct opening video playback, locked placeholders, full-screen viewer with variant cycling (`L/R` or `Left/Right`), `Square` HUD toggle, and live `Triangle` Content Mode switching (`All-Ages` completely omits all 27 adult CG entries without even showing locked placeholders)

### Known Limitations

- The engine compiles cleanly with VitaSDK and launches on real PS Vita hardware (`FSNRV0001`), while full playthrough coverage across all late-route scenes, H-scene restoration edge cases, and Video/Flowchart/Music Room/CG Gallery interactions on real Vita / Vita3K still awaits community playtesting.
- Multiple numbered save slots are not yet implemented beyond Quick-Save (`save0.json`) + Flowchart scene jumping.
- Route endings run via the in-engine scripted `staffroll.ks` credits rather than pre-rendered ED movies.

---

## Controls (PS Vita)

| Input | Action |
| :--- | :--- |
| **D-Pad** | Navigate menus / choices / Flowchart nodes & days (`Left/Right`) / extras lists & grids / scroll Backlog |
| **Cross / Circle** | Advance dialogue / jump to Flowchart node / confirm or go back (region-aware in submenus, pause, Flowchart & extras) |
| **Triangle** | Open / close Backlog in-game; Play/Pause in Music Room; switch Content Mode in CG Gallery; Back in Flowchart |
| **Square** | Toggle dialogue box or Gallery HUD overlay; toggle New/Old OST in Music Room; Page Down in Flowchart |
| **L / R Triggers** | Hold `R` to fast-forward text in-game; switch Flowchart route tabs / Music Room tracks / CG categories & variants |
| **Start / Select** | `Start` opens Pause menu in-game (or toggles Music Room Loop); `Select` toggles Unlock All in Flowchart, Gallery & Music Room |
| **Front Touchscreen** | Advance dialogue, pick choices, tap Flowchart tabs/days/nodes, Gallery thumbnails / Music Room controls, or tap to skip videos |

---

## Repository Layout

- `vita_engine/` — Native PS Vita C++17 engine (`CMakeLists.txt`, `build_wsl.sh`, `src/`, `sce_sys/`, `third_party/`)
- `pipeline/` — Python scripts (`build_full.py`, `epk_decrypt.py`, `fcd_decrypt.py`, `fpd_stream.py`, `xp3_reader.py`, `build_slice.py`) to extract, decrypt, merge, and downscale assets for the Vita's 960x544 display

---

## Building the Vita VPK

Requires [VitaSDK](https://vitasdk.org/) installed on Linux or WSL2 (`$VITASDK`, e.g. `/usr/local/vitasdk`) with `vitaGL`, `SceShaccCg`, `freetype`, `libpng`, `libvorbisfile`, and standard VitaSDK libraries:

```bash
chmod +x vita_engine/build_wsl.sh
./vita_engine/build_wsl.sh
```

Output package: `vita_engine/build/FateReVita.vpk` (Title ID: `FSNRV0001`).

---

## Preparing Game Assets (PC)

The conversion script merges Steam REMASTERED assets (`1920x1080` -> `960x544`) with Ultimate Edition archives.

### Prerequisites

1. **Python 3.10+** with **Pillow** (`pip install pillow`) on Windows (required by the Windows DLL/executable wrappers used for `.epk` and Type-2 `.fcd` decryption).
2. **FFmpeg** (`ffmpeg` on `PATH`) for transcoding opening movies (`op01`–`op03`) into PS Vita `960x544` H.264 Baseline + `.vidx` + Vorbis `.ogg` streams under `video/`. Optional if running with `--no-video`.
3. **Legally owned game files:**
   - Steam **Fate/stay night REMASTERED** `Blade/` folder (default: `steam_files/Fate ⁄ stay night REMASTERED/Blade`, or pass `--steam <path>`)
   - **Fate/stay night [Réalta Nua] Ultimate Edition** folder containing `.xp3` archives (`patch.xp3`, `patch_lang_english.xp3`, `bgm.xp3`, `patch_h.xp3`, `patch_decensor.xp3`, `patch_hd.xp3`, `bgimage.xp3`, `fgimage.xp3`, `image.xp3`, `rule.xp3`, `sound.xp3`, `voice.xp3`) and `decensor_config.dic` (default: `ue_files/`, or pass `--ue <path>`)
4. **External decryption tools** placed under `tools/` (not included in this repository):
   - `tools/FSN-Remastered-Decompiler/keys/decryptKey.bin` (or pass `--key <path>`)
   - `tools/FSNr_tools/build/main.exe` & `SomeKey.bin`
   - `tools/FSNr_tools/scripts/fcd/fzmedia-win64vc14-release-dynamic.dll`

### Run the Asset Pipeline

```bash
python pipeline/build_full.py --steam "<path-to-Blade>" --ue "<path-to-Ultimate-Edition>"
```

Useful flags:
- `--no-voices`: Skip exporting 28k+ voice lines for fast engine tests
- `--no-video`: Skip `ffmpeg` movie transcoding (`op01`–`op03`) if `ffmpeg` is not installed or you want faster builds
- `--no-tar`: Skip automatically packing `dist/FateReVita_base.tar` and `dist/FateReVita_voice.tar`
- `--out <dir>`: Output directory (default: `vita_data_full`)
- `--workers <n>`: Thread worker count (default: `12`)

The pipeline also generates `audio/bgm_index.json`, `gallery/cg_index.json` with `160x90` JPEG thumbnails under `gallery/thumbs/`, extracts fonts/UI directly from `Blade/`, and automatically packs `dist/FateReVita_base.tar` and `dist/FateReVita_voice.tar` unless `--no-tar` is passed.

---

## Deploying to PS Vita

1. Install `FateReVita.vpk` (from [Releases](https://github.com/SEMSEM66/FateREVita/releases) or built via `./vita_engine/build_wsl.sh`) using **VitaShell**.
2. Transfer `dist/FateReVita_base.tar` and `dist/FateReVita_voice.tar` to `ux0:data/` via VitaShell USB or FTP (or copy the contents of `vita_data_full/` directly into `ux0:data/FateReVita/` if using `--no-tar`).
3. Launch **Fate/stay night REMASTERED** on the Vita — it will unpack the base files and index `voice.tar` directly on boot (`audio/voice.tar` + `audio/voice.idx`).
