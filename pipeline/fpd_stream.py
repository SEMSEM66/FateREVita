"""Streaming FPD (.bin/.dat) reader and fileinfo_*.txt index for Fate/stay night REMASTERED.

Avoids loading multi-gigabyte FPD containers into memory by seeking directly
to entry byte ranges and applying the repeating 64 KiB XOR key on demand.
"""

from __future__ import annotations

import io
import re
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional

HDR_SIZE = 56
_FILEINFO_RE = re.compile(
    r"^(.+?)::(.+?)::(.+?)::(\d+)::(\d+)::([a-f0-9]{32})::(.*)$"
)


def _xor_bytes(data: bytearray, key: bytes, start_phase: int = 0) -> bytes:
    """XOR data in-place with a repeating key starting at `start_phase`."""
    key_len = len(key)
    phase = start_phase % key_len
    # Slice-by-slice XOR using int.from_bytes is much faster than a Python per-byte loop
    # when data is large, so process in key-aligned chunks with bytes translation/int XOR.
    n = len(data)
    if n == 0:
        return b""
    if phase == 0 and n >= key_len:
        out = bytearray(n)
        full_blocks = n // key_len
        key_int = int.from_bytes(key, "little")
        mv = memoryview(data)
        for b in range(full_blocks):
            s = b * key_len
            chunk_int = int.from_bytes(mv[s : s + key_len], "little") ^ key_int
            out[s : s + key_len] = chunk_int.to_bytes(key_len, "little")
        rem = n - full_blocks * key_len
        if rem:
            s = full_blocks * key_len
            rem_int = int.from_bytes(mv[s:], "little") ^ int.from_bytes(key[:rem], "little")
            out[s:] = rem_int.to_bytes(rem, "little")
        return bytes(out)

    # General offset case: align first partial chunk, then XOR full key-sized blocks
    out = bytearray(n)
    mv = memoryview(data)
    pos = 0
    first = min(n, key_len - phase)
    first_int = int.from_bytes(mv[:first], "little") ^ int.from_bytes(
        key[phase : phase + first], "little"
    )
    out[:first] = first_int.to_bytes(first, "little")
    pos += first
    if pos < n:
        key_int = int.from_bytes(key, "little")
        while pos + key_len <= n:
            chunk_int = int.from_bytes(mv[pos : pos + key_len], "little") ^ key_int
            out[pos : pos + key_len] = chunk_int.to_bytes(key_len, "little")
            pos += key_len
        rem = n - pos
        if rem:
            rem_int = int.from_bytes(mv[pos:], "little") ^ int.from_bytes(key[:rem], "little")
            out[pos:] = rem_int.to_bytes(rem, "little")
    return bytes(out)


@dataclass(frozen=True)
class FPDEntry:
    filepath: str
    offset: int
    size: int
    uncompressed_size: int


@dataclass(frozen=True)
class PackAssetEntry:
    name: str
    ext: str
    bin_path: Path
    dat_abs_offset: int
    sub_offset: int
    size: int
    md5: str
    source_dat: str


