"""High-level edit workflow: back up, bump the serial, fix CRCs, write the
changed bytes (unlocking if asked), re-lock, and verify."""

from __future__ import annotations

import datetime
from pathlib import Path
from typing import Callable, List, Tuple

from .device import Programmer, ProgrammerError
from .fields import fields_for
from .image import SpdImage

BACKUP_DIR = Path.home() / ".spdtool" / "backups"

# SPD5118 register names (JESD300-5B Table 102), for "spdtool regs".
MR_NAMES = {
    0: "device type MSB", 1: "device type LSB", 2: "device revision", 3: "vendor ID byte 0",
    4: "vendor ID byte 1", 5: "device capability", 6: "write recovery time", 11: "I2C legacy mode",
    12: "WP blocks 7:0", 13: "WP blocks 15:8", 14: "host/local interface config", 18: "device configuration",
    26: "TS configuration", 27: "interrupt configuration", 28: "TS high limit L", 29: "TS high limit H",
    30: "TS low limit L", 31: "TS low limit H", 32: "TS crit high L", 33: "TS crit high H",
    34: "TS crit low L", 35: "TS crit low H", 48: "device status", 49: "temperature L", 50: "temperature H",
    51: "TS status", 52: "hub/thermal/NVM error status",
}


def backup(img: SpdImage, tag: str = "") -> Path:
    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = BACKUP_DIR / ("%s-%s-%s%s.bin" % (img.generation.lower(), img.serial.hex(), stamp, tag))
    img.save(path)
    return path


def prepare(current: SpdImage, edited: SpdImage, bump_serial: bool = True, fix_crc: bool = True) -> SpdImage:
    """The image that will be written: CRCs fixed and, if anything else
    changed, a new serial number (BIOSes cache SPD data by serial)."""
    target = edited.copy()
    if fix_crc:
        target.fix_crc()
    if bump_serial and target.serial == current.serial:
        others = [r for r in current.diff(target) if not _only_serial(current, r)]
        if others:
            target.serial = target.content_serial()
    return target


def _only_serial(img: SpdImage, rng) -> bool:
    off, n = img.layout.serial
    start, _, new = rng
    return start >= off and start + len(new) <= off + n


def field_diff(old: SpdImage, new: SpdImage) -> List[Tuple[str, str, str]]:
    out = []
    fo, fn = fields_for(old), fields_for(new)
    for name, f in fo.items():
        try:
            a, b = f.fmt(f.get(old)), fn[name].fmt(fn[name].get(new))
        except Exception:
            continue
        if a != b:
            out.append((name, a, b))
    return out


def apply(prog: Programmer, current: SpdImage, target: SpdImage, unlock: bool = False,
          log: Callable[[str], None] = print) -> None:
    """Write `target` over `current` (already read from the module) in an open session."""
    ranges = current.diff(target)
    if not ranges:
        log("Nothing to write: the module already holds this image.")
        return
    path = backup(current)
    log("Backup of the current contents: %s" % path)

    need = current.blocks_of(ranges)
    mask, _, _ = prog.wp()
    blocked = need & mask
    relock = None
    if blocked:
        if not unlock:
            raise ProgrammerError("WP", "blocks %s are write protected; run with --unlock (unlocks, writes, re-locks)"
                                  % _blocks(blocked))
        relock = prog.unlock()
        log("Unlocked (was protected: blocks %s)" % _blocks(relock))
    try:
        for off, _, new in ranges:
            prog.write(off, new)
        log("Wrote %d byte(s) in %d range(s)." % (sum(len(r[2]) for r in ranges), len(ranges)))
    finally:
        if relock:
            prog.lock(relock)
            log("Re-locked blocks %s" % _blocks(relock))
    check = prog.read_image()
    if check.data != target.data:
        bad = check.diff(target)
        raise ProgrammerError("VERIFY", "read-back differs at %d range(s), first at byte %d" % (len(bad), bad[0][0]))
    log("Verified.")


def _blocks(mask: int) -> str:
    return ",".join(str(i) for i in range(16) if mask >> i & 1) or "none"
