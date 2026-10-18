/*
 * What the SingleStepTests suite doesn't cover: IRQ and NMI entry in both
 * modes, WAI (with I clear and set), STP, an interrupted MVN, wait cycles
 * from the bus (penalty, the cycle a callback sees), w65816_end_run(), an
 * IRQ raised by a callback, the WDM trap and run() budgets.
 *
 *   irq        (prints the failures; exit status 1 if any)
 */
#include <stdio.h>
#include <string.h>
#include "w65816.h"

static uint8_t s_mem[1 << 20];          /* banks $00-$0F */
static uint8_t *s_pages[65536];
static w65816_t s_cpu;
static int s_failed;

/* I/O: page $00C0. Reads add `io_penalty` and remember the cycle they saw;
 * a write to $C0FF ends the run, to $C0FE raises IRQ
 */
static int s_io_penalty;
static uint64_t s_io_cycle;

static uint8_t io_read(void *ctx, uint32_t a)
{
    s_io_cycle = s_cpu.cycles;
    s_cpu.penalty += s_io_penalty;
    return a & 0xFF;
}

static void io_write(void *ctx, uint32_t a, uint8_t v)
{
    if (a == 0xC0FF) {
        w65816_end_run(&s_cpu);
    } else if (a == 0xC0FE) {
        w65816_set_irq(&s_cpu, true);
    }
}

static int s_wdm_operand = -1;

static void wdm(void *ctx, w65816_t *cpu, uint8_t operand)
{
    s_wdm_operand = operand;
    cpu->a = 0x1234;                    /* a trap that returns a value */
    cpu->cycles += 10;                  /* and takes time */
}

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failed++; \
        } \
    } while (0)

/* A fresh CPU at 00:1000 with the code given; vectors point at $8000 + their offset */
static void setup(bool e, uint8_t p, const uint8_t *code, int len)
{
    memset(s_mem, 0, sizeof(s_mem));
    for (int v = 0xFFE0; v < 0x10000; v += 2) {
        s_mem[v] = v & 0xFF;
        s_mem[v + 1] = 0x80;
    }
    s_mem[0xFFFC] = 0x00;
    s_mem[0xFFFD] = 0x10;
    memcpy(s_mem + 0x1000, code, len);
    memset(&s_cpu, 0, sizeof(s_cpu));
    s_cpu.rd = s_cpu.wr = s_pages;
    s_cpu.bus = (w65816_bus_t){ .read = io_read, .write = io_write, .wdm = wdm };
    w65816_reset(&s_cpu);
    CHECK(s_cpu.pc == 0x1000 && s_cpu.e && s_cpu.cycles == 7);
    s_cpu.e = e;
    s_cpu.p = p;
    s_cpu.s = e ? 0x1FF : 0x1FF0;
    s_cpu.cycles = 0;
}

