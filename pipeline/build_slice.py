"""Build the PS Vita vertical-slice data bundle (Title Screen + Prologue Day 1).

Outputs clean, Vita-ready ASCII-named assets and resolved JSON scripts under
`vita_data/` without modifying the original Steam files.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import shlex
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(
    0,
    str(Path(__file__).resolve().parent.parent / "tools" / "FSN-Remastered-Decompiler"),
)

from core.epk_names import ks_to_epk_hash
from epk_decrypt import decrypt_epk_bytes
from fcd_decrypt import fcd_to_ogg
from fpd_stream import (
    PackAssetEntry,
    StreamingFPD,
    build_pack_asset_index,
    read_pack_asset,
)

ROOT = Path(__file__).resolve().parent.parent
BLADE_DIR = ROOT / "steam_files" / "Fate ⁄ stay night REMASTERED" / "Blade"
OBB_DIR = BLADE_DIR / "obb"
KEY_PATH = (
    ROOT / "tools" / "FSN-Remastered-Decompiler" / "keys" / "decryptKey.bin"
)
OUT_DIR = ROOT / "vita_data"

_CMD_ARG_RE = re.compile(r'([A-Za-z0-9_]+)=("([^"]*)"|([^\s]+))')


def ascii_id(name: str, prefix: str) -> str:
    """Deterministic pure-ASCII asset filename stem for Vita sceIoOpen safety."""
    if name.isascii() and re.fullmatch(r"[A-Za-z0-9._-]+", name):
        return name
    digest = hashlib.sha1(name.encode("utf-8")).hexdigest()[:12]
    return f"{prefix}_{digest}"


def parse_ks_args(arg_str: str) -> Dict[str, str]:
    """Parse KAG command parameters `key=value` into a dict."""
    args: Dict[str, str] = {}
    for m in _CMD_ARG_RE.finditer(arg_str):
        k = m.group(1).lower()
        v = m.group(3) if m.group(3) is not None else m.group(4)
        args[k] = v
    return args


def parse_ks_script(
    ks_text: str, epk_us: Dict[str, str], epk_jp: Dict[str, str]
) -> Dict[str, Any]:
    """Parse a .ks scenario script and resolve all $$$message_*$$$ placeholders."""
    ops: List[Dict[str, Any]] = []
    pages: Dict[str, int] = {}

    for raw_line in ks_text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("*"):
            # Page/label marker: *page0|&f.scripttitle
            label_part = line[1:].split("|", 1)[0].strip()
            pages[label_part] = len(ops)
            ops.append({"op": "label", "name": label_part})
            continue
        if line.startswith("@"):
            rest = line[1:].strip()
            if not rest:
                continue
            parts = rest.split(None, 1)
            cmd = parts[0].lower()
            args = parse_ks_args(parts[1]) if len(parts) > 1 else {}
            ops.append({"op": "cmd", "cmd": cmd, "args": args})
            continue

        # Dialogue / narration line (resolve placeholder if present)
        text = line
        if line.startswith("$$$"):
            text = epk_us.get(line) or epk_jp.get(line) or line
        ops.append({"op": "text", "raw": text})

    return {"ops": ops, "pages": pages}


def collect_referenced_assets(ops: List[Dict[str, Any]]) -> Dict[str, Set[str]]:
    """Collect all image, rule, BGM, SE, and voice asset names referenced by ops."""
    imgs: Set[str] = set()
    rules: Set[str] = set()
    bgms: Set[str] = set()
    ses: Set[str] = set()
    voices: Set[str] = set()

    for item in ops:
        if item.get("op") != "cmd":
            continue
        cmd = item["cmd"]
        args = item["args"]

        if cmd in ("play", "play_", "fadebgm", "xchgbgm", "playstop", "playstop_"):
            for key in ("file", "storage"):
                if key in args:
                    stem = Path(args[key]).stem
                    bgms.add(stem)
        elif cmd in ("se", "se_", "seloop", "seloop_", "fadese", "sestop", "sestop_"):
            for key in ("file", "storage"):
                if key in args:
                    stem = Path(args[key]).stem
                    ses.add(stem)
        elif cmd == "say":
            if "storage" in args:
                voices.add(Path(args["storage"]).stem)
        else:
            for key in ("file", "storage"):
                if key in args and cmd not in ("call", "jump", "playmovie"):
                    imgs.add(args[key])
            if "storages" in args:
                for sub in args["storages"].split(","):
                    sub = sub.strip()
                    if sub:
                        imgs.add(sub)
            if "rule" in args and args["rule"] != "クロスフェード":
                rules.add(args["rule"])

    return {
        "images": imgs,
        "rules": rules,
        "bgm": bgms,
        "se": ses,
        "voice": voices,
    }


def save_resized_png(
    raw_bytes: bytes, dest: Path, scale: float = 0.5, grayscale: bool = False
) -> Tuple[int, int]:
    """Decode WebP/PNG bytes, resize cleanly for Vita, and write PNG."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(io.BytesIO(raw_bytes)) as im:
        if grayscale:
            im = im.convert("L")
        elif im.mode not in ("RGB", "RGBA"):
            im = im.convert("RGBA")
        if scale != 1.0:
            nw = max(1, round(im.width * scale))
            nh = max(1, round(im.height * scale))
            im = im.resize((nw, nh), Image.Resampling.LANCZOS)
        im.save(dest, format="PNG", optimize=False)
        return im.size


