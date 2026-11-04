#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "edit.h"
#include "presets.h"
#include "ui.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "power.h"
#include "spd.h"
#include "spd5118.h"

#define MAX_ARGS 8

static uint32_t idle_timeout_s = 10;

uint32_t cmd_idle_timeout_s(void) {
    return idle_timeout_s;
}

const char *err_name(err_t e) {
    static const char *names[] = {
        [E_OK] = "OK",          [E_ARGS] = "ARGS",       [E_CMD] = "CMD",
        [E_SWITCH] = "SWITCH",  [E_SEAT] = "SEAT",       [E_VDDSPD] = "VDDSPD",
        [E_FAULT] = "FAULT",    [E_NOACK] = "NOACK",     [E_IDENT] = "IDENT",
        [E_WP] = "WP",          [E_VERIFY] = "VERIFY",   [E_STATE] = "STATE",
        [E_TIMEOUT] = "TIMEOUT", [E_OVERRIDE] = "OVERRIDE", [E_REFUSED] = "REFUSED",
        [E_FLASH] = "FLASH",
    };
    return e < count_of(names) && names[e] ? names[e] : "UNKNOWN";
}

static const char *err_text(err_t e) {
    switch (e) {
    case E_SWITCH: return "mode switches are between positions";
    case E_SEAT: return "seating check failed (contact 8 is not VSS)";
    case E_VDDSPD: return "VDDSPD (contact 5) below 2.25 V: short or missing supply";
    case E_FAULT: return "rail over-current fault";
    case E_NOACK: return "no acknowledge from the device";
    case E_IDENT: return "SPD type does not match the mode switches";
    case E_WP: return "write protected";
    case E_VERIFY: return "read-back differs";
    case E_TIMEOUT: return "timeout";
    case E_OVERRIDE: return "write-protect override not active (HSA not grounded at power-up)";
    default: return "";
    }
}

static void reply_err(err_t e, const char *msg) {
    printf("ERR %s %s\n", err_name(e), msg ? msg : err_text(e));
    led_set(PIN_LED_FAULT, true);
    led_set(PIN_LED_DONE, false);
}

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        printf("%02x", buf[i]);
}

