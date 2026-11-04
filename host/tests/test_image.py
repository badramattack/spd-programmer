import pytest

from samples import ddr4_3200_8gb, ddr5_4800_16gb
from spdtool.decode import capacity_gb, format_summary, summary
from spdtool.fields import fields_for, get_field, parse_time, set_field
from spdtool.image import DDR4, DDR5, SpdImage, crc16
from spdtool.program import field_diff, prepare
from spdtool.speed import cas_latency, nck, rate_of, set_speed


def test_crc16_xmodem_check_value():
    assert crc16(b"123456789") == 0x31C3


def test_generation_detection_and_crc():
    for make, gen in ((ddr4_3200_8gb, DDR4), (ddr5_4800_16gb, DDR5)):
        img = make()
        assert img.generation == gen
        assert img.crc_ok()
        img[18] ^= 1
        assert not img.crc_ok()


def test_ddr4_tck_example_from_annex_l():
    # Annex L: MTB 9, fine 0xCA (-54) -> 1.071 ns (DDR4-1866)
    img = ddr4_3200_8gb()
    img[18], img[125] = 9, 0xCA
    assert get_field(img, "tCKmin").get(img) == 1071
    assert rate_of(img) == 1866


def test_ddr4_cl_examples_from_annex_l():
    img = ddr4_3200_8gb()
    f = get_field(img, "CL")
    img.data[20:24] = bytes([0xB4, 0x05, 0, 0])
    assert f.get(img) == [9, 11, 12, 14, 15, 17]
    img.data[20:24] = bytes([0, 0x78, 0x01, 0x80])
    assert f.get(img) == [34, 35, 36, 37, 39]
    f.set(img, [34, 35, 36, 37, 39])
    assert bytes(img.data[20:24]) == bytes([0, 0x78, 0x01, 0x80])


def test_ddr5_cl_example_from_jesd400():
    img = ddr5_4800_16gb()
    img.data[24:29] = bytes([0x7A, 0x05, 0, 0, 0])
    assert get_field(img, "CL").get(img) == [22, 26, 28, 30, 32, 36, 40]


@pytest.mark.parametrize("make", [ddr4_3200_8gb, ddr5_4800_16gb])
def test_timing_fields_round_trip(make):
    img = make()
    for name, f in fields_for(img).items():
        if f.parse is not parse_time:
            continue
        for ps in (13750, 16000, 1071, 350000 if name.startswith("tRFC") else 45750):
            try:
                f.set(img, ps)
            except ValueError:
                assert img.generation == DDR4 and ps > 255 * 125  # 8-bit MTB fields
                continue
            got = f.get(img)
            # DDR4 fields without a fine byte round up to 125 ps, DDR5 tRFC to 1 ns
            assert ps <= got < ps + 1000, (name, ps, got)


def test_ddr4_fine_encoding_matches_jedec_convention():
    img = ddr4_3200_8gb()
    set_field(img, "tCKmin", "0.833ns")
    assert (img[18], img[125]) == (7, 0xD6)  # 7 * 125 - 42


def test_capacity_and_summary():
    assert capacity_gb(ddr4_3200_8gb()) == 8
    assert capacity_gb(ddr5_4800_16gb()) == 16
    d4 = dict(summary(ddr4_3200_8gb()))
    assert d4["Max speed"].startswith("DDR4-3200")
    assert d4["Timings @ max"].startswith("CL22-22-22-52")
    d5 = dict(summary(ddr5_4800_16gb()))
    assert d5["Max speed"].startswith("DDR5-4800")
    assert d5["Timings @ max"].startswith("CL40-39-39")
    assert "Samsung" in d4["Module vendor"] and "SK hynix" in d5["Module vendor"]
    format_summary(ddr5_4800_16gb())


def test_set_ranks_doubles_capacity():
    img = ddr5_4800_16gb()
    set_field(img, "ranks", "2")
    assert capacity_gb(img) == 32


def test_set_speed_down_and_up():
    img = ddr5_4800_16gb()
    set_speed(img, 4000)
    assert get_field(img, "tCKmin").get(img) == 500
    assert cas_latency(img, 500) == 32
    img = ddr5_4800_16gb()
    notes = set_speed(img, 5600, cl=46)
    assert get_field(img, "tAA").get(img) == 46 * 357
    assert 46 in get_field(img, "CL").get(img)
    assert cas_latency(img, 357) == 46
    assert any("CL46" in n for n in notes)


def test_set_speed_ddr4_keeps_ns_timings():
    img = ddr4_3200_8gb()
    set_speed(img, 2400)
    assert get_field(img, "tCKmin").get(img) == 833
    assert nck(img, 13750, 833) == 17
    with pytest.raises(ValueError):
        set_speed(img, 3000)


def test_prepare_bumps_serial_only_for_real_changes():
    cur = ddr5_4800_16gb()
    same = prepare(cur, cur.copy())
    assert same.serial == cur.serial and not cur.diff(same)

    edited = cur.copy()
    set_field(edited, "tRFC1", "350ns")
    target = prepare(cur, edited)
    assert target.serial != cur.serial
    assert target.crc_ok()
    assert target.serial == prepare(cur, edited).serial  # deterministic
    assert prepare(cur, edited, bump_serial=False).serial == cur.serial
    names = [n for n, _, _ in field_diff(cur, target)]
    assert names == ["tRFC1", "serial"]

    explicit = cur.copy()
    set_field(explicit, "serial", "deadbeef")
    assert prepare(cur, explicit).serial == bytes.fromhex("deadbeef")


def test_blocks_of_ranges():
    cur = ddr5_4800_16gb()
    t = cur.copy()
    t[30] = 1
    t[520] = 2
    assert cur.blocks_of(cur.diff(t)) == (1 << 0) | (1 << 8)


def test_part_number_and_date():
    img = ddr4_3200_8gb()
    assert get_field(img, "part_number").get(img) == "TEST-DDR4-3200"
    assert get_field(img, "mfg_date").get(img) == (2021, 15)
    set_field(img, "part_number", "MY-PART")
    assert bytes(img.data[329:349]) == b"MY-PART".ljust(20, b" ")
    with pytest.raises(ValueError):
        set_field(img, "part_number", "X" * 21)


def test_bad_image_size():
    with pytest.raises(ValueError):
        SpdImage(bytes(300))
