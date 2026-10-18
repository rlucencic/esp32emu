/*
 * w65816_run() against w65816_step(): two CPUs run the same random memory
 * (random code: everything the opcodes do, jumps all over, page and bank
 * ends), one an instruction per call (each call looks the code page up
 * afresh), the other in slices of random length (the code page pointer
 * kept from instruction to instruction, given up at callbacks). Some pages
 * go through the bus (code runs there too), with wait states, and act as
 * soft switches: writes, and reads of $xx5x, switch a bank between two
 * blocks of RAM (the code's own too, mid-run). Banks $10 up are open bus.
 * After each slice the stepping CPU catches up: the cycle
 * counts must meet on an instruction boundary, with the same registers and
 * memory. WAI is ended by an IRQ, STP by a reset with a new seed.
 *
 *   diff [seeds]    (exit status 1 on a difference)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w65816.h"

#define MEM     (1 << 20)               /* banks $00-$0F */

typedef struct {
    w65816_t cpu;
    uint8_t *mem, *alt;                 /* banks $00-$0F, and the other RAM */
    uint16_t switched;                  /* the banks in alt */
    uint8_t *pages[65536];
} machine_t;

static machine_t s_a, s_b;              /* a steps, b runs */
static uint32_t s_rand;

static uint32_t rnd(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return s_rand >> 8;
}

/* A page through the bus: every 16th one in RAM */
static bool io_page(uint32_t page)
{
    return (page * 0x9E37u >> 4) % 16 == 0;
}

static uint8_t *ram(machine_t *m, uint32_t a)
{
    return ((m->switched >> (a >> 16)) & 1 ? m->alt : m->mem) + a;
}

static void map_bank(machine_t *m, int bank)
{
    for (uint32_t i = bank << 8; i < (uint32_t)(bank + 1) << 8; i++) {
        m->pages[i] = io_page(i) ? NULL : ram(m, i << 8);
    }
}

static void switch_bank(machine_t *m, int bank)
{
    m->switched ^= 1 << bank;
    map_bank(m, bank);
}

static uint8_t bus_read(void *ctx, uint32_t a)
{
    machine_t *m = ctx;
    if (a >= MEM) {
        return (a ^ (a >> 8)) & 0xFF;   /* open bus */
    }
    m->cpu.penalty += a & 1;            /* wait states */
    uint8_t v = *ram(m, a);
    if ((a & 0xF0) == 0x50) {
        switch_bank(m, a & 0xF);
    }
    return v;
}

static void bus_write(void *ctx, uint32_t a, uint8_t v)
{
    machine_t *m = ctx;
    if (a < MEM) {
        m->cpu.penalty += 2;
        *ram(m, a) = v;
        switch_bank(m, v & 0xF);
    }
}

static void setup(machine_t *m, const uint8_t *image)
{
    memcpy(m->mem, image, MEM);
    for (int i = 0; i < MEM; i++) {
        m->alt[i] = image[i] ^ 0x5A;
    }
    m->switched = 0;
    memset(m->pages, 0, sizeof(m->pages));
    for (int b = 0; b < 16; b++) {
        map_bank(m, b);
    }
    memset(&m->cpu, 0, sizeof(m->cpu));
    m->cpu.rd = m->cpu.wr = m->pages;
    m->cpu.bus = (w65816_bus_t){ .read = bus_read, .write = bus_write, .ctx = m };
}

static int same(const char *when, long seed)
{
    const w65816_t *a = &s_a.cpu, *b = &s_b.cpu;
    if (a->cycles != b->cycles || a->pc != b->pc || a->pbr != b->pbr || a->a != b->a || a->x != b->x ||
        a->y != b->y || a->s != b->s || a->d != b->d || a->dbr != b->dbr || a->p != b->p || a->e != b->e ||
        a->waiting != b->waiting || a->stopped != b->stopped) {
        printf("seed %ld, %s: step: %llu %02X:%04X a=%04X x=%04X y=%04X s=%04X d=%04X dbr=%02X p=%02X e=%d\n"
               "               run:  %llu %02X:%04X a=%04X x=%04X y=%04X s=%04X d=%04X dbr=%02X p=%02X e=%d\n",
               seed, when, (unsigned long long)a->cycles, a->pbr, a->pc, a->a, a->x, a->y, a->s, a->d, a->dbr, a->p,
               a->e, (unsigned long long)b->cycles, b->pbr, b->pc, b->a, b->x, b->y, b->s, b->d, b->dbr, b->p, b->e);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    long seeds = argc > 1 ? atol(argv[1]) : 5000;
    uint8_t *image = malloc(MEM);
    s_a.mem = malloc(MEM);
    s_b.mem = malloc(MEM);
    s_a.alt = malloc(MEM);
    s_b.alt = malloc(MEM);
    if (image == NULL || s_a.mem == NULL || s_b.mem == NULL || s_a.alt == NULL || s_b.alt == NULL) {
        return 2;
    }
    long instructions = 0, slices = 0, waits = 0, stops = 0;
    for (long seed = 1; seed <= seeds; seed++) {
        s_rand = (uint32_t)seed;
        for (int i = 0; i < MEM; i++) {
            image[i] = rnd();
        }
        setup(&s_a, image);
        setup(&s_b, image);
        w65816_reset(&s_a.cpu);
        w65816_reset(&s_b.cpu);
        for (int k = 0; k < 2000; k++) {
            int slice = 1 + rnd() % 400;
            w65816_run(&s_b.cpu, slice);
            slices++;
            while (s_a.cpu.cycles < s_b.cpu.cycles && w65816_step(&s_a.cpu) > 0) {
                instructions++;         /* (0: stopped, or waiting with no IRQ) */
            }
            if (!same("after a slice", seed)) {
                return 1;
            }
            if (s_b.cpu.stopped) {
                stops++;
                break;
            }
            if (s_b.cpu.waiting) {
                waits++;
                w65816_set_irq(&s_a.cpu, true);
                w65816_set_irq(&s_b.cpu, true);
            } else if (rnd() % 8 == 0) {
                bool level = rnd() & 1;         /* IRQs now and then (I masks most) */
                w65816_set_irq(&s_a.cpu, level);
                w65816_set_irq(&s_b.cpu, level);
            }
        }
        if (memcmp(s_a.mem, s_b.mem, MEM) != 0 || memcmp(s_a.alt, s_b.alt, MEM) != 0) {
            printf("seed %ld: memory differs\n", seed);
            return 1;
        }
    }
    printf("%ld seeds, %ld instructions in %ld slices (%ld WAIs, %ld STPs): run() and step() agree\n", seeds,
           instructions, slices, waits, stops);
    return 0;
}
