"""Serial-port client for the SPD programmer firmware.

The protocol is line based (see firmware/README.md): every command ends with
one "OK ..." or "ERR <name> <message>" line; lines starting with '#' are
notices.
"""

from __future__ import annotations

import contextlib
import re
import time
from typing import Dict, Iterator, List, Optional, Tuple

import serial
import serial.tools.list_ports

from .image import DDR4, SpdImage

USB_VID = 0x2E8A  # Raspberry Pi
USB_PRODUCT = "SPD Programmer"
PROTO_VERSION = 1
CHUNK = 256  # bytes per write command


class ProgrammerError(Exception):
    """An ERR reply from the firmware."""

    def __init__(self, name: str, message: str, command: str = ""):
        super().__init__("%s: %s%s" % (name, message, " (command: %s)" % command if command else ""))
        self.name = name
        self.message = message


def find_ports() -> List[str]:
    ports = [p for p in serial.tools.list_ports.comports() if p.vid == USB_VID]
    named = [p for p in ports if (p.product or "") == USB_PRODUCT]
    return [p.device for p in (named or ports)]


def _kv(s: str) -> Dict[str, str]:
    return dict(m.groups() for m in re.finditer(r"(\w+)=(\S+)", s))


class Programmer:
    def __init__(self, port: Optional[str] = None, timeout: float = 15.0, verbose: bool = False, transport=None):
        """`transport` replaces the serial port (any object with write/readline), for tests."""
        self.verbose = verbose
        self.notices: List[str] = []
        if transport is not None:
            self.ser = transport
        else:
            self.ser = self._open_serial(port, timeout)
        info = self.version()
        if int(info.get("proto", 0)) != PROTO_VERSION:
            raise ProgrammerError("PROTO", "firmware protocol %s, tool expects %d: update one of them"
                                  % (info.get("proto"), PROTO_VERSION))

    @staticmethod
    def _open_serial(port: Optional[str], timeout: float):
        if port is None:
            ports = find_ports()
            if not ports:
                raise ProgrammerError("PORT", "no SPD programmer found (is it plugged in? use --port)")
            if len(ports) > 1:
                raise ProgrammerError("PORT", "several boards found (%s); pick one with --port" % ", ".join(ports))
            port = ports[0]
        ser = serial.Serial(port, 115200, timeout=timeout)  # baud is ignored by USB CDC
        ser.dtr = True  # the firmware ends any session when DTR drops
        ser.reset_input_buffer()
        ser.write(b"\n")
        time.sleep(0.05)
        ser.reset_input_buffer()
        return ser

    def close(self) -> None:
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- protocol ------------------------------------------------------------------

    def command(self, line: str) -> str:
        """Send one command; return the text after "OK", raise on "ERR"."""
        if self.verbose:
            print(">", line)
        self.ser.write(line.encode("ascii") + b"\n")
        while True:
            raw = self.ser.readline()
            if not raw:
                raise ProgrammerError("TIMEOUT", "no reply from the board", line)
            reply = raw.decode("ascii", "replace").strip()
            if self.verbose:
                print("<", reply)
            if reply.startswith("#"):
                self.notices.append(reply[1:].strip())
                continue
            if reply == "OK" or reply.startswith("OK "):
                return reply[3:]
            if reply.startswith("ERR"):
                parts = reply.split(" ", 2)
                raise ProgrammerError(parts[1] if len(parts) > 1 else "?", parts[2] if len(parts) > 2 else "", line)
            # anything else is noise from before a reconnect; ignore it

    # -- board ---------------------------------------------------------------------

    def version(self) -> Dict[str, str]:
        return _kv(self.command("version"))

    def status(self) -> Dict[str, str]:
        return _kv(self.command("status"))

    def set_timeout(self, seconds: int) -> None:
        self.command("timeout %d" % seconds)

    def power_on(self, force: bool = False) -> Dict[str, str]:
        return _kv(self.command("power on force" if force else "power on"))

    def power_off(self) -> None:
        self.command("power off")

    @contextlib.contextmanager
    def session(self, force: bool = False, idle_timeout: int = 30) -> Iterator["Programmer"]:
        """Keep the module powered for a series of commands.

        The firmware switches off by itself after `idle_timeout` seconds without
        a command, and whenever the port is closed.
        """
        self.set_timeout(idle_timeout)
        self.power_on(force)
        try:
            yield self
        finally:
            with contextlib.suppress(ProgrammerError, serial.SerialException):
                self.power_off()

    def debug(self, hv: bool = False) -> Dict[str, str]:
        return _kv(self.command("debug hv" if hv else "debug"))

    # -- Program / Restore (firmware/src/edit.h) -----------------------------------

    def program(self, preset: bytes) -> Dict[str, str]:
        """Apply a preset (spdtool.preset) with a backup record."""
        return _kv(self.command("program " + preset.hex()))

    def restore(self, force: bool = False) -> Dict[str, str]:
        return _kv(self.command("restore force" if force else "restore"))

    def record(self) -> Dict[str, str]:
        """Marked-serial and backup-record state of the inserted module."""
        return _kv(self.command("record"))

    def program_active(self) -> Dict[str, str]:
        """What the Program button does: apply the board's active preset."""
        return _kv(self.command("program"))

    def button(self) -> Dict[str, str]:
        """Result of the last button press; `msg` is the rest of the line."""
        reply = self.command("button")
        head, _, msg = reply.partition("msg=")
        out = _kv(head)
        out["msg"] = msg
        return out

    # -- presets stored on the board -----------------------------------------------

    def preset_list(self) -> Dict[str, str]:
        return _kv(self.command("preset list"))

    def preset_get(self, name: str) -> bytes:
        return bytes.fromhex(_kv(self.command("preset get " + name))["hex"])

    def preset_add(self, preset: bytes) -> None:
        self.command("preset add " + preset.hex())

    def preset_use(self, name: str) -> None:
        self.command("preset use " + name)

    def preset_del(self, name: str) -> None:
        self.command("preset del " + name)

    def preset_reset(self) -> None:
        self.command("preset reset")

    def led_test(self) -> None:
        self.command("leds")

    def leds(self, on: bool) -> None:
        self.command("leds on" if on else "leds off")

    def led_level(self, led: str, level: Optional[int] = None) -> int:
        """Read or set an LED's PWM brightness (0-1000) until the next reset."""
        reply = self.command("led %s" % led if level is None else "led %s %d" % (led, level))
        return int(_kv(reply)[led])

    def bootsel(self) -> None:
        self.command("bootsel")
        self.close()

    # -- module --------------------------------------------------------------------

    def ident(self) -> Dict[str, str]:
        return _kv(self.command("ident"))

    def read(self, addr: int, length: int) -> bytes:
        return bytes.fromhex(self.command("read %d %d" % (addr, length)))

    def read_image(self) -> SpdImage:
        size = int(self.ident()["size"])
        return SpdImage(self.read(0, size))

    def write(self, addr: int, data: bytes) -> None:
        """Raw write with read-back verification in the firmware. No CRC update."""
        for i in range(0, len(data), CHUNK):
            self.command("write %d %s" % (addr + i, data[i : i + CHUNK].hex()))

    def crc(self, fix: bool = False) -> Dict[str, Tuple[int, int]]:
        out = {}
        for k, v in _kv(self.command("crc fix" if fix else "crc")).items():
            stored, calc = v.split("/")
            out[k] = (int(stored, 16), int(calc, 16))
        return out

    def wp(self) -> Tuple[int, int, bool]:
        """(protected block mask, number of blocks, override available)."""
        kv = _kv(self.command("wp"))
        return int(kv["mask"], 16), int(kv["blocks"]), kv["override"] == "1"

    def unlock(self) -> int:
        """Clear all write protection; returns the mask that was protected."""
        return int(_kv(self.command("unlock"))["prev"], 16)

    def lock(self, mask: Optional[int] = None) -> int:
        """Protect the blocks in `mask` (default: what the last unlock cleared)."""
        cmd = "lock" if mask is None else "lock 0x%x" % mask
        return int(_kv(self.command(cmd))["mask"], 16)

    def temperature(self) -> float:
        return int(_kv(self.command("temp"))["milli_c"]) / 1000

    def scan(self) -> List[int]:
        return [int(a, 16) for a in self.command("scan").split()]

    def mr_read(self, reg: int, length: int = 1) -> bytes:
        return bytes.fromhex(self.command("mr %d %d" % (reg, length)))

    def mr_write(self, reg: int, value: int) -> None:
        self.command("mr %d = %d" % (reg, value))
