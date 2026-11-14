"""Full-game asset, script, and flowchart compiler for FateReVita.

Combines:
- Official 2024 Steam Fate/stay night REMASTERED (All-Ages scripts, US English EPKs,
  HD 16:9 visuals, New OST, SE, Japanese voices)
- Ultimate Edition v0.9.x archives (Old 2004/PS2 OST + .sli loop points, 18+ restored
  scripts & flowcharts, 18+ Mosaic H-CGs, 18+ Uncensored/Demosaic CGs via decensor_config.dic)

Usage:
  python pipeline/build_full.py --no-voices     # Fast test build (all assets except 28k voices)
  python pipeline/build_full.py                 # Full game build (~1.8 GB total)
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from PIL import Image

PIPELINE_DIR = Path(__file__).resolve().parent
ROOT = PIPELINE_DIR.parent
sys.path.insert(0, str(PIPELINE_DIR))

from epk_decrypt import decrypt_epk_bytes
from fcd_decrypt import fcd_to_ogg
from fpd_stream import StreamingFPD, build_pack_asset_index, read_pack_asset
from xp3_reader import XP3Archive, decode_text, parse_decensor_dic, parse_sli

STEAM_DEFAULT = ROOT / "steam_files" / "Fate ⁄ stay night REMASTERED" / "Blade"
UE_DEFAULT = next(
    (
        p
        for p in (
            ROOT / "ue_files",
            *(sorted(ROOT.parent.glob("*Ultimate*Edition*")) if ROOT.parent.exists() else []),
        )
        if p.exists()
    ),
    ROOT / "ue_files",
)
KEY_DEFAULT = ROOT / "tools" / "FSN-Remastered-Decompiler" / "keys" / "decryptKey.bin"

CMD_ARG_RE = re.compile(r'([A-Za-z0-9_]+)=("([^"]*)"|\'([^\']*)\'|([^\s]+))')
PLACEHOLDER_RE = re.compile(r"\$\$\$[^$]+\$\$\$")


def ascii_id(prefix: str, key: str) -> str:
    h = hashlib.md5(key.encode("utf-8")).hexdigest()[:12]
    return f"{prefix}_{h}"


def parse_args_str(arg_str: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for m in CMD_ARG_RE.finditer(arg_str):
        val = (
            m.group(3)
            if m.group(3) is not None
            else (m.group(4) if m.group(4) is not None else m.group(5))
        )
        out[m.group(1).lower()] = val
    return out


def eval_ue_cond(expr: str, ctx: Dict[str, object]) -> bool:
    """Evaluate Ultimate Edition KAG @if / @elsif expressions for 18+ Mixed mode."""
    s = expr.strip()
    if not s:
        return True
    if "voiceSetting" in s or "playUnmatchedVoices" in s:
        return "sf.gtext=='realta'" not in s and "sf.gtext == 'realta'" not in s

    # Replace TJS field/dict accesses with Python ctx lookups
    py_expr = s
    py_expr = re.sub(r"sf\['([^']+)'\]", lambda m: f"sf.get({m.group(1)!r}, False)", py_expr)
    py_expr = re.sub(r"sf\.([A-Za-z0-9_]+)", lambda m: f"sf.get({m.group(1)!r}, False)", py_expr)
    py_expr = re.sub(r"f\['([^']+)'\]", lambda m: f"f.get({m.group(1)!r}, 0)", py_expr)
    py_expr = re.sub(r"f\.([A-Za-z0-9_]+)", lambda m: f"f.get({m.group(1)!r}, 0)", py_expr)
    py_expr = py_expr.replace("&&", " and ").replace("||", " or ")
    py_expr = re.sub(r"!(?!=)", " not ", py_expr)
    py_expr = py_expr.replace("true", "True").replace("false", "False")
    try:
        return bool(eval(py_expr, {"__builtins__": {}}, ctx))
    except Exception:
        # Default conservatively on unknown non-trivial guards
        if "ghentai" in s or "gmature" in s:
            return not s.startswith("!")
        return False


def filter_kag_conditionals(lines: List[str], ue_ctx: Optional[Dict[str, object]]) -> List[str]:
    """Resolve @if / @elsif / @else / @endif when compiling static KAG scripts."""
    if ue_ctx is None:
        return lines

    # Stack entries: [branch_active, any_branch_taken]
    stack: List[List[bool]] = []
    out: List[str] = []

    def is_active() -> bool:
        return all(entry[0] for entry in stack)

    for raw_ln in lines:
        s = raw_ln.strip()
        if s.startswith("@if "):
            args = parse_args_str(s[4:])
            cond_res = eval_ue_cond(args.get("exp", ""), ue_ctx)
            parent_on = is_active()
            active = parent_on and cond_res
            stack.append([active, cond_res])
            continue
        if s.startswith("@elsif "):
            if not stack:
                continue
            args = parse_args_str(s[7:])
            parent_on = all(entry[0] for entry in stack[:-1])
            already = stack[-1][1]
            if already:
                stack[-1][0] = False
            else:
                cond_res = eval_ue_cond(args.get("exp", ""), ue_ctx)
                stack[-1][0] = parent_on and cond_res
                if cond_res:
                    stack[-1][1] = True
            continue
        if s == "@else" or s.startswith("@else "):
            if not stack:
                continue
            parent_on = all(entry[0] for entry in stack[:-1])
            already = stack[-1][1]
            stack[-1][0] = parent_on and (not already)
            stack[-1][1] = True
            continue
        if s == "@endif" or s.startswith("@endif "):
            if stack:
                stack.pop()
            continue

        if is_active():
            out.append(raw_ln)

    return out


def parse_ks_script(
    raw_text: str,
    msgs: Optional[Dict[str, str]],
    coord_base: Tuple[int, int],
    ue_ctx: Optional[Dict[str, object]] = None,
) -> dict:
    """Compile a KAG .ks file into normalized JSON VM ops."""
    lines = raw_text.splitlines()
    lines = filter_kag_conditionals(lines, ue_ctx)

    ops: List[dict] = []
    labels: Dict[str, int] = {}

    for line in lines:
        s = line.strip()
        if not s or s.startswith(";"):
            continue
        if s.startswith("*"):
            lbl = s[1:].split("|", 1)[0].strip()
            if lbl:
                labels[lbl] = len(ops)
                ops.append({"op": "label", "name": lbl})
            continue
        if s.startswith("@"):
            parts = s[1:].split(None, 1)
            if not parts:
                continue
            cmd = parts[0].lower()
            if cmd in ("if", "elsif", "else", "endif"):
                # Stray unmatched conditional (e.g. Steam セイバーエピローグ.ks line 1505)
                continue
            arg_str = parts[1] if len(parts) > 1 else ""
            args = parse_args_str(arg_str)
            if not args and arg_str and cmd == "eval":
                args = {"exp": arg_str}
            ops.append({"op": "cmd", "cmd": cmd, "args": args})
            continue

        if msgs is not None:
            resolved = PLACEHOLDER_RE.sub(lambda m: msgs.get(m.group(0), m.group(0)), s)
        else:
            # Strip indent spaces at start of UE English lines while keeping leading dialogue indent tidy
            resolved = re.sub(r"^\s+", "", s)

        if not resolved:
            continue

        # Handle inline [p] / [pg] page-break markers inside localization strings
        if "[p]" in resolved:
            before, _sep, after = resolved.partition("[p]")
            if before.strip():
                ops.append({"op": "text", "raw": before})
            ops.append({"op": "cmd", "cmd": "pg", "args": {}})
            if after.strip():
                ops.append({"op": "text", "raw": after})
        else:
            ops.append({"op": "text", "raw": resolved})

    return {
        "coord_base": [coord_base[0], coord_base[1]],
        "ops": ops,
        "labels": labels,
    }


def collect_script_assets(
    script_json: dict,
    images_out: Set[str],
    rules_out: Set[str],
    bgm_out: Set[str],
    se_out: Set[str],
    voice_out: Set[str],
) -> None:
    def add_img_val(val: str) -> None:
        for tok in val.split(","):
            t = tok.strip()
            if not t or t.startswith("&"):
                continue
            if t in ("black", "white"):
                images_out.add(t)
                continue
            # Strip known image extensions if specified explicitly
            t = re.sub(r"\.(png|webp|bmp|jpg)$", "", t, flags=re.I)
            images_out.add(t)

    for op in script_json["ops"]:
        if op["op"] != "cmd":
            continue
        cmd = op["cmd"]
        args = op["args"]

        if "rule" in args and args["rule"] and not args["rule"].startswith("&"):
            rname = re.sub(r"\.(png|webp|bmp)$", "", args["rule"], flags=re.I)
            if rname != "クロスフェード":
                rules_out.add(rname)

        if cmd in ("play",):
            if "file" in args:
                bgm_out.add(Path(args["file"]).stem.lower())
            if "storage" in args:
                bgm_out.add(Path(args["storage"]).stem.lower())
        elif cmd in ("se", "seloop", "se_"):
            if "file" in args and not args["file"].startswith("&"):
                se_out.add(Path(args["file"]).stem.lower())
            if "storage" in args and not args["storage"].startswith("&"):
                se_out.add(Path(args["storage"]).stem.lower())
        elif cmd == "say":
            if "storage" in args and not args["storage"].startswith("&"):
                voice_out.add(Path(args["storage"]).stem.lower())
        elif cmd in (
            "fadein",
            "fadeout",
            "bg",
            "image",
            "imageex",
            "fg",
            "ld",
            "ld_auto",
            "ld_notrans",
            "dash",
            "dashcombo",
            "dashcombot",
            "splinemovecombo",
            "rep",
            "chgfg",
            "superpose",
            "imgchg",
            "env",
            "flash",
            "move",
        ):
            for ak in ("file", "storage", "storages", "bg", "fg", "l", "lc", "c", "rc", "r"):
                if ak in args and args[ak]:
                    add_img_val(args[ak])
        elif cmd == "ldall":
            # ldall uses character expressions or positions; scan any file-looking values
            for ak in ("storage", "file", "bg"):
                if ak in args and args[ak]:
                    add_img_val(args[ak])


def parse_fcf_content(
    txt: str, msgs: Optional[Dict[str, str]]
) -> dict:
    lines = txt.splitlines()
    title = lines[0].strip() if lines else ""
    nodes: Dict[str, dict] = {}
    start_id = 0
    seen_ids: List[int] = []

    def sub_msg(s: str) -> str:
        if msgs is None:
            return s
        return PLACEHOLDER_RE.sub(lambda m: msgs.get(m.group(0), m.group(0)), s)

    def parse_flag_expr(token: str) -> dict:
        # Format: <flag>//<v0>//<v1>
        parts = token.split("//")
        return {
            "flag": parts[0],
            "v0": int(parts[1]) if len(parts) > 1 and parts[1] else 0,
            "v1": int(parts[2]) if len(parts) > 2 and parts[2] else 0,
        }

    for ln in lines[1:]:
        if ";" not in ln:
            continue
        _nid_s, rest = ln.split(";", 1)
        pm = rest.split("'")
        if len(pm) < 2:
            continue
        ntype = pm[0]
        if ntype in ("ROUTE", "OPTION"):
            continue
        nid = int(pm[1])
        seen_ids.append(nid)

        if ntype == "SCENE":
            stitle = sub_msg(pm[10]) if len(pm) > 10 else ""
            nj = int(pm[11]) if len(pm) > 11 and pm[11] else 0
            jumps = []
            for j in range(nj):
                jraw = pm[12 + j]
                # <cond_count>:<cond0>:...:<link>:<target_id>[:<route_idx>]
                jp = jraw.split(":")
                cond_cnt = int(jp[0]) if jp[0] else 0
                conds = [parse_flag_expr(jp[1 + c]) for c in range(cond_cnt)]
                link = int(jp[1 + cond_cnt]) if len(jp) > 1 + cond_cnt and jp[1 + cond_cnt] else 1
                target = int(jp[2 + cond_cnt]) if len(jp) > 2 + cond_cnt and jp[2 + cond_cnt] else -1
                jumps.append({"link": link, "target": target, "conds": conds})

            flag_ops = []
            fop_raw = pm[12 + nj] if len(pm) > 12 + nj else "0"
            if fop_raw and fop_raw != "0":
                fp = fop_raw.split(":")
                ocnt = int(fp[0]) if fp[0] else 0
                for o in range(ocnt):
                    flag_ops.append(parse_flag_expr(fp[1 + o]))

            nodes[str(nid)] = {
                "id": nid,
                "type": "SCENE",
                "title": stitle,
                "jumps": jumps,
                "flag_ops": flag_ops,
            }
        elif ntype == "SELECTER":
            prompt = sub_msg(pm[10]) if len(pm) > 10 else ""
            na = int(pm[11]) if len(pm) > 11 and pm[11] else 0
            alts = []
            for j in range(na):
                base = 12 + j * 3
                if base + 2 < len(pm):
                    alts.append({
                        "target": int(pm[base]),
                        "text": sub_msg(pm[base + 2]),
                    })
            nodes[str(nid)] = {
                "id": nid,
                "type": "SELECTER",
                "title": prompt,
                "choices": alts,
            }
        elif ntype == "OUTERLABEL":
            dest_file = pm[10] if len(pm) > 10 else ""
            dest_node = int(pm[11]) if len(pm) > 11 and pm[11] else 0
            ltitle = sub_msg(pm[12]) if len(pm) > 12 else ""
            nodes[str(nid)] = {
                "id": nid,
                "type": "OUTERLABEL",
                "file": dest_file,
                "target": dest_node,
                "title": ltitle,
            }

    if "0" not in nodes and seen_ids:
        start_id = seen_ids[0]

    return {"title": title, "start": start_id, "nodes": nodes}


def overlay_steam_titles_on_ue_fcf(ue_fcf: dict, steam_fcf: Optional[dict]) -> dict:
    """Use crisp 2024 Steam official titles/choices on UE nodes where text matches node IDs."""
    if not steam_fcf:
        return ue_fcf
    snodes = steam_fcf["nodes"]
    for nid_str, node in ue_fcf["nodes"].items():
        if nid_str in snodes:
            snode = snodes[nid_str]
            if node["type"] == snode["type"]:
                if snode.get("title"):
                    node["title"] = snode["title"]
                if node["type"] == "SELECTER" and len(node["choices"]) == len(snode["choices"]):
                    for idx, ch in enumerate(node["choices"]):
                        if snode["choices"][idx]["text"]:
                            ch["text"] = snode["choices"][idx]["text"]
    return ue_fcf


def has_meaningful_alpha(im: Image.Image) -> bool:
    if im.mode in ("RGBA", "LA"):
        alpha = im.getchannel("A")
        lo, _hi = alpha.getextrema()
        return lo < 252
    if im.mode == "P":
        return "transparency" in im.info
    return False


def export_image_bytes(
    raw: bytes,
    out_path_no_ext: Path,
    scale: float,
    force_png: bool = False,
) -> Tuple[str, int, int]:
    """Decode raw webp/png/bmp, scale, write JPEG (opaque) or PNG (alpha), return (ext, w, h)."""
    with Image.open(io.BytesIO(raw)) as im:
        nw = max(1, round(im.width * scale))
        nh = max(1, round(im.height * scale))
        if (nw, nh) != im.size:
            im = im.resize((nw, nh), Image.Resampling.LANCZOS)

        use_png = force_png or has_meaningful_alpha(im)
        if use_png:
            if im.mode not in ("RGBA", "L", "RGB"):
                im = im.convert("RGBA")
            dest = out_path_no_ext.with_suffix(".png")
            im.save(dest, format="PNG", optimize=True)
            return ".png", im.width, im.height
        else:
            if im.mode != "RGB":
                im = im.convert("RGB")
            dest = out_path_no_ext.with_suffix(".jpg")
            im.save(dest, format="JPEG", quality=90, optimize=True)
            return ".jpg", im.width, im.height


def export_rule_bytes(raw: bytes, dest_png: Path) -> Tuple[int, int]:
    with Image.open(io.BytesIO(raw)) as im:
        if im.width > 1024:
            nw = max(1, round(im.width * 0.5))
            nh = max(1, round(im.height * 0.5))
            im = im.resize((nw, nh), Image.Resampling.LANCZOS)
        if im.mode != "L":
            im = im.convert("L")
        im.save(dest_png, format="PNG", optimize=True)
        return im.width, im.height


def decrypt_all_us_epks(
    p00m: StreamingFPD, p00m_p: StreamingFPD, cache_path: Path, workers: int
) -> Dict[str, str]:
    if cache_path.exists():
        print(f"[EPK] Loading cached US localization strings from {cache_path}")
        return json.loads(cache_path.read_text(encoding="utf-8"))

    epks: Dict[str, Tuple[StreamingFPD, object]] = {}
    for arc in (p00m, p00m_p):
        for e in arc.entries:
            if e.filepath.startswith("root/data/locale/us/epk/") and e.filepath.endswith(".epk"):
                epks[Path(e.filepath).stem] = (arc, e)

    print(f"[EPK] Decrypting {len(epks)} Steam US localization EPKs ({workers} workers)...")

    def dec(item):
        stem, (arc, ent) = item
        return decrypt_epk_bytes(arc.read_entry(ent), stem)

    merged: Dict[str, str] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        for res in ex.map(dec, epks.items()):
            merged.update(res)

    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(merged, ensure_ascii=False), encoding="utf-8")
    print(f"[EPK] Cached {len(merged)} US strings.")
    return merged


def copy_fonts_and_ui(out_dir: Path) -> Tuple[Dict[str, str], Dict[str, dict]]:
    """Reuse clean pre-extracted Vita fonts & UI from vita_data/."""
    src_manifest = ROOT / "vita_data" / "manifest.json"
    if not src_manifest.exists():
        raise FileNotFoundError(f"Expected {src_manifest} for UI & font extraction.")
    sm = json.loads(src_manifest.read_text(encoding="utf-8"))
    fonts: Dict[str, str] = {}
    ui: Dict[str, dict] = {}
    (out_dir / "fonts").mkdir(parents=True, exist_ok=True)
    (out_dir / "images" / "ui").mkdir(parents=True, exist_ok=True)

    for k, rel in sm["fonts"].items():
        src = ROOT / "vita_data" / rel
        dst = out_dir / rel
        shutil.copy2(src, dst)
        fonts[k] = rel

    for k, info in sm["ui"].items():
        rel = info["path"]
        src = ROOT / "vita_data" / rel
        dst = out_dir / rel
        shutil.copy2(src, dst)
        ui[k] = dict(info)

    return fonts, ui


def decode_steam_audio(raw: bytes) -> Tuple[bytes, str]:
    if raw[:4] == b"OggS":
        return raw, ".ogg"
    if raw[:4] == b"RIFF":
        return raw, ".wav"
    return fcd_to_ogg(raw), ".ogg"


def main() -> None:
    parser = argparse.ArgumentParser(description="Build full-game Vita assets + UE restoration.")
    parser.add_argument("--steam", type=Path, default=STEAM_DEFAULT)
    parser.add_argument("--ue", type=Path, default=UE_DEFAULT)
    parser.add_argument("--key", type=Path, default=KEY_DEFAULT)
    parser.add_argument("--out", type=Path, default=ROOT / "vita_data_full")
    parser.add_argument("--no-voices", action="store_true", help="Skip 28k voice exports for rapid tests")
    parser.add_argument("--workers", type=int, default=12)
    args = parser.parse_args()

    out = args.out
    for sub in (
        "scripts",
        "scripts_h",
        "flowcharts",
        "flowcharts_h",
        "images/scene",
        "images/h_orig",
        "images/h_demosaic",
        "images/rules",
        "audio/bgm",
        "audio/bgm_old",
        "audio/se",
        "audio/voice",
        "config",
        ".cache",
    ):
        (out / sub).mkdir(parents=True, exist_ok=True)

    key = args.key.read_bytes()
    obb = args.steam / "obb"
    p00m = StreamingFPD(obb / "pack00m.bin", key)
    p00m_p = StreamingFPD(obb / "patch00m.bin", key)

    print("[INIT] Indexing Steam pack & patch archives...")
    pack_idx = build_pack_asset_index(obb, key)
    pack_lower = {(k[0].lower(), k[1].lower()): v for k, v in pack_idx.items()}

    print("[INIT] Indexing Ultimate Edition XP3 archives...")
    ue_patch = XP3Archive(args.ue / "patch.xp3")
    ue_eng = XP3Archive(args.ue / "patch_lang_english.xp3")
    ue_bgm = XP3Archive(args.ue / "bgm.xp3")
    ue_ph = XP3Archive(args.ue / "patch_h.xp3")
    ue_pdec = XP3Archive(args.ue / "patch_decensor.xp3")
    ue_phd = XP3Archive(args.ue / "patch_hd.xp3")
    ue_bg = XP3Archive(args.ue / "bgimage.xp3")
    ue_fg = XP3Archive(args.ue / "fgimage.xp3")
    ue_im = XP3Archive(args.ue / "image.xp3")
    ue_rule = XP3Archive(args.ue / "rule.xp3")
    ue_sound = XP3Archive(args.ue / "sound.xp3")
    ue_voice = XP3Archive(args.ue / "voice.xp3")
    decensor_cfg = parse_decensor_dic(args.ue / "decensor_config.dic")

    us_msgs = decrypt_all_us_epks(p00m, p00m_p, out / ".cache" / "us_epk_strings.json", args.workers)

    # Collect Steam .ks & .fcf
    steam_ks: Dict[str, Tuple[StreamingFPD, object]] = {}
    steam_fcf_raw: Dict[str, str] = {}
    for arc in (p00m, p00m_p):
        for e in arc.entries:
            if e.filepath.startswith("root/data/kag/"):
                rel = e.filepath.split("root/data/kag/", 1)[1]
                if rel.endswith(".ks"):
                    steam_ks[rel] = (arc, e)
                elif rel.endswith(".fcf"):
                    steam_fcf_raw[rel] = arc.read_entry(e).decode("utf-8-sig", errors="replace")

    ref_images: Set[str] = {"typemoon", "typemoon_b", "black", "white", "トゥルーエンド"}
    ref_rules: Set[str] = set()
    ref_bgm: Set[str] = {"bgm01"}
    ref_se: Set[str] = set()
    ref_voice: Set[str] = set()

    # --- 1. Compile Steam All-Ages scripts ---
    print(f"[SCRIPTS] Compiling {len(steam_ks)} Steam REMASTERED scripts...")
    manifest_scripts: Dict[str, str] = {}
    for ks_rel, (arc, ent) in sorted(steam_ks.items()):
        stem = ks_rel[:-3]
        raw = arc.read_entry(ent).decode("utf-8-sig", errors="replace")
        sc = parse_ks_script(raw, us_msgs, (1920, 1080), ue_ctx=None)
        collect_script_assets(sc, ref_images, ref_rules, ref_bgm, ref_se, ref_voice)
        fname = f"scripts/{ascii_id('sc', stem)}.json"
        (out / fname).write_text(json.dumps(sc, ensure_ascii=False), encoding="utf-8")
        manifest_scripts[stem] = fname

    # Backward-compat alias if anything loads prologue_day1
    if "プロローグ1日目" in manifest_scripts:
        manifest_scripts["prologue_day1"] = manifest_scripts["プロローグ1日目"]

    # Minimal staffroll helper if Last Episode calls staffroll.ks
    if "staffroll" not in manifest_scripts and "staffroll.ks" in ue_patch.entries:
        sr_raw = decode_text(ue_patch.read("staffroll.ks"))
        sr_sc = parse_ks_script(sr_raw, None, (800, 600), ue_ctx=None)
        fname = f"scripts/{ascii_id('sc', 'staffroll')}.json"
        (out / fname).write_text(json.dumps(sr_sc, ensure_ascii=False), encoding="utf-8")
        manifest_scripts["staffroll"] = fname

    # --- 2. Compile UE 18+ H & Mature restored scripts ---
    ue_ctx = {
        "sf": {
            "ghentai": 1,
            "gmature": 1,
            "gtext": "mixed",
            "rinGermanChant": 0,
            "playUnmatchedVoices": 1,
            "g桜クリア": 1,
            "gレアルタ": 1,
        },
        "f": {},
        "kag": {"isHd": 1},
        "singleRoute": 0,
        "playVitaOp": 0,
    }
    manifest_scripts_h: Dict[str, str] = {}
    ue_h_count = 0
    for ks_name, ent in sorted(ue_eng.entries.items()):
        if not ks_name.endswith(".ks"):
            continue
        raw = decode_text(ue_eng.read_entry(ent))
        is_h_only = ks_name.startswith("h/") or any(
            x in ks_name for x in ("-101.ks", "-110.ks", "-130.ks")
        )
        has_h_cond = ("ghentai" in raw) or ("gmature" in raw)
        if not is_h_only and not has_h_cond:
            continue

        sc = parse_ks_script(raw, None, (800, 600), ue_ctx=ue_ctx)
        collect_script_assets(sc, ref_images, ref_rules, ref_bgm, ref_se, ref_voice)

        stem = ks_name[:-3]
        if ks_name in ("h/セイバールート十四日目-01.ks", "h/凛ルート十四日目-30.ks"):
            # Legacy duplicates; keep under h/ key only so we don't clobber normal scenes
            fname = f"scripts_h/{ascii_id('sch', stem)}.json"
            (out / fname).write_text(json.dumps(sc, ensure_ascii=False), encoding="utf-8")
            manifest_scripts_h[stem] = fname
        else:
            norm_stem = stem[2:] if stem.startswith("h/") else stem
            fname = f"scripts_h/{ascii_id('sch', norm_stem)}.json"
            (out / fname).write_text(json.dumps(sc, ensure_ascii=False), encoding="utf-8")
            manifest_scripts_h[norm_stem] = fname
            if stem != norm_stem:
                manifest_scripts_h[stem] = fname
        ue_h_count += 1
    print(f"[SCRIPTS] Compiled {ue_h_count} 18+/Mature restoration scripts into scripts_h/.")

    # --- 3. Flowcharts (Steam All-Ages + UE 18+) ---
    manifest_flowcharts: Dict[str, str] = {}
    parsed_steam_fcf: Dict[str, dict] = {}
    for fcf_name, fcf_raw in sorted(steam_fcf_raw.items()):
        stem = fcf_name[:-4]
        if stem == "プロローグ":
            stem = "prologue"
        fc = parse_fcf_content(fcf_raw, us_msgs)
        parsed_steam_fcf[stem] = fc
        fname = f"flowcharts/{ascii_id('fc', stem)}.json"
        (out / fname).write_text(json.dumps(fc, ensure_ascii=False), encoding="utf-8")
        manifest_flowcharts[stem] = fname

    manifest_flowcharts_h: Dict[str, str] = {}
    for fcf_name, ent in sorted(ue_eng.entries.items()):
        if not fcf_name.endswith(".fcf"):
            continue
        stem = fcf_name[:-4]
        if stem == "プロローグ":
            stem = "prologue"
        raw = decode_text(ue_eng.read_entry(ent))
        fc = parse_fcf_content(raw, None)
        fc = overlay_steam_titles_on_ue_fcf(fc, parsed_steam_fcf.get(stem))
        fname = f"flowcharts_h/{ascii_id('fch', stem)}.json"
        (out / fname).write_text(json.dumps(fc, ensure_ascii=False), encoding="utf-8")
        manifest_flowcharts_h[stem] = fname
    print(f"[FLOW] Compiled {len(manifest_flowcharts)} Steam & {len(manifest_flowcharts_h)} UE flowcharts.")

    # --- 4. Export Images (Steam + UE Fallbacks + Solid black/white) ---
    print(f"[IMAGES] Exporting {len(ref_images)} referenced base/UE images...")
    manifest_images: Dict[str, dict] = {}
    img_tasks: List[Tuple[str, bytes, float, bool]] = []
    missing_imgs: List[str] = []

    def find_ue_img(stem: str) -> Optional[Tuple[bytes, float]]:
        # Returns (raw_bytes, scale)
        for arc, prefixes, sc_ratio in [
            (ue_phd, ("",), 544.0 / 960.0),
            (ue_bg, ("",), 544.0 / 600.0),
            (ue_fg, ("",), 544.0 / 600.0),
            (ue_im, ("",), 544.0 / 600.0),
            (ue_patch, ("",), 544.0 / 600.0),
            (ue_ph, ("h-hd/", "h/"), 544.0 / 960.0),
        ]:
            for pref in prefixes:
                for ext in (".webp", ".png", ".bmp"):
                    ent = arc.get_ci(pref + stem + ext)
                    if ent is not None:
                        raw = arc.read_entry(ent)
                        with Image.open(io.BytesIO(raw)) as probe:
                            if probe.width >= 1900:
                                sc_ratio = 0.5
                            elif probe.height == 960 or (probe.width == 1280 and probe.height >= 960):
                                sc_ratio = 544.0 / 960.0
                            elif probe.width <= 820 and probe.height <= 620:
                                sc_ratio = 544.0 / 600.0
                            else:
                                sc_ratio = 544.0 / 600.0
                        return raw, sc_ratio
        return None

    for stem in sorted(ref_images):
        if stem in ("black", "white"):
            col = (0, 0, 0) if stem == "black" else (255, 255, 255)
            im = Image.new("RGB", (1020, 600), col)
            rel = f"images/scene/{stem}.png"
            im.save(out / rel, format="PNG")
            manifest_images[stem] = {"path": rel, "w": 1020, "h": 600}
            continue

        # Try Steam first
        s_lo = stem.lower()
        ent = None
        for ext in ("webp", "png", "bmp"):
            ent = pack_idx.get((stem, ext)) or pack_lower.get((s_lo, ext))
            if ent is not None:
                break
        if ent is not None:
            raw = read_pack_asset(ent, key)
            img_tasks.append((stem, raw, 0.5, False))
        else:
            res = find_ue_img(stem)
            if res is not None:
                raw, sc = res
                img_tasks.append((stem, raw, sc, False))
            else:
                missing_imgs.append(stem)

    def proc_base_img(t):
        stem, raw, sc, force_png = t
        aid = ascii_id("img", stem)
        ext, w, h = export_image_bytes(raw, out / "images" / "scene" / aid, sc, force_png=force_png)
        return stem, f"images/scene/{aid}{ext}", w, h

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        for stem, rel, w, h in ex.map(proc_base_img, img_tasks):
            manifest_images[stem] = {"path": rel, "w": w, "h": h}
            lo = stem.lower()
            if lo not in manifest_images:
                manifest_images[lo] = {"path": rel, "w": w, "h": h}

    print(f"[IMAGES] Exported {len(img_tasks)} base images (missing={len(missing_imgs)}).")
    if missing_imgs:
        print("  Sample missing images:", missing_imgs[:20])

    # --- 5. Export 18+ Mosaic H-CGs (patch_h.xp3) ---
    ph_map: Dict[str, object] = {}
    for k, ent in ue_ph.entries.items():
        if k.startswith("h-hd/") and k.lower().endswith(".webp"):
            ph_map[Path(k).stem.lower()] = (ent, 544.0 / 960.0)
    for k, ent in ue_ph.entries.items():
        if k.startswith("h/") and k.lower().endswith(".png"):
            st = Path(k).stem.lower()
            if st not in ph_map:
                ph_map[st] = (ent, 544.0 / 600.0)

    print(f"[18+ MOSAIC] Exporting {len(ph_map)} Mosaic H-CGs...")
    manifest_images_h: Dict[str, dict] = {}
    def proc_h_orig(item):
        st, (ent, sc) = item
        aid = ascii_id("hcg", st)
        raw = ue_ph.read_entry(ent)
        ext, w, h = export_image_bytes(raw, out / "images" / "h_orig" / aid, sc)
        return st, f"images/h_orig/{aid}{ext}", w, h

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        for st, rel, w, h in ex.map(proc_h_orig, sorted(ph_map.items())):
            manifest_images_h[st] = {"path": rel, "w": w, "h": h}

    # --- 6. Export 18+ Demosaic H-CGs (patch_decensor.xp3 + decensor_config.dic) ---
    artist_names = {1: "bishopcruz", 2: "bellandy", 3: "original"}
    dec_map: Dict[str, object] = {}
    for k, ent in ue_pdec.entries.items():
        parts = k.split("/")
        if len(parts) == 4 and parts[0].lower() == "decensored-hd" and k.lower().endswith(".webp"):
            grp = parts[1].lower()
            artist = parts[2].lower()
            st = Path(parts[3]).stem.lower()
            pref_id = decensor_cfg.get(grp, 1)
            pref_artist = artist_names.get(pref_id, "bellandy")
            if artist == pref_artist or (st not in dec_map and artist in ("bellandy", "bishopcruz")):
                dec_map[st] = ent

    print(f"[18+ DEMOSAIC] Exporting {len(dec_map)} Demosaic H-CGs...")
    manifest_images_dec: Dict[str, dict] = {}
    def proc_h_dec(item):
        st, ent = item
        aid = ascii_id("dhcg", st)
        raw = ue_pdec.read_entry(ent)
        with Image.open(io.BytesIO(raw)) as probe:
            sc = 544.0 / 960.0 if probe.height >= 900 else (544.0 / 600.0)
        ext, w, h = export_image_bytes(raw, out / "images" / "h_demosaic" / aid, sc)
        return st, f"images/h_demosaic/{aid}{ext}", w, h

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        for st, rel, w, h in ex.map(proc_h_dec, sorted(dec_map.items())):
            manifest_images_dec[st] = {"path": rel, "w": w, "h": h}

    # --- 7. Export Rules ---
    print(f"[RULES] Exporting {len(ref_rules)} wipe rules...")
    manifest_rules: Dict[str, dict] = {}
    missing_rules: List[str] = []
    rule_tasks = []
    for rstem in sorted(ref_rules):
        ent = (
            pack_idx.get((rstem, "png"))
            or pack_idx.get((rstem, "webp"))
            or pack_lower.get((rstem.lower(), "png"))
            or pack_lower.get((rstem.lower(), "webp"))
        )
        if ent is not None:
            rule_tasks.append((rstem, read_pack_asset(ent, key)))
        else:
            ue_r = (
                ue_phd.get_ci(rstem + ".png")
                or ue_rule.get_ci(rstem + ".png")
                or ue_rule.get_ci(rstem + ".bmp")
            )
            if ue_r is not None:
                arc = ue_phd if ue_phd.get_ci(rstem + ".png") else ue_rule
                rule_tasks.append((rstem, arc.read_entry(ue_r)))
            else:
                missing_rules.append(rstem)

    def proc_rule(item):
        rstem, raw = item
        aid = ascii_id("rule", rstem)
        rel = f"images/rules/{aid}.png"
        w, h = export_rule_bytes(raw, out / rel)
        return rstem, rel, w, h

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        for rstem, rel, w, h in ex.map(proc_rule, rule_tasks):
            manifest_rules[rstem] = {"path": rel, "w": w, "h": h}
    if missing_rules:
        print(f"  Missing rules ({len(missing_rules)}):", missing_rules[:10])

    # --- 8. Audio: New OST + Old OST + SLI Loops + SE + Voices ---
    print("[AUDIO] Exporting Steam New OST & UE Old OST + .sli loop points...")
    manifest_bgm: Dict[str, str] = {}
    for (stem, ext), ent in sorted(pack_idx.items()):
        if stem.startswith("bgm") and ext == "fcd":
            data, aext = decode_steam_audio(read_pack_asset(ent, key))
            rel = f"audio/bgm/{stem.lower()}{aext}"
            (out / rel).write_bytes(data)
            manifest_bgm[stem.lower()] = rel

    manifest_bgm_old: Dict[str, str] = {}
    manifest_bgm_old_loops: Dict[str, List[int]] = {}
    old_sources: Dict[str, Tuple[XP3Archive, object]] = {}
    for k, ent in ue_bgm.entries.items():
        if k.lower().endswith(".ogg"):
            old_sources[Path(k).stem.lower()] = (ue_bgm, ent)
    for k, ent in ue_patch.entries.items():
        if k.lower().startswith("bgm") and k.lower().endswith(".ogg"):
            old_sources[Path(k).stem.lower()] = (ue_patch, ent)

    for stem, (arc, ent) in sorted(old_sources.items()):
        rel = f"audio/bgm_old/{stem}.ogg"
        (out / rel).write_bytes(arc.read_entry(ent))
        manifest_bgm_old[stem] = rel
        sli_ent = ue_bgm.get_ci(f"{stem}.ogg.sli")
        if sli_ent is not None:
            ls, ll = parse_sli(ue_bgm.read_entry(sli_ent))
            if ll > 0:
                manifest_bgm_old_loops[stem] = [ls, ll]

    print(f"[AUDIO] Exporting {len(ref_se)} SE tracks...")
    manifest_se: Dict[str, str] = {}
    missing_se: List[str] = []
    se_tasks = []
    for sstem in sorted(ref_se):
        ent = pack_lower.get((sstem, "fcd")) or pack_lower.get((sstem, "ogg"))
        if ent is not None:
            se_tasks.append(("steam", sstem, ent))
            continue
        ue_s = ue_sound.get_ci(sstem + ".ogg") or ue_sound.get_ci(sstem + ".wav")
        if ue_s is not None:
            se_tasks.append(("ue_sound", sstem, ue_s))
            continue
        ue_p = ue_patch.get_ci(sstem + ".wav") or ue_patch.get_ci(sstem + ".ogg")
        if ue_p is not None:
            se_tasks.append(("ue_patch", sstem, ue_p))
            continue
        ue_hs = ue_ph.get_ci("h/" + sstem + ".wav") or ue_ph.get_ci("h/" + sstem + ".ogg")
        if ue_hs is not None:
            se_tasks.append(("ue_ph", sstem, ue_hs))
            continue
        missing_se.append(sstem)

    def proc_se(item):
        kind, sstem, ent = item
        if kind == "steam":
            data, ext = decode_steam_audio(read_pack_asset(ent, key))
        elif kind == "ue_sound":
            data = ue_sound.read_entry(ent)
            ext = ".wav" if ent.name.lower().endswith(".wav") else ".ogg"
        elif kind == "ue_patch":
            data = ue_patch.read_entry(ent)
            ext = ".wav" if ent.name.lower().endswith(".wav") else ".ogg"
        else:
            data = ue_ph.read_entry(ent)
            ext = ".wav" if ent.name.lower().endswith(".wav") else ".ogg"
        rel = f"audio/se/{sstem}{ext}"
        (out / rel).write_bytes(data)
        return sstem, rel

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        for sstem, rel in ex.map(proc_se, se_tasks):
            manifest_se[sstem] = rel
    if missing_se:
        print(f"  Missing SE ({len(missing_se)}):", missing_se[:10])

    manifest_voice: Dict[str, str] = {}
    missing_voices: List[str] = []
    if not args.no_voices:
        print(f"[VOICE] Exporting {len(ref_voice)} voice lines (this takes ~1-2 min)...")
        v_tasks = []
        for vstem in sorted(ref_voice):
            ent = pack_lower.get((vstem, "fcd")) or pack_lower.get((vstem, "ogg"))
            if ent is not None:
                v_tasks.append(("steam", vstem, ent))
                continue
            ue_v = ue_voice.get_ci(vstem + ".ogg") or ue_ph.get_ci("h/" + vstem + ".ogg")
            if ue_v is not None:
                src_arc = "ue_voice" if ue_voice.get_ci(vstem + ".ogg") is not None else "ue_ph"
                v_tasks.append((src_arc, vstem, ue_v))
                continue
            missing_voices.append(vstem)

        def proc_voice(item):
            kind, vstem, ent = item
            dest_ogg = out / f"audio/voice/{vstem}.ogg"
            dest_wav = out / f"audio/voice/{vstem}.wav"
            if dest_ogg.exists():
                return vstem, f"audio/voice/{vstem}.ogg"
            if dest_wav.exists():
                return vstem, f"audio/voice/{vstem}.wav"
            if kind == "steam":
                data, ext = decode_steam_audio(read_pack_asset(ent, key))
            elif kind == "ue_voice":
                data, ext = ue_voice.read_entry(ent), ".ogg"
            else:
                data, ext = ue_ph.read_entry(ent), ".ogg"
            rel = f"audio/voice/{vstem}{ext}"
            (out / rel).write_bytes(data)
            return vstem, rel

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
            for vstem, rel in ex.map(proc_voice, v_tasks):
                manifest_voice[vstem] = rel
        print(f"[VOICE] Exported {len(manifest_voice)} voices (missing={len(missing_voices)}).")
    else:
        print(f"[VOICE] Skipped {len(ref_voice)} voice exports (--no-voices set).")

    # --- 9. Fonts & UI & Manifest ---
    fonts, ui = copy_fonts_and_ui(out)
    manifest = {
        "slice": "full_game_ue_restoration",
        "vita_resolution": [960, 544],
        "source_resolution": [1920, 1080],
        "fonts": fonts,
        "ui": ui,
        "scripts": manifest_scripts,
        "scripts_h": manifest_scripts_h,
        "flowcharts": manifest_flowcharts,
        "flowcharts_h": manifest_flowcharts_h,
        "images": manifest_images,
        "images_h": manifest_images_h,
        "images_demosaic": manifest_images_dec,
        "rules": manifest_rules,
        "bgm": manifest_bgm,
        "bgm_old": manifest_bgm_old,
        "bgm_old_loops": manifest_bgm_old_loops,
        "se": manifest_se,
        "voice": manifest_voice,
        "missing": {
            "images": missing_imgs,
            "rules": missing_rules,
            "se": missing_se,
            "voice": missing_voices,
        },
    }
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    # Summary sizes
    total_bytes = sum(f.stat().st_size for f in out.rglob("*") if f.is_file() and ".cache" not in f.parts)
    print(
        f"\n[DONE] Built {out}:\n"
        f"  scripts={len(manifest_scripts)} scripts_h={len(manifest_scripts_h)}\n"
        f"  flowcharts={len(manifest_flowcharts)} flowcharts_h={len(manifest_flowcharts_h)}\n"
        f"  images={len(manifest_images)} images_h={len(manifest_images_h)} images_demosaic={len(manifest_images_dec)}\n"
        f"  rules={len(manifest_rules)} bgm={len(manifest_bgm)} bgm_old={len(manifest_bgm_old)} se={len(manifest_se)} voice={len(manifest_voice)}\n"
        f"  Total asset payload (excl. .cache): {total_bytes / (1024*1024):.1f} MB"
    )


if __name__ == "__main__":
    main()
