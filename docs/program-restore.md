# Program / Restore: design and formats

Program applies a *preset* to the inserted module and keeps a backup of every
byte it changes; Restore puts them back. This document describes how: the
marked serial, the backup record and where it is kept, the flash layout, the
preset format, and the exact sequence of both operations. User-level
documentation (buttons, `spdtool program`, writing presets) is in
[`host/README.md`](../host/README.md); the code is `firmware/src/edit.c`,
`store.c`, `presets.c`, `timing.c` and `ui.c`, with `host/spdtool/preset.py` on
the PC side.

## SPD locations used

| | DDR4 (512 B) | DDR5 (1024 B) |
|---|---|---|
| Serial number | 325–328 | 517–520 |
| CRC sections (CRC-16/XMODEM, stored LSB first) | 0–125 → 126–127; 128–253 → 254–255 | 0–509 → 510–511 |
| End-user area | 384–511 | 640–1023 |
| **Record area** (last 64 B of the user area) | **448–511** | **960–1023** |
| Module manufacturer, manufacturing date | 320–321, 323–324 | 512–513, 515–516 |

The record area lies outside every CRC and outside the blocks shipped modules
normally protect (DDR4 blocks 0–1, DDR5 blocks 0–8).

## Marked serial

Program sets the module's serial number to

```
5D h0 h1 h2      h = CRC-32 (zlib) of the whole SPD with the serial bytes,
                     the CRC bytes and the record area set to 0;
                     h0 h1 h2 = its low 24 bits, LSB first
```

- **Modified or not:** any board, `spdtool info` and dmidecode can tell a
  module was modified by Program even when its record is elsewhere (the chance
  of a genuine serial matching is about 1 in 4 × 10⁹).
- **Unchanged since Program:** Restore recomputes h; a mismatch means the
  contents changed after Program (e.g. a later `spdtool set`), and a plain
  Restore refuses.
- **Different edits, different serials:** BIOSes cache SPD data by serial
  number, so they never reuse cached data for a modified module. The same edit
  on the same module always gives the same serial.
- `host/spdtool/image.py` (`content_hash`, `marked_serial`) computes the same
  value.

## Backup record

Little endian, at most 23 + 3n bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `'S' 'R'` |
| 2 | 1 | version (1) |
| 3 | 1 | flags: bit 0 = the record area was all 0xFF before Program (else all 0x00) |
| 4 | 1 | n: number of entries |
| 5 | 4 | preset ID: CRC-32 of the preset's binary encoding |
| 9 | 4 | original serial number |
| 13 | 4 | new (marked) serial number: identifies which module state the record belongs to |
| 17 | 4 | original CRC bytes (DDR4: base, module section; DDR5: base, then 0) |
| 21 | 3n | entries: byte offset (2), original byte (1), for every changed byte except serial and CRCs |
| 21+3n | 2 | CRC-16 (as the SPD CRCs) of bytes 0 … 20+3n |

The changed values themselves are not stored: the marked serial already proves
the module still holds them.

### Where it is kept

- **On the module**, at the start of the record area, if that area is all 0x00
  or all 0xFF before Program, n ≤ 13 (64-byte area), and the module has no
  XMP/EXPO profiles (XMP 2.0 magic `0C 4A` at byte 384 on DDR4, XMP 3.0 at 640
  or `EXPO` at 832 on DDR5). The profile check matters: XMP 3.0 reserves the
  whole DDR5 user area, including empty slots for *user* profiles that the BIOS
  may write later, and the DDR5 record area is the last of those slots. The rest of the area is
  filled with the original fill byte. The record travels with the module, so
  any board can restore it, even after this board's flash is erased.
- **In the Pico flash, always** (n ≤ 74). Modules whose user area holds
  XMP/EXPO profiles (vendor-written, common on consumer gaming modules; absent
  on JEDEC and server modules) never get a record on the module: only the
  board that programmed them can restore them. (Without that board, only a dump made
  beforehand helps: `spdtool write-image`.)

