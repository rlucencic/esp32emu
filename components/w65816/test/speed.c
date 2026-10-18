/*
 * Speed of the 65C816 core on the host: a loop of mixed code in bank 0
 * (native 16-bit and 8-bit A, emulation mode 6502 code, calls, a block
 * move), first through w65816_step() to count instructions, then through
 * w65816_run() in machine-sized slices (a IIGS line is ~180 cycles; 1000
 * here) for the speed. Memory is allocated (PSRAM on an ESP32: the page
 * tables are 256 KB), so that the same program can run on the chips.
 *
 *   speed [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "w65816.h"

#define BANKS   2
static uint8_t *s_mem;                  /* banks 0-1 */
static uint8_t **s_pages;

static uint8_t no_read(void *ctx, uint32_t a)
{
    return 0xFF;
}

static void no_write(void *ctx, uint32_t a, uint8_t v)
{
}

/* ---- A tiny assembler: bytes at s_at, backward branches by label ---- */

static uint16_t s_at = 0x1000;

static void b(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++) {
        s_mem[s_at++] = (uint8_t)__builtin_va_arg(ap, int);
    }
    __builtin_va_end(ap);
}

#define W(x)    ((x) & 0xFF), ((x) >> 8)

static void branch(int op, uint16_t to)
{
    b(2, op, (uint8_t)(to - (s_at + 2)));
}

