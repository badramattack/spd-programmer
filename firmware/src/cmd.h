// Line-based command interface over USB CDC.
//
// Every command gets exactly one final line, "OK [key=value ...]" or
// "ERR <name> <message>". Lines starting with '#' are asynchronous notices
// (e.g. an automatic power-off) and may appear between commands.
#pragma once

#include "pico/stdlib.h"

void cmd_execute(char *line);
// Seconds of inactivity after which a powered session is switched off.
uint32_t cmd_idle_timeout_s(void);
