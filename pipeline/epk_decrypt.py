"""EPK (.epk) decryptor wrapper for Fate/stay night REMASTERED."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Dict

_DEFAULT_TOOLS_DIR = (
    Path(__file__).resolve().parent.parent / "tools" / "FSNr_tools" / "build"
)


def parse_epk_dat_text(text: str) -> Dict[str, str]:
    """Parse decrypted EPK DAT text into {label: value}."""
    lines = text.splitlines()
    if not lines or not lines[0].startswith("DAT"):
        raise ValueError("Unexpected EPK decrypted format (missing DAT header)")
    result: Dict[str, str] = {}
    for line in lines[2:]:
        if not line:
            continue
        parts = line.split("::")
        if len(parts) >= 3:
            label = parts[1]
            val = parts[2]
            result[label] = val
    return result


def decrypt_epk_bytes(
    epk_bytes: bytes,
    stem: str,
    tools_dir: Path = _DEFAULT_TOOLS_DIR,
) -> Dict[str, str]:
    """Decrypt raw .epk bytes given the EPK stem (26-char hash or name like 'uistring')."""
    main_exe = tools_dir / "main.exe"
    some_key = tools_dir / "SomeKey.bin"
    if not main_exe.exists() or not some_key.exists():
        raise FileNotFoundError(f"Missing main.exe or SomeKey.bin in {tools_dir}")

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        in_file = td_path / f"{stem}.epk"
        in_file.write_bytes(epk_bytes)
        proc = subprocess.run(
            [str(main_exe), "dec", str(in_file)],
            cwd=str(tools_dir),
            capture_output=True,
            text=True,
            check=False,
        )
        out_file = td_path / f"{stem}.epk_dec"
        if proc.returncode != 0 or not out_file.exists():
            raise RuntimeError(
                f"main.exe dec failed for {stem} (rc={proc.returncode}): {proc.stdout}\n{proc.stderr}"
            )
        txt = out_file.read_text(encoding="utf-8", errors="replace")
    return parse_epk_dat_text(txt)
