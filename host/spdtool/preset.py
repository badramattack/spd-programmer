"""Presets for the Program button, and the backup records Program leaves behind.

A preset is a short list of operations per generation. The firmware runs them
against whatever module is inserted (firmware/src/edit.h has the binary
format), so a preset works on any module without a PC. Operations name SPD
fields; this module turns them into byte offsets, bit masks and code tables:

    Preset("hide-ecc", "report no ECC bus width extension",
           ddr4=[set_to("ecc_bits", 0)], ddr5=[set_to("ecc_bits", 0)])
"""

from __future__ import annotations

import struct
import zlib
from fractions import Fraction
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

from .fields import Field, fields_for, fmt_time, parse_time
from .image import DDR4, DDR5, LAYOUTS, SpdImage, crc16

PRESET_VERSION = 1
OP_SET, OP_MAP, OP_SPEED, OP_IF, OP_TIME = 1, 2, 3, 4, 5
SPEED_RATE, SPEED_LOWEST, SPEED_STEP = 0, 1, 2
TIME_SET, TIME_SCALE, TIME_ADD, TIME_CLOCKS = 0, 1, 2, 3

_FIELDS = {gen: fields_for(SpdImage(bytes(LAYOUTS[gen].size), gen)) for gen in (DDR4, DDR5)}


def _enum(gen: str, name: str) -> Field:
    f = _FIELDS[gen].get(name)
    if f is None:
        raise ValueError("%s has no field %r" % (gen, name))
    if f.bits is None:
        raise ValueError("%s field %r is not a bit field; presets can only change bit fields" % (gen, name))
    return f


def _mask(hi: int, lo: int) -> int:
    return ((1 << (hi - lo + 1)) - 1) << lo


@dataclass
class Op:
    """One preset operation; `encode` turns it into firmware bytes for a generation."""

    encode: Callable[[str], bytes]
    text: str

    def __str__(self) -> str:
        return self.text


def set_to(name: str, value: Any) -> Op:
    """Set an enum field to `value` (in its unit, e.g. ecc_bits=0)."""

    def encode(gen):
        f = _enum(gen, name)
        off, hi, lo = f.bits
        codes = [c for c, v in f.codes.items() if v == value]
        if not codes:
            raise ValueError("%s %s: %r is not one of %s" % (gen, name, value, list(f.codes.values())))
        return struct.pack("<BHBB", OP_SET, off, _mask(hi, lo), codes[0] << lo)

    return Op(encode, "%s = %s" % (name, value))


def map_values(name: str, fn: Callable[[Any], Any], text: str, keep_zero_code: bool = False) -> Op:
    """Change an enum field from value v to fn(v). Modules whose current value
    has no valid fn(v) are refused. With keep_zero_code, code 0 stays 0 (the
    DDR5 second-density bytes are 0x00 on symmetric modules)."""

    def encode(gen):
        f = _enum(gen, name)
        off, hi, lo = f.bits
        rev = {v: c for c, v in f.codes.items()}
        pairs = []
        for code, value in sorted(f.codes.items()):
            if keep_zero_code and code == 0:
                pairs.append((0, 0))
                continue
            new = fn(value)
            if new in rev:
                pairs.append((code << lo, rev[new] << lo))
        return struct.pack("<BHBB", OP_MAP, off, _mask(hi, lo), len(pairs)) + bytes(b for p in pairs for b in p)

    return Op(encode, "%s %s" % (name, text))


def inc(name: str, by: int = 1, keep_zero_code: bool = False) -> Op:
    return map_values(name, lambda v: v + by, "+= %d" % by, keep_zero_code)


def double(name: str, keep_zero_code: bool = False) -> Op:
    return map_values(name, lambda v: v * 2, "*= 2", keep_zero_code)


def speed_lowest() -> Op:
    """Lowest standard speed (DDR4-1600, DDR5-3200). Timings stay in ns; the
    CL a controller needs at the new clock is added to the supported list."""
    return Op(lambda gen: struct.pack("<BBh", OP_SPEED, SPEED_LOWEST, 0), "speed = lowest standard grade")


