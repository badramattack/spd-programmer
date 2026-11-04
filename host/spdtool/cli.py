"""Command-line interface: `spdtool <command> ...` (or `python -m spdtool`)."""

from __future__ import annotations

import argparse
import runpy
import sys
import time
from typing import List

from . import __version__
from .decode import extra_profiles, format_fields, format_summary
from .xmp import format_profiles as format_xmp
from .device import Programmer, ProgrammerError, find_ports
from .fields import get_field, set_field
from .image import DDR5, SpdImage
from .preset import DEFAULTS, Preset, Record
from .program import MR_NAMES, _blocks, apply, backup, field_diff, prepare
from .speed import set_speed


def _open(args) -> Programmer:
    return Programmer(args.port, verbose=args.verbose)


def _source_image(args) -> SpdImage:
    """The image to look at: --file if given, otherwise read from the board."""
    if getattr(args, "file", None):
        return SpdImage.from_file(args.file)
    with _open(args) as p:
        return p.read_image()


# -- read-only commands ---------------------------------------------------------------


def cmd_status(args):
    with _open(args) as p:
        st = p.status()
        ver = p.version()
    print("Firmware      %s" % ver.get("fw"))
    print("Mode switches %s" % st["mode"])
    print("Power         %s" % st["state"])
    seat = int(st["seat_mv"])
    seat_txt = "module seated" if seat < 200 else "empty socket" if seat > 2500 else "MISALIGNED module?"
    print("Seat sense    %d mV (%s)" % (seat, seat_txt))
    print("Contact 5     %s mV" % st["vddspd_mv"])
    print("Rail fault    %s" % ("no" if st["fault_n"] == "1" else "YES"))
    print("Buttons       program=%s restore=%s" % (st["btn_program"], st["btn_restore"]))


def cmd_info(args):
    print(format_summary(_source_image(args)))


def cmd_xmp(args):
    img = _source_image(args)
    text = format_xmp(img)
    if text:
        print(text)
    elif extra_profiles(img):
        print("%s: detected, but only DDR5 XMP 3.0 is decoded." % ", ".join(extra_profiles(img)))
    else:
        print("No XMP/EXPO profiles (JEDEC only).")


def cmd_fields(args):
    print(format_fields(_source_image(args)))


def cmd_get(args):
    img = _source_image(args)
    for name in args.field:
        f = get_field(img, name)
        print("%s = %s" % (f.name, f.fmt(f.get(img))))


def cmd_dump(args):
    with _open(args) as p:
        img = p.read_image()
    img.save(args.output)
    print("Saved %s %d bytes to %s (serial %s, CRC %s)" % (
        img.generation, len(img.data), args.output, img.serial.hex(), "OK" if img.crc_ok() else "BAD"))


def cmd_diff(args):
    a = SpdImage.from_file(args.reference)
    b = _source_image(args)
    ranges = a.diff(b)
    if not ranges:
        print("Identical.")
        return
    for name, old, new in field_diff(a, b):
        print("%-14s %s -> %s" % (name, old, new))
    for off, old, new in ranges:
        print("byte %4d (0x%03x): %s -> %s" % (off, off, old.hex(" "), new.hex(" ")))


def cmd_hexdump(args):
    img = _source_image(args)
    for off in range(0, len(img.data), 16):
        row = img.data[off : off + 16]
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
        print("%03x: %s  %s" % (off, row.hex(" "), text))


# -- edits --------------------------------------------------------------------------


def _edit(args, modify):
    """Run `modify(img) -> notes` on the board image (or --file), then write it back."""
    offline = bool(args.file)
    if offline:
        current = SpdImage.from_file(args.file)
        target = current.copy()
        for n in modify(target) or []:
            print(n)
        target = prepare(current, target, bump_serial=not args.keep_serial, fix_crc=not args.no_crc)
        _show_changes(current, target)
        out = args.output or args.file
        if not args.dry_run:
            target.save(out)
            print("Saved to %s" % out)
        return

    with _open(args) as p, p.session(force=args.force):
        current = p.read_image()
        target = current.copy()
        for n in modify(target) or []:
            print(n)
        target = prepare(current, target, bump_serial=not args.keep_serial, fix_crc=not args.no_crc)
        _show_changes(current, target)
        if args.dry_run:
            print("Dry run: nothing written.")
            return
        apply(p, current, target, unlock=args.unlock)


