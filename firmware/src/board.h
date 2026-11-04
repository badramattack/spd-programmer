// Pin map and electrical constants of the SPD programmer board.
// Source of truth: the KiCad schematic (kicad/spd-programmer.kicad_sch).
#pragma once

#include "pico/stdlib.h"

#define FW_VERSION "0.1.0"
#define PROTO_VERSION 1

// Module supply switches (all have 100 k pull-downs: off by default).
#define PIN_EN_VIN_BULK 0   // U1 TPS2553, 5 V, DDR5 UDIMM only (contact 1)
#define PIN_EN_VIN_MGMT 1   // U2 TPS2553, 3.3 V, DDR5 RDIMM only (contact 3)
#define PIN_FAULT_N     2   // U1/U2 FAULT, wire-OR, 10 k pull-up
#define PIN_EN_VDDSPD   6   // U4 switch 1, 2.5 V to contact 5, DDR4 only
#define PIN_EN_VIO      7   // U4 switch 2, 2.5 V I2C pull-up rail
#define PIN_HSA_GND     8   // Q4 gate, 100 k pull-up: high = contact 148 grounded
#define PIN_HV_SHDN     28  // U5 SD + U6 SHUTDOWN, 10 k pull-up: high = VHV off

// User interface. Select lines are low when that position is selected.
#define PIN_SEL_RDIMM   9
#define PIN_SEL_UDIMM   16
#define PIN_SEL_DDR5    17
#define PIN_SEL_DDR4    18
#define PIN_BTN_PROGRAM 10
#define PIN_BTN_RESTORE 11
#define PIN_LED_DONE    12
#define PIN_LED_MOD     13
#define PIN_LED_BUSY    14
#define PIN_LED_FAULT   15  // the four LEDs must stay consecutive (board.c)

// LED brightness, PWM duty in 1/LED_LEVEL_MAX. The green Done LED is much
// brighter than the yellow and red ones at the same current.
#define LED_LEVEL_MAX   1000
#define LED_DONE_LEVEL  100   // matched by eye to the yellow and red LEDs (spdtool ledtune)
#define LED_MOD_LEVEL   1000
#define LED_BUSY_LEVEL  1000
#define LED_FAULT_LEVEL 1000

// I2C. Contact 4 is wired to both GP21 (I2C0 SCL) and GP22 (I2C1 SDA).
#define PIN_BUS_C       19  // contact 148: DDR4 SCL (I2C1) / DDR5 HSA
#define PIN_BUS_B       20  // contact 5:   DDR5 HSDA (I2C0 SDA) / DDR4 VDDSPD
#define PIN_BUS_A_I2C0  21  // contact 4:   DDR5 HSCL (I2C0 SCL)
#define PIN_BUS_A_I2C1  22  // contact 4:   DDR4 SDA (I2C1 SDA)

// ADC inputs.
#define PIN_SEAT_SENSE  26  // ADC0, contact 8 through 1 k
#define PIN_VDDSPD_MON  27  // ADC1, contact 5 through 10 k
#define ADC_CH_SEAT     0
#define ADC_CH_VDDSPD   1

// Seating check. Contact 8 is VSS on every DDR4 and DDR5 module type; a module
// offset by one pin puts a DQ or RFU pad there instead. The internal pull-up
// (50-80 k), pulsed for 1 ms so the module's supplies barely charge, against
// contact 8 = VSS through 1 k gives about 40-65 mV; a DQ pin clamped by its
// ESD diode reads 0.3-0.5 V; an open contact reads about 3.3 V.
#define SEAT_PULSE_US        1000
#define SEAT_SEATED_MAX_MV   200
#define SEAT_EMPTY_MIN_MV    2500

// DDR4 VDDSPD at contact 5 must be at least this before the first access.
#define VDDSPD_MIN_MV        2250

// Timing.
#define T_DISCHARGE_MS       500   // rails off -> next power-up
#define T_VDDSPD_SETTLE_MS   5
#define T_DDR5_POR_MS        50    // VIN on -> PMIC LDO + hub POR
#define T_HV_SETTLE_MS       50    // VHV on -> doubler + LDO settled
#define T_HV_OFF_MS          100   // VHV off -> normal SPD access
#define DEBUG_IDLE_S         300   // debug mode: rails off after this long without a command
#define DEBUG_HV_IDLE_S      60    // debug mode with VHV

#define I2C_BAUD             100000

void board_init(void);
void led_set(uint pin, bool on);
bool led_get(uint pin);
void led_set_level(uint pin, uint level);
uint led_level(uint pin);
uint32_t adc_read_mv(uint channel);
