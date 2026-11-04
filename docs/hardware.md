# Hardware

How the board works, block by block, and why it is built that way. Part
references (U1, Q1, …) are those of the schematic in `kicad/`. Contact numbers
are socket positions on the board (see "Socket" below), not module pins.

## Overview

```
USB ── Pico ──┬─ VBUS 5 V ──┬─ U1 TPS2553 (EN GP0, ≈180 mA) ──────────► VIN_BULK_SW ─► contact 1
              │             └─ U5 LM2665 doubler ─ U6 LP2951 8.5 V ───► 150 Ω ─► contact 150 (SA0)
              │                (both shut down by GP28)                └─ HV LED
              └─ 3V3 ───────┬─ U2 TPS2553 (EN GP1, ≈120 mA) ──────────► VIN_MGMT_SW ─► contact 3
                            ├─ U3 2.5 V LDO ─┬─ U4 switch 1 (GP6) ─ 47 Ω ──► contact 5 (DDR4 VDDSPD)
                            │                └─ U4 switch 2 (GP7) ─ VIO ─ 4.7 k pull-ups on 4, 5, 148
                            └─ logic, LEDs

GP19–GP22 ── I2C on contacts 4 / 5 / 148     Q1 (GP8, on by default) ── contact 148 to GND (DDR5 HSA)
GP26 (ADC) ─ 1 k ── contact 8 (seating)      GP27 (ADC) ─ 10 k ── contact 5 (VDDSPD monitor)
```

The board only talks to the SPD/hub of a module: it powers the parts needed
for that and nothing else. All module rails are off by default (pull-downs on
the enables) and are switched on by the firmware only for the duration of an
operation.

## Socket

- **Part:** Amphenol DDR504112805K, a 288-position, 0.85 mm pitch, vertical
  SMT DDR5 DIMM socket. It is cut at the key into a 75- and a 69-position part;
  each part makes one board. The key bump is removed from the part that has
  it.
- **Footprint:** positions 1–69 counted from the end tower, so both parts fit.
  Positions 70–75 of the larger part are soldered for strength but
  unconnected. Only the end tower's hold-down is used.
- **Numbering:** front row = contacts 1–69 (= DDR5 pins 1–69), back row =
  contacts 145–213 (= DDR5 pins 145–213); contact 144+p sits behind contact p.
- **Insertion:** DDR5 modules go in normally (pin-1 end in the half socket),
  DDR4 modules rotated 180° (pin-144 end in the half socket). The rest of the
  module overhangs the board edge; there is no end stop.
- **DDR4 mapping:** DDR4 front pin *i* lands on contact **289 − i**, DDR4 back
  pin *144 + i* on contact **145 − i**. Example: DDR4 SCL (pin 141) → contact
  148, SDA (285) → 4, VDDSPD (284) → 5, SA0 (139) → 150.
- **Key gap:** DDR4's key sits after pin 77, DDR5's after pin 75, so contacts
  near the key don't line up with DDR4 pads. All used contacts are far from it
  (highest: 51 front, 151 back).

The pin assignments were checked against JEDEC 4.20.28 (DDR4 RDIMM, which also
lists the UDIMM functions) and the DDR5 UDIMM/RDIMM pin tables.

## Contact map

Only these contacts are connected; every other pad is soldered for strength
only.

| Contact | DDR4 (rotated) | DDR5 UDIMM | DDR5 RDIMM | Board connection |
|---|---|---|---|---|
| 1 | VPP | VIN_BULK | VIN_BULK | VIN_BULK_SW: 5 V via U1, DDR5 UDIMM only |
| 3 | VPP | RFU | VIN_MGMT | VIN_MGMT_SW: 3.3 V via U2, DDR5 RDIMM only |
| 4 | SDA | HSCL | HSCL | I2C (GP21 and GP22) |
| 5 | VDDSPD | HSDA | HSDA | DDR4: 2.5 V via U4 + 47 Ω; DDR5: I2C data (GP20); monitored by GP27 |
| 6 | VSS | VSS | VSS | GND |
| 8 | VSS | VSS | VSS | seating check (GP26) |
| 51 | SA2 | CB0_A | CB0_A | 10 k to GND |
| 148 | SCL | HSA | HSA | DDR4: I2C clock (GP19); DDR5: grounded by Q1 |
| 149 | SA1 | RFU | RFU | 10 k to GND |
| 150 | SA0 | VSS | RFU | 10 k to GND; VHV path for DDR4 write protection |
| 151 | VSS | PWR_EN | VSS | 10 k to GND: keeps a DDR5 UDIMM's PMIC regulators off |