def _show_changes(old: SpdImage, new: SpdImage):
    changes = field_diff(old, new)
    for name, a, b in changes:
        print("  %-14s %s -> %s" % (name, a, b))
    n = sum(len(r[2]) for r in old.diff(new))
    print("%d byte(s) change." % n)


def cmd_set(args):
    def modify(img):
        for assignment in args.assignment:
            if "=" not in assignment:
                raise SystemExit("expected FIELD=VALUE, got %r" % assignment)
            name, value = assignment.split("=", 1)
            set_field(img, name.strip(), value.strip())

    _edit(args, modify)


def cmd_setbyte(args):
    def modify(img):
        for assignment in args.assignment:
            addr, value = (int(x, 0) for x in assignment.split("=", 1))
            if not 0 <= value <= 0xFF:
                raise SystemExit("byte value out of range: %r" % assignment)
            img[addr] = value

    _edit(args, modify)


def cmd_speed(args):
    _edit(args, lambda img: set_speed(img, args.rate, args.cl, args.trcd, args.trp))


def cmd_write_image(args):
    new = SpdImage.from_file(args.image)
    with _open(args) as p, p.session(force=args.force):
        current = p.read_image()
        target = prepare(current, new, bump_serial=args.new_serial, fix_crc=not args.no_crc)
        _show_changes(current, target)
        if not args.dry_run:
            apply(p, current, target, unlock=args.unlock)


# -- write protection / misc ------------------------------------------------------------


def cmd_wp(args):
    with _open(args) as p:
        mask, n, ovr = p.wp()
    print("Protected blocks: %s (of %d)" % (_blocks(mask), n))
    if n == 16:
        print("Override (offline mode): %s" % ("yes, blocks can be unlocked" if ovr else "NO (HSA not grounded at power-up)"))


def cmd_unlock(args):
    with _open(args) as p:
        prev = p.unlock()
    print("Unlocked. Previously protected blocks: %s" % _blocks(prev))
    print("Re-lock with: spdtool lock 0x%x" % prev)


def cmd_lock(args):
    with _open(args) as p:
        mask = p.lock(int(args.mask, 0) if args.mask else None)
    print("Protected blocks: %s" % _blocks(mask))


def cmd_crc(args):
    with _open(args) as p:
        res = p.crc(fix=args.fix)
    for name, (stored, calc) in res.items():
        print("%-7s stored %04x computed %04x %s" % (name, stored, calc, "OK" if stored == calc else "BAD"))


def cmd_regs(args):
    with _open(args) as p, p.session():
        if p.ident()["mode"].startswith("ddr4"):
            raise SystemExit("registers are only available on DDR5 (SPD5118)")
        regs = p.mr_read(0, 128)
    for i, v in enumerate(regs):
        if i in MR_NAMES or v:
            print("MR%-3d 0x%02x  %02x  %s" % (i, i, v, MR_NAMES.get(i, "")))


def cmd_temp(args):
    with _open(args) as p:
        print("%.2f °C" % p.temperature())


def cmd_scan(args):
    with _open(args) as p:
        print(" ".join("0x%02x" % a for a in p.scan()) or "nothing")


def cmd_debug(args):
    with _open(args) as p:
        p.debug(hv=args.hv)
        print("Rails ON for %s with an empty socket. Measure now; Ctrl-C to switch off."
              % p.status()["powered_mode"])
        try:
            while True:
                st = p.status()
                if st["state"] != "debug":
                    print("\nBoard switched off: %s" % ("; ".join(p.notices) or "see status"))
                    return
                print("\rseat %4s mV  contact 5 %4s mV  HV %s  fault %s   " % (
                    st["seat_mv"], st["vddspd_mv"], "ON " if st["hv"] == "1" else "off",
                    "no" if st["fault_n"] == "1" else "YES"), end="", flush=True)
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        finally:
            p.power_off()
            print("\nRails off.")


def cmd_leds(args):
    with _open(args) as p:
        print("Done, Modified, Busy, Fault in turn, then all on for 2 s.")
        p.led_test()


def _getkey() -> str:
    """One keypress, without waiting for Enter."""
    try:
        import msvcrt  # Windows
        return msvcrt.getwch()
    except ImportError:
        import termios
        import tty
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setcbreak(fd)
            return sys.stdin.read(1)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)


