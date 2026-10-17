/*
 * Z80 core test: runs CP/M test programs (prelim.com, zexdoc.cim,
 * zexall.cim, from github.com/superzazu/z80 roms/) on a flat 64 KB with
 * the two BDOS calls they use.
 *
 *   z80_test <program.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"

static uint8_t s_mem[65536];

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program.com>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fread(s_mem + 0x100, 1, 0xFE00, f);
    fclose(f);
    for (int i = 0; i < 4; i++) {
        z80_rd[i] = z80_wr[i] = s_mem + i * 0x4000;
    }
    z80_set_bus(&(z80_bus_t){ 0 });
    z80_reset();
    z80.pc.w = 0x100;
    z80.sp.w = 0xF000;
    s_mem[5] = 0xC9;        /* BDOS: handled below, then RET */
    uint64_t time = 0, instructions = 0;
    for (;;) {
        uint16_t pc = z80.pc.w;
        if (pc == 0) {
            break;
        }
        if (pc == 5) {
            if (z80.bc.b.l == 2) {
                putchar(z80.de.b.l);
            } else if (z80.bc.b.l == 9) {
                for (uint16_t a = z80.de.w; s_mem[a] != '$'; a++) {
                    putchar(s_mem[a]);
                }
            }
            fflush(stdout);
        }
        time += z80_step();
        instructions++;
    }
#ifdef CONFIG_Z80_TIMING_CPC
    const char *unit = "CPC us";
#else
    const char *unit = "T-states";
#endif
    printf("\n%llu instructions, %llu %s\n", (unsigned long long)instructions,
           (unsigned long long)time, unit);
    return 0;
}