Contacts 2, 145, 146 (more VIN_BULK on DDR5, VPP on DDR4) and 147 (PWR_GOOD /
PCAMP) are deliberately left open: one contact carries the tens of mA needed.

**Addresses:** DDR4 SPD 0x50, temperature sensor 0x18, page select 0x36/0x37,
write-protect commands 0x30–0x35. DDR5 hub 0x50 (HSA grounded).

**Why 10 k pull-downs, not GND, on SA0–SA2:** they must be a logic 0 for DDR4
but are CB0_A, RFU or VSS on DDR5, where a hard short would be wrong. 10 k is
still a solid 0: the DDR4 register's SA inputs are ≥ 30 kΩ.

## Supplies

| Rail | From | Switch | Mode | Notes |
|---|---|---|---|---|
| VIN_BULK_SW (contact 1) | VBUS 5 V | U1 TPS2553, EN = GP0 | DDR5 UDIMM | R_ILIM 150 k → ≈180 mA (155–207 mA). The PMIC's LDO powers the hub; PWR_EN is held low, so its switching regulators (DRAM supplies) stay off. |
| VIN_MGMT_SW (contact 3) | 3V3 | U2 TPS2553, EN = GP1 | DDR5 RDIMM | R_ILIM 220 k → ≈123 mA (105–144 mA). VIN_MGMT alone powers an RDIMM's hub. |
| VDDSPD (contact 5) | 2.5 V (U3) | U4 74LVC2G66 switch 1, EN = GP6 | DDR4 | 47 Ω in series. |
| VIO (I2C pull-ups) | 2.5 V (U3) | U4 switch 2, EN = GP7 | both | Three 4.7 k pull-ups (contacts 4, 5, 148). Switched so the pull-ups never back-power an unpowered module. |

- VIN_BULK and VIN_MGMT are never on together, and DDR4 mode never enables
  either.
- U1/U2 FAULT outputs are wire-ORed into GP2 (10 k pull-up). The firmware
  checks it during and after power-up and before every I2C transfer; a fault
  switches everything off.
- **Why 2.5 V for VDDSPD and the pull-ups:** a DDR4 RDIMM's VDDSPD also powers
  the register's (RCD) I2C interface, rated 2.2–2.8 V. DDR5 and DDR4 UDIMMs
  would accept 3.3 V, but contacts 4 and 148 are shared between generations, so
  one 2.5 V rail suits every case. The Pico's internal pull-ups (3.3 V, 50–80 k)
  are neither the right voltage nor strong enough.
- **Contact-5 resistor (47 Ω):** at ≤ 2 mA (SPD + temperature sensor +
  register at 100 kHz) contact 5 stays ≥ 2.35 V; a short to VSS draws ≈46 mA,
  within U4's 50 mA rating. GP27 reads contact 5 through 10 k; the firmware
  requires ≥ 2.25 V before the first I2C access. I2C runs at 100 kHz.
