# FateREVita

A native PS Vita port and PC asset pipeline for **Fate/stay night REMASTERED** (2024 Steam) combined with **Fate/stay night [Réalta Nua] Ultimate Edition** restoration patches.

> **Disclaimer:** This repository contains **only** the custom C++17 / `vitaGL` Vita engine (`vita_engine/`) and Python asset-conversion scripts (`pipeline/`). No copyrighted game assets, scripts, audio, keys, or third-party binaries are included.

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
  - Startup configuration panel & persistent settings (`ux0:data/FateReVita/config.json`)
  - Quick-Save / `Continue` (`ux0:data/FateReVita/save0.json`)
  - `START` Pause Menu to switch Soundtrack or Content Mode on the fly, Quick-Save, view Backlog, or return to Title
  - Chapter / Route quick-start presets in New Game setup
  - Region-aware Circle / Cross confirm button detection (`sceAppUtilSystemParamGetInt`)
  - Built-in USTAR `.tar` installer on boot: extracts `ux0:data/FateReVita_base.tar` and indexes `ux0:data/FateReVita_voice.tar` in-place (`audio/voice.tar` + `audio/voice.idx`) so 28k+ voice lines stream without extracting tens of thousands of loose files onto FAT32/exFAT

### Known Limitations

- Video playback (`playmovie` / OP & ED movies) is skipped.
- Standalone CG Gallery and Music Room extras are not implemented.

---

## Controls (PS Vita)

| Input | Action |
| :--- | :--- |
| **D-Pad** | Navigate menus / choices / scroll Backlog |
| **Cross / Circle** | Advance dialogue / confirm selection (region-aware in submenus & pause) |
| **Triangle** | Open / close Backlog (when not in a choice) |
| **Square** | Toggle dialogue box visibility |
| **R Trigger (Hold)** | Skip text |
| **Start** | Open / close Pause menu |
| **Front Touchscreen** | Tap to advance text or pick choices |

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
2. **Legally owned game files:**
   - Steam **Fate/stay night REMASTERED** `Blade/` folder (default: `steam_files/Fate ⁄ stay night REMASTERED/Blade`, or pass `--steam <path>`)
   - **Fate/stay night [Réalta Nua] Ultimate Edition** folder containing `.xp3` archives (`patch.xp3`, `patch_lang_english.xp3`, `bgm.xp3`, `patch_h.xp3`, `patch_decensor.xp3`, `patch_hd.xp3`, `bgimage.xp3`, `fgimage.xp3`, `image.xp3`, `rule.xp3`, `sound.xp3`, `voice.xp3`) and `decensor_config.dic` (default: `ue_files/`, or pass `--ue <path>`)
3. **External decryption tools** placed under `tools/` (not included in this repository):
   - `tools/FSN-Remastered-Decompiler/keys/decryptKey.bin` (or pass `--key <path>`)
   - `tools/FSNr_tools/build/main.exe` & `SomeKey.bin`
   - `tools/FSNr_tools/scripts/fcd/fzmedia-win64vc14-release-dynamic.dll`

### Run the Asset Pipeline

```bash
python pipeline/build_full.py --steam "<path-to-Blade>" --ue "<path-to-Ultimate-Edition>"
```

Useful flags:
- `--no-voices`: Skip exporting 28k+ voice lines for fast engine tests
- `--out <dir>`: Output directory (default: `vita_data_full`)
- `--workers <n>`: Thread worker count (default: `12`)

---

## Deploying to PS Vita

1. Install `FateReVita.vpk` using **VitaShell**.
2. Transfer `vita_data_full/` to `ux0:data/FateReVita/`:
   - **Direct copy (USB / FTP):** Copy the contents of `vita_data_full/` directly into `ux0:data/FateReVita/`.
   - **Fast `.tar` installer:** Pack uncompressed POSIX USTAR archives `FateReVita_base.tar` (everything except `audio/voice/`) and `FateReVita_voice.tar` (`audio/voice/*.ogg` / `.wav`) into `ux0:data/` via VitaShell FTP/USB, then launch **Fate/stay night REMASTERED** on the Vita — it will unpack the base files and index `voice.tar` directly on boot.
