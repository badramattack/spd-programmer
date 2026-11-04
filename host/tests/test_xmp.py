import struct

from samples import ddr4_3200_8gb, ddr5_4800_16gb
from spdtool.decode import format_summary
from spdtool.image import crc16
from spdtool.xmp import format_profiles, profiles


def _block(body: bytes) -> bytes:
    body = body.ljust(62, b"\0")
    return body + struct.pack("<H", crc16(body))


def _with_xmp(img, vdd=0x27):
    """A DDR5 image with an XMP 3.0 header and one DDR5-6000 profile."""
    names = b"".join(n.ljust(16, b"\0") for n in (b"Fast\r", b"", b""))
    header = _block(b"\x0c\x4a\x30\x01" + bytes(10) + names)
    tck = 333  # DDR5-6000
    cls = sum(1 << ((cl - 20) // 2) for cl in (30, 32, 36, 40))
    prof = bytes([0x30, vdd, vdd, 0, 0]) + struct.pack("<H", tck) + cls.to_bytes(5, "little") + b"\0"
    prof += struct.pack("<6H", 12000, 12666, 12666, 26640, 39000, 30000) + struct.pack("<3H", 295, 160, 130)
    img.data[640:704] = header
    img.data[704:768] = _block(prof)
    return img


def test_xmp3_profile_decodes():
    img = _with_xmp(ddr5_4800_16gb())
    (p,) = profiles(img)
    assert (p.number, p.name, p.crc_ok) == (1, "Fast", True)
    assert p.rate(img) == 6000 and p.tck == 333
    assert p.cls == [30, 32, 36, 40] and p.cl(img) == 36
    assert (p.taa, p.trcd, p.tras, p.twr, p.trfc1) == (12000, 12666, 26640, 30000, 295)
    assert round(p.vdd, 2) == 1.35 and round(p.vpp, 2) == 1.80
    assert "XMP 1" in format_summary(img) and "DDR5-6000 CL36" in format_summary(img)
    assert "VDD 1.35 V" in format_profiles(img)


def test_xmp3_crc_mismatch_is_reported():
    img = _with_xmp(ddr5_4800_16gb())
    img[720] ^= 1
    (p,) = profiles(img)
    assert not p.crc_ok and "MISMATCH" in format_profiles(img)


def test_no_profiles():
    assert profiles(ddr5_4800_16gb()) == [] and format_profiles(ddr5_4800_16gb()) is None
    assert profiles(ddr4_3200_8gb()) == []
