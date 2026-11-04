# spdtool

Host tool for the DDR4/DDR5 SPD programmer board. It talks to the Pico
firmware (`../firmware`) over USB serial and decodes and edits SPD contents.

Only dependency: `pyserial`. Works on Linux, macOS and Windows, Python ≥ 3.8.

```sh
pip install ./host          # or: pipx install ./host
spdtool --help
```

The board is found automatically (USB product "SPD Programmer"); use `--port`
if more than one is plugged in. `spdtool -v ...` shows the serial protocol.

## Before you start

Set the two mode switches (DDR4/DDR5, UDIMM/RDIMM) to match the module, and
insert it (DDR4 rotated 180°). The board powers the module only for the
duration of a command, after checking:

1. the switches are in a valid position,
2. contact 8 reads as VSS (the module is seated, not offset by a pin),
3. DDR4: VDDSPD came up, and
4. the SPD type bytes match the switches (`--force` skips this check only,
   for repairing a corrupted SPD).

```sh
spdtool status     # switches, seat sense, rail fault
spdtool info       # decoded summary
```

## Reading

| Command | What it does |
|---|---|
| `spdtool info` | Summary: type, capacity, organization and addressing (row/column bits, banks), speed, timings, vendor, part number, CRC, Program/Restore state, XMP/EXPO profiles |
| `spdtool fields` | Every editable field with its current value |
| `spdtool get tAA tRFC1` | Selected fields |
| `spdtool dump module.bin` | Save the whole SPD to a file |
| `spdtool diff module.bin` | Field and byte differences between the module and a file |
| `spdtool temp` | Module temperature sensor |
| `spdtool crc` | Stored and computed CRCs |
| `spdtool xmp` | DDR5 XMP 3.0 profiles: name, speed, timings, voltages (read-only; XMP 2.0 and EXPO are detected but not decoded) |

`info`, `fields`, `get`, `xmp` and `diff` also take `--file image.bin`
to work on a file instead of the module.

## Editing

Every edit command reads the module, applies the change, then:

- **updates the CRC(s)** (DDR4 bytes 126–127 and 254–255, DDR5 510–511),
- **gives the module a new serial number**, because BIOSes cache SPD data by
  serial. The new serial is derived from the contents, so the same edit always
  gets the same serial and two different edits never share one. Use
  `--keep-serial` to prevent this,
- **saves a backup** of the old contents to `~/.spdtool/backups/` before writing,
- writes only the bytes that changed, then reads everything back to verify.

```sh
spdtool set tRFC1=350ns                    # one field (ns by default; "350000ps" works too)
spdtool set ranks=2 part_number=TEST-2R    # several fields at once
spdtool setbyte 0x12=0x05 0x7d=0x00        # raw bytes
spdtool speed 4800                          # max speed grade, timings kept in ns
spdtool speed 5600 --cl 46 --trcd 45 --trp 45   # speed grade with timings in clocks
```

Useful options: `-n/--dry-run` (show what would change), `--unlock` (see
below), `--file in.bin [-o out.bin]` (edit an image file, no board needed).

Field names (see `spdtool fields` for the full list):

- **Speed and timings:** `tCKmin`, `CL` (list of supported CAS latencies),
  `tAA`, `tRCD`, `tRP`, `tRAS`, `tRC`, `tWR`, `tFAW`, `tRRD_L`, `tCCD_L`, …
- **Refresh:** DDR4 `tRFC1/2/4`; DDR5 `tRFC1/2`, `tRFCsb` and the 3DS `_dlr`
  variants.
- **Capacity:** `density` (Gb per die), `ranks`, `device_width`, `rows`,
  `cols`, `bank_groups`, `banks_per_group`, `bus_width`, `ecc_bits`; DDR4
  `die_per_package`; DDR5 `package`, `subchannels`. These fields are
  independent bytes: changing `density` does not adjust `rows`, so keep them
  consistent yourself.
- **Identity:** `part_number`, `serial`, `mfg_date` (e.g. `2024-W12`).

`speed RATE` sets `tCKmin` to the JEDEC clock period for that grade. Without
`--cl` the module keeps its timings in ns, and the controller works out the
clock counts. With `--cl/--trcd/--trp`, those timings are set to exactly that
many clocks. Either way, the resulting CAS latency is added to the
supported-CL list if it is missing.

