"""Example presets: upload with `spdtool preset add examples/my_presets.py --use`.

Every Preset in this file is added (or replaces the board's preset with the
same name); --use makes the last one active, so the Program button applies it.
Field names: `spdtool fields`. Operations: see spdtool/preset.py.
"""

from spdtool.preset import (Preset, clocks, double, inc, scale_time, set_time, set_to, speed_lowest,
                            speed_rate, speed_up, when)

PRESETS = [
    # A fixed speed instead of the lowest one.
    Preset("ddr5-4000", "DDR5-4000; DDR4 modules are refused", ddr5=[speed_rate(4000)]),
    # BadRAM with an extra column bit instead of a row bit.
    Preset("badram-col", "BadRAM: one extra column address bit, density doubled",
           ddr4=[inc("cols"), double("density")],
           ddr5=[inc("cols"), double("density"),
                 when("rank_mix", "asymmetrical", [double("density2")])]),
    # Refresh timings: twice the refresh recovery time.
    Preset("slow-refresh", "tRFC1/tRFC2 doubled",
           ddr4=[scale_time("tRFC1", 2), scale_time("tRFC2", 2)],
           ddr5=[scale_time("tRFC1", 2), scale_time("tRFC2", 2)]),
    # Lowest speed with an explicit CAS latency (DDR5-3200 CL26).
    Preset("ddr5-3200-cl26", "DDR5-3200 CL26-26-26", ddr5=[speed_lowest(), clocks("tAA", 26),
                                                          clocks("tRCD", 26), clocks("tRP", 26)]),
    # Several changes at once.
    Preset("one-rank-slower", "report a single rank, one speed grade slower",
           ddr4=[set_to("ranks", 1), speed_up(-1)],
           ddr5=[set_to("ranks", 1), speed_up(-1)]),
]
