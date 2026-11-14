"""FCD (.fcd) -> Ogg Vorbis (.ogg) decryptor for Fate/stay night REMASTERED.

Supports both plaintext FCD containers (direct OggS payload) and Type-2
encrypted FCD containers (decrypted in-memory via fzmedia.dll RVA 0x3BDE0).
"""

from __future__ import annotations

import ctypes
import os
import struct
import sys
from pathlib import Path
from typing import Optional

_DECRYPT_RVA = 0x3BDE0
_DEFAULT_DLL_DIR = (
    Path(__file__).resolve().parent.parent
    / "tools"
    / "FSNr_tools"
    / "scripts"
    / "fcd"
)

_decrypt_fn = None


def _get_decrypt_fn(dll_dir: Path = _DEFAULT_DLL_DIR):
    global _decrypt_fn
    if _decrypt_fn is not None:
        return _decrypt_fn
    dll_path = dll_dir / "fzmedia-win64vc14-release-dynamic.dll"
    if not dll_path.exists():
        raise FileNotFoundError(f"fzmedia DLL not found: {dll_path}")
    if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(dll_dir))
    fzmedia = ctypes.WinDLL(str(dll_path))
    _decrypt_fn = ctypes.CFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint
    )(fzmedia._handle + _DECRYPT_RVA)
    return _decrypt_fn


def fcd_to_ogg(fcd_bytes: bytes, dll_dir: Path = _DEFAULT_DLL_DIR) -> bytes:
    """Convert raw .fcd file bytes into standard .ogg Vorbis bytes."""
    if len(fcd_bytes) < 28 or fcd_bytes[:4] != b"FCD\x00":
        raise ValueError("Not a valid FCD payload")

    data_offset = struct.unpack(">I", fcd_bytes[8:12])[0]
    chunk_type = struct.unpack(">I", fcd_bytes[12:16])[0]

    if fcd_bytes[data_offset : data_offset + 4] == b"OggS":
        return fcd_bytes[data_offset:]

    if chunk_type == 2:
        buf = bytearray(fcd_bytes)
        buf[12:16] = struct.pack("<I", 2)
        buf[6:8] = struct.pack("<H", 1)
        fn = _get_decrypt_fn(dll_dir)
        ptr = ctypes.addressof(ctypes.c_char.from_buffer(buf))
        fn(ptr, ptr + 28, len(buf) - 28)
        ogg_pos = buf.find(b"OggS", 28)
        if ogg_pos == -1:
            raise ValueError("Decrypted Type-2 FCD does not contain OggS header")
        return bytes(buf[ogg_pos:])

    ogg_pos = fcd_bytes.find(b"OggS", 0)
    if ogg_pos != -1:
        return fcd_bytes[ogg_pos:]
    raise ValueError(f"Unsupported FCD chunk_type={chunk_type}")
