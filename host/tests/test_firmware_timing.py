"""The firmware's timing encoder (firmware/src/timing.c) must match spdtool's.

Compiles timing.c with the host C compiler and compares it, field by field,
with the Field setters for every DDR4 and DDR5 timing field. Skipped without
a C compiler.
"""

import ctypes
import random
import shutil
import subprocess
from pathlib import Path

import pytest

from samples import ddr4_3200_8gb, ddr5_4800_16gb
from spdtool.fields import fields_for

SRC = Path(__file__).parents[2] / "firmware" / "src" / "timing.c"
CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")


class Timing(ctypes.Structure):
    _fields_ = [("enc", ctypes.c_uint8), ("shift", ctypes.c_uint8), ("off", ctypes.c_uint16),
                ("off2", ctypes.c_uint16), ("off3", ctypes.c_uint16)]


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    if not CC or not SRC.exists():
        pytest.skip("no C compiler or firmware sources")
    so = tmp_path_factory.mktemp("timing") / "timing.so"
    subprocess.run([CC, "-shared", "-fPIC", "-O1", "-o", str(so), str(SRC)], check=True)
    lib = ctypes.CDLL(str(so))
    lib.timing_set.restype = lib.timing_get.restype = ctypes.c_bool
    return lib


def _values():
    rng = random.Random(1)
    fixed = [0, 1, 124, 125, 126, 999, 1000, 1001, 13750, 31875, 32000, 350000, 511875, 65535, 65536,
             511999, 512000, 4095 * 125, 4096 * 125, 65535 * 125, 65536 * 125, 65535 * 1000, 65536 * 1000]
    return fixed + [rng.randrange(0, 70_000_000) for _ in range(300)]


@pytest.mark.parametrize("make", [ddr4_3200_8gb, ddr5_4800_16gb])
def test_timing_encoders_match(lib, make):
    base = make()
    fields = [f for f in fields_for(base).values() if f.timing]
    assert len(fields) > 10
    for f in fields:
        t = Timing(f.timing[0], f.timing[4], *f.timing[1:4])
        for ps in _values():
            want = base.copy()
            try:
                f.set(want, ps)
                ok = True
            except ValueError:
                ok = False
            buf = (ctypes.c_uint8 * len(base.data)).from_buffer_copy(bytes(base.data))
            assert lib.timing_set(buf, ctypes.byref(t), ps) == ok, (f.name, ps)
            if ok:
                assert bytes(buf) == bytes(want.data), (f.name, ps)
                got = ctypes.c_int32()
                assert lib.timing_get(buf, ctypes.byref(t), ctypes.byref(got))
                assert got.value == f.get(want), (f.name, ps)
            else:
                assert bytes(buf) == bytes(base.data), (f.name, ps)  # unchanged when refused
