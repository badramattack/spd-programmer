"""Named SPD fields with decoding and encoding.

Timings are handled in picoseconds internally. On the command line they accept
"13.75ns", "13750ps" or a bare number in ns. Byte locations: JEDEC Annex L
(DDR4) and JESD400-5B (DDR5).
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Tuple

from .image import DDR4, DDR5, SpdImage


@dataclass
class Field:
    name: str
    help: str
    get: Callable[[SpdImage], Any]
    set: Optional[Callable[[SpdImage, Any], None]]
    parse: Callable[[str], Any]
    fmt: Callable[[Any], str]
    # Enum fields: (byte, high bit, low bit) and code -> value, for presets.
    bits: Optional[Tuple[int, int, int]] = None
    codes: Optional[Dict[int, Any]] = None
    # Timing fields: (encoding, off, off2, off3, shift) as in firmware/src/timing.h.
    timing: Optional[Tuple[int, int, int, int, int]] = None


# Timing encodings (firmware/src/timing.h).
T_PS16, T_NS16, T_MTB_FINE, T_MTB12, T_MTB16 = 1, 2, 3, 4, 5
T_NO_FINE = 0xFFFF


# -- value parsing / formatting ---------------------------------------------------


def parse_time(s: str) -> int:
    m = re.fullmatch(r"\s*([0-9.]+)\s*(ns|ps)?\s*", s.lower())
    if not m:
        raise ValueError("bad time %r (use e.g. 13.75ns or 13750ps)" % s)
    v = float(m.group(1))
    return int(round(v if m.group(2) == "ps" else v * 1000))


def fmt_time(ps: int) -> str:
    return "%.3f ns" % (ps / 1000)


def parse_int(s: str) -> int:
    return int(s, 0)


def parse_int_list(s: str) -> List[int]:
    return sorted({int(x) for x in s.replace(",", " ").split()})


def fmt_list(v) -> str:
    return ",".join(str(x) for x in v)


def _s8(b: int) -> int:
    return b - 256 if b & 0x80 else b


def _bits(img: SpdImage, off: int, hi: int, lo: int) -> int:
    return (img[off] >> lo) & ((1 << (hi - lo + 1)) - 1)


def _set_bits(img: SpdImage, off: int, hi: int, lo: int, v: int) -> None:
    width = hi - lo + 1
    if not 0 <= v < (1 << width):
        raise ValueError("code %d does not fit in byte %d bits %d..%d" % (v, off, hi, lo))
    mask = ((1 << width) - 1) << lo
    img[off] = (img[off] & ~mask) | (v << lo)


def enum_field(name, help, off, hi, lo, table: Dict[int, Any], fmt=str, parse=None) -> Field:
    """A bit field whose codes map to values through `table`."""
    rev = {v: k for k, v in table.items()}

    def get(img):
        code = _bits(img, off, hi, lo)
        return table.get(code, "code %d?" % code)

    def set_(img, value):
        if value not in rev:
            raise ValueError("%s: %r not one of %s" % (name, value, ", ".join(map(str, table.values()))))
        _set_bits(img, off, hi, lo, rev[value])

    def fmt_(v):
        return fmt(v) if not isinstance(v, str) else v

    return Field(name, help, get, set_, parse or parse_int, fmt_, bits=(off, hi, lo), codes=table)


# -- DDR4 timing encodings (Annex L: MTB = 125 ps, FTB = 1 ps) -------------------

MTB = 125


def _mtb_fine_encode(ps: int):
    """Medium units rounded up, then a negative fine correction (JEDEC convention)."""
    units = math.ceil(ps / MTB)
    return units, ps - units * MTB


def ddr4_mtb_fine(name, help, off, fine) -> Field:
    def get(img):
        return img[off] * MTB + _s8(img[fine])

    def set_(img, ps):
        units, f = _mtb_fine_encode(ps)
        if not 1 <= units <= 255:
            raise ValueError("%s: %d ps out of range" % (name, ps))
        img[off], img[fine] = units, f & 0xFF

    return Field(name, help, get, set_, parse_time, fmt_time, timing=(T_MTB_FINE, off, fine, T_NO_FINE, 0))


def ddr4_mtb12(name, help, msn, shift, lsb, fine=None) -> Field:
    """12-bit MTB value: upper nibble in byte `msn` at `shift`, low byte in `lsb`."""

    def get(img):
        units = ((img[msn] >> shift) & 0xF) << 8 | img[lsb]
        return units * MTB + (_s8(img[fine]) if fine is not None else 0)

    def set_(img, ps):
        if fine is not None:
            units, f = _mtb_fine_encode(ps)
            img[fine] = f & 0xFF
        else:
            units = math.ceil(ps / MTB)
        if not 0 <= units <= 0xFFF:
            raise ValueError("%s: %d ps out of range" % (name, ps))
        img[msn] = (img[msn] & ~(0xF << shift)) | ((units >> 8) << shift)
        img[lsb] = units & 0xFF

    return Field(name, help, get, set_, parse_time, fmt_time,
                 timing=(T_MTB12, msn, lsb, T_NO_FINE if fine is None else fine, shift))


def ddr4_mtb16(name, help, off) -> Field:
    return Field(
        name,
        help,
        lambda img: img.u16(off) * MTB,
        lambda img, ps: img.set_u16(off, math.ceil(ps / MTB)),
        parse_time,
        fmt_time,
        timing=(T_MTB16, off, 0, T_NO_FINE, 0),
    )


def ddr4_cl_get(img: SpdImage) -> List[int]:
    raw = img[20] | img[21] << 8 | img[22] << 16 | (img[23] & 0x3F) << 24
    base = 23 if img[23] & 0x80 else 7
    return [base + i for i in range(30) if raw >> i & 1]


def ddr4_cl_set(img: SpdImage, cls: List[int]) -> None:
    if not cls:
        raise ValueError("need at least one CAS latency")
    base = 7 if max(cls) <= 36 else 23
    raw = 0
    for cl in cls:
        if not base <= cl < base + 30:
            raise ValueError("CL %d not encodable together with CL %d..%d" % (cl, min(cls), max(cls)))
        raw |= 1 << (cl - base)
    img[20], img[21], img[22] = raw & 0xFF, raw >> 8 & 0xFF, raw >> 16 & 0xFF
    img[23] = (raw >> 24 & 0x3F) | (0x80 if base == 23 else 0)


# -- DDR5 timing encodings (JESD400-5B: absolute ps or ns, 16-bit LE) ------------


def ddr5_ps(name, help, off) -> Field:
    return Field(name, help, lambda img: img.u16(off), lambda img, ps: img.set_u16(off, ps), parse_time, fmt_time,
                 timing=(T_PS16, off, 0, T_NO_FINE, 0))


def ddr5_ns(name, help, off) -> Field:
    return Field(
        name,
        help,
        lambda img: img.u16(off) * 1000,
        lambda img, ps: img.set_u16(off, math.ceil(ps / 1000)),
        parse_time,
        fmt_time,
        timing=(T_NS16, off, 0, T_NO_FINE, 0),
    )


def ddr5_nck(name, help, off) -> Field:
    def set_(img, v):
        if not 1 <= v <= 255:
            raise ValueError("%s: nCK must be 1..255" % name)
        img[off] = v

    return Field(name, help, lambda img: img[off], set_, parse_int, str)


def ddr5_cl_get(img: SpdImage) -> List[int]:
    raw = int.from_bytes(bytes(img.data[24:29]), "little")
    return [20 + 2 * i for i in range(40) if raw >> i & 1]


def ddr5_cl_set(img: SpdImage, cls: List[int]) -> None:
    raw = 0
    for cl in cls:
        if cl % 2 or not 20 <= cl <= 98:
            raise ValueError("DDR5 CL must be even and 20..98, got %d" % cl)
        raw |= 1 << ((cl - 20) // 2)
    img.data[24:29] = raw.to_bytes(5, "little")


# -- Manufacturing fields (both generations) --------------------------------------


def part_number_field(gen_off: int, length: int) -> Field:
    def get(img):
        return bytes(img.data[gen_off : gen_off + length]).decode("ascii", "replace").rstrip(" \0")

    def set_(img, s: str):
        b = s.encode("ascii")
        if len(b) > length:
            raise ValueError("part number longer than %d characters" % length)
        img.data[gen_off : gen_off + length] = b.ljust(length, b" ")

    return Field("part_number", "module part number (ASCII)", get, set_, str, str)


def serial_field() -> Field:
    def set_(img, v: bytes):
        img.serial = v

    return Field(
        "serial",
        "module serial number (hex, e.g. 1a2b3c4d)",
        lambda img: img.serial,
        set_,
        lambda s: bytes.fromhex(s.replace("0x", "")),
        lambda v: v.hex(),
    )


def _bcd(v: int) -> int:
    return (v >> 4) * 10 + (v & 0xF)


def _to_bcd(v: int) -> int:
    return (v // 10) << 4 | (v % 10)


def mfg_date_field(off: int) -> Field:
    def get(img):
        return (2000 + _bcd(img[off]), _bcd(img[off + 1]))

    def set_(img, v):
        year, week = v
        img[off], img[off + 1] = _to_bcd(year % 100), _to_bcd(week)

    def parse(s):
        y, w = re.split(r"[-/ ]|w", s.lower().replace("wk", "w"), maxsplit=1)
        return int(y), int(w)

    return Field("mfg_date", "manufacturing date, YYYY-WW", get, set_, parse, lambda v: "%04d-W%02d" % v)


# -- Field tables -------------------------------------------------------------------


def _ddr4_fields() -> List[Field]:
    f = [
        ddr4_mtb_fine("tCKmin", "minimum clock period (sets max speed)", 18, 125),
        ddr4_mtb_fine("tCKmax", "maximum clock period", 19, 124),
        Field("CL", "supported CAS latencies, e.g. 16,18,20,22", ddr4_cl_get, ddr4_cl_set, parse_int_list, fmt_list),
        ddr4_mtb_fine("tAA", "CAS latency time", 24, 123),
        ddr4_mtb_fine("tRCD", "RAS to CAS delay", 25, 122),
        ddr4_mtb_fine("tRP", "row precharge time", 26, 121),
        ddr4_mtb12("tRAS", "active to precharge time", 27, 0, 28),
        ddr4_mtb12("tRC", "active to active/refresh time", 27, 4, 29, fine=120),
        ddr4_mtb16("tRFC1", "refresh recovery, 1x mode", 30),
        ddr4_mtb16("tRFC2", "refresh recovery, 2x mode", 32),
        ddr4_mtb16("tRFC4", "refresh recovery, 4x mode", 34),
        ddr4_mtb12("tFAW", "four activate window", 36, 0, 37),
        ddr4_mtb_fine("tRRD_S", "activate to activate, different bank group", 38, 119),
        ddr4_mtb_fine("tRRD_L", "activate to activate, same bank group", 39, 118),
        ddr4_mtb_fine("tCCD_L", "CAS to CAS, same bank group", 40, 117),
        ddr4_mtb12("tWR", "write recovery time", 41, 0, 42),
        ddr4_mtb12("tWTR_S", "write to read, different bank group", 43, 0, 44),
        ddr4_mtb12("tWTR_L", "write to read, same bank group", 43, 4, 45),
        enum_field("density", "SDRAM density per die in Gb (0.25..32)", 4, 3, 0,
                   {0: 0.25, 1: 0.5, 2: 1, 3: 2, 4: 4, 5: 8, 6: 16, 7: 32, 8: 12, 9: 24},
                   fmt=lambda v: "%g Gb" % v, parse=float),
        enum_field("bank_groups", "bank groups", 4, 7, 6, {0: 1, 1: 2, 2: 4}),
        enum_field("banks_per_group", "banks per bank group", 4, 5, 4, {0: 4, 1: 8}),
        enum_field("rows", "row address bits", 5, 5, 3, {i: 12 + i for i in range(7)}),
        enum_field("cols", "column address bits", 5, 2, 0, {i: 9 + i for i in range(4)}),
        enum_field("die_per_package", "dies per package", 6, 6, 4, {i: i + 1 for i in range(8)}),
        enum_field("device_width", "SDRAM I/O width (4, 8, 16, 32)", 12, 2, 0, {0: 4, 1: 8, 2: 16, 3: 32}),
        enum_field("ranks", "package ranks per DIMM (1..8)", 12, 5, 3, {i: i + 1 for i in range(8)}),
        enum_field("bus_width", "primary bus width in bits", 13, 2, 0, {0: 8, 1: 16, 2: 32, 3: 64}),
        enum_field("ecc_bits", "bus width extension (ECC) in bits", 13, 4, 3, {0: 0, 1: 8}),
        enum_field("rank_mix", "rank mix: symmetrical or asymmetrical", 12, 6, 6,
                   {0: "symmetrical", 1: "asymmetrical"}, parse=str),
    ]
    return f


def _ddr5_fields() -> List[Field]:
    return [
        ddr5_ps("tCKmin", "minimum clock period (sets max speed)", 20),
        ddr5_ps("tCKmax", "maximum clock period", 22),
        Field("CL", "supported CAS latencies (even), e.g. 40,42,46", ddr5_cl_get, ddr5_cl_set, parse_int_list, fmt_list),
        ddr5_ps("tAA", "CAS latency time", 30),
        ddr5_ps("tRCD", "RAS to CAS delay", 32),
        ddr5_ps("tRP", "row precharge time", 34),
        ddr5_ps("tRAS", "active to precharge time", 36),
        ddr5_ps("tRC", "active to active/refresh time", 38),
        ddr5_ps("tWR", "write recovery time", 40),
        ddr5_ns("tRFC1", "normal refresh recovery (stored in ns)", 42),
        ddr5_ns("tRFC2", "fine granularity refresh recovery (stored in ns)", 44),
        ddr5_ns("tRFCsb", "same bank refresh recovery (stored in ns)", 46),
        ddr5_ns("tRFC1_dlr", "3DS: refresh recovery, different logical rank (ns)", 48),
        ddr5_ns("tRFC2_dlr", "3DS: fine granularity refresh, different logical rank (ns)", 50),
        ddr5_ns("tRFCsb_dlr", "3DS: same bank refresh, different logical rank (ns)", 52),
        ddr5_ps("tRRD_L", "activate to activate, same bank group", 70),
        ddr5_nck("tRRD_L_nck", "tRRD_L lower limit in clocks", 72),
        ddr5_ps("tCCD_L", "CAS to CAS, same bank group", 73),
        ddr5_nck("tCCD_L_nck", "tCCD_L lower limit in clocks", 75),
        ddr5_ps("tFAW", "four activate window", 82),
        ddr5_nck("tFAW_nck", "tFAW lower limit in clocks", 84),
        ddr5_ps("tRTP", "read to precharge", 91),
        ddr5_nck("tRTP_nck", "tRTP lower limit in clocks", 93),
        enum_field("density", "SDRAM density per die in Gb (4..64)", 4, 4, 0,
                   {1: 4, 2: 8, 3: 12, 4: 16, 5: 24, 6: 32, 7: 48, 8: 64},
                   fmt=lambda v: "%g Gb" % v, parse=float),
        enum_field("package", "die package: mono, ddp, 3ds-2h, 3ds-4h, 3ds-8h, 3ds-16h", 4, 7, 5,
                   {0: "mono", 1: "ddp", 2: "3ds-2h", 3: "3ds-4h", 4: "3ds-8h", 5: "3ds-16h"}, parse=str),
        enum_field("rows", "row address bits", 5, 4, 0, {0: 16, 1: 17, 2: 18}),
        enum_field("cols", "column address bits", 5, 7, 5, {0: 10, 1: 11}),
        enum_field("device_width", "SDRAM I/O width (4, 8, 16, 32)", 6, 7, 5, {0: 4, 1: 8, 2: 16, 3: 32}),
        enum_field("bank_groups", "bank groups", 7, 7, 5, {0: 1, 1: 2, 2: 4, 3: 8}),
        enum_field("banks_per_group", "banks per bank group", 7, 2, 0, {0: 1, 1: 2, 2: 4}),
        enum_field("ranks", "package ranks per sub-channel (1..8)", 234, 5, 3, {i: i + 1 for i in range(8)}),
        enum_field("subchannels", "sub-channels per DIMM", 235, 7, 5, {0: 1, 1: 2, 2: 4, 3: 8}),
        enum_field("bus_width", "primary bus width per sub-channel in bits", 235, 2, 0, {0: 8, 1: 16, 2: 32, 3: 64}),
        enum_field("ecc_bits", "bus width extension per sub-channel in bits", 235, 4, 3, {0: 0, 1: 4, 2: 8}),
        enum_field("rank_mix", "rank mix: symmetrical or asymmetrical", 234, 6, 6,
                   {0: "symmetrical", 1: "asymmetrical"}, parse=str),
        # Odd ranks of asymmetrical modules only; 0x00 on symmetrical ones (JESD400-5B 8.1.9-10).
        enum_field("density2", "asymmetrical: odd-rank SDRAM density in Gb (0 = none)", 8, 4, 0,
                   {0: 0, 1: 4, 2: 8, 3: 12, 4: 16, 5: 24, 6: 32, 7: 48, 8: 64},
                   fmt=lambda v: "%g Gb" % v, parse=float),
        enum_field("rows2", "asymmetrical: odd-rank row address bits", 9, 4, 0, {0: 16, 1: 17, 2: 18}),
    ]


def fields_for(img: SpdImage) -> Dict[str, Field]:
    lay = img.layout
    common = [
        part_number_field(*lay.part_number),
        serial_field(),
        mfg_date_field(lay.mfg_date),
    ]
    gen = _ddr4_fields() if img.generation == DDR4 else _ddr5_fields()
    return {f.name: f for f in gen + common}


def get_field(img: SpdImage, name: str) -> Field:
    fields = fields_for(img)
    for k, f in fields.items():
        if k.lower() == name.lower():
            return f
    raise KeyError("unknown %s field %r (known: %s)" % (img.generation, name, ", ".join(fields)))


def set_field(img: SpdImage, name: str, value: str) -> None:
    f = get_field(img, name)
    f.set(img, f.parse(value))