def main() -> None:
    key = KEY_PATH.read_bytes()
    print("[1/6] Indexing FPD containers...")
    pack_idx = build_pack_asset_index(OBB_DIR, key)
    pack_lower: Dict[Tuple[str, str], PackAssetEntry] = {
        (k[0].lower(), k[1].lower()): v for k, v in pack_idx.items()
    }

    m_fpd_order = [
        StreamingFPD(OBB_DIR / "pack00m.bin", key),
        StreamingFPD(OBB_DIR / "patch00m.bin", key),
        StreamingFPD(OBB_DIR / "pack00d.bin", key),
        StreamingFPD(OBB_DIR / "patch00d.bin", key),
    ]
    boot_fpd = StreamingFPD(BLADE_DIR / "boot.dat", key)

    m_entries: Dict[str, Tuple[StreamingFPD, Any]] = {}
    for fpd in [boot_fpd] + m_fpd_order:
        for e in fpd.entries:
            m_entries[e.filepath] = (fpd, e)

    def read_m(path: str) -> Optional[bytes]:
        pair = m_entries.get(path)
        if not pair:
            return None
        return pair[0].read_entry(pair[1])

    def find_pack(name: str, exts: Tuple[str, ...]) -> Optional[PackAssetEntry]:
        for ext in exts:
            hit = pack_idx.get((name, ext)) or pack_lower.get(
                (name.lower(), ext.lower())
            )
            if hit:
                return hit
        return None

    # 2. Config + Fonts
    print("[2/6] Extracting config JSONs, EPK UI strings, and fonts...")
    cfg_dir = OUT_DIR / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    for cfg_name in [
        "KAGConfig.json",
        "KAGColorConfig.json",
        "KAGTextConfig.json",
        "correct.json",
    ]:
        data = read_m(f"root/data/fuzz/release/root/resident/{cfg_name}")
        if data is not None:
            (cfg_dir / cfg_name).write_bytes(data)

    for epk_name in ("uistring", "statictext"):
        raw = read_m(f"root/data/locale/us/epk/{epk_name}.epk")
        if raw:
            parsed = decrypt_epk_bytes(raw, epk_name)
            (cfg_dir / f"{epk_name}_us.json").write_text(
                json.dumps(parsed, ensure_ascii=False, indent=2), encoding="utf-8"
            )

    fonts_dir = OUT_DIR / "fonts"
    fonts_dir.mkdir(parents=True, exist_ok=True)
    font_manifest: Dict[str, str] = {}
    for font_name in [
        "HelveticaNeueWorld-55Roman.otf",
        "FOT-TsukuMinPr6N-M.otf",
        "FOT-UDKakugo_LargePr6-M.otf",
    ]:
        fdata = read_m(f"root/data/fuzz/release/root/resident/gui/font/{font_name}")
        if fdata:
            (fonts_dir / font_name).write_bytes(fdata)
            font_manifest[font_name] = f"fonts/{font_name}"

    # 3. Parse Prologue Day 1 script + FCF
    print("[3/6] Decrypting and compiling Prologue Day 1 script...")
    scripts_dir = OUT_DIR / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)

    ks_name = "プロローグ1日目"
    ks_raw = read_m(f"root/data/kag/{ks_name}.ks")
    if ks_raw is None:
        raise FileNotFoundError(f"Missing {ks_name}.ks")
    ks_text = ks_raw.decode("utf-8", errors="replace")

    h = ks_to_epk_hash(ks_name)
    epk_us_raw = read_m(f"root/data/locale/us/epk/{h}.epk")
    epk_jp_raw = read_m(f"root/data/epk/{h}.epk")
    epk_us = decrypt_epk_bytes(epk_us_raw, h) if epk_us_raw else {}
    epk_jp = decrypt_epk_bytes(epk_jp_raw, h) if epk_jp_raw else {}

    compiled = parse_ks_script(ks_text, epk_us, epk_jp)
    (scripts_dir / "prologue_day1.json").write_text(
        json.dumps(compiled, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    fcf_raw = read_m("root/data/kag/prologue.fcf")
    if fcf_raw:
        (scripts_dir / "prologue.fcf").write_bytes(fcf_raw)

    refs = collect_referenced_assets(compiled["ops"])

    # 4. Convert Images & Rules for Prologue Day 1
    print(
        f"[4/6] Converting {len(refs['images'])} images and {len(refs['rules'])} rules (0.5x for 960x544)..."
    )
    manifest: Dict[str, Any] = {
        "slice": "title_and_prologue_day1",
        "vita_resolution": [960, 544],
        "source_resolution": [1920, 1080],
        "fonts": font_manifest,
        "images": {},
        "rules": {},
        "ui": {},
        "bgm": {},
        "se": {},
        "voice": {},
        "missing": [],
    }

    img_dir = OUT_DIR / "images" / "scene"
    for name in sorted(refs["images"]):
        aid = ascii_id(name, "img")
        dest = img_dir / f"{aid}.png"
        asset = find_pack(name, ("webp", "png"))
        if asset:
            raw = read_pack_asset(asset, key)
            w, h_px = save_resized_png(raw, dest, scale=0.5, grayscale=False)
            manifest["images"][name] = {
                "path": f"images/scene/{aid}.png",
                "w": w,
                "h": h_px,
            }
        else:
            manifest["missing"].append(f"img:{name}")

    rule_dir = OUT_DIR / "images" / "rules"
    for name in sorted(refs["rules"]):
        aid = ascii_id(name, "rule")
        dest = rule_dir / f"{aid}.png"
        asset = find_pack(name, ("webp", "png"))
        if asset:
            raw = read_pack_asset(asset, key)
            w, h_px = save_resized_png(raw, dest, scale=0.5, grayscale=True)
            manifest["rules"][name] = {
                "path": f"images/rules/{aid}.png",
                "w": w,
                "h": h_px,
            }
        else:
            manifest["missing"].append(f"rule:{name}")

    # 5. Convert UI / Title / Textbox / Boot assets
    print("[5/6] Converting Title Screen, Boot Logo, and Textbox UI assets...")
    ui_dir = OUT_DIR / "images" / "ui"
    ui_pack_items = {
        "tw_normal_us": ("tw_ノーマル(通常)_us", 0.5),
        "tw_interlude_us": ("tw_インタールード_us", 0.5),
        "tw_normal_thin": ("tw_ノーマル(薄い)", 0.5),
    }
    for alias, (pname, scale) in ui_pack_items.items():
        asset = find_pack(pname, ("webp", "png"))
        if asset:
            dest = ui_dir / f"{alias}.png"
            w, h_px = save_resized_png(
                read_pack_asset(asset, key), dest, scale=scale
            )
            manifest["ui"][alias] = {"path": f"images/ui/{alias}.png", "w": w, "h": h_px}

    # UI textures from pack00m/patch00m/boot.dat
    ui_m_items = [
        ("boot_attention_us", "root/data/bootlogo/attention_us.png", 0.5),
        ("title_bg_01", "root/data/fuzz/release/root/resident/gui/textures/02_title/title_01.png", 0.75),
        ("title_bg_02", "root/data/fuzz/release/root/resident/gui/textures/02_title/title_02.png", 0.75),
        ("title_bg_03", "root/data/fuzz/release/root/resident/gui/textures/02_title/title_03.png", 0.75),
        ("title_logo", "root/data/fuzz/release/root/resident/gui/textures/02_title/mainlogo.png", 0.75),
        ("title_press_1", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_press_1.png", 0.75),
        ("title_press_2", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_press_2.png", 0.75),
        ("btn_newgame_1", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_newgame_1.png", 1.0),
        ("btn_newgame_2", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_newgame_2.png", 1.0),
        ("btn_continue_1", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_continue_1.png", 1.0),
        ("btn_continue_2", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_continue_2.png", 1.0),
        ("btn_load_1", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_load_1.png", 1.0),
        ("btn_load_2", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_load_2.png", 1.0),
        ("btn_config_1", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_config_1.png", 1.0),
        ("btn_config_2", "root/data/fuzz/release/root/resident/gui/textures/02_title/t_config_2.png", 1.0),
        ("linebreak_a", "root/data/fuzz/release/root/resident/LineBreak_a.png", 0.5),
        ("pagebreak_a", "root/data/fuzz/release/root/resident/PageBreak_a.png", 0.5),
    ]
    for alias, mpath, scale in ui_m_items:
        raw = read_m(mpath)
        if raw:
            dest = ui_dir / f"{alias}.png"
            w, h_px = save_resized_png(raw, dest, scale=scale)
            manifest["ui"][alias] = {"path": f"images/ui/{alias}.png", "w": w, "h": h_px}

    # 6. Decrypt Audio (BGM + SE + Voices)
    # Include bgm01 (title theme) + UI title SEs in addition to Prologue Day 1 audio
    all_bgms = set(refs["bgm"]) | {"bgm01"}
    print(
        f"[6/6] Decrypting audio to Ogg Vorbis: {len(all_bgms)} BGMs, {len(refs['se'])} SEs, {len(refs['voice'])} voices..."
    )

    for cat, names in [
        ("bgm", sorted(all_bgms)),
        ("se", sorted(refs["se"])),
        ("voice", sorted(refs["voice"])),
    ]:
        out_sub = OUT_DIR / "audio" / cat
        out_sub.mkdir(parents=True, exist_ok=True)
        for name in names:
            aid = ascii_id(name, cat)
            dest = out_sub / f"{aid}.ogg"
            asset = find_pack(name, ("fcd",))
            if asset:
                ogg = fcd_to_ogg(read_pack_asset(asset, key))
                dest.write_bytes(ogg)
                manifest[cat][name] = f"audio/{cat}/{aid}.ogg"
            else:
                manifest["missing"].append(f"{cat}:{name}")

    # Title UI sound effects from pack00m
    for alias, mpath in [
        ("se_title_decide", "root/data/fuzz/release/root/resident/sound/se14_decide_title.fcd"),
        ("se_title_move", "root/data/fuzz/release/root/resident/sound/se16_btnon_title.fcd"),
    ]:
        raw = read_m(mpath)
        if raw:
            ogg = fcd_to_ogg(raw)
            dest = OUT_DIR / "audio" / "se" / f"{alias}.ogg"
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(ogg)
            manifest["se"][alias] = f"audio/se/{alias}.ogg"

    (OUT_DIR / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        f"Done! Output in {OUT_DIR} | "
        f"images={len(manifest['images'])}, rules={len(manifest['rules'])}, "
        f"ui={len(manifest['ui'])}, bgm={len(manifest['bgm'])}, "
        f"se={len(manifest['se'])}, voice={len(manifest['voice'])}, "
        f"missing={len(manifest['missing'])}"
    )
    if manifest["missing"]:
        print("Missing:", manifest["missing"])


if __name__ == "__main__":
    main()