static bool parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s)
        return false;
    char *end;
    bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    unsigned long v = strtoul(s, &end, hex ? 16 : 10);  // "010" is ten, not octal
    if (*end)
        return false;
    *out = (uint32_t)v;
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, size_t max) {
    size_t n = strlen(s);
    if (n % 2 || n / 2 > max)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(s[2 * i]), lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

// ---------------------------------------------------------------------------
// Commands that need a powered, identified module. Without an open session
// they power the module up for just this command.

typedef err_t (*module_fn)(int argc, char **argv);

static void with_module(module_fn fn, int argc, char **argv) {
    pwr_state_t st = power_state();
    if (st == PWR_DEBUG) {
        reply_err(E_STATE, "not available in debug mode");
        return;
    }
    if (st == PWR_OFF) {
        err_t e = power_on(false);
        if (e != E_OK) {
            reply_err(e, NULL);
            return;
        }
    }
    err_t e = fn(argc, argv);
    if (st == PWR_OFF)
        power_off();
    if (e != E_OK)
        reply_err(e, NULL);
}

static uint8_t buf[SPD5118_SIZE];

static err_t do_read(int argc, char **argv) {
    uint32_t addr, len;
    if (argc != 3 || !parse_u32(argv[1], &addr) || !parse_u32(argv[2], &len) || len == 0 ||
        addr + len > spd_size())
        return E_ARGS;
    err_t e = spd_read(addr, buf, len);
    if (e != E_OK)
        return e;
    printf("OK ");
    print_hex(buf, len);
    printf("\n");
    return E_OK;
}

static err_t do_write(int argc, char **argv) {
    uint32_t addr;
    int len;
    if (argc != 3 || !parse_u32(argv[1], &addr) || (len = parse_hex(argv[2], buf, sizeof buf)) <= 0 ||
        addr + len > spd_size())
        return E_ARGS;
    err_t e = spd_write(addr, buf, len);
    if (e != E_OK)
        return e;
    led_set(PIN_LED_DONE, true);
    printf("OK written=%d\n", len);
    return E_OK;
}

// "crc" reports, "crc fix" rewrites wrong CRCs.
static err_t do_crc(int argc, char **argv) {
    bool fix = argc == 2 && strcmp(argv[1], "fix") == 0;
    if (argc > 2 || (argc == 2 && !fix))
        return E_ARGS;
    err_t e = spd_read(0, buf, spd_size());
    if (e != E_OK)
        return e;
    unsigned n;
    const crc_section_t *sec = spd_crc_sections(&n);
    char line[160];
    int pos = snprintf(line, sizeof line, "OK");
    for (unsigned i = 0; i < n; i++) {
        uint16_t calc = spd_crc16(buf + sec[i].start, sec[i].end - sec[i].start + 1);
        uint16_t stored = buf[sec[i].at] | buf[sec[i].at + 1] << 8;
        if (fix && calc != stored) {
            const uint8_t le[2] = {calc & 0xFF, calc >> 8};
            e = spd_write(sec[i].at, le, 2);
            if (e != E_OK)
                return e;
            stored = calc;
        }
        pos += snprintf(line + pos, sizeof line - pos, " %s=%04x/%04x", sec[i].name, stored, calc);
    }
    if (fix)
        led_set(PIN_LED_DONE, true);
    printf("%s\n", line);
    return E_OK;
}

static err_t do_wp(int argc, char **argv) {
    uint16_t mask;
    bool ovr;
    err_t e = spd_wp_status(&mask, &ovr);
    if (e == E_OK)
        printf("OK mask=0x%04x blocks=%u override=%d\n", mask, spd_blocks(), ovr);
    return e;
}

static uint16_t last_unlocked = 0;
static bool have_last_unlocked = false;
static uint8_t last_unlocked_id[8];

static err_t do_unlock(int argc, char **argv) {
    uint16_t prev;
    bool ovr;
    uint8_t id[8];
    err_t e = spd_wp_status(&prev, &ovr);
    if (e == E_OK)
        e = spd_module_id(id);
    if (e == E_OK)
        e = spd_wp_clear();
    if (e != E_OK)
        return e;
    last_unlocked = prev;
    memcpy(last_unlocked_id, id, sizeof id);
    have_last_unlocked = true;
    led_set(PIN_LED_DONE, true);
    printf("OK prev=0x%04x\n", prev);
    return E_OK;
}

// "lock <mask>" protects the given blocks; "lock" restores what the last
// "unlock" cleared.
static err_t do_lock(int argc, char **argv) {
    uint32_t mask;
    if (argc == 1) {
        uint8_t id[8];
        if (!have_last_unlocked) {
            reply_err(E_STATE, "no unlock to undo: give a block mask");
            return E_OK;
        }
        err_t e = spd_module_id(id);
        if (e != E_OK)
            return e;
        if (memcmp(id, last_unlocked_id, sizeof id) != 0) {
            reply_err(E_STATE, "not the module that was unlocked: give a block mask");
            return E_OK;
        }
        mask = last_unlocked;
    } else if (argc != 2 || !parse_u32(argv[1], &mask) || mask > 0xFFFF) {
        return E_ARGS;
    }
    err_t e = mask ? spd_wp_set((uint16_t)mask) : E_OK;
    if (e != E_OK)
        return e;
    led_set(PIN_LED_DONE, true);
    printf("OK mask=0x%04x\n", (unsigned)mask);
    return E_OK;
}

// "mr <reg> <len>" reads, "mr <reg> = <value>" writes (DDR5 only).
static err_t do_mr(int argc, char **argv) {
    if (!mode_is_ddr5(power_mode()))
        return E_STATE;
    uint32_t reg, v;
    if (argc == 3 && parse_u32(argv[1], &reg) && parse_u32(argv[2], &v) && v > 0 &&
        reg + v <= SPD5118_NREGS) {
        err_t e = spd5118_reg_read(reg, buf, v);
        if (e != E_OK)
            return e;
        printf("OK ");
        print_hex(buf, v);
        printf("\n");
        return E_OK;
    }
    if (argc == 4 && strcmp(argv[2], "=") == 0 && parse_u32(argv[1], &reg) && parse_u32(argv[3], &v) &&
        reg < SPD5118_NREGS && v <= 0xFF) {
        if (reg == 11) {
            reply_err(E_ARGS, "MR11 (addressing mode/page) is managed by the firmware");
            return E_OK;
        }
        err_t e = spd5118_reg_write(reg, v);
        if (e == E_OK)
            printf("OK\n");
        return e;
    }
    return E_ARGS;
}

static err_t do_temp(int argc, char **argv) {
    int32_t mc;
    err_t e = spd_temp(&mc);
    if (e == E_STATE || (e == E_NOACK && mode_is_ddr4(power_mode()))) {
        reply_err(e, "no (enabled) temperature sensor on this module");
        return E_OK;
    }
    if (e == E_OK)
        printf("OK milli_c=%ld\n", (long)mc);
    return e;
}

static err_t do_scan(int argc, char **argv) {
    printf("OK");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (bus_probe(a))
            printf(" %02x", a);
        watchdog_update();
    }
    printf("\n");
    return E_OK;
}

