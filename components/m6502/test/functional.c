/*
 * Klaus Dormann's 6502 functional test (decimal mode included): the 64 KB
 * image from github.com/Klaus2m5/6502_65C02_functional_tests bin_files/,
 * started at $0400. It loops on itself where a test fails, and at the
 * success address when everything passed.
 *
 *   functional <6502_functional_test.bin> [success address, default 3469]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m6502.h"

static uint8_t s_mem[65536];

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <6502_functional_test.bin> [success address]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL || fread(s_mem, 1, sizeof(s_mem), f) != sizeof(s_mem)) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    unsigned success = argc > 2 ? strtoul(argv[2], NULL, 16) : 0x3469;
    m6502_t c;
    m6502_init(&c, &(m6502_bus_t){ 0 });
    for (int i = 0; i < 256; i++) {
        c.rd[i] = c.wr[i] = s_mem + i * 256;
    }
    c.pc = 0x0400;
    c.s = 0xFF;
    uint64_t instructions = 0;
    for (;;) {
        uint16_t pc = c.pc;
        m6502_step(&c);
        instructions++;
        if (c.pc == pc) {
            break;              /* a trap: JMP * or a branch to itself */
        }
    }
    printf("stopped at $%04X after %llu instructions, %llu cycles: %s\n", c.pc,
           (unsigned long long)instructions, (unsigned long long)c.cycles,
           c.pc == success ? "PASSED" : "FAILED");
    return c.pc == success ? 0 : 1;
}
