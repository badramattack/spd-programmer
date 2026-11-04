"""A minimal simulation of the firmware's serial protocol, for host tests."""

from __future__ import annotations

from typing import List


class FakeBoard:
    def __init__(self, image: bytes, blocks: int, protected: int = 0):
        self.mem = bytearray(image)
        self.blocks = blocks
        self.bsize = len(image) // blocks
        self.wp = protected
        self.powered = False
        self.out: List[bytes] = []
        self.log: List[str] = []
        self.last_unlocked = None

    # serial-port interface
    def write(self, data: bytes):
        line = data.decode().strip()
        self.log.append(line)
        self.out.append((self.handle(line) + "\n").encode())

    def readline(self) -> bytes:
        return self.out.pop(0) if self.out else b""

    def close(self):
        pass

    def handle(self, line: str) -> str:
        a = line.split()
        c = a[0]
        if c == "version":
            return "OK fw=0.1.0 proto=1"
        if c == "timeout":
            return "OK timeout_s=%s" % a[1]
        if c == "power":
            self.powered = a[1] == "on"
            return "OK mode=ddr5-udimm key=%s size=%d blocks=%d" % (self.mem[:4].hex(), len(self.mem), self.blocks) \
                if self.powered else "OK"
        if c == "ident":
            return "OK mode=ddr5-udimm key=%s size=%d blocks=%d" % (self.mem[:4].hex(), len(self.mem), self.blocks)
        if c == "read":
            addr, n = int(a[1]), int(a[2])
            return "OK " + self.mem[addr : addr + n].hex()
        if c == "write":
            addr, data = int(a[1]), bytes.fromhex(a[2])
            for i in range(len(data)):
                if self.wp >> ((addr + i) // self.bsize) & 1:
                    return "ERR WP write protected"
            self.mem[addr : addr + len(data)] = data
            return "OK written=%d" % len(data)
        if c == "wp":
            return "OK mask=0x%04x blocks=%d override=1" % (self.wp, self.blocks)
        if c == "unlock":
            prev, self.wp, self.last_unlocked = self.wp, 0, self.wp
            return "OK prev=0x%04x" % prev
        if c == "lock":
            mask = int(a[1], 0) if len(a) > 1 else self.last_unlocked
            self.wp |= mask
            return "OK mask=0x%04x" % mask
        return "ERR CMD unknown"
