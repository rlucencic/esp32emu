/*
 * MOS 6502 core (6510, 8502, the drives' 6502), shared by the Commodore
 * machines (../esp32cbm) and whatever else needs one.
 *
 * Every cycle is a bus access, as on the chip: dummy reads (of the next
 * byte, of the wrong page when an index crosses one, of the stack) and the
 * read-modify-write instructions' double write happen, so instruction times
 * come out right by themselves and each access happens on its own cycle.
 * All 256 opcodes: the documented ones with decimal mode, the stable
 * undocumented ones (SLO RLA SRE RRA SAX LAX DCP ISC ANC ALR ARR SBX LAS,
 * the NOPs), the unstable ones as the common chips do them (SHA SHX SHY
 * TAS ANE LXA), and JAM, which stops the CPU until a reset.
 *
 * Several CPUs at once: each has its own state and bus. Memory goes
 * through 256-byte pages (rd/wr pointers; NULL: the bus's read/write
 * functions, e.g. I/O). `cycles` counts bus cycles; a machine that steals
 * them (the VIC-II's bad lines, BA) sets `stall_from`: a read at or after
 * that cycle calls `stall()` first, which may move `cycles` on and set the
 * next `stall_from`. With `stall_writes` writes stall too (the Atari's
 * 6502C, whose HALT stops any cycle; RDY only stops reads).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define M6502_HOT   IRAM_ATTR
#else
#define M6502_HOT
#endif

typedef struct m6502 m6502_t;

typedef struct {
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t v);
    /* Called on a read at or after cpu->stall_from (NULL: never) */
    void (*stall)(void *ctx, m6502_t *cpu);
    void *ctx;
} m6502_bus_t;

enum {
    M6502_C = 0x01, M6502_Z = 0x02, M6502_I = 0x04, M6502_D = 0x08,
    M6502_B = 0x10, M6502_U = 0x20, M6502_V = 0x40, M6502_N = 0x80,
};

struct m6502 {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint64_t cycles;                /* bus cycles since power-on */
    uint64_t stall_from;            /* reads from this cycle on ask bus.stall() */
    bool stall_writes;              /* ... and writes (set before m6502_run()) */
    bool stall_is_write;            /* in bus.stall(): the access waiting is a write */
    /* Optional DMA tables, for a machine whose DMA takes known cycles of a
     * period (the Atari's ANTIC, per scan line): for the cycle offsets c <
     * dma_len from dma_base, dma_free[c] is the first cycle the CPU may use
     * from c on and dma_busy[c] the first it may not (dma_len: none; both
     * hold dma_len + 1 entries). An access at or after stall_from then skips
     * taken cycles by itself, and calls bus.stall() only when that runs out
     * of the table or reaches dma_stop (the machine's business: a WSYNC
     * wait). NULL: bus.stall() for every stall.
     */
    const uint8_t *dma_free, *dma_busy;
    uint64_t dma_base, dma_stop;
    uint32_t dma_len;
    /* m6502_run() stops before an instruction at `until` or later (bus
     * callbacks may lower it, an earlier device event), and after one
     * that leaves pc at stop_pc (> 0xFFFF: none)
     */
    uint64_t until;
    uint32_t stop_pc;
    bool limits_changed;            /* until or stall_from moved during m6502_run() */
    /* Stop after a taken BVC/BVS to itself, or a BPL/BMI 5 bytes back (a
     * floppy drive waiting for a byte: BVC *, BIT $180F / BMI)
     */
    bool stop_on_spin;
    /* Interrupt inputs: irq is a level (any bit set = asserted, one bit per
     * source), nmi an edge (m6502_nmi())
     */
    uint32_t irq;
    bool nmi_pending;
    uint64_t nmi_cycle;             /* when the pending NMI's edge came */
    bool jammed;                    /* a JAM opcode: stopped until reset */
    bool decimal;                   /* honour the D flag (the 2A03 doesn't) */
    /* The 6510's I/O port at $00/$01 is the machine's business */
    uint8_t *rd[256];               /* 256-byte pages; NULL: bus.read */
    uint8_t *wr[256];               /* NULL: bus.write */
    m6502_bus_t bus;
    /* An IRQ is taken after the instruction that saw it at its last
     * cycle but one; set by the core
     */
    bool irq_seen;
    uint8_t poll_i;                 /* the I flag that polling used (m6502_repoll()) */
    uint64_t poll_cycle;            /* the cycle interrupts were polled at (the last but one) */
    uint64_t hijack_cycle;          /* BRK/IRQ: an NMI by then takes over its vector */
};

void m6502_init(m6502_t *cpu, const m6502_bus_t *bus);

/* RESET: the vector at $FFFC (7 cycles) */
void m6502_reset(m6502_t *cpu);

/* Run instructions (and interrupts) up to cpu->until or cpu->stop_pc.
 * The registers live in locals meanwhile; the m6502_t is up to date in
 * bus callbacks (cycles, until, stall_from) and afterwards.
 */
void m6502_run(m6502_t *cpu);

/* One instruction (or an interrupt); its cycles */
int m6502_step(m6502_t *cpu);

/* An NMI edge at cycle 'at' (taken after the instruction whose poll_cycle
 * it came by; 0: before the next instruction)
 */
static inline void m6502_nmi_at(m6502_t *cpu, uint64_t at)
{
    if (!cpu->nmi_pending) {
        cpu->nmi_pending = true;
        cpu->nmi_cycle = at;
    }
}

static inline void m6502_nmi(m6502_t *cpu)
{
    m6502_nmi_at(cpu, 0);
}

/* From bus callbacks during m6502_run(): stop earlier; reads stall from t */
static inline void m6502_lower_until(m6502_t *cpu, uint64_t t)
{
    if (t < cpu->until) {
        cpu->until = t;
        cpu->limits_changed = true;
    }
}

static inline void m6502_set_stall_from(m6502_t *cpu, uint64_t t)
{
    cpu->stall_from = t;
    cpu->limits_changed = true;
}

/* After m6502_run(): the IRQ line changed during the last instruction, by its
 * poll_cycle (a machine that brings its devices up to date between
 * instructions finds that out late): poll it again
 */
void m6502_repoll(m6502_t *cpu);

/* The SO pin's falling edge: sets V (the 1541 has it on "byte ready").
 * Between m6502_run() calls only: inside one (a bus callback) the
 * registers are in the run's locals and V would be lost.
 */
static inline void m6502_set_overflow(m6502_t *cpu)
{
    cpu->p |= M6502_V;
}

/* Memory as the CPU sees it, without side effects (I/O pages read 0xFF
 * unless peek is given)
 */
uint8_t m6502_peek(const m6502_t *cpu, uint16_t addr);

/* Disassemble the instruction at pc into buf; its length */
int m6502_disassemble(const m6502_t *cpu, uint16_t pc, char *buf, int len);