static void assemble(void)
{
    uint16_t sub = 0x0800, esub = 0x0900;
    /* Data: a pointer [$2010] -> $013000, values in the direct page */
    s_mem[0x2010] = 0x00;
    s_mem[0x2011] = 0x30;
    s_mem[0x2012] = 0x01;
    for (int i = 0; i < 0x1000; i++) {
        s_mem[0x3000 + i] = (uint8_t)(i * 7);
    }
    s_mem[0x0006] = 0x00;               /* ($06) -> $3000 for the 6502 part */
    s_mem[0x0007] = 0x30;

    uint16_t top = s_at;
    b(1, 0x18);                         /* CLC */
    b(1, 0xFB);                         /* XCE: native */
    b(2, 0xC2, 0x30);                   /* REP #$30 */
    b(3, 0xA9, W(0x2000));              /* LDA #$2000 */
    b(1, 0x5B);                         /* TCD */
    b(3, 0xA2, W(0));                   /* LDX #0 */
    b(3, 0xA0, W(0));                   /* LDY #0 */
    uint16_t l16 = s_at;
    b(3, 0xBD, W(0x3000));              /* LDA $3000,X */
    b(1, 0x18);                         /* CLC */
    b(3, 0x69, W(0x1234));              /* ADC #$1234 */
    b(3, 0x9D, W(0x4000));              /* STA $4000,X */
    b(2, 0xB7, 0x10);                   /* LDA [$10],Y */
    b(2, 0x45, 0x20);                   /* EOR $20 */
    b(2, 0x85, 0x22);                   /* STA $22 */
    b(1, 0xC8);                         /* INY */
    b(1, 0xE8);                         /* INX */
    b(1, 0xE8);                         /* INX */
    b(3, 0xE0, W(0x0200));              /* CPX #$200 */
    branch(0xD0, l16);                  /* BNE */
    b(3, 0x20, W(sub));                 /* JSR sub */
    b(2, 0xE2, 0x20);                   /* SEP #$20 */
    b(3, 0xA0, W(0));                   /* LDY #0 */
    uint16_t l8 = s_at;
    b(3, 0xB9, W(0x3000));              /* LDA $3000,Y */
    b(2, 0xC9, 0x80);                   /* CMP #$80 */
    b(2, 0x90, 0x02);                   /* BCC +2 */
    b(2, 0xA9, 0x7F);                   /* LDA #$7F */
    b(3, 0x99, W(0x5000));              /* STA $5000,Y */
    b(1, 0xC8);                         /* INY */
    b(3, 0xC0, W(0x0100));              /* CPY #$100 */
    branch(0xD0, l8);                   /* BNE */
    b(2, 0xC2, 0x20);                   /* REP #$20 */
    b(1, 0x8B);                         /* PHB */
    b(3, 0xA9, W(0x003F));              /* LDA #$3F: 64 bytes */
    b(3, 0xA2, W(0x3000));              /* LDX #$3000 */
    b(3, 0xA0, W(0x6000));              /* LDY #$6000 */
    b(3, 0x54, 0x00, 0x00);             /* MVN $00,$00 */
    b(1, 0xAB);                         /* PLB */
    b(3, 0xA9, W(0x0000));              /* LDA #0 */
    b(1, 0x5B);                         /* TCD: D = 0 for the 6502 part */
    b(1, 0x38);                         /* SEC */
    b(1, 0xFB);                         /* XCE: emulation */
    b(2, 0xA2, 0x80);                   /* LDX #$80 */
    b(2, 0xA0, 0x00);                   /* LDY #0 */
    uint16_t le = s_at;
    b(3, 0xBD, W(0x0400));              /* LDA $0400,X */
    b(2, 0x49, 0x80);                   /* EOR #$80 */
    b(3, 0x9D, W(0x0400));              /* STA $0400,X */
    b(2, 0xB1, 0x06);                   /* LDA ($06),Y */
    b(1, 0xC8);                         /* INY */
    b(1, 0x18);                         /* CLC */
    b(2, 0x65, 0x08);                   /* ADC $08 */
    b(2, 0x85, 0x08);                   /* STA $08 */
    b(2, 0xE6, 0x09);                   /* INC $09 */
    b(1, 0xCA);                         /* DEX */
    branch(0xD0, le);                   /* BNE */
    b(3, 0x20, W(esub));                /* JSR esub */
    b(3, 0x4C, W(top));                 /* JMP top */

    s_at = sub;                         /* native, 16-bit */
    b(1, 0x48);                         /* PHA */
    b(2, 0xA5, 0x22);                   /* LDA $22 */
    b(1, 0x0A);                         /* ASL A */
    b(2, 0x65, 0x24);                   /* ADC $24 */
    b(2, 0x85, 0x24);                   /* STA $24 */
    b(1, 0x68);                         /* PLA */
    b(1, 0x60);                         /* RTS */

    s_at = esub;                        /* emulation */
    b(1, 0x08);                         /* PHP */
    b(2, 0xA9, 0x55);                   /* LDA #$55 */
    b(2, 0x29, 0x0F);                   /* AND #$0F */
    b(1, 0x28);                         /* PLP */
    b(1, 0x60);                         /* RTS */
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 2.0;
    s_mem = calloc(BANKS, 1 << 16);
    s_pages = calloc(65536, sizeof(*s_pages));
    if (s_mem == NULL || s_pages == NULL) {
        return 2;
    }
    for (int i = 0; i < BANKS * 256; i++) {
        s_pages[i] = s_mem + i * 256;
    }
    assemble();
    s_mem[0xFFFC] = 0x00;
    s_mem[0xFFFD] = 0x10;
    w65816_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.rd = cpu.wr = s_pages;
    cpu.bus = (w65816_bus_t){ .read = no_read, .write = no_write };
    w65816_reset(&cpu);
    cpu.s = 0x1FF;

    /* Instructions per cycle, by stepping */
    uint64_t steps = 0, c0 = cpu.cycles;
    double t0 = now();
    while (now() - t0 < seconds / 2) {
        for (int i = 0; i < 100000; i++) {
            w65816_step(&cpu);
        }
        steps += 100000;
    }
    double ts = now() - t0;
    double cpi = (double)(cpu.cycles - c0) / steps;

    /* The speed, by running */
    c0 = cpu.cycles;
    t0 = now();
    while (now() - t0 < seconds / 2) {
        for (int i = 0; i < 1000; i++) {
            w65816_run(&cpu, 1000);
        }
    }
    double tr = now() - t0;
    double cps = (cpu.cycles - c0) / tr;
    printf("%.2f cycles per instruction\n", cpi);
    printf("w65816_step(): %.1f M instructions/s\n", steps / ts / 1e6);
    printf("w65816_run():  %.1f M instructions/s, %.1f M cycles/s (%.0f x a 2.8 MHz IIGS)\n",
           cps / cpi / 1e6, cps / 1e6, cps / 2.8e6);
    if (cpu.stopped || cpu.pbr != 0 || cpu.pc < 0x0800 || cpu.pc > 0x1100) {
        printf("the program went astray: pc %02X:%04X\n", cpu.pbr, cpu.pc);
        return 1;
    }
    return 0;
}