def speed_up(grades: int = 1) -> Op:
    """`grades` standard speed grades faster (negative: slower). Faster than
    the module is rated is overclocking: it may not train or run stably."""
    return Op(lambda gen: struct.pack("<BBh", OP_SPEED, SPEED_STEP, grades), "speed %+d grade(s)" % grades)


def speed_rate(rate: int) -> Op:
    return Op(lambda gen: struct.pack("<BBh", OP_SPEED, SPEED_RATE, rate), "speed = %d MT/s" % rate)


def _timing(gen: str, name: str) -> Field:
    f = _FIELDS[gen].get(name)
    if f is None:
        raise ValueError("%s has no field %r" % (gen, name))
    if f.timing is None:
        raise ValueError("%s field %r is not a timing field" % (gen, name))
    return f


def _time_op(name: str, action: int, a: int, b: int, text: str) -> Op:
    def encode(gen):
        enc, off, off2, off3, shift = _timing(gen, name).timing
        return struct.pack("<BBHHHBBii", OP_TIME, enc, off, off2, off3, shift, action, a, b)

    return Op(encode, text)


def _ps(value) -> int:
    """A time as ps: an int is ps, a string like "350ns" or "-1ns" is parsed."""
    if isinstance(value, int):
        return value
    v = value.strip()
    return -parse_time(v[1:]) if v.startswith("-") else parse_time(v)


def set_time(name: str, value) -> Op:
    """Set a timing field, e.g. set_time("tRFC1", "350ns")."""
    return _time_op(name, TIME_SET, _ps(value), 0, "%s = %s" % (name, fmt_time(_ps(value))))


def scale_time(name: str, factor: float) -> Op:
    """Multiply a timing field, e.g. scale_time("tRFC1", 2); rounded up to the field's resolution."""
    fr = Fraction(factor).limit_denominator(1000)
    if fr <= 0:
        raise ValueError("scale factor must be positive")
    return _time_op(name, TIME_SCALE, fr.numerator, fr.denominator, "%s *= %s" % (name, fr))


def add_time(name: str, delta) -> Op:
    """Add to a timing field, e.g. add_time("tAA", "1ns") or add_time("tRP", "-500ps")."""
    ps = _ps(delta)
    return _time_op(name, TIME_ADD, ps, 0, "%s %s= %s" % (name, "-" if ps < 0 else "+", fmt_time(abs(ps))))


def clocks(name: str, n: int) -> Op:
    """Set a timing field to n clock cycles at the module's tCKmin (after any
    speed op before it), e.g. clocks("tAA", 22) for CL22."""
    if n <= 0:
        raise ValueError("clock count must be positive")
    return _time_op(name, TIME_CLOCKS, n, 0, "%s = %d nCK" % (name, n))


def when(name: str, value: Any, ops: List[Op]) -> Op:
    """Run `ops` only on modules whose field `name` equals `value`."""

    def encode(gen):
        f = _enum(gen, name)
        off, hi, lo = f.bits
        codes = [c for c, v in f.codes.items() if v == value]
        if not codes:
            raise ValueError("%s %s: %r is not one of %s" % (gen, name, value, list(f.codes.values())))
        body = b"".join(op.encode(gen) for op in ops)
        return struct.pack("<BHBBB", OP_IF, off, _mask(hi, lo), codes[0] << lo, len(ops)) + body

    return Op(encode, "if %s == %s: %s" % (name, value, "; ".join(map(str, ops))))