static err_t do_ident(int argc, char **argv) {
    printf("OK mode=%s key=", mode_name(power_mode()));
    print_hex(spd_ident_bytes(), 4);
    printf(" size=%u blocks=%u\n", spd_size(), spd_blocks());
    return E_OK;
}

static char detail[160];

// Replies for edit_program / edit_restore: refusals carry their own message.
static err_t edit_reply(err_t e) {
    if (e == E_OK) {
        led_set(PIN_LED_DONE, true);
        printf("OK %s\n", detail);
    } else {
        reply_err(e, detail[0] ? detail : NULL);
    }
    return E_OK;
}

static uint8_t preset_buf[1024];

// "program": apply the active preset; "program <preset hex>": apply this one.
static err_t do_program(int argc, char **argv) {
    const uint8_t *p = preset_buf;
    size_t len;
    if (argc == 2) {
        int n = parse_hex(argv[1], preset_buf, sizeof preset_buf);
        if (n <= 0)
            return E_ARGS;
        len = n;
    } else if (argc == 1) {
        int a = presets_active();
        if (a < 0) {
            snprintf(detail, sizeof detail, "no active preset (preset use <name>)");
            return edit_reply(E_REFUSED);
        }
        p = presets_get(a, &len);
    } else {
        return E_ARGS;
    }
    return edit_reply(edit_program(p, len, detail, sizeof detail));
}

// "restore [force]": undo the last Program from its backup record.
static err_t do_restore(int argc, char **argv) {
    bool force = argc == 2 && strcmp(argv[1], "force") == 0;
    if (argc > 2 || (argc == 2 && !force))
        return E_ARGS;
    return edit_reply(edit_restore(force, detail, sizeof detail));
}

static err_t do_record(int argc, char **argv) {
    return argc == 1 ? edit_report() : E_ARGS;
}

// ---------------------------------------------------------------------------
// Commands that do not need a module.

static void cmd_status(void) {
    // Contact 5 is only meaningful as a supply in DDR4 mode; report it anyway.
    printf("OK state=%s mode=%s powered_mode=%s hv=%d fault_n=%d seat_mv=%lu vddspd_mv=%lu "
           "timeout_s=%lu btn_program=%d btn_restore=%d\n",
           power_state() == PWR_OFF ? "off" : power_state() == PWR_ON ? "on" : "debug",
           mode_name(mode_read()), mode_name(power_mode()), power_hv(), gpio_get(PIN_FAULT_N),
           (unsigned long)seat_measure_mv(), (unsigned long)adc_read_mv(ADC_CH_VDDSPD),
           (unsigned long)idle_timeout_s, !gpio_get(PIN_BTN_PROGRAM), !gpio_get(PIN_BTN_RESTORE));
}

static void cmd_power(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "off") == 0 && argc == 2) {
        power_off();
        printf("OK\n");
        return;
    }
    bool force = argc == 3 && strcmp(argv[2], "force") == 0;
    if (argc < 2 || strcmp(argv[1], "on") != 0 || (argc == 3 && !force) || argc > 3) {
        reply_err(E_ARGS, "usage: power on [force] | power off");
        return;
    }
    if (power_state() != PWR_OFF) {
        reply_err(E_STATE, "already powered");
        return;
    }
    err_t e = power_on(force);
    if (e != E_OK) {
        reply_err(e, NULL);
        return;
    }
    do_ident(0, NULL);
}

