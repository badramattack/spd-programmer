"""Speed grades: standard clock periods, clock-count rounding, set_speed()."""

from __future__ import annotations

import math
from typing import List, Optional

from .fields import get_field
from .image import DDR4, DDR5, SpdImage

# tCKAVGmin in ps for each standard data rate.
DDR4_TCK = {1600: 1250, 1866: 1071, 2133: 938, 2400: 833, 2666: 750, 2933: 682, 3200: 625}
# JESD79-5: tCK is 2/rate truncated to whole ps (4800 -> 416, 5600 -> 357).
DDR5_TCK = {r: 2_000_000 // r for r in range(3200, 8801, 400)}


def speed_table(gen: str):
    return DDR4_TCK if gen == DDR4 else DDR5_TCK


def rate_of(img: SpdImage, tck_ps: Optional[int] = None) -> int:
    """Data rate in MT/s for a clock period: the standard grade if within 1%."""
    tck = tck_ps if tck_ps is not None else get_field(img, "tCKmin").get(img)
    if tck <= 0:
        return 0
    for rate, t in speed_table(img.generation).items():
        if abs(t - tck) <= t * 0.01:
            return rate
    return int(round(2_000_000 / tck))


def nck(img: SpdImage, t_ps: int, tck_ps: int) -> int:
    """Clock count the memory controller derives from a time."""
    if t_ps <= 0:
        return 0
    if img.generation == DDR5:
        # JESD400-5B rounding algorithm (0.3% guard band).
        return (t_ps * 997 // tck_ps + 1000) // 1000
    # Annex L: subtract a 0.01 guard band, then round up.
    return math.ceil(t_ps / tck_ps - 0.01)


def cas_latency(img: SpdImage, tck_ps: int) -> int:
    """The CL a controller would pick at this clock: the lowest supported CL >= nCK(tAA)."""
    need = nck(img, get_field(img, "tAA").get(img), tck_ps)
    if img.generation == DDR5 and need % 2:
        need += 1
    supported = [cl for cl in get_field(img, "CL").get(img) if cl >= need]
    return min(supported) if supported else need


def set_speed(
    img: SpdImage,
    rate: int,
    cl: Optional[int] = None,
    trcd: Optional[int] = None,
    trp: Optional[int] = None,
) -> List[str]:
    """Set the maximum speed grade, optionally with timings in clocks.

    Without cl/trcd/trp the module keeps its timings in ns, so the controller
    derives the clock counts for the new speed itself. With them, tAA/tRCD/tRP
    are set to exactly n clocks at the new speed. The resulting CL is added to
    the supported CL list if it is missing. Returns notes for the user.
    """
    table = speed_table(img.generation)
    if rate not in table:
        raise ValueError("%s-%d is not a standard speed (known: %s)" % (img.generation, rate, ", ".join(map(str, table))))
    tck = table[rate]
    notes = []
    get_field(img, "tCKmin").set(img, tck)
    notes.append("tCKmin = %d ps (%s-%d)" % (tck, img.generation, rate))
    for name, n in (("tAA", cl), ("tRCD", trcd), ("tRP", trp)):
        if n is not None:
            get_field(img, name).set(img, n * tck)
            notes.append("%s = %d nCK = %d ps" % (name, n, n * tck))

    f_cl = get_field(img, "CL")
    need = nck(img, get_field(img, "tAA").get(img), tck)
    if img.generation == DDR5 and need % 2:
        need += 1
    cls = f_cl.get(img)
    if need not in cls:
        try:
            f_cl.set(img, sorted(set(cls) | {need}))
            notes.append("added CL%d to the supported CAS latencies" % need)
        except ValueError as e:
            notes.append("warning: could not add CL%d: %s" % (need, e))
    notes.append("CL at this speed: %d" % cas_latency(img, tck))
    return notes
