/*
 * How fast the ARM2 core runs on the host: four loops (built from
 * clang -target armv4-none-eabi), each for 50 s of the emulated
 * machine's 8 MHz, in 20 ms runs as the Archimedes does.
 *
 *   build/bench        million instructions a second, ns an instruction
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "arm2.h"

#define MEM_SIZE    (64 * 1024)
#define RUNS        2500                /* of 20 ms */

static const uint32_t s_code[] = {
    /* 0x00 alu: add, cmp, bne */
    0xE3A00000, 0xE2800001, 0xE3500301, 0x1AFFFFFC, 0xEAFFFFFA, 0xE1A00000, 0xE1A00000, 0xE1A00000,
    0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000,
    /* 0x40 mem: ldr, str post-indexed, subs, bne */
    0xE3A02902, 0xE3A03A09, 0xE3A04B01, 0xE4921004, 0xE4831004, 0xE2544001, 0x1AFFFFFB, 0xEAFFFFF7,
    0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000,
    /* 0x80 blk: ldmia, stmia of 8 registers */
    0xE3A02902, 0xE3A03A09, 0xE3A0C080, 0xE8B20FF0, 0xE8A30FF0, 0xE25CC001, 0x1AFFFFFB, 0xEAFFFFF7,
    0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000, 0xE1A00000,
    /* 0xC0 mix: shifted operands, flags, a condition */
    0xE3A01001, 0xE3A02003, 0xE3A05801, 0xE0911182, 0xE02332E1, 0xE20340FF, 0xE1846231, 0xE1B07146,
    0x42677000, 0xE2555001, 0x1AFFFFF7, 0xEAFFFFF3,
};

static uint32_t bus_read(void *ctx, uint32_t addr, bool byte, bool *abort)
{
    *abort = true;
    return 0;
}

static void bus_write(void *ctx, uint32_t addr, uint32_t v, bool byte, bool *abort)
{
    *abort = true;
}

int main(void)
{
    static const struct { const char *name; uint32_t start; } loops[] = {
        { "alu", 0x00 }, { "mem", 0x40 }, { "blk", 0x80 }, { "mix", 0xC0 },
    };
    uint8_t *mem = aligned_alloc(4096, MEM_SIZE);
    static uintptr_t page[ARM2_PAGES];
    for (uint32_t p = 0; p < MEM_SIZE / ARM2_PAGE_SIZE; p++) {
        page[p] = (uintptr_t)(mem + p * ARM2_PAGE_SIZE) | ARM2_USER_R | ARM2_USER_W | ARM2_PRIV_R | ARM2_PRIV_W;
    }
    double total_ns = 0, total_insns = 0;
    for (size_t l = 0; l < sizeof(loops) / sizeof(loops[0]); l++) {
        memset(mem, 0, MEM_SIZE);
        memcpy(mem, s_code, sizeof(s_code));
        static arm2_t cpu;
        arm2_bus_t bus = { .read = bus_read, .write = bus_write };
        arm2_init(&cpu, &bus, page);
        arm2_reset(&cpu);
        cpu.pc = loops[l].start;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int run = 0; run < RUNS; run++) {
            arm2_run(&cpu, 160000);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        printf("%-4s %7.1f MIPS  %5.2f ns an instruction  (%.2f instructions a tick)\n", loops[l].name,
               cpu.insns / ns * 1e3, ns / cpu.insns, cpu.insns / ((double)RUNS * 160000));
        total_ns += ns;
        total_insns += cpu.insns;
    }
    printf("all  %7.1f MIPS\n", total_insns / total_ns * 1e3);
    return 0;
}
