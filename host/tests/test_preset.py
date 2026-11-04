import struct
from pathlib import Path

import pytest

from samples import ddr4_3200_8gb, ddr5_4800_16gb
from spdtool.decode import modified_text
from spdtool.image import MARKER, crc16
from spdtool.preset import DEFAULTS, Preset, Record, c_defaults, inc, is_marked, set_to, when


def test_defaults_round_trip():
    for p in DEFAULTS:
        q = Preset.decode(p.encode())
        assert q.encode() == p.encode()
        assert (q.name, q.description) == (p.name, p.description)


def test_firmware_defaults_header_is_current():
    header = Path(__file__).parents[2] / "firmware" / "src" / "default_presets.h"
    assert header.read_text() == c_defaults(), "regenerate: python -m spdtool.preset --c-defaults"


def test_ops_encode_field_positions():
    # DDR5 ecc_bits: byte 235 bits 4..3; DDR4 rows: byte 5 bits 5..3, codes 12+i.
    assert set_to("ecc_bits", 0).encode("DDR5") == struct.pack("<BHBB", 1, 235, 0x18, 0)
    rows = inc("rows").encode("DDR4")
    assert rows[:5] == struct.pack("<BHBB", 2, 5, 0x38, 6)  # 12..17 -> 13..18
    assert rows[5:7] == bytes([0 << 3, 1 << 3])
    guarded = when("rank_mix", "asymmetrical", [inc("rows2")]).encode("DDR5")
    assert guarded[:6] == struct.pack("<BHBBB", 4, 234, 0x40, 0x40, 1)


def test_unknown_field_or_value_is_rejected():
    with pytest.raises(ValueError):
        set_to("nosuch", 1).encode("DDR5")
    with pytest.raises(ValueError):
        set_to("ecc_bits", 16).encode("DDR5")


def test_marked_serial_depends_on_contents_only():
    img = ddr5_4800_16gb()
    a = img.marked_serial()
    assert a[0] == MARKER
    img.serial = b"\x01\x02\x03\x04"
    img[510] ^= 0xFF  # CRC bytes and serial are excluded
    img[1000] = 0x55  # so is the record area
    assert img.marked_serial() == a
    img[235] ^= 0x08
    assert img.marked_serial() != a


def _record(img, edited, entries, pid=0x12345678):
    """A backup record as the firmware writes it (firmware/src/edit.c)."""
    crcs = [img.u16(s.at) for s in img.layout.crc] + [0]
    raw = b"SR\x01\x00" + bytes([len(entries)]) + struct.pack("<I", pid) + img.serial
    raw += edited.marked_serial() + struct.pack("<HH", *crcs[:2])
    raw += b"".join(struct.pack("<HB", off, img[off]) for off, _ in entries)
    return raw + struct.pack("<H", crc16(raw))


def test_record_rebuilds_the_original():
    for make in (ddr4_3200_8gb, ddr5_4800_16gb):
        orig = make()
        edit = orig.copy()
        edit[5] ^= 0x08
        raw = _record(orig, edit, [(5, None)])
        edit.serial = raw[13:17]
        edit.fix_crc()
        off, n = edit.layout.record_area
        edit.data[off : off + len(raw)] = raw
        assert is_marked(edit) and "record on the module" in modified_text(edit)
        rec = Record.decode(raw)
        back = rec.original(edit)
        assert back.data[:off] == orig.data[:off]
        assert modified_text(orig) == ""


def test_record_crc_is_checked():
    img = ddr5_4800_16gb()
    raw = bytearray(_record(img, img, [(4, None)]))
    raw[21] ^= 1
    with pytest.raises(ValueError):
        Record.decode(bytes(raw))