int main(void)
{
    for (int i = 0; i < 65536; i++) {
        s_pages[i] = (i < (int)(sizeof(s_mem) >> 8) && i != 0xC0) ? s_mem + i * 256 : NULL;
    }
    static const uint8_t nops[] = { 0xEA, 0xEA, 0xEA, 0xEA };

    /* IRQ, native: PBR, PC, P pushed; 8 cycles; I set, D clear, PBR 0 */
    setup(false, 0x08, nops, 4);
    s_cpu.pbr = 0x00;
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_step(&s_cpu) == 8);
    CHECK(s_cpu.pc == 0x80EE && s_cpu.pbr == 0 && s_cpu.p == 0x04 && s_cpu.s == 0x1FEC);
    CHECK(s_mem[0x1FF0] == 0x00 && s_mem[0x1FEF] == 0x10 && s_mem[0x1FEE] == 0x00 && s_mem[0x1FED] == 0x08);
    /* the level stays, I masks it now: the handler's NOP runs */
    s_mem[0x80EE] = 0xEA;
    CHECK(w65816_step(&s_cpu) == 2 && s_cpu.pc == 0x80EF);

    /* IRQ, emulation: no PBR, B clear in the copy; 7 cycles */
    setup(true, 0x30, nops, 4);
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_step(&s_cpu) == 7);
    CHECK(s_cpu.pc == 0x80FE && s_cpu.s == 0x1FC && s_mem[0x1FD] == 0x20 && s_mem[0x1FE] == 0x00 && s_mem[0x1FF] == 0x10);

    /* masked: the instruction runs */
    setup(true, 0x34, nops, 4);
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_step(&s_cpu) == 2 && s_cpu.pc == 0x1001);

    /* NMI ignores I, both modes; taken once */
    setup(false, 0x04, nops, 4);
    w65816_nmi(&s_cpu);
    CHECK(w65816_step(&s_cpu) == 8 && s_cpu.pc == 0x80EA && !s_cpu.nmi);
    setup(true, 0x34, nops, 4);
    w65816_nmi(&s_cpu);
    CHECK(w65816_step(&s_cpu) == 7 && s_cpu.pc == 0x80FA && s_mem[0x1FD] == 0x24);

    /* CLI with IRQ asserted: taken right after it */
    static const uint8_t cli[] = { 0x58, 0xEA };
    setup(false, 0x04, cli, 2);
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_run(&s_cpu, 2) == 2 && s_cpu.pc == 0x1001);
    CHECK(w65816_step(&s_cpu) == 8 && s_cpu.pc == 0x80EE && s_mem[0x1FEE] == 0x01);

    /* WAI, I clear: waits (run returns at once), then the IRQ is taken
     * with the PC after WAI
     */
    static const uint8_t wai[] = { 0xCB, 0xEA };
    setup(false, 0x00, wai, 2);
    CHECK(w65816_run(&s_cpu, 100) == 3 && s_cpu.waiting && s_cpu.pc == 0x1001);
    CHECK(w65816_run(&s_cpu, 100) == 0 && w65816_step(&s_cpu) == 0);
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_step(&s_cpu) == 8 && !s_cpu.waiting && s_cpu.pc == 0x80EE && s_mem[0x1FEE] == 0x01);

    /* WAI, I set: the IRQ ends it, execution goes on after it */
    setup(false, 0x04, wai, 2);
    CHECK(w65816_step(&s_cpu) == 3 && s_cpu.waiting);
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_step(&s_cpu) == 2 && !s_cpu.waiting && s_cpu.pc == 0x1002);

    /* STP: stays stopped until a reset */
    static const uint8_t stp[] = { 0xDB };
    setup(false, 0x00, stp, 1);
    CHECK(w65816_step(&s_cpu) == 3 && s_cpu.stopped);
    w65816_set_irq(&s_cpu, true);
    w65816_nmi(&s_cpu);
    CHECK(w65816_run(&s_cpu, 1000) == 0);
    w65816_reset(&s_cpu);
    CHECK(!s_cpu.stopped && s_cpu.pc == 0x1000);

    /* MVN, 10 bytes $02:2000 -> $03:3000: 7 cycles a byte; an IRQ between
     * bytes pushes the MVN's own address, RTI goes back to finish it
     */
    static const uint8_t mvn[] = { 0x54, 0x03, 0x02, 0xEA };
    setup(false, 0x00, mvn, 4);
    for (int i = 0; i < 10; i++) {
        s_mem[0x22000 + i] = 0xA0 + i;
    }
    s_mem[0x80EE] = 0x40;                   /* the IRQ handler: RTI */
    s_cpu.a = 9;
    s_cpu.x = 0x2000;
    s_cpu.y = 0x3000;
    CHECK(w65816_run(&s_cpu, 21) == 21 && s_cpu.pc == 0x1000 && s_cpu.a == 6 && s_cpu.dbr == 3);
    w65816_set_irq(&s_cpu, true);
    CHECK(w65816_step(&s_cpu) == 8 && s_cpu.pc == 0x80EE && s_mem[0x1FEF] == 0x10 && s_mem[0x1FEE] == 0x00);
    w65816_set_irq(&s_cpu, false);
    CHECK(w65816_step(&s_cpu) == 7 && s_cpu.pc == 0x1000);        /* RTI */
    CHECK(w65816_run(&s_cpu, 7 * 7) == 49 && s_cpu.pc == 0x1003 && s_cpu.a == 0xFFFF);
    CHECK(s_cpu.x == 0x200A && s_cpu.y == 0x300A && memcmp(s_mem + 0x33000, s_mem + 0x22000, 10) == 0);

    /* Wait cycles: LDA $C010 (abs, 8-bit) reads I/O on its fourth cycle;
     * the callback sees that cycle and adds 3
     */
    static const uint8_t lda_io[] = { 0xAD, 0x10, 0xC0, 0xEA };
    setup(true, 0x30, lda_io, 4);
    s_cpu.cycles = 1000;
    s_io_penalty = 3;
    CHECK(w65816_step(&s_cpu) == 4 + 3 && s_io_cycle == 1003 && s_cpu.a == 0x10 && s_cpu.cycles == 1007);
    s_io_penalty = 0;

    /* A write that ends the run: STA $C0FF, NOP ... stops after the STA */
    static const uint8_t end[] = { 0x8D, 0xFF, 0xC0, 0xEA, 0xEA, 0xEA };
    setup(true, 0x30, end, 6);
    CHECK(w65816_run(&s_cpu, 1000) == 4 && s_cpu.pc == 0x1003);
    CHECK(w65816_run(&s_cpu, 1000) >= 1000);                       /* not sticky */

    /* An IRQ raised by a callback: taken after that instruction */
    static const uint8_t raise[] = { 0x8D, 0xFE, 0xC0, 0xEA };
    setup(false, 0x30, raise, 4);           /* 8-bit A: only $C0FE */
    s_mem[0x80EE] = 0x80;                   /* the handler: BRA * */
    s_mem[0x80EF] = 0xFE;
    CHECK(w65816_run(&s_cpu, 1000) >= 1000 && s_cpu.pc == 0x80EE && s_mem[0x1FEE] == 0x03);

    /* WDM: the trap gets its operand and the registers, may change them */
    static const uint8_t trap[] = { 0x42, 0x5A, 0xEA };
    setup(false, 0x00, trap, 3);
    s_cpu.a = 0x1111;
    CHECK(w65816_step(&s_cpu) == 2 + 10 && s_wdm_operand == 0x5A && s_cpu.a == 0x1234 && s_cpu.pc == 0x1002);
    s_cpu.bus.wdm = NULL;                                         /* no trap: a 2-byte NOP */
    s_cpu.pc = 0x1000;
    CHECK(w65816_step(&s_cpu) == 2 && s_cpu.pc == 0x1002 && s_cpu.a == 0x1234);

    /* Budgets: whole instructions until at least that many cycles */
    setup(true, 0x30, nops, 4);
    CHECK(w65816_run(&s_cpu, 5) == 6 && s_cpu.pc == 0x1003);
    CHECK(w65816_run(&s_cpu, 0) == 0 && w65816_run(&s_cpu, -5) == 0);

    printf("%s\n", s_failed ? "FAILED" : "all passed");
    return s_failed != 0;
}