@dataclass
class Preset:
    name: str
    description: str
    ddr4: List[Op] = field(default_factory=list)
    ddr5: List[Op] = field(default_factory=list)

    def encode(self) -> bytes:
        name, desc = self.name.encode("ascii"), self.description.encode("ascii")
        if len(name) > 23 or len(desc) > 255:
            raise ValueError("preset name is at most 23 and the description at most 255 characters")
        out = b"SP" + bytes([PRESET_VERSION, len(name)]) + name + bytes([len(desc)]) + desc
        for gen, ops in ((DDR4, self.ddr4), (DDR5, self.ddr5)):
            if len(ops) > 255:
                raise ValueError("too many operations")
            out += bytes([len(ops)]) + b"".join(op.encode(gen) for op in ops)
        return out

    @property
    def id(self) -> int:
        """What the firmware stores in a backup record: CRC-32 of the encoding."""
        return zlib.crc32(self.encode())

    @classmethod
    def decode(cls, raw: bytes) -> "Preset":
        """A preset read back from the board; its ops re-encode to the same bytes."""
        if raw[:3] != b"SP" + bytes([PRESET_VERSION]):
            raise ValueError("not a version-%d preset" % PRESET_VERSION)
        pos = 3
        name = raw[pos + 1 : pos + 1 + raw[pos]].decode("ascii", "replace")
        pos += 1 + raw[pos]
        desc = raw[pos + 1 : pos + 1 + raw[pos]].decode("ascii", "replace")
        pos += 1 + raw[pos]
        lists = []
        for gen in (DDR4, DDR5):
            count = raw[pos]
            pos += 1
            ops, pos = _decode_ops(raw, pos, count, gen)
            lists.append(ops)
        if pos != len(raw):
            raise ValueError("trailing bytes after the preset")
        return cls(name, desc, *lists)


def _field_at(gen: str, off: int, mask: int) -> Optional[Field]:
    for f in _FIELDS[gen].values():
        if f.bits and f.bits[0] == off and _mask(f.bits[1], f.bits[2]) == mask:
            return f
    return None


def _decode_ops(raw: bytes, pos: int, count: int, gen: str) -> Tuple[List[Op], int]:
    ops = []
    for _ in range(count):
        start, t = pos, raw[pos]
        if t == OP_SET:
            _, off, mask, val = struct.unpack_from("<BHBB", raw, pos)
            pos += 5
            f = _field_at(gen, off, mask)
            text = ("%s = %s" % (f.name, f.codes.get(val >> f.bits[2], "code?")) if f
                    else "byte %d &0x%02x = 0x%02x" % (off, mask, val))
        elif t == OP_MAP:
            _, off, mask, n = struct.unpack_from("<BHBB", raw, pos)
            pairs = [(raw[pos + 5 + 2 * i], raw[pos + 6 + 2 * i]) for i in range(n)]
            pos += 5 + 2 * n
            f = _field_at(gen, off, mask)
            if f:
                lo = f.bits[2]
                text = "%s: %s" % (f.name, ", ".join("%s->%s" % (f.codes.get(a >> lo, "?"), f.codes.get(b >> lo, "?"))
                                                     for a, b in pairs))
            else:
                text = "byte %d &0x%02x: %s" % (off, mask, ", ".join("%02x->%02x" % p for p in pairs))
        elif t == OP_SPEED:
            _, kind, value = struct.unpack_from("<BBh", raw, pos)
            pos += 4
            text = {SPEED_RATE: "speed = %d MT/s" % value, SPEED_LOWEST: "speed = lowest standard grade",
                    SPEED_STEP: "speed %+d grade(s)" % value}.get(kind, "speed kind %d?" % kind)
        elif t == OP_TIME:
            _, enc, off, off2, off3, shift, action, a, b = struct.unpack_from("<BBHHHBBii", raw, pos)
            pos += 18
            f = next((x for x in _FIELDS[gen].values() if x.timing == (enc, off, off2, off3, shift)), None)
            fname = f.name if f else "timing at byte %d" % off
            text = {TIME_SET: lambda: "%s = %s" % (fname, fmt_time(a)),
                    TIME_SCALE: lambda: "%s *= %s" % (fname, Fraction(a, b) if b else "?"),
                    TIME_ADD: lambda: "%s %s= %s" % (fname, "-" if a < 0 else "+", fmt_time(abs(a))),
                    TIME_CLOCKS: lambda: "%s = %d nCK" % (fname, a)}.get(action, lambda: "time action %d?" % action)()
        elif t == OP_IF:
            _, off, mask, val, n = struct.unpack_from("<BHBBB", raw, pos)
            body, end = _decode_ops(raw, pos + 6, n, gen)
            f = _field_at(gen, off, mask)
            cond = ("%s == %s" % (f.name, f.codes.get(val >> f.bits[2], "?")) if f
                    else "byte %d &0x%02x == 0x%02x" % (off, mask, val))
            text = "if %s: %s" % (cond, "; ".join(map(str, body)))
            pos = end
        else:
            raise ValueError("unknown preset op %d at byte %d" % (t, pos))
        chunk = bytes(raw[start:pos])
        ops.append(Op(lambda g, c=chunk: c, text))
    return ops, pos