static void cmd_debug(int argc, char **argv) {
    bool hv = argc == 2 && strcmp(argv[1], "hv") == 0;
    if (argc > 2 || (argc == 2 && !hv)) {
        reply_err(E_ARGS, "usage: debug [hv]");
        return;
    }
    if (power_state() != PWR_OFF) {
        reply_err(E_STATE, "power off first");
        return;
    }
    err_t e = power_debug(hv);
    if (e == E_SEAT)
        reply_err(e, "debug mode needs an empty socket");
    else if (e == E_STATE)
        reply_err(e, "VHV is only available in DDR4 mode");
    else if (e != E_OK)
        reply_err(e, NULL);
    else
        cmd_status();
}

static const struct {
    const char *name;
    uint pin;
} leds[] = {
    {"done", PIN_LED_DONE}, {"mod", PIN_LED_MOD}, {"busy", PIN_LED_BUSY}, {"fault", PIN_LED_FAULT},
};

// leds: each LED alone for 0.5 s, then all on for 2 s, then the previous
// state (longer than the watchdog period, so it feeds the watchdog).
// leds on|off: hold all four on or off, for comparing brightness.
static void cmd_leds(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        for (unsigned i = 0; i < count_of(leds); i++)
            led_set(leds[i].pin, argv[1][1] == 'n');
        printf("OK\n");
        return;
    }
    if (argc != 1) {
        reply_err(E_ARGS, "usage: leds [on|off]");
        return;
    }
    bool prev[count_of(leds)];
    for (unsigned i = 0; i < count_of(leds); i++)
        prev[i] = led_get(leds[i].pin);

    for (int step = 0; step <= (int)count_of(leds); step++) {
        for (unsigned i = 0; i < count_of(leds); i++)
            led_set(leds[i].pin, step == (int)count_of(leds) || (int)i == step);
        int ms = step == (int)count_of(leds) ? 2000 : 500;
        for (int t = 0; t < ms; t += 100) {
            watchdog_update();
            sleep_ms(100);
        }
    }
    for (unsigned i = 0; i < count_of(leds); i++)
        led_set(leds[i].pin, prev[i]);
    printf("OK\n");
}

// led <name> [level]: read or set an LED's PWM brightness (0-LED_LEVEL_MAX),
// until the next reset.
static void cmd_led(int argc, char **argv) {
    uint32_t level;
    if (argc >= 2) {
        for (unsigned i = 0; i < count_of(leds); i++) {
            if (strcmp(argv[1], leds[i].name) != 0)
                continue;
            if (argc == 3 && parse_u32(argv[2], &level) && level <= LED_LEVEL_MAX)
                led_set_level(leds[i].pin, level);
            else if (argc != 2)
                break;
            printf("OK %s=%u\n", leds[i].name, led_level(leds[i].pin));
            return;
        }
    }
    reply_err(E_ARGS, "usage: led done|mod|busy|fault [0-1000]");
}

// preset [list] | get <name> | add <hex> | del <name> | use <name> | reset
static void cmd_preset(int argc, char **argv) {
    const char *sub = argc >= 2 ? argv[1] : "list";
    char name[32];
    int i = argc == 3 ? presets_find(argv[2]) : -1;
    err_t e = E_OK;
    if (strcmp(sub, "list") == 0 && argc <= 2) {
        int a = presets_active();
        if (a >= 0)
            presets_name(a, name, sizeof name);
        printf("OK count=%u active=%s source=%s names=", presets_count(), a >= 0 ? name : "-",
               presets_builtin() ? "builtin" : "flash");
        for (unsigned k = 0; k < presets_count(); k++) {
            presets_name(k, name, sizeof name);
            printf("%s%s", k ? "," : "", name);
        }
        printf("\n");
        return;
    }
    if (strcmp(sub, "reset") == 0 && argc == 2) {
        e = presets_reset();
    } else if (strcmp(sub, "add") == 0 && argc == 3) {
        int n = parse_hex(argv[2], preset_buf, sizeof preset_buf);
        if (n <= 0) {
            reply_err(E_ARGS, "preset add <hex>");
            return;
        }
        e = edit_preset_check(preset_buf, n, name, sizeof name, detail, sizeof detail);
        if (e != E_OK) {
            reply_err(e, detail);
            return;
        }
        e = presets_add(preset_buf, n);
    } else if ((strcmp(sub, "get") == 0 || strcmp(sub, "del") == 0 || strcmp(sub, "use") == 0) && argc == 3) {
        if (i < 0) {
            reply_err(E_ARGS, "no such preset");
            return;
        }
        if (sub[0] == 'g') {
            size_t len;
            const uint8_t *p = presets_get(i, &len);
            printf("OK name=%s hex=", argv[2]);
            print_hex(p, len);
            printf("\n");
            return;
        }
        e = sub[0] == 'd' ? presets_delete(i) : presets_use(i);
    } else {
        reply_err(E_ARGS, "usage: preset [list] | get|del|use <name> | add <hex> | reset");
        return;
    }
    if (e != E_OK)
        reply_err(e, e == E_FLASH ? "preset storage full or flash write failed" : NULL);
    else
        printf("OK\n");
}

