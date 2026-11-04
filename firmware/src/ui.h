// Program / Restore buttons and the Fault LED blink codes.
//
// Program: hold 1 s; applies the active preset. Restore: short press.
// Both are ignored while a session or debug mode is open. Afterwards Done
// lights on success; on failure Fault stays on and goes dark briefly N times
// every few seconds, until the next button press or command:
//   1 insertion   switches between positions, module not seated / misaligned
//   2 electrical  no answer on I2C, timeout, VDDSPD low, over-current
//   3 wrong type  SPD type does not match the switches, or the preset has no
//                 ops for this generation
//   4 refused     already modified, nothing to restore, preset not applicable
//   5 write       write protection could not be cleared, verify failed, flash
#pragma once

#include "err.h"
#include "pico/stdlib.h"

void ui_poll(void);
// Stops a blink code (any command does).
void ui_clear(void);
// "OK action=... result=... code=... msg=..." for the last button action.
void ui_report(void);
