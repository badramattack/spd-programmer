// Module power sequencing and the safety checks around it. All rails are off
// except during an operation; the mode switches and the seating check are
// verified before any rail is enabled.
#pragma once

#include "err.h"
#include "pico/stdlib.h"

typedef enum { MODE_INVALID, MODE_DDR4U, MODE_DDR4R, MODE_DDR5U, MODE_DDR5R } dimm_mode_t;
typedef enum { PWR_OFF, PWR_ON, PWR_DEBUG } pwr_state_t;

dimm_mode_t mode_read(void);
const char *mode_name(dimm_mode_t m);
static inline bool mode_is_ddr4(dimm_mode_t m) { return m == MODE_DDR4U || m == MODE_DDR4R; }
static inline bool mode_is_ddr5(dimm_mode_t m) { return m == MODE_DDR5U || m == MODE_DDR5R; }
static inline bool mode_is_rdimm(dimm_mode_t m) { return m == MODE_DDR4R || m == MODE_DDR5R; }

// One seating measurement: 1 ms pull-up pulse on contact 8, then an ADC sample.
uint32_t seat_measure_mv(void);

// Normal power-up: switch check, seating check, rail sequence, then the SPD
// identity check (skipped with `force`, e.g. to repair a corrupted SPD).
err_t power_on(bool force);
// Debug power-up: requires an EMPTY socket. Rails stay on until power_off.
// `hv` additionally enables VHV on contact 150 (DDR4 mode only).
err_t power_debug(bool hv);
// Full power-down sequence, including the discharge wait.
void power_off(void);

pwr_state_t power_state(void);
dimm_mode_t power_mode(void);  // mode the module was powered in
bool power_hv(void);

// VHV for the DDR4 write-protect commands. Only in PWR_ON + DDR4 mode.
err_t hv_enable(void);
void hv_disable(void);

// Periodic safety checks while powered. Returns a reason string when the
// module was powered off, otherwise NULL.
const char *power_poll(void);
void power_init_time(void);