class StreamingFPD:
    """Read-only streaming parser for an FPD v2 package."""

    def __init__(self, path: Path, key: bytes):
        self.path = Path(path)
        self.key = key
        self.version = 0
        self.data_section_start = 0
        self.entries: List[FPDEntry] = []
        self._by_path: Dict[str, FPDEntry] = {}
        self._parse_header()

    def _parse_header(self) -> None:
        with self.path.open("rb") as f:
            hdr = f.read(HDR_SIZE)
            if len(hdr) < HDR_SIZE or hdr[:4] != b"FPD\x00":
                raise ValueError(f"Invalid FPD header: {self.path}")
            self.version = int.from_bytes(hdr[4:8], "big")
            entry_count = int.from_bytes(hdr[8:16], "big")
            raw_block_size = int.from_bytes(hdr[16:24], "big")
            block_size = raw_block_size - HDR_SIZE
            self.data_section_start = HDR_SIZE + block_size
            raw_block = bytearray(f.read(block_size))

        dec_block = _xor_bytes(raw_block, self.key, 0)
        table_end = entry_count * 32
        str_block = zlib.decompress(dec_block[table_end:])

        mv = memoryview(dec_block)
        for i in range(entry_count):
            base = i * 32
            fso = int.from_bytes(mv[base : base + 8], "big")
            off = int.from_bytes(mv[base + 8 : base + 16], "big")
            sz = int.from_bytes(mv[base + 16 : base + 24], "big")
            unc = int.from_bytes(mv[base + 24 : base + 32], "big")
            end = str_block.index(0, fso)
            filepath = str_block[fso:end].decode("utf-8", errors="replace")
            entry = FPDEntry(filepath=filepath, offset=off, size=sz, uncompressed_size=unc)
            self.entries.append(entry)
            self._by_path[filepath] = entry

    def get(self, filepath: str) -> Optional[FPDEntry]:
        return self._by_path.get(filepath)

    def read_entry(self, entry: FPDEntry) -> bytes:
        with self.path.open("rb") as f:
            f.seek(self.data_section_start + entry.offset)
            raw = bytearray(f.read(entry.size))
        dec = _xor_bytes(raw, self.key, 0)
        if entry.uncompressed_size != 0:
            dec = zlib.decompress(dec)
        return dec

    def read_subrange(self, dat_entry: FPDEntry, sub_offset: int, size: int) -> bytes:
        """Read a byte slice from an uncompressed .dat entry inside this FPD."""
        if dat_entry.uncompressed_size != 0:
            raise ValueError(f"Expected uncompressed DAT entry, got {dat_entry.filepath}")
        abs_off = self.data_section_start + dat_entry.offset + sub_offset
        with self.path.open("rb") as f:
            f.seek(abs_off)
            raw = bytearray(f.read(size))
        return _xor_bytes(raw, self.key, sub_offset)


def build_pack_asset_index(
    obb_dir: Path, key: bytes
) -> Dict[tuple[str, str], PackAssetEntry]:
    """Index all fileinfo_*.txt assets across pack10d/11d/12d and patch01d.

    Later packages override earlier ones (patch01d overrides base packs).
    Returns {(stem, ext): PackAssetEntry}.
    """
    order = ["pack10d.bin", "pack11d.bin", "pack12d.bin", "patch01d.bin"]
    index: Dict[tuple[str, str], PackAssetEntry] = {}

    for bin_name in order:
        bin_path = obb_dir / bin_name
        if not bin_path.exists():
            continue
        fpd = StreamingFPD(bin_path, key)
        dat_offsets: Dict[str, int] = {}
        for e in fpd.entries:
            if e.filepath.endswith(".dat"):
                dat_name = e.filepath.rsplit("/", 1)[-1]
                dat_offsets[dat_name] = fpd.data_section_start + e.offset

        for e in fpd.entries:
            if not e.filepath.endswith(".txt"):
                continue
            txt = fpd.read_entry(e).decode("utf-8", errors="replace")
            for line in txt.splitlines():
                m = _FILEINFO_RE.match(line.strip())
                if not m:
                    continue
                name, ext, source_dat, off_s, sz_s, md5, _ = m.groups()
                if source_dat not in dat_offsets:
                    continue
                index[(name, ext)] = PackAssetEntry(
                    name=name,
                    ext=ext,
                    bin_path=bin_path,
                    dat_abs_offset=dat_offsets[source_dat],
                    sub_offset=int(off_s),
                    size=int(sz_s),
                    md5=md5,
                    source_dat=source_dat,
                )
    return index


def read_pack_asset(asset: PackAssetEntry, key: bytes) -> bytes:
    """Stream-read and XOR-decrypt a single asset from a pack*.bin/.dat container."""
    with asset.bin_path.open("rb") as f:
        f.seek(asset.dat_abs_offset + asset.sub_offset)
        raw = bytearray(f.read(asset.size))
    return _xor_bytes(raw, key, asset.sub_offset)