static void cmd_timeout(int argc, char **argv) {
    uint32_t s;
    if (argc != 2 || !parse_u32(argv[1], &s) || s < 1 || s > 3600) {
        reply_err(E_ARGS, "usage: timeout <1..3600 s>");
        return;
    }
    idle_timeout_s = s;
    printf("OK timeout_s=%lu\n", (unsigned long)s);
}

static const char help_text[] =
    "# version | status | help\n"
    "# power on [force] | power off      open/close a session (force: skip SPD type check)\n"
    "# debug [hv]                        rails on with an EMPTY socket, for measuring\n"
    "# timeout <s>                       idle power-off of a session\n"
    "# leds [on|off]                     LED test: Done, Modified, Busy, Fault, then all\n"
    "# led <done|mod|busy|fault> [level] LED brightness, 0-1000\n"
    "# ident | read <addr> <len> | write <addr> <hex>\n"
    "# crc [fix] | wp | unlock | lock [mask] | temp | scan\n"
    "# mr <reg> <len> | mr <reg> = <val>  (DDR5 hub registers)\n"
    "# program [<preset hex>] | restore [force] | record   apply/undo a preset (default: active)\n"
    "# preset [list] | get|del|use <name> | add <hex> | reset\n"
    "# button                            result of the last Program/Restore button press\n"
    "# bootsel | reboot\n";

static const struct {
    const char *name;
    module_fn fn;
} module_cmds[] = {
    {"read", do_read}, {"write", do_write}, {"crc", do_crc},   {"wp", do_wp},
    {"unlock", do_unlock}, {"lock", do_lock}, {"mr", do_mr}, {"temp", do_temp},
    {"scan", do_scan}, {"ident", do_ident}, {"program", do_program}, {"restore", do_restore},
    {"record", do_record},
};

void cmd_execute(char *line) {
    char *argv[MAX_ARGS];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < MAX_ARGS; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    if (argc == 0)
        return;
    const char *c = argv[0];
    // LED commands leave the Fault LED alone, so it can be compared too.
    if (strcmp(c, "leds") == 0) {
        cmd_leds(argc, argv);
        stdio_flush();
        return;
    }
    if (strcmp(c, "led") == 0) {
        cmd_led(argc, argv);
        stdio_flush();
        return;
    }
    ui_clear();
    led_set(PIN_LED_FAULT, false);

    if (strcmp(c, "version") == 0) {
        printf("OK fw=%s proto=%d\n", FW_VERSION, PROTO_VERSION);
    } else if (strcmp(c, "help") == 0) {
        printf("%sOK\n", help_text);
    } else if (strcmp(c, "status") == 0) {
        cmd_status();
    } else if (strcmp(c, "power") == 0) {
        cmd_power(argc, argv);
    } else if (strcmp(c, "debug") == 0) {
        cmd_debug(argc, argv);
    } else if (strcmp(c, "timeout") == 0) {
        cmd_timeout(argc, argv);
    } else if (strcmp(c, "preset") == 0) {
        cmd_preset(argc, argv);
    } else if (strcmp(c, "button") == 0) {
        ui_report();
    } else if (strcmp(c, "bootsel") == 0 || strcmp(c, "reboot") == 0) {
        power_off();
        printf("OK\n");
        stdio_flush();
        sleep_ms(100);
        if (c[0] == 'b')
            reset_usb_boot(0, 0);
        watchdog_reboot(0, 0, 0);
        while (true)
            tight_loop_contents();
    } else {
        for (unsigned i = 0; i < count_of(module_cmds); i++) {
            if (strcmp(c, module_cmds[i].name) == 0) {
                with_module(module_cmds[i].fn, argc, argv);
                stdio_flush();
                return;
            }
        }
        reply_err(E_CMD, "unknown command (try: help)");
    }
    stdio_flush();
}