- **Discharge:** permanent 2.2 k bleeders on VIN_BULK_SW and VIN_MGMT_SW
  (τ ≈ 20–50 ms with a module's bulk capacitance). The firmware waits ≥ 500 ms
  between power-ups, so the hub always sees a clean power-on. Contact 5 and VIO
  are drained through the pull-ups into contact 148, which Q1 grounds whenever
  the board is idle.

## DDR5: HSA and offline mode

Q1 (AO3400A) grounds contact 148 (HSA). Its gate has a 100 k pull-up, so HSA is
grounded by default; GP8 releases it in DDR4 mode, where contact 148 is SCL.
In DDR5 mode the firmware grounds HSA **before** enabling the supply: the hub
samples HSA at power-up, and HSA grounded selects address 0x50 and the hub's
offline (tester) mode, in which the block write protection (MR12/MR13) can be
cleared. JP1 (open solder jumper) bypasses Q1 for debugging.

## DDR4: SA0 high voltage

The EE1004 SPD's reversible write-protect commands (set: SWPn, clear: CWP) need
7–10 V on SA0 (contact 150). On an RDIMM, SA0 also reaches the register through
1 k into a clamp at ≈3.1 V, so the source must supply ≈5 mA: a resistive
divider from 5 V cannot do it.

- **U5 LM2665** doubles VBUS (≈9.3–10.4 V). D1 (SS14) is its start-up diode.
- **U6 LP2951** (adjustable, 4.7 k + 1.2 k over 1 k → 8.52 V) regulates it and
  provides a true off state. Without U6, a shut-down doubler still passes
  VBUS − 0.3 V ≈ 4.7 V through D1 (a logic 1 on SA0), and a running one gives
  up to 10.5 V, above the 10 V limit.
- **GP28** drives both shutdown inputs (10 k pull-up: off by default).
- **Output:** 150 Ω to contact 150, 10 k to GND there. SA0 ≈ 8.4 V on a UDIMM,
  ≈ 7.7 V on an RDIMM; ≤ 40 mV when off. A short draws ≈57 mA (the pump sags).
- The **HV LED** (from U6's output) is lit whenever VHV is present.
- The firmware enables VHV only in DDR4 mode, only around a write-protect
  command, and waits 50 ms after on and 100 ms after off.

## I2C

| Contact | DDR5 | DDR4 | GPIO |
|---|---|---|---|
| 4 | HSCL | SDA | GP21 (I2C0 SCL) and GP22 (I2C1 SDA) |
| 5 | HSDA | VDDSPD | GP20 (I2C0 SDA), high-Z in DDR4 mode |
| 148 | HSA (grounded) | SCL | GP19 (I2C1 SCL) |

DDR5 uses I2C0 (GP20/GP21), DDR4 I2C1 (GP22/GP19). Contact 4 has two GPIOs on
it; the firmware attaches only one peripheral at a time and leaves the other
pin high-Z. No GPIO ever sees more than 2.5 V, so the RP2040's lack of 5 V
tolerance doesn't matter (a Pico 2 has the same pinout and also works).

## Seating check

Contact 8 is VSS on every DDR4 and DDR5 module type, and its neighbours are
data or unused pads. Before any rail is enabled, the firmware turns on GP26's
internal pull-up for 1 ms and samples the ADC through 1 k:

| Reading | Meaning |
|---|---|
| < 200 mV (≈ 40–70 mV) | VSS: module seated |
| ≈ 0.3–0.5 V | a data pin clamped by its ESD diode: module offset by one pin |
| > 2.5 V (≈ 3.1 V) | open: empty socket (or an unused pad) |

A short pulse rather than a logic read, because charging a module's data-pin
supply to logic levels would exceed the DRAM's absolute maximum; 1 ms charges
it by under 10 mV. Offsets of two pins (ground lands on ground again) and
half-pin offsets are not detected; the SPD type check and the current-limited
rails cover those.

## User interface

| Element | GPIO | Notes |
|---|---|---|
| SW1 DDR4 / DDR5 | GP18 = DDR4, GP17 = DDR5 | SPDT slide switch, common to GND, internal pull-ups |
| SW4 UDIMM / RDIMM | GP16 = UDIMM, GP9 = RDIMM | same; LRDIMM and MRDIMM count as RDIMM |
| SW2 Program (silkscreen: BadRAM) | GP10 | tactile, to GND |
| SW3 Restore | GP11 | tactile, to GND |
| Done (green) | GP12 | 220 Ω, PWM-dimmed to 10 % by the firmware |
| Modified (yellow; silkscreen: BadRAM) | GP13 | 680 Ω |
| Busy (yellow) | GP14 | 680 Ω; lit while the module is powered and during the discharge wait: do not remove the module |
| Fault (red) | GP15 | 680 Ω; blink codes after a failed button action |
| HV (red) | – | 3.3 k from U6's output: VHV present |
| Mode LEDs DDR4 / DDR5 / UDIMM / RDIMM (green) | – | 3V3 → 2.7 k → LED → switch line: the selected throw lights its LED, without firmware |

Each switch must have exactly one line low; otherwise the firmware refuses to
power anything (switch between positions).

**LED currents:** the yellow and red LEDs (Vf ≈ 2.0 V) run at ≈1.9 mA. The
green InGaN LEDs look several times brighter at the same current; on the first
boards 220 Ω (≈3 mA) was far too bright, and ≈0.3 mA matched the others. Hence
2.7 k on the mode LEDs and PWM on Done. (The first boards were assembled with
220 Ω on the mode LEDs and reworked by hand.)

## Pico pin map

| GPIO | Net | Default |
|---|---|---|
| GP0 | EN_VIN_BULK (U1) | 100 k pull-down: off |
| GP1 | EN_VIN_MGMT (U2) | 100 k pull-down: off |
| GP2 | FAULT_n (U1/U2) | 10 k pull-up |
| GP6 | EN_VDDSPD (U4 1E) | 100 k pull-down: off |
| GP7 | EN_VIO (U4 2E) | 100 k pull-down: off |
| GP8 | HSA_GND (Q1 gate) | 100 k pull-up: HSA grounded |
| GP9, GP16, GP17, GP18 | mode switches | internal pull-ups |
| GP10, GP11 | Program, Restore | internal pull-ups |
| GP12–GP15 | LEDs Done, Modified, Busy, Fault | PWM |
| GP19 | I2C1 SCL → contact 148 | high-Z |
| GP20 | I2C0 SDA → contact 5 | high-Z |
| GP21 | I2C0 SCL → contact 4 | high-Z |
| GP22 | I2C1 SDA → contact 4 | high-Z |
| GP26 / ADC0 | seating sense ← 1 k ← contact 8 | pull-up only during the measurement |
| GP27 / ADC1 | contact 5 monitor ← 10 k | input |
| GP28 | HV_SHDN (U5 SD, U6 SHUTDOWN) | 10 k pull-up: VHV off |
| GP3–GP5 | spare | |

**Safe default state.** With the Pico in reset, in BOOTSEL or before the
firmware runs, the hardware defaults leave all rails off (bleeding down), VHV
off and HSA grounded. One caveat: RP2040 pads leave reset with a weak internal
pull-down, which pulls Q1's gate to ≈1.1–1.5 V, around its threshold. HSA is
therefore only guaranteed grounded once the firmware drives GP8; this is
harmless because every rail is off until then, and the firmware grounds HSA
before any DDR5 rail. The watchdog (3 s) resets a hung firmware, which returns
everything to these defaults.

## Building

- **PCB and assembly:** 2-layer, 1.6 mm, ENIG (flat pads for the 0.85 mm
  socket). `kicad/` has the Gerbers and JLCPCB BOM/placement files; everything
  except the Pico, the socket and JP1 can be assembled by JLCPCB.
- **Pico:** a Raspberry Pi Pico (RP2040), soldered by its castellations.
- **Socket:** cut as described above. Solder it **by hand**: hot air without a
  bottom heater melted the housing on the first board. Solder the end tower's
  hold-down too (plated slot, from the underside); it takes the insertion
  force.
- **After assembly**, with an empty socket: `spdtool status` (seat sense
  ≈3.1 V, no fault), then `spdtool debug` in each mode while measuring the
  contacts against the contact map. Contacts 1 and 3 must be 0 V in DDR4 mode
  (they are VPP on a DDR4 module).

## Known limitations

- Two-pin and half-pin offsets are not detected by the seating check.
- DDR4 modules are stiff to insert in the cut socket.
- DDR5 UDIMMs (the VIN_BULK path) are not yet tested on real modules.
- Only the SPD is accessible: RCD and PMIC registers are not used.