# -- built-in presets -------------------------------------------------------------------
# One example per kind of change; more can be added from Python (`spdtool preset add`)
# without reflashing. The firmware's defaults are generated from this list.

DEFAULTS = [
    Preset("lowest-speed", "lowest standard speed: DDR4-1600 / DDR5-3200",
           ddr4=[speed_lowest()], ddr5=[speed_lowest()]),
    Preset("speed-up", "one standard speed grade faster (overclocking: may not train)",
           ddr4=[speed_up(1)], ddr5=[speed_up(1)]),
    Preset("badram-row", "BadRAM: one extra row address bit, density doubled",
           ddr4=[inc("rows"), double("density")],
           ddr5=[inc("rows"), double("density"),
                 when("rank_mix", "asymmetrical", [inc("rows2"), double("density2")])]),
    Preset("hide-ecc", "report no ECC bus width extension",
           ddr4=[set_to("ecc_bits", 0)], ddr5=[set_to("ecc_bits", 0)]),
]
DEFAULT_ACTIVE = "badram-row"


def c_defaults() -> str:
    """firmware/src/default_presets.h: the DEFAULTS as a preset sector image."""
    names = [p.name for p in DEFAULTS]
    body = bytes([ord("P"), ord("S"), 1, names.index(DEFAULT_ACTIVE), len(DEFAULTS)])
    for p in DEFAULTS:
        raw = p.encode()
        body += struct.pack("<H", len(raw)) + raw
    lines = ["// Generated by `python -m spdtool.preset --c-defaults` from DEFAULTS in",
             "// host/spdtool/preset.py. Do not edit.",
             "// " + ", ".join(names) + " (active: %s)" % DEFAULT_ACTIVE,
             "#pragma once", "", "static const uint8_t default_presets[] = {"]
    for i in range(0, len(body), 16):
        lines.append("    " + " ".join("0x%02x," % b for b in body[i : i + 16]))
    lines.append("};")
    return "\n".join(lines) + "\n"


# -- backup records -------------------------------------------------------------------


@dataclass
class Record:
    preset_id: int
    area_was_ff: bool
    serial_orig: bytes
    serial_new: bytes
    crc_orig: Tuple[int, int]
    entries: List[Tuple[int, int]]  # (offset, original byte)

    @classmethod
    def decode(cls, raw: bytes) -> "Record":
        if len(raw) < 23 or raw[:3] != b"SR\x01":
            raise ValueError("not a version-1 backup record")
        n = raw[4]
        end = 21 + 3 * n
        if len(raw) < end + 2 or crc16(raw[:end]) != raw[end] | raw[end + 1] << 8:
            raise ValueError("backup record CRC mismatch")
        pid, = struct.unpack_from("<I", raw, 5)
        crcs = struct.unpack_from("<HH", raw, 17)
        entries = [struct.unpack_from("<HB", raw, 21 + 3 * i) for i in range(n)]
        return cls(pid, bool(raw[3] & 1), bytes(raw[9:13]), bytes(raw[13:17]), crcs, entries)

    def original(self, img: SpdImage) -> SpdImage:
        """The image before Program, rebuilt from the current one."""
        out = img.copy()
        for off, b in self.entries:
            out[off] = b
        out.serial = self.serial_orig
        for s, crc in zip(img.layout.crc, self.crc_orig):
            out.set_u16(s.at, crc)
        return out


def is_marked(img: SpdImage) -> bool:
    """True if the serial is the marked serial of the current contents."""
    return img.serial == img.marked_serial()


if __name__ == "__main__":
    import sys

    if sys.argv[1:] == ["--c-defaults"]:
        sys.stdout.write(c_defaults())
    else:
        for p in DEFAULTS:
            print("%-14s %08x  %s" % (p.name, p.id, p.description))
            for gen, ops in (("DDR4", p.ddr4), ("DDR5", p.ddr5)):
                for op in ops:
                    print("    %s: %s" % (gen, op))
