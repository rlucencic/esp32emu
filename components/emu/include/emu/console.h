#pragma once

/*
 * Line commands on the serial console (tools/emu_console.py):
 *
 *   s                  dump the machine's screen (RGB565, base64)
 *   L                  dump the whole LCD (1024x600)
 *   r                  cold reset
 *   F <drive> [name]   insert <name> from the disk folder into drive 0, 1, ...;
 *                      eject without a name, "+" for a new blank disk
 *   t <x> <y> [ms]     touch the screen there (default 150 ms)
 *   i                  print the machine state
 *
 * Anything else goes to the machine (emu_machine_t.console_command), e.g.
 * keys and mouse.
 *
 * Screen dumps: "EMUSCREEN <width> <height> <row repeat>", base64 lines of
 * big-endian RGB565, "EMUSCREEN END". Row repeat 2 means the pixels are
 * twice as tall as wide (ST medium resolution).
 */

/* Printf to the console (for the machine's console commands) */
void emu_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
