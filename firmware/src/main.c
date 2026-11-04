// SPD programmer firmware: USB CDC command interface for reading and
// writing DDR4 (EE1004) and DDR5 (SPD5118) SPD EEPROMs.

#include <stdio.h>

#include "board.h"
#include "cmd.h"
#include "hardware/watchdog.h"
#include "pico/binary_info.h"
#include "pico/stdio_usb.h"
#include "power.h"
#include "ui.h"

#define LINE_MAX 2200  // "write <addr> " + 1024 bytes as hex

bi_decl(bi_1pin_with_name(PIN_EN_VIN_BULK, "EN_VIN_BULK"));
bi_decl(bi_1pin_with_name(PIN_EN_VIN_MGMT, "EN_VIN_MGMT"));
bi_decl(bi_1pin_with_name(PIN_FAULT_N, "FAULT_N"));
bi_decl(bi_1pin_with_name(PIN_EN_VDDSPD, "EN_VDDSPD"));
bi_decl(bi_1pin_with_name(PIN_EN_VIO, "EN_VIO"));
bi_decl(bi_1pin_with_name(PIN_HSA_GND, "HSA_GND"));
bi_decl(bi_1pin_with_name(PIN_HV_SHDN, "HV_SHDN"));
bi_decl(bi_2pins_with_names(PIN_BUS_B, "DDR5 HSDA (I2C0)", PIN_BUS_A_I2C0, "DDR5 HSCL (I2C0)"));
bi_decl(bi_2pins_with_names(PIN_BUS_A_I2C1, "DDR4 SDA (I2C1)", PIN_BUS_C, "DDR4 SCL (I2C1)"));
bi_decl(bi_2pins_with_names(PIN_SEAT_SENSE, "SEAT_SENSE", PIN_VDDSPD_MON, "VDDSPD_MON"));

int main(void) {
    board_init();
    power_init_time();
    stdio_init_all();
    // Any hang resets the Pico, and a reset returns every rail to its
    // hardware default (off).
    watchdog_enable(3000, true);

    static char line[LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    absolute_time_t last_cmd = get_absolute_time();

    while (true) {
        watchdog_update();

        int ch = getchar_timeout_us(1000);
        if (ch >= 0) {
            if (ch == '\n' || ch == '\r') {
                if (overflow) {
                    printf("ERR ARGS line too long\n");
                } else if (len > 0) {
                    line[len] = 0;
                    cmd_execute(line);
                }
                len = 0;
                overflow = false;
                last_cmd = get_absolute_time();
            } else if (len < LINE_MAX - 1) {
                line[len++] = (char)ch;
            } else {
                overflow = true;
            }
        }

        ui_poll();

        const char *reason = power_poll();
        if (reason)
            printf("# power-off: %s\n", reason);

        // Debug mode has its own, longer limit (the host polls "status" while
        // the user measures); VHV, meant only for brief write-protect commands,
        // gets a shorter one.
        int64_t limit_s = power_state() == PWR_ON ? cmd_idle_timeout_s()
                          : power_hv()             ? DEBUG_HV_IDLE_S
                                                   : DEBUG_IDLE_S;
        if (power_state() != PWR_OFF &&
            absolute_time_diff_us(last_cmd, get_absolute_time()) > limit_s * 1000000) {
            power_off();
            printf("# power-off: idle timeout\n");
        }
        // Closing the serial port (DTR low) ends any session, debug included.
        if (power_state() != PWR_OFF && !stdio_usb_connected())
            power_off();
    }
}