def cmd_ledtune(args):
    with _open(args) as p:
        level = p.led_level(args.led)
        p.leds(True)
        print("All LEDs on. u/d: %s brighter/dimmer (U/D: fine steps), q: quit" % args.led)
        try:
            while True:
                print("\r%s = %4d / 1000  (%5.1f %%)   " % (args.led, level, level / 10), end="", flush=True)
                k = _getkey()
                if k in "qQ\x03\x1b":
                    break
                if k in "uU":
                    level = min(1000, level + 1) if k == "U" else min(1000, max(level + 1, round(level * 1.25)))
                elif k in "dD":
                    level = max(0, level - 1) if k == "D" else max(0, min(level - 1, round(level / 1.25)))
                level = p.led_level(args.led, level)
        finally:
            p.leds(False)
        print("\n%s level %d: until reset only; make it the default in firmware/src/board.h" % (args.led, level))


# -- Program / Restore and presets ---------------------------------------------------


def _load_presets(source: str) -> List[Preset]:
    """Presets from a built-in name or a Python file (every Preset in it)."""
    builtin = {p.name: p for p in DEFAULTS}
    if source in builtin:
        return [builtin[source]]
    if not source.endswith(".py"):
        raise SystemExit("error: %r is neither a built-in preset (%s) nor a .py file" % (source, ", ".join(builtin)))
    found: List[Preset] = []
    for v in runpy.run_path(source).values():
        for item in v if isinstance(v, (list, tuple)) else [v]:
            if isinstance(item, Preset) and item not in found:
                found.append(item)
    if not found:
        raise SystemExit("error: %s defines no Preset" % source)
    return found


def _print_preset(pr: Preset, active: bool = False) -> None:
    print("%s%s  (id %08x)  %s" % (pr.name, " [active]" if active else "", pr.id, pr.description))
    for gen, ops in (("DDR4", pr.ddr4), ("DDR5", pr.ddr5)):
        print("    %s: %s" % (gen, "; ".join(map(str, ops)) if ops else "(nothing: refused)"))


def cmd_preset(args):
    with _open(args) as p:
        if args.action == "list":
            st = p.preset_list()
            names = [n for n in st.get("names", "").split(",") if n]
            print("Presets on the board (%s):" % ("built-in defaults" if st.get("source") == "builtin" else "flash"))
            for n in names:
                _print_preset(Preset.decode(p.preset_get(n)), n == st.get("active"))
            if st.get("active", "-") == "-":
                print("No active preset: the Program button is refused (spdtool preset use <name>).")
        elif args.action == "add":
            if not args.arg:
                raise SystemExit("usage: spdtool preset add <built-in name | file.py> [--use]")
            loaded = _load_presets(args.arg)
            for pr in loaded:
                p.preset_add(pr.encode())
                print("Added %s (id %08x)" % (pr.name, pr.id))
            if args.use:
                p.preset_use(loaded[-1].name)
                print("Active: %s" % loaded[-1].name)
        elif args.action in ("use", "del", "show"):
            if not args.arg:
                raise SystemExit("usage: spdtool preset %s <name>" % args.action)
            if args.action == "show":
                _print_preset(Preset.decode(p.preset_get(args.arg)), p.preset_list().get("active") == args.arg)
            elif args.action == "use":
                p.preset_use(args.arg)
                print("Active: %s (the Program button applies it)" % args.arg)
            else:
                p.preset_del(args.arg)
                print("Deleted %s" % args.arg)
        elif args.action == "reset":
            p.preset_reset()
            print("Back to the built-in presets: %s" % ", ".join(pr.name for pr in DEFAULTS))


def cmd_program(args):
    with _open(args) as p:
        if args.preset:
            names = p.preset_list().get("names", "").split(",")
            raw = p.preset_get(args.preset) if args.preset in names else _load_presets(args.preset)[0].encode()
            r = p.program(raw)
        else:
            r = p.program_active()
    if "preset" in r:
        print("Applied %s: %s byte(s) changed, backup record in %s, new serial %s" % (
            r["preset"], r["changed"], r["record"].replace("+", " and "), r["serial"]))
    else:
        print("Already applied: nothing to do.")


def cmd_restore(args):
    with _open(args) as p:
        r = p.restore(force=args.force)
    print("Restored %s byte(s) from the record in %s; serial back to %s" % (
        r["restored"], r["record"].replace("+", " and "), r["serial"]))


