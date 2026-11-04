"""Human-readable summary of an SPD image, in the spirit of decode-dimms."""

from __future__ import annotations

from typing import List, Tuple

from . import jep106
from .fields import fields_for, get_field
from .image import DDR4, DDR5, MARKER, SpdImage
from .speed import cas_latency, nck, rate_of

DDR4_MODULE = {1: "RDIMM", 2: "UDIMM", 3: "SO-DIMM", 4: "LRDIMM", 5: "Mini-RDIMM", 6: "Mini-UDIMM",
               8: "72b-SO-RDIMM", 9: "72b-SO-UDIMM", 12: "16b-SO-DIMM", 13: "32b-SO-DIMM"}
DDR5_MODULE = {1: "RDIMM", 2: "UDIMM", 3: "SODIMM", 4: "LRDIMM", 5: "CUDIMM", 6: "CSODIMM",
               7: "MRDIMM", 8: "CAMM2", 10: "DDIMM", 11: "Solder down"}

# Densities in the order JEDEC steps down for asymmetric DDR4 modules.
_DDR4_DENSITY_STEPS = [0.25, 0.5, 1, 2, 4, 8, 12, 16, 24, 32]


def _f(img, name):
    return get_field(img, name).get(img)


def module_type(img: SpdImage) -> str:
    table = DDR4_MODULE if img.generation == DDR4 else DDR5_MODULE
    base = img[3] & 0x0F
    t = table.get(base, "type 0x%x" % base)
    if img[3] & 0x80:
        t += " (hybrid)"
    return t


