"""Read-only decoding of DDR5 XMP 3.0 profiles.

Intel's XMP specification is not public; this layout follows the commonly
used reverse-engineered description and was checked against a real module
(every timing matches its JEDEC part, every block CRC matches). DDR4 XMP 2.0
and AMD EXPO are only detected (decode.extra_profiles), not decoded.

Layout (DDR5 SPD bytes 640-1023, six 64-byte blocks):
  640  header: magic 0C 4A, version 0x30, ..., names of profiles 1-3 at
       14 / 30 / 46 (16 ASCII characters each)
  704, 768, 832  vendor profiles 1-3
  896, 960       user profiles 4-5 (may be written by the BIOS)
Every non-empty block ends with a CRC-16 (as the JEDEC part) of its first 62
bytes. Profile block:
  0 VPP, 1 VDD, 2 VDDQ (bits 6:5 volts, bits 4:0 x 50 mV)
  5  tCKAVGmin (ps)       7  supported CAS latencies (5 bytes, as bytes 24-28)
  13 tAA, 15 tRCD, 17 tRP, 19 tRAS, 21 tRC, 23 tWR (ps)
  25 tRFC1, 27 tRFC2, 29 tRFCsb (ns)
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional

from .image import DDR5, SpdImage, crc16
from .speed import nck, rate_of

XMP3_BASE = 640
BLOCK = 64


@dataclass
class XmpProfile:
    number: int            # 1-3 vendor, 4-5 user
    name: str
    crc_ok: bool
    vpp: float
    vdd: float
    vddq: float
    tck: int               # ps
    cls: List[int]
    taa: int
    trcd: int
    trp: int
    tras: int
    trc: int
    twr: int
    trfc1: int             # ns
    trfc2: int
    trfcsb: int

    def rate(self, img: SpdImage) -> int:
        return rate_of(img, self.tck)

    def cl(self, img: SpdImage) -> int:
        """The CAS latency at the profile's clock: lowest supported CL >= nCK(tAA)."""
        need = nck(img, self.taa, self.tck)
        need += need % 2
        ok = [c for c in self.cls if c >= need]
        return min(ok) if ok else need

    def nck(self, img: SpdImage, t_ps: int) -> int:
        return nck(img, t_ps, self.tck)


def _volts(b: int) -> float:
    return ((b >> 5) & 3) + (b & 0x1F) * 0.05


def _u16(blk: bytes, off: int) -> int:
    return blk[off] | blk[off + 1] << 8


def has_xmp3(img: SpdImage) -> bool:
    return img.generation == DDR5 and img[XMP3_BASE : XMP3_BASE + 3] == b"\x0c\x4a\x30"


def header_crc_ok(img: SpdImage) -> bool:
    h = bytes(img.data[XMP3_BASE : XMP3_BASE + BLOCK])
    return crc16(h[:62]) == _u16(h, 62)


def profiles(img: SpdImage) -> List[XmpProfile]:
    """The non-empty XMP 3.0 profiles of a DDR5 image (empty list if none)."""
    if not has_xmp3(img):
        return []
    header = bytes(img.data[XMP3_BASE : XMP3_BASE + BLOCK])
    out = []
    for n in range(1, 6):
        blk = bytes(img.data[XMP3_BASE + n * BLOCK : XMP3_BASE + (n + 1) * BLOCK])
        if not any(blk):
            continue
        if n <= 3:
            raw = header[14 + 16 * (n - 1) : 30 + 16 * (n - 1)]
            name = raw.split(b"\r")[0].split(b"\0")[0].decode("ascii", "replace").strip()
        else:
            name = "user profile %d" % n
        raw_cl = int.from_bytes(blk[7:12], "little")
        out.append(XmpProfile(
            number=n, name=name or "profile %d" % n, crc_ok=crc16(blk[:62]) == _u16(blk, 62),
            vpp=_volts(blk[0]), vdd=_volts(blk[1]), vddq=_volts(blk[2]),
            tck=_u16(blk, 5), cls=[20 + 2 * i for i in range(40) if raw_cl >> i & 1],
            taa=_u16(blk, 13), trcd=_u16(blk, 15), trp=_u16(blk, 17), tras=_u16(blk, 19),
            trc=_u16(blk, 21), twr=_u16(blk, 23),
            trfc1=_u16(blk, 25), trfc2=_u16(blk, 27), trfcsb=_u16(blk, 29)))
    return out


def summary_line(img: SpdImage, p: XmpProfile) -> str:
    """e.g. 'DDR5-6000 CL36-38-38-80, 1.35 V'."""
    return "%s %s-%d CL%d-%d-%d-%d, %.2f V%s" % (
        p.name, img.generation, p.rate(img), p.cl(img), p.nck(img, p.trcd), p.nck(img, p.trp),
        p.nck(img, p.tras), p.vdd, "" if p.crc_ok else " (CRC MISMATCH)")


def format_profiles(img: SpdImage) -> Optional[str]:
    """Every decoded value of every profile, for `spdtool xmp`."""
    ps = profiles(img)
    if not ps:
        return None
    lines = ["XMP 3.0 (header CRC %s)" % ("OK" if header_crc_ok(img) else "MISMATCH")]
    for p in ps:
        lines += [
            "",
            "Profile %d: %s%s" % (p.number, p.name, "" if p.crc_ok else "  (block CRC MISMATCH)"),
            "  Speed       %s-%d (tCK %.3f ns)" % (img.generation, p.rate(img), p.tck / 1000),
            "  Timings     CL%d-%d-%d-%d (tAA-tRCD-tRP-tRAS)" % (
                p.cl(img), p.nck(img, p.trcd), p.nck(img, p.trp), p.nck(img, p.tras)),
            "  tAA/tRCD/tRP/tRAS/tRC/tWR  %s ns" % " / ".join(
                "%.3f" % (t / 1000) for t in (p.taa, p.trcd, p.trp, p.tras, p.trc, p.twr)),
            "  tRFC1/2/sb  %d / %d / %d ns" % (p.trfc1, p.trfc2, p.trfcsb),
            "  CAS lat.    %s" % ", ".join(map(str, p.cls)),
            "  Voltages    VDD %.2f V, VDDQ %.2f V, VPP %.2f V" % (p.vdd, p.vddq, p.vpp),
        ]
    return "\n".join(lines)