def cmd_record(args):
    with _open(args) as p:
        r = p.record()
        img = None
        known = {pr.id: pr.name for pr in DEFAULTS}
        if r.get("record", "none") != "none":
            img = p.read_image()
            for n in filter(None, p.preset_list().get("names", "").split(",")):
                known[Preset.decode(p.preset_get(n)).id] = n
    print("Modified      %s" % {"1": "yes", "0": "no"}[r["modified"]])
    print("Serial check  %s" % {"ok": "matches the contents", "changed": "contents changed since Program",
                                "-": "not a marked serial"}[r["hash"]])
    print("Record        %s" % r["record"].replace("+", " and "))
    print("Record area   %s" % {"free": "free (all 00 or FF)", "record": "holds the record",
                                "profiles": "XMP/EXPO profiles: records go to flash only",
                                "used": "in use: records go to flash only"}[r["area"]])
    print("Flash records %s on this board" % r["flash_records"])
    if img is not None:
        rec = Record.decode(bytes.fromhex(r["rec"]))
        print("Preset        %s" % known.get(rec.preset_id, "%08x (not on this board)" % rec.preset_id))
        print("Orig. serial  %s" % rec.serial_orig.hex())
        print("Changes (original -> now):")
        for name, before, now in field_diff(rec.original(img), img):
            if name != "serial":
                print("  %-12s %s -> %s" % (name, before, now))


def cmd_button(args):
    with _open(args) as p:
        b = p.button()
    if b.get("action") == "none":
        print("No button pressed since the board started.")
    else:
        print("%s: %s%s%s" % (b["action"].capitalize(), b["result"],
                              " (blink code %s)" % b["code"] if b["code"] != "0" else "",
                              ": " + b["msg"] if b["msg"] else ""))


def cmd_bootsel(args):
    with _open(args) as p:
        p.bootsel()
    print("Rebooted into BOOTSEL; flash with: picotool load -x spd_programmer.uf2")


def cmd_ports(args):
    print("\n".join(find_ports()) or "no boards found")


# -- argument parsing ----------------------------------------------------------------------


def _edit_options(p, image_input=True):
    if image_input:
        p.add_argument("--file", help="edit this image file instead of the module (offline)")
        p.add_argument("-o", "--output", help="with --file: write the result here (default: in place)")
        p.add_argument("--keep-serial", action="store_true", help="do not change the serial number")
    p.add_argument("--unlock", action="store_true", help="clear write protection for the write, re-lock after")
    p.add_argument("--no-crc", action="store_true", help="do not update the CRCs")
    p.add_argument("--dry-run", "-n", action="store_true", help="show the changes only")
    p.add_argument("--force", action="store_true", help="skip the SPD type check (for corrupted SPDs)")


# `spdtool --help` lists the commands by group (like git); each group lists
# its commands in the order they are added below.
GROUPS = [
    ("read", "Reading"),
    ("edit", "Editing (backup, new serial, CRCs and verification are automatic)"),
    ("wp", "Write protection"),
    ("program", "Program / Restore (what the buttons do)"),
    ("board", "Board"),
    ("debug", "Debugging and bring-up"),
]