A preset that changes more than 74 bytes is refused.

## Flash layout

The top 256 KB of the Pico's 2 MB flash (offset 0x1C0000), outside the
firmware image; `picotool load` and UF2 copying only write the blocks the
firmware covers, so presets and records survive firmware updates.

```
0x1C0000  preset sector (4 KB)
0x1C1000  1008 record slots of 256 B (one flash page each)
```

**Record slot** (`store.c`):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | state: 0xFF free, 0xA5 valid, 0x00 deleted |
| 1 | 8 | module ID: SPD bytes 0–3, module manufacturer (2), manufacturing date (2) |
| 9 | 1 | record length |
| 10 | ≤ 246 | record |

A record is found by module ID plus new serial (record offset 13); the newest
valid slot wins. Deleting only clears the state byte (flash can turn bits from
1 to 0 without an erase). When no slot is free, the 4 KB sector with the fewest
valid records is erased. Program first deletes any older record with the same
module ID and new serial (left over from an interrupted attempt); Restore
deletes them all.

**Preset sector** (`presets.c`):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `'P' 'S'` |
| 2 | 1 | version (1) |
| 3 | 1 | index of the active preset (0xFF: none) |
| 4 | 1 | count |
| 5 | … | count × (length (2), preset) |

The sector is rewritten as a whole on every change. If it is empty or invalid
the firmware uses the built-in defaults (`firmware/src/default_presets.h`, the
same layout, generated from `DEFAULTS` in `host/spdtool/preset.py`).
`preset reset` writes the defaults back.

## Preset format

```
'S' 'P'  version (1)
name length, name (≤ 23 ASCII)
description length, description (≤ 255)
DDR4 op count, DDR4 ops
DDR5 op count, DDR5 ops
```

At most 1024 bytes in total. The ops for the powered module's generation run
in order on a copy of its SPD; a generation with no ops is refused.

| Op | Encoding (after the type byte) | Effect |
|---|---|---|
| 1 SET | offset (2), mask, value | `byte = byte & ~mask | value & mask` |
| 2 MAP | offset (2), mask, n, n × (from, to) | `byte & mask` is looked up in the from-values and replaced by its to-value; a value with no entry refuses the preset (e.g. density already at the maximum) |
| 3 SPEED | kind, value (2, signed) | maximum speed to a standard grade: kind 0 = value MT/s, 1 = the lowest grade, 2 = value grades up (negative: down). tCKmin is set to the JEDEC clock period; the timings stay in ns; the CAS latency a controller needs at the new clock (JEDEC rounding) is added to the supported list. Refused if the result is not encodable or slower than tCKmax. |
| 4 IF | offset (2), mask, value, n | the next n ops (nested inside this one) run only if `byte & mask == value & mask` |
| 5 TIME | encoding, offset, offset2, offset3 (2 each), shift, action, a (4), b (4) | a timing field: action 0 sets it to a ps, 1 scales it by a/b (rounded up), 2 adds a ps, 3 sets it to a clock cycles at the current tCKmin. Refused if the result does not fit. |

**Timing encodings** (`timing.h`; the host's `Field.timing` carries the same
tuple):

| Encoding | Fields | Stored as |
|---|---|---|
| 1 PS16 | DDR5 tAA, tRCD, tRP, tRAS, tRC, tWR, … | 16-bit ps at offset |
| 2 NS16 | DDR5 tRFC1/2/sb (and 3DS variants) | 16-bit ns at offset (set: rounded up) |
| 3 MTB_FINE | DDR4 tCKmin, tAA, tRCD, tRP, … | 125 ps units at offset (1–255), signed fine ps at offset2 |
| 4 MTB12 | DDR4 tRAS, tRC, tWR, tWTR | 12-bit units: high nibble of offset at `shift`, low byte at offset2, optional fine at offset3 (0xFFFF: none) |
| 5 MTB16 | DDR4 tRFC1/2/4 | 16-bit 125 ps units at offset |

