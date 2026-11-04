// Error codes reported as "ERR <name> <message>" over the command interface.
#pragma once

typedef enum {
    E_OK = 0,
    E_ARGS,        // bad command arguments
    E_CMD,         // unknown command
    E_SWITCH,      // mode switches not in a valid position
    E_SEAT,        // seating check failed (contact 8 not VSS / socket not empty)
    E_VDDSPD,      // DDR4 VDDSPD did not come up
    E_FAULT,       // TPS2553 over-current fault
    E_NOACK,       // device did not acknowledge
    E_IDENT,       // SPD type bytes do not match the switches
    E_WP,          // write blocked by write protection
    E_VERIFY,      // read-back differs from what was written
    E_STATE,       // command not allowed in the current power state / mode
    E_TIMEOUT,     // I2C or write-cycle timeout
    E_OVERRIDE,    // DDR5: write-protect override not available (HSA not grounded at power-up)
    E_REFUSED,     // Program/Restore refused for a logical reason (message says which)
    E_FLASH,       // Pico flash record store failed
} err_t;

const char *err_name(err_t e);