def build_parser() -> argparse.ArgumentParser:
    commands = {g: [] for g, _ in GROUPS}
    ap = argparse.ArgumentParser(prog="spdtool", description="DDR4/DDR5 SPD programmer",
                                 usage="spdtool [--port PORT] [-v] <command> [options]",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show the serial protocol")
    ap.add_argument("--version", action="version", version="spdtool " + __version__)
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="command", help=argparse.SUPPRESS)

    def add(name, fn, help, file_opt=False, group="read"):
        # No help= here: argparse would list every command in one flat block.
        p = sub.add_parser(name, description=help)
        p.set_defaults(fn=fn)
        commands[group].append((name, help))
        if file_opt:
            p.add_argument("--file", help="use this image file instead of the module")
        return p

    add("status", cmd_status, "board status: switches, power, seat sense", group="board")
    add("info", cmd_info, "decoded summary (size, speed, timings, vendor)", True)
    add("fields", cmd_fields, "all editable fields with their values", True)
    p = add("get", cmd_get, "print named fields", True)
    p.add_argument("field", nargs="+")
    p = add("dump", cmd_dump, "save the SPD contents to a file")
    p.add_argument("output")
    p = add("hexdump", cmd_hexdump, "hex dump of the SPD contents", True, group="debug")
    p = add("diff", cmd_diff, "compare the module (or --file) with an image file", True)
    p.add_argument("reference")

    p = add("set", cmd_set, "set fields, e.g. tRFC1=350ns part_number=TEST", group="edit")
    p.add_argument("assignment", nargs="+", metavar="FIELD=VALUE")
    _edit_options(p)
    p = add("setbyte", cmd_setbyte, "set raw bytes, e.g. 0x12=0x05", group="edit")
    p.add_argument("assignment", nargs="+", metavar="ADDR=VALUE")
    _edit_options(p)
    p = add("speed", cmd_speed, "set the max speed grade, e.g. 'speed 4800' or 'speed 4800 --cl 40'", group="edit")
    p.add_argument("rate", type=int, help="data rate in MT/s (e.g. 2400, 3200, 4800, 5600)")
    p.add_argument("--cl", type=int, help="CAS latency in clocks at this speed (sets tAA)")
    p.add_argument("--trcd", type=int, help="tRCD in clocks at this speed")
    p.add_argument("--trp", type=int, help="tRP in clocks at this speed")
    _edit_options(p)
    p = add("write-image", cmd_write_image, "write an image file (e.g. a backup) to the module", group="edit")
    p.add_argument("image")
    _edit_options(p, image_input=False)
    p.add_argument("--new-serial", action="store_true", help="give the written image a new serial number")

    add("wp", cmd_wp, "show write protection", group="wp")
    add("unlock", cmd_unlock, "clear all write protection (DDR4: uses VHV)", group="wp")
    p = add("lock", cmd_lock, "write-protect blocks (default: those the last unlock cleared)", group="wp")
    p.add_argument("mask", nargs="?", help="block bit mask, e.g. 0x3 or 0x1ff")
    add("xmp", cmd_xmp, "decoded XMP 3.0 profiles (DDR5): speed, timings, voltages", True)
    p = add("crc", cmd_crc, "check the CRCs")
    p.add_argument("--fix", action="store_true", help="rewrite wrong CRCs")
    add("regs", cmd_regs, "DDR5: dump the SPD5118 hub registers", group="debug")
    add("temp", cmd_temp, "read the module temperature sensor")
    add("scan", cmd_scan, "list I2C addresses that respond", group="debug")
    p = add("debug", cmd_debug, "switch the rails on with an EMPTY socket, for measuring", group="debug")
    p.add_argument("--hv", action="store_true", help="also enable VHV on contact 150 (DDR4 mode only)")
    add("leds", cmd_leds, "LED test: light the firmware-driven LEDs", group="board")
    p = add("ledtune", cmd_ledtune, "adjust an LED's PWM brightness interactively (u/d)", group="debug")
    p.add_argument("led", nargs="?", default="done", choices=["done", "mod", "busy", "fault"])
    p = add("preset", cmd_preset, "presets on the board: list, show, add, use, del, reset", group="program")
    p.add_argument("action", nargs="?", default="list", choices=["list", "show", "add", "use", "del", "reset"])
    p.add_argument("arg", nargs="?", help="preset name, or for add: a built-in name or a .py file")
    p.add_argument("--use", action="store_true", help="add: also make it the active preset")
    p = add("program", cmd_program, "apply a preset with a backup record (what the Program button does)", group="program")
    p.add_argument("--preset", help="preset on the board, built-in name or .py file (default: the active one)")
    p = add("restore", cmd_restore, "undo the last Program from its backup record", group="program")
    p.add_argument("--force", action="store_true", help="also when the contents changed since Program")
    add("record", cmd_record, "Program/Restore state of the module and its backup record", group="program")
    add("button", cmd_button, "result of the last button press", group="board")
    add("bootsel", cmd_bootsel, "reboot the Pico into BOOTSEL mode for flashing", group="debug")
    add("ports", cmd_ports, "list detected boards", group="debug")
    width = max(len(n) for cmds in commands.values() for n, _ in cmds)
    ap.epilog = "\n\n".join(
        title + ":\n" + "\n".join("  %-*s  %s" % (width, n, h) for n, h in commands[g])
        for g, title in GROUPS) + "\n\nspdtool <command> --help shows the options of a command."
    return ap


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        args.fn(args)
    except ProgrammerError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    except (ValueError, KeyError) as e:
        print("error: %s" % (e.args[0] if e.args else e), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
