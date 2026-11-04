"""In-memory SPD image: generation detection, CRCs, serial number, diffs.

Byte layout references: JEDEC Annex L (DDR4) and JESD400-5B (DDR5).
"""

from __future__ import annotations

import hashlib
import zlib
from dataclasses import dataclass
from typing import List, Tuple

DDR4 = "DDR4"
DDR5 = "DDR5"


@dataclass(frozen=True)
class CrcSection:
    name: str
    start: int  # first covered byte
    end: int  # last covered byte (inclusive)
    at: int  # CRC location, LSB first


@dataclass(frozen=True)
class Layout:
    generation: str
    size: int
    block_size: int  # write-protect block size
    crc: Tuple[CrcSection, ...]
    serial: Tuple[int, int]  # (offset, length), not covered by any CRC
    part_number: Tuple[int, int]
    module_mfr: int
    mfg_date: int  # year, week (BCD)
    dram_mfr: int
    user_area: Tuple[int, int]  # (start, end) inclusive, end-user programmable

    @property
    def record_area(self) -> Tuple[int, int]:
        """(offset, length) of the backup record area: the end of the user area."""
        return self.size - RECORD_LEN, RECORD_LEN


LAYOUTS = {
    DDR4: Layout(
        generation=DDR4,
        size=512,
        block_size=128,
        crc=(CrcSection("base", 0, 125, 126), CrcSection("module", 128, 253, 254)),
        serial=(325, 4),
        part_number=(329, 20),
        module_mfr=320,
        mfg_date=323,
        dram_mfr=350,
        user_area=(384, 511),
    ),
    DDR5: Layout(
        generation=DDR5,
        size=1024,
        block_size=64,
        crc=(CrcSection("base", 0, 509, 510),),
        serial=(517, 4),
        part_number=(521, 30),
        module_mfr=512,
        mfg_date=515,
        dram_mfr=552,
        user_area=(640, 1023),
    ),
}

KEY_BYTE = {0x0C: DDR4, 0x0E: DDR4, 0x12: DDR5}

# Program/Restore (firmware/src/edit.h): first byte of a marked serial and the
# size of the record area.
MARKER = 0x5D
RECORD_LEN = 64


def crc16(data: bytes) -> int:
    """CRC-16/XMODEM: polynomial 0x1021, init 0, MSB first (Annex L / JESD400-5B)."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


class SpdImage:
    """A complete SPD image (512 bytes for DDR4, 1024 for DDR5)."""

    def __init__(self, data: bytes, generation: str | None = None):
        if generation is None:
            generation = KEY_BYTE.get(data[2]) if len(data) > 2 else None
            if generation is None:
                generation = {512: DDR4, 1024: DDR5}.get(len(data))
            if generation is None:
                raise ValueError("cannot tell DDR4 from DDR5 (key byte 0x%02x, %d bytes)" % (data[2], len(data)))
        self.layout = LAYOUTS[generation]
        if len(data) != self.layout.size:
            raise ValueError("%s image must be %d bytes, got %d" % (generation, self.layout.size, len(data)))
        self.data = bytearray(data)

    @classmethod
    def from_file(cls, path) -> "SpdImage":
        with open(path, "rb") as f:
            return cls(f.read())

    def save(self, path) -> None:
        with open(path, "wb") as f:
            f.write(self.data)

    def copy(self) -> "SpdImage":
        return SpdImage(bytes(self.data), self.generation)

    @property
    def generation(self) -> str:
        return self.layout.generation

    def __getitem__(self, i):
        return self.data[i]

    def __setitem__(self, i, v):
        self.data[i] = v

    def u16(self, off: int) -> int:
        return self.data[off] | self.data[off + 1] << 8

    def set_u16(self, off: int, v: int) -> None:
        if not 0 <= v <= 0xFFFF:
            raise ValueError("value %d does not fit in 16 bits" % v)
        self.data[off] = v & 0xFF
        self.data[off + 1] = v >> 8

    # -- CRC -------------------------------------------------------------------

    def crc_status(self) -> List[Tuple[CrcSection, int, int]]:
        """(section, stored, computed) for every CRC section."""
        return [(s, self.u16(s.at), crc16(bytes(self.data[s.start : s.end + 1]))) for s in self.layout.crc]

    def crc_ok(self) -> bool:
        return all(stored == calc for _, stored, calc in self.crc_status())

    def fix_crc(self) -> None:
        for s, _, calc in self.crc_status():
            self.set_u16(s.at, calc)

    # -- Serial number -----------------------------------------------------------

    @property
    def serial(self) -> bytes:
        off, n = self.layout.serial
        return bytes(self.data[off : off + n])

    @serial.setter
    def serial(self, value: bytes) -> None:
        off, n = self.layout.serial
        if len(value) != n:
            raise ValueError("serial number is %d bytes" % n)
        self.data[off : off + n] = value

    def content_hash(self) -> int:
        """CRC-32 of the image with the serial, CRC bytes and record area zeroed
        (firmware/src/edit.c)."""
        tmp = bytearray(self.data)
        off, n = self.layout.serial
        tmp[off : off + n] = bytes(n)
        for s in self.layout.crc:
            tmp[s.at : s.at + 2] = b"\0\0"
        roff, rlen = self.layout.record_area
        tmp[roff : roff + rlen] = bytes(rlen)
        return zlib.crc32(bytes(tmp))

    def marked_serial(self) -> bytes:
        h = self.content_hash()
        return bytes([MARKER, h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF])

    def content_serial(self) -> bytes:
        """A serial number derived from the rest of the image.

        BIOSes cache the SPD contents by serial number, so every edited image
        needs a new one. Deriving it from the contents means the same edit
        always gets the same serial and different edits never share one.
        """
        tmp = bytearray(self.data)
        off, n = self.layout.serial
        tmp[off : off + n] = bytes(n)
        for s in self.layout.crc:  # CRCs follow from the contents anyway
            tmp[s.at : s.at + 2] = b"\0\0"
        return hashlib.sha256(bytes(tmp)).digest()[:n]

    # -- Diffs -------------------------------------------------------------------

    def diff(self, other: "SpdImage") -> List[Tuple[int, bytes, bytes]]:
        """Contiguous changed ranges: (offset, old bytes, new bytes)."""
        if other.generation != self.generation:
            raise ValueError("cannot diff %s against %s" % (self.generation, other.generation))
        out = []
        i, n = 0, self.layout.size
        while i < n:
            if self.data[i] == other.data[i]:
                i += 1
                continue
            j = i
            while j < n and self.data[j] != other.data[j]:
                j += 1
            out.append((i, bytes(self.data[i:j]), bytes(other.data[i:j])))
            i = j
        return out

    def blocks_of(self, ranges) -> int:
        """Bitmask of the write-protect blocks touched by (offset, ..., new) ranges."""
        mask = 0
        bs = self.layout.block_size
        for off, _, new in ranges:
            for b in range(off // bs, (off + len(new) - 1) // bs + 1):
                mask |= 1 << b
        return mask