To restore a backup exactly (including its serial number):

```sh
spdtool write-image ~/.spdtool/backups/ddr5-a1b2c3d4-20261029-153000.bin --unlock
```

## Write protection

```sh
spdtool wp         # which blocks are protected
spdtool unlock     # clear all protection
spdtool lock       # re-protect what the last unlock cleared
spdtool lock 0x3ff # or protect an explicit block mask
```

- **DDR4 (EE1004):** four 128-byte blocks. `wp` reads RPS0–3 (an ACK means not
  protected). `unlock` sends CWP with the high voltage on SA0; `lock` sends SWPn
  per block, also with HV.
- **DDR5 (SPD5118):** sixteen 64-byte blocks in MR12/MR13. Clearing bits needs
  the hub's offline mode (HSA tied to GND at power-up), which the board
  always provides; `wp` shows whether the hub agrees (`override`).

Shipped modules usually protect DDR4 blocks 0–1 and DDR5 blocks 0–8. The
serial number lives in a protected block, so in practice every edit needs
`--unlock`, which unlocks, writes and re-locks the original blocks.

## Program and Restore (buttons and presets)

The board can change a module without a PC: insert it, set the switches,
**hold Program for 1 s**. The board applies its *active preset*, keeps a
backup of every byte it changed, and lights Done and Modified. A **short
press on Restore** puts the original contents back. The same from a PC:

```sh
spdtool program                    # the active preset, like the button
spdtool program --preset hide-ecc  # another preset (on the board, built-in, or a .py file)
spdtool record                     # modified? which preset? what changed?
spdtool restore                    # undo (--force: also if the SPD changed since)
spdtool button                     # what the last button press did
```

Built-in presets (one example per kind of change):

| Preset | DDR4 | DDR5 |
|---|---|---|
| `badram-row` (active by default) | +1 row address bit, density ×2 | same; also the odd-rank bytes on asymmetrical modules |
| `lowest-speed` | DDR4-1600 | DDR5-3200 |
| `speed-up` | one standard grade faster | one standard grade faster |
| `hide-ecc` | no ECC bus width extension | same |

Speed changes keep the timings in ns, so the BIOS derives the clock counts
for the new speed; the CL it needs is added to the supported list.
`speed-up` is overclocking: the module may not train or run stably.

**How it keeps track.** Program gives the module a *marked serial*
(`5Dxxxxxx`, derived from the new contents), so every board, `spdtool info`
and `dmidecode` can see it was modified. The backup record goes to the end of
the module's user area when that area is free, and always to the board's
flash. On modules with XMP/EXPO profiles the record goes to flash only, so
the profiles (and the empty user-profile slots the BIOS may fill later) are
never touched; only the board that programmed such a module can restore it. A module that is already modified is
refused by Program until it is restored. Restore it before editing it with
`spdtool set` and friends: an edit on top of a programmed module changes the
contents, so a plain Restore refuses it afterwards (`spdtool restore --force`
still restores the recorded bytes).

The record format, the flash layout and the exact sequence are described in
[`../docs/program-restore.md`](../docs/program-restore.md).

**Fault codes:** after a failed press, Fault stays lit and goes dark briefly
N times every few seconds (until the next press or command); N is:
1 insertion (switches, seating), 2 electrical (no I2C answer, VDDSPD,
over-current), 3 wrong module type, 4 refused (already modified, nothing to
restore, preset not applicable), 5 write failed. `spdtool button` gives the
details.

### Your own presets

Presets are written in Python and uploaded over USB; no reflashing. A
preset lists operations per generation; the board runs them against
whichever module is inserted. Example (`examples/my_presets.py`):

```python
from spdtool.preset import Preset, double, inc, set_to, speed_rate, speed_up, when

PRESETS = [
    Preset("ddr5-4000", "DDR5-4000; DDR4 modules are refused", ddr5=[speed_rate(4000)]),
    Preset("badram-col", "BadRAM: one extra column address bit, density doubled",
           ddr4=[inc("cols"), double("density")],
           ddr5=[inc("cols"), double("density"),
                 when("rank_mix", "asymmetrical", [double("density2")])]),
]
```