Encoding rounds the 125 ps units up and stores a negative fine correction, as
JEDEC does and as `spdtool` does; `host/tests/test_firmware_timing.py` compiles
`timing.c` and checks it against the host's encoders for every field.

Ops may not touch the serial, the CRC bytes or the record area (refused as a
malformed preset). The preset ID stored in records is the CRC-32 of the whole
encoding, so any change to a preset (even its description) gives a new ID.

## Program

1. Power up the module (switch, seating and type checks) and read the whole SPD.
2. If the serial starts with 0x5D, look for its record (record area first,
   then flash):
   - record found, same preset ID, contents unchanged → "already applied",
     success without writing;
   - record found otherwise, or the serial is a valid marked serial without a
     record → refused ("restore first");
   - neither → a genuine serial that happens to start with 0x5D: continue.
3. Run the preset's ops on a copy. Collect every changed byte (excluding
   serial, CRCs, record area); none → refused, more than 74 → refused.
4. Build the record, compute the marked serial and the new CRCs. Decide where
   the record goes (module and flash, or flash only).
5. Store the record in flash.
6. If any changed byte lies in a write-protected block, clear the protection
   (DDR4: CWP with VHV; DDR5: MR12/MR13 in offline mode).
7. Write, in this order: the record area, the marked serial, the changed bytes
   and CRCs. Only bytes that differ are written.
8. Set the original protection again; read the whole SPD back and compare.

An interrupted Program (unplugged, power lost) leaves the record and usually the
marked serial in place but the contents half-written: a plain Restore then
refuses (contents changed), `restore force` (or `spdtool restore --force`)
restores the recorded bytes.

## Restore

1. Power up, read the SPD. Serial not starting with 0x5D → refused ("nothing
   to restore").
2. Find the record whose new serial is the module's serial (record area first,
   then this board's flash); none → refused.
3. Unless forced, check the marked serial against the contents → refused if
   they changed.
4. Build the original: entries, serial, CRC bytes from the record; the record
   area back to its original fill if the record was on the module.
5. Clear protection if needed; write the changed bytes and CRCs, then the
   record area, then the serial **last** (so the module shows as modified until
   everything else is back); restore the protection; verify.
6. Delete the flash record(s).

## Buttons and LEDs

- **Program:** held for 1 s; applies the active preset. **Restore:** short
  press (30 ms debounce). Both are ignored while a USB session (`power on`) or
  debug mode is open, including a press still held when it ends.
- **Done** on success. **Modified** reflects the marked serial after Program,
  Restore or `record`. **Busy** while the module is powered.
- **Fault** stays on and goes dark briefly N times (200 ms dark, 300 ms on),
  then stays on for 2 s, and repeats, until the next press or USB command. N
  is the code:

| Code | Category | Firmware errors |
|---|---|---|
| 1 | Insertion | `SWITCH` (switch between positions), `SEAT` (not seated / offset) |
| 2 | Electrical | `NOACK`, `TIMEOUT`, `VDDSPD`, `FAULT`, `OVERRIDE` |
| 3 | Wrong module | `IDENT` (SPD type vs switches; preset without ops for this generation) |
| 4 | Refused | `REFUSED` (already modified, nothing to restore, not applicable, contents changed), `ARGS` (bad preset) |
| 5 | Write | `WP`, `VERIFY`, `FLASH` |

The last result's details: `button` over USB (`spdtool button`), and a
`# button ...` notice to a connected host.

## Interaction with other edits

- `spdtool set` and the other host edits give a module a new serial derived
  from its contents, but **not** a marked one: such a module is not "modified"
  for Program/Restore.
- Editing a programmed module with `spdtool` changes its contents: a plain
  Restore then refuses; `--force` restores the recorded bytes (and leaves the
  later edit in any byte the record does not cover).
- Restore also restores the original serial number, so the BIOS sees the
  original module again.
