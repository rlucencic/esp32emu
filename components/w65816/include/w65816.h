/*
 * WDC 65C816 core, for the Apple IIGS and whatever else needs one.
 *
 * Instruction-stepped, with the chip's cycle counts. Each instruction makes
 * the bus accesses the chip makes with VDA or VPA asserted (opcodes,
 * operands, data, stack, vectors), in the chip's order; the internal
 * cycles (VDA = VPA = 0: the 6502's dummy reads, the extra cycle when DL is
 * not 0, an index crossing a page, ...) are only counted, so I/O sees what
 * a machine that decodes VDA/VPA sees. All 256 opcodes in native and
 * emulation mode: 8 and 16-bit A and X/Y, decimal mode in both widths, the
 * emulation mode's page 1 stack and direct page wrapping (only for the
 * 6502's instructions and addressing modes, as on the chip), MVN/MVP one
 * byte per step, WAI, STP. Checked against the SingleStepTests 65816 suite,
 * and w65816_run() against w65816_step() on random code (test/).
 *
 * Memory is 24-bit, in 256-byte pages: rd[addr >> 8] / wr[addr >> 8] point
 * to a page (index it with addr & 0xFF); NULL sends the access to the bus's
 * read/write (I/O, shadowed or slow memory), which may add wait cycles in
 * `penalty`. The machine owns the 65536-entry tables and may change their
 * entries at any time, from bus callbacks too (bank switching); the two
 * table pointers themselves only between w65816_run() calls. 16-bit
 * operands are two byte accesses, so page and bank boundaries behave as on
 * the chip.
 *
 * Interrupts are taken between instructions (and between the bytes of
 * MVN/MVP). The core looks at irq and nmi when a run starts, after every
 * bus callback and after an instruction that may clear I: set them there
 * (or between runs), not from another task.
 *
 * To start: zero the struct, set rd, wr and bus, then w65816_reset().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define W65816_HOT  IRAM_ATTR
#else
#define W65816_HOT
#endif

typedef struct w65816 w65816_t;

typedef struct {
    /* Pages whose rd/wr pointer is NULL: I/O, shadowed or slow memory. In
     * these, cpu->cycles is the cycle of the access (the registers are
     * those from before the run); add wait states to cpu->penalty
     */
    uint8_t (*read)(void *ctx, uint32_t addr);
    void (*write)(void *ctx, uint32_t addr, uint8_t v);
    /* WDM ($42 nn): the machine's trap (the IIGS's fake SmartPort card),
     * called with the registers in the w65816_t and pc past the operand;
     * it may change them (and cycles, penalty). NULL: a 2-byte NOP
     */
    void (*wdm)(void *ctx, w65816_t *cpu, uint8_t operand);
    void *ctx;
} w65816_bus_t;

/* P; in emulation mode M and X read as 1 and X's bit is B in pushed copies */
enum {
    W65816_C = 0x01, W65816_Z = 0x02, W65816_I = 0x04, W65816_D = 0x08,
    W65816_X = 0x10, W65816_M = 0x20, W65816_V = 0x40, W65816_N = 0x80,
    W65816_B = 0x10,
};

struct w65816 {
    uint16_t a, x, y, s, d, pc;     /* a is C (B:A) */
    uint8_t pbr, dbr, p;
    bool e;                         /* emulation mode */
    bool stopped;                   /* STP: until w65816_reset() */
    bool waiting;                   /* WAI: until IRQ or NMI */
    bool irq;                       /* the IRQ line (a level) */
    bool nmi;                       /* an NMI edge is pending */
    int penalty;                    /* wait cycles from bus callbacks (the core adds and clears it) */
    uint8_t vector_bank;            /* where vectors are read ($00; the IIGS: $FF, its ROM) */
    uint8_t **rd, **wr;             /* 65536 page pointers by addr >> 8 (the machine's tables) */
    w65816_bus_t bus;
    uint64_t cycles;                /* since power-on */
    /* The debugger (emudbg.h): before an instruction in a page whose bit
     * is set in dbg_map (by 256-byte page), w65816_run() calls
     * emudbg_hit() with dbg (NULL: never). Set by emudbg_arch_65c816.
     */
    const uint8_t *volatile dbg_map;
    void *dbg;
    /* The core's own */
    uint64_t run_base;              /* cycles when w65816_run() started */
    int32_t run_budget;             /* its cycles */
    int32_t run_held;               /* cycles left, held while the run loop looks at the inputs */
    bool end_run;                   /* w65816_end_run() */
    uint16_t code_pc;               /* during a run: the pc of code_base[0] */
    const uint8_t *code_base;       /* the code's page (pc is stale meanwhile) */
};

/* RESET: emulation mode, flags M = X = I = 1 and D = 0, registers D = DBR =
 * PBR = 0, S in page 1, the vector at $00FFFC (7 cycles); ends STP and WAI
 */
void w65816_reset(w65816_t *cpu);

/* Whole instructions (and interrupt entries) until at least `cycles` have
 * passed, or the CPU is stopped or waiting with no interrupt to end it, or
 * a bus callback called w65816_end_run(); the cycles used (penalties
 * included). While waiting or stopped it returns at once: the machine moves
 * its own clock on, to its next event.
 */
int w65816_run(w65816_t *cpu, int cycles);

/* One instruction (or interrupt entry, or one byte of MVN/MVP); its cycles */
int w65816_step(w65816_t *cpu);

/* The IRQ line's level (the machine ORs its sources) */
static inline void w65816_set_irq(w65816_t *cpu, bool level)
{
    cpu->irq = level;
}

/* An NMI edge: taken before the next instruction */
static inline void w65816_nmi(w65816_t *cpu)
{
    cpu->nmi = true;
}

/* From a bus callback: w65816_run() returns after this instruction (a
 * speed change, an event that came earlier)
 */
static inline void w65816_end_run(w65816_t *cpu)
{
    cpu->end_run = true;
}

/* Memory as the CPU sees it, without side effects (NULL pages read 0xFF) */
static inline uint8_t w65816_peek(const w65816_t *cpu, uint32_t addr)
{
    const uint8_t *p = cpu->rd[(addr >> 8) & 0xFFFF];
    return p ? p[addr & 0xFF] : 0xFF;
}

/* Disassemble the instruction at the 24-bit addr into buf, immediates
 * sized by the current m and x flags; its length
 */
int w65816_disassemble(const w65816_t *cpu, uint32_t addr, char *buf, int len);