```sh
spdtool preset add examples/my_presets.py --use   # upload all, make the last one active
spdtool preset                                    # list (with the decoded operations)
spdtool preset use badram-row                     # choose what the button does
spdtool preset del badram-col
spdtool preset reset                              # back to the built-in presets
```

Operations (field names as in `spdtool fields`); they run in order:

| Operation | Does |
|---|---|
| `set_to(field, value)` | set a field, e.g. `set_to("ranks", 1)` |
| `inc(field, n=1)`, `double(field)` | change a field relative to its current value; a module whose value has no valid result (already at the maximum) is refused |
| `map_values(field, fn, text)` | the general form of `inc`/`double`: value v becomes fn(v) |
| `speed_lowest()`, `speed_up(n)`, `speed_rate(mts)` | maximum speed to a standard grade |
| `set_time(field, "350ns")` | set a timing field (`tAA`, `tRFC1`, …) |
| `scale_time(field, 1.5)`, `add_time(field, "-1ns")` | change a timing field relative to its value (scaled values are rounded up) |
| `clocks(field, n)` | n clock cycles at the module's tCKmin (after any speed op before it), e.g. `clocks("tAA", 26)` for CL26 |
| `when(field, value, [ops])` | ops only on modules where the field has this value |

A generation without operations is refused for that generation. A result that
does not fit the field refuses the whole preset. The serial number, CRCs and
the record area cannot be changed by presets; neither can XMP/EXPO profiles
(their own CRCs are not updated), and speed presets have no effect while XMP is
enabled in the BIOS. To change the
board's built-in defaults, edit `DEFAULTS` in `spdtool/preset.py`, run
`python -m spdtool.preset --c-defaults > ../firmware/src/default_presets.h`,
and rebuild the firmware.

## The board

| Command | What it does |
|---|---|
| `spdtool status` | Mode switches, power state, seat sense, rail fault, buttons |
| `spdtool button` | What the last Program/Restore button press did |
| `spdtool leds` | LED test: Done, Modified, Busy, Fault in turn, then all |

## Firmware update

Build as described in `../firmware/README.md`, then:

```sh
picotool load -f -x build/firmware/spd_programmer.uf2   # reboots into BOOTSEL itself
```

Without picotool: `spdtool bootsel`, then copy the `.uf2` onto the RPI-RP2
drive. Presets and backup records in the Pico flash survive firmware updates.

## Debugging and bring-up

For checking a new board or looking under the hood; not needed for normal use.

| Command | What it does |
|---|---|
| `spdtool -v <command>` | Show the serial protocol |
| `spdtool ports` | Detected boards (for `--port` when several are connected) |
| `spdtool hexdump` | Hex dump of the SPD (also with `--file`) |
| `spdtool regs` | DDR5: SPD5118 hub registers (MR0–MR127) |
| `spdtool scan` | I2C addresses that respond |
| `spdtool ledtune [done\|mod\|busy\|fault]` | Adjust an LED's PWM brightness with u/d (U/D: single steps), to pick the defaults in `firmware/src/board.h` |

### Measuring the rails

```sh
spdtool debug        # rails of the selected mode on, with an EMPTY socket
spdtool debug --hv   # DDR4 mode only: also VHV on contact 150
```

The rails stay on until Ctrl-C and the seat sense and contact-5 voltage are
shown live. The board refuses debug mode with a module inserted, switches off
as soon as one is inserted, and refuses `--hv` outside DDR4 mode. Use it to
check, for example, that contact 150 stays at 0 V in DDR5 mode.

## Python API

```python
from spdtool.device import Programmer
from spdtool.fields import set_field
from spdtool.program import apply, prepare
from spdtool.decode import format_summary

with Programmer() as p, p.session():
    cur = p.read_image()
    print(format_summary(cur))
    new = cur.copy()
    set_field(new, "tRFC1", "350ns")
    apply(p, cur, prepare(cur, new), unlock=True)
```

`Programmer` also exposes the raw commands: `read`, `write`, `wp`, `unlock`,
`lock`, `crc`, `mr_read`, `mr_write`, `temperature`, `scan`, `status`,
`debug`, `led_test`, `leds`, `led_level`, `program`, `program_active`,
`restore`, `record`, `button`, `preset_list/get/add/use/del/reset`, `bootsel`.

## Tests

```sh
pip install './host[test]' && pytest host/tests
```

The tests use synthetic images and a simulated board, so no hardware is needed.
