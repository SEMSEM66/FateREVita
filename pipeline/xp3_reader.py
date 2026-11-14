"""Minimal XP3 v2 archive reader and Ultimate Edition helper parsers."""

from __future__ import annotations

import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class XP3Segment:
    flags: int
    offset: int
    orig_size: int
    comp_size: int


@dataclass(frozen=True)
class XP3Entry:
    name: str
    orig_size: int
    segments: List[XP3Segment]


class XP3Archive:
    """Read-only parser for unencrypted/zlib Kirikiri2 XP3 v2 archives."""

    MAGIC = b"XP3\r\n \n\x1a\x8b\x67\x01"

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.entries: Dict[str, XP3Entry] = {}
        self._lower: Dict[str, XP3Entry] = {}
        self._parse_index()

    def _parse_index(self) -> None:
        with self.path.open("rb") as f:
            sig = f.read(11)
            if sig != self.MAGIC:
                raise ValueError(f"Invalid XP3 signature in {self.path}: {sig!r}")
            idx_off = struct.unpack("<Q", f.read(8))[0]
            if idx_off == 0x17:
                f.seek(idx_off + 1 + 8)
                idx_off = struct.unpack("<Q", f.read(8))[0]
            f.seek(idx_off)
            comp_flag = f.read(1)[0]
            if comp_flag & 1:
                comp_size, _orig_size = struct.unpack("<QQ", f.read(16))
                raw = zlib.decompress(f.read(comp_size))
            else:
                idx_size = struct.unpack("<Q", f.read(8))[0]
                raw = f.read(idx_size)

        pos = 0
        end = len(raw)
        while pos + 12 <= end:
            tag = raw[pos : pos + 4]
            sz = struct.unpack("<Q", raw[pos + 4 : pos + 12])[0]
            pos += 12
            body = raw[pos : pos + sz]
            pos += sz
            if tag != b"File":
                continue

            p2 = 0
            name: Optional[str] = None
            orig_size = 0
            segs: List[XP3Segment] = []
            while p2 + 12 <= len(body):
                t2 = body[p2 : p2 + 4]
                s2 = struct.unpack("<Q", body[p2 + 4 : p2 + 12])[0]
                p2 += 12
                b2 = body[p2 : p2 + s2]
                p2 += s2
                if t2 == b"info" and len(b2) >= 22:
                    _flags, orig_size, _comp, nlen = struct.unpack("<IQQH", b2[:22])
                    name = (
                        b2[22 : 22 + nlen * 2]
                        .decode("utf-16-le", errors="replace")
                        .replace("\\", "/")
                    )
                elif t2 == b"segm":
                    for i in range(0, len(b2), 28):
                        if i + 28 > len(b2):
                            break
                        sf, off, osz, psz = struct.unpack("<IQQQ", b2[i : i + 28])
                        segs.append(XP3Segment(sf, off, osz, psz))

            if name and segs:
                ent = XP3Entry(name=name, orig_size=orig_size, segments=segs)
                self.entries[name] = ent
                self._lower[name.lower()] = ent

    def read(self, name: str) -> bytes:
        ent = self.entries[name]
        return self.read_entry(ent)

    def read_ci(self, name: str) -> Optional[bytes]:
        ent = self.entries.get(name) or self._lower.get(name.lower())
        return self.read_entry(ent) if ent is not None else None

    def get_ci(self, name: str) -> Optional[XP3Entry]:
        return self.entries.get(name) or self._lower.get(name.lower())

    def read_entry(self, ent: XP3Entry) -> bytes:
        out = bytearray()
        with self.path.open("rb") as f:
            for seg in ent.segments:
                f.seek(seg.offset)
                chunk = f.read(seg.comp_size)
                if seg.flags & 7:
                    chunk = zlib.decompress(chunk)
                out.extend(chunk)
        return bytes(out)


def decode_text(raw: bytes) -> str:
    if raw.startswith(b"\xff\xfe"):
        return raw.decode("utf-16-le", errors="replace").lstrip("\ufeff")
    if raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16-be", errors="replace").lstrip("\ufeff")
    return raw.decode("utf-8-sig", errors="replace")


def parse_decensor_dic(path: Path) -> Dict[str, int]:
    """Parse Ultimate Edition decensor_config.dic mapping CG group -> artist id."""
    txt = decode_text(Path(path).read_bytes())
    txt = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)
    txt = re.sub(r"//[^\r\n]*", "", txt)
    out: Dict[str, int] = {}
    for m in re.finditer(r'"([^"]+)"\s*=>\s*"(\d+)"', txt):
        out[m.group(1).lower()] = int(m.group(2))
    return out


def parse_sli(raw: bytes) -> Tuple[int, int]:
    """Parse Kirikiri .sli loop metadata: returns (loop_start, loop_length) in samples."""
    txt = raw.decode("utf-8", errors="ignore")
    start = 0
    length = 0
    m1 = re.search(r"LoopStart\s*=\s*(\d+)", txt)
    m2 = re.search(r"LoopLength\s*=\s*(\d+)", txt)
    if m1:
        start = int(m1.group(1))
    if m2:
        length = int(m2.group(1))
    return start, length
