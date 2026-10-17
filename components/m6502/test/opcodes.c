/* Every opcode once on a flat 64 KB: which ones jam, and their cycles */
#include <stdio.h>
#include <string.h>
#include "m6502.h"
static uint8_t mem[65536];
static uint8_t r(void *x, uint16_t a) { return mem[a]; }
static void w(void *x, uint16_t a, uint8_t v) { mem[a] = v; }
int main(void)
{
    m6502_t c;
    m6502_bus_t bus = { r, w, NULL, NULL };
    for (int op = 0; op < 256; op++) {
        memset(mem, 0, sizeof(mem));
        m6502_init(&c, &bus);
        c.pc = 0x1000; c.s = 0xFF; c.x = 0; c.y = 0;
        mem[0x1000] = op; mem[0x1001] = 0x10; mem[0x1002] = 0x20;
        int t = m6502_step(&c);
        char d[32];
        m6502_disassemble(&c, 0x1000, d, sizeof(d));
        printf("%02X %-16s %d%s\n", op, d, t, c.jammed ? " JAM" : "");
    }
}