def capacity_gb(img: SpdImage) -> float:
    """Module capacity in GB, without ECC (Annex L / JESD400-5B formulas)."""
    width = _f(img, "device_width")
    ranks = _f(img, "ranks")
    if img.generation == DDR4:
        dies = _f(img, "die_per_package")
        logical = ranks * (dies if (img[6] & 0x03) == 0x02 else 1)  # 3DS: one logical rank per die
        density = _f(img, "density")
        per_rank = density / 8 * _f(img, "bus_width") / width
        if img[12] & 0x40:  # asymmetric: odd ranks use the secondary density
            step = (img[10] >> 2) & 0x03
            i = _DDR4_DENSITY_STEPS.index(density)
            odd_density = _DDR4_DENSITY_STEPS[max(0, i - step)]
            even, odd = (logical + 1) // 2, logical // 2
            return per_rank * even + odd_density / 8 * _f(img, "bus_width") / width * odd
        return per_rank * logical
    dies = {"mono": 1, "ddp": 2, "3ds-2h": 2, "3ds-4h": 4, "3ds-8h": 8, "3ds-16h": 16}.get(_f(img, "package"), 1)
    per = _f(img, "subchannels") * _f(img, "bus_width") / width * dies
    if img[234] & 0x40:  # asymmetric: even ranks bytes 4-7, odd ranks bytes 8-11
        second = {1: 4, 2: 8, 3: 12, 4: 16, 5: 24, 6: 32, 7: 48, 8: 64}.get(img[8] & 0x1F, 0)
        return per * (_f(img, "density") / 8 * ((ranks + 1) // 2) + second / 8 * (ranks // 2))
    return per * _f(img, "density") / 8 * ranks


def modified_text(img: SpdImage) -> str:
    """Program/Restore state from the marked serial and the record area ("" if unmodified)."""
    from .preset import is_marked, Record, DEFAULTS

    off, n = img.layout.record_area
    try:
        rec = Record.decode(bytes(img.data[off : off + n]))
        if rec.serial_new != img.serial:
            rec = None
    except ValueError:
        rec = None
    if not is_marked(img) and not (rec and img.serial[0] == MARKER):
        return ""
    names = {p.id: p.name for p in DEFAULTS}
    out = "yes (Program button)" if is_marked(img) else "yes, but changed since Program"
    if rec:
        out += "; record on the module: preset %s, %d byte(s), original serial %s" % (
            names.get(rec.preset_id, "%08x" % rec.preset_id), len(rec.entries), rec.serial_orig.hex())
    else:
        out += "; no record on the module (it may be in the board's flash)"
    return out


def extra_profiles(img: SpdImage) -> List[str]:
    """Overclocking profiles in the user area (vendor formats, by their magic bytes)."""
    found = []
    if img.generation == DDR4 and img[384:386] == b"\x0c\x4a":
        found.append("XMP 2.0")
    if img.generation == DDR5:
        if img[640:642] == b"\x0c\x4a":
            found.append("XMP 3.0")
        if img[832:836] == b"EXPO":
            found.append("EXPO")
    return found


def summary(img: SpdImage) -> List[Tuple[str, str]]:
    rows: List[Tuple[str, str]] = []
    add = lambda k, v: rows.append((k, v))
    lay = img.layout

    ecc = _f(img, "ecc_bits")
    add("Type", "%s %s%s" % (img.generation, module_type(img), " ECC" if ecc else ""))
    cap = capacity_gb(img)
    add("Capacity", ("%g GB" % cap) if cap else "?")
    org = "%d rank(s), x%d, %s dies" % (_f(img, "ranks"), _f(img, "device_width"), get_field(img, "density").fmt(_f(img, "density")))
    if img.generation == DDR4:
        if _f(img, "die_per_package") > 1:
            org += ", %d die/package" % _f(img, "die_per_package")
    else:
        if _f(img, "package") != "mono":
            org += ", " + _f(img, "package")
        org += ", %d sub-channel(s) x %d bit" % (_f(img, "subchannels"), _f(img, "bus_width"))
    add("Organization", org)
    addr = "%s row, %s column bits; %s bank group(s) x %s bank(s)" % (
        _f(img, "rows"), _f(img, "cols"), _f(img, "bank_groups"), _f(img, "banks_per_group"))
    if img.generation == DDR5 and _f(img, "rank_mix") == "asymmetrical":
        addr += "; odd ranks: %s dies, %s row bits" % (
            get_field(img, "density2").fmt(_f(img, "density2")), _f(img, "rows2"))
    add("Addressing", addr)

    tck = _f(img, "tCKmin")
    rate = rate_of(img)
    add("Max speed", "%s-%d (tCK %.3f ns)" % (img.generation, rate, tck / 1000))
    if tck > 0:
        cl = cas_latency(img, tck)
        add("Timings @ max", "CL%d-%d-%d-%d (tAA-tRCD-tRP-tRAS)" % (
            cl, nck(img, _f(img, "tRCD"), tck), nck(img, _f(img, "tRP"), tck), nck(img, _f(img, "tRAS"), tck)))
    add("tAA / tRCD / tRP", "%.3f / %.3f / %.3f ns" % (_f(img, "tAA") / 1000, _f(img, "tRCD") / 1000, _f(img, "tRP") / 1000))
    rfc = ["tRFC1", "tRFC2", "tRFC4"] if img.generation == DDR4 else ["tRFC1", "tRFC2", "tRFCsb"]
    add("/".join(rfc), " / ".join("%g ns" % (_f(img, n) / 1000) for n in rfc))
    add("CAS latencies", ", ".join(map(str, _f(img, "CL"))))

    m = lay.module_mfr
    add("Module vendor", jep106.name(img[m], img[m + 1]))
    d = lay.dram_mfr
    add("DRAM vendor", jep106.name(img[d], img[d + 1]))
    add("Part number", _f(img, "part_number") or "(empty)")
    add("Serial", img.serial.hex())
    mod = modified_text(img)
    if mod:
        add("Modified", mod)
    if img[lay.mfg_date] or img[lay.mfg_date + 1]:
        add("Manufactured", get_field(img, "mfg_date").fmt(_f(img, "mfg_date")))
    if img.generation == DDR5:
        hub_t, pmic_t = img[196], img[200]
        add("SPD hub", "%s, %s" % (jep106.name(img[194], img[195]), {0: "SPD5118", 1: "ESPD5216"}.get(hub_t & 0xF, "type %d" % (hub_t & 0xF))))
        if pmic_t & 0x80:
            add("PMIC", jep106.name(img[198], img[199]))
        if (img[3] & 0xF) in (1, 4, 7) and img[242] & 0x80:
            add("RCD", "%s, DDR5RCD0%d" % (jep106.name(img[240], img[241]), (img[242] & 0xF) + 1))

    crc = ", ".join("%s %s" % (s.name, "OK" if st == c else "BAD (stored %04x, computed %04x)" % (st, c))
                    for s, st, c in img.crc_status())
    add("CRC", crc)
    prof = extra_profiles(img)
    add("Profiles", ", ".join(prof) if prof else "none (JEDEC only)")
    from .xmp import profiles, summary_line

    for p in profiles(img):
        add("  XMP %d" % p.number, summary_line(img, p))
    if "XMP 2.0" in prof or "EXPO" in prof:
        add("", "(XMP 2.0 and EXPO profiles are detected but not decoded)")
    return rows


def format_summary(img: SpdImage) -> str:
    rows = summary(img)
    w = max(len(k) for k, _ in rows)
    return "\n".join("%-*s  %s" % (w, k, v) for k, v in rows)


def format_fields(img: SpdImage) -> str:
    out = []
    fields = fields_for(img)
    w = max(len(n) for n in fields)
    for name, f in fields.items():
        try:
            v = f.fmt(f.get(img))
        except Exception as e:  # undecodable value: show why rather than abort
            v = "? (%s)" % e
        out.append("%-*s  %-22s %s" % (w, name, v, f.help))
    return "\n".join(out)
