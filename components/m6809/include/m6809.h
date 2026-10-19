/*
 * Motorola 6809 and Hitachi 6309 core, for the Tandy Color Computers
 * (../esp32coco) and whatever else needs one.
 *
 * Instruction level, with each instruction's documented cycle count (the
 * 6809's, or the 6309's in emulation or native mode) and the documented
 * results and flags. The 6309 adds its registers (E, F, W, Q, V, MD), its
 * instructions (register-register arithmetic, TFM, the bit operations,
 * DIVD, DIVQ, MULD, the W and E/F operations, LDQ/STQ, OIM/AIM/EIM/TIM),
 * its indexed modes (E, F, W offsets, ,W modes), native mode (W stacked by
 * interrupts and RTI, FIRQ stacking everything when MD's FM bit is set)
 * and its traps (illegal opcodes and division by zero, vector $FFF0).
 * The 6809's undocumented opcodes do what the common descriptions say
 * (the $01/$05/$0B aliases of NEG, LSR, DEC; $3E as a RESET), the 6309's
 * illegal ones trap.
 *
 * Memory goes through 256-byte pages (rd/wr pointers; NULL: the bus's
 * read/write functions, e.g. I/O). `cycles` counts CPU cycles since
 * power-on; m6809_run() runs instructions until `until`.
 *
 * Interrupts: IRQ and FIRQ are levels (any bit set = asserted, one bit per
 * source), NMI an edge (m6809_nmi()); NMIs are ignored until the program
 * has loaded S (as on the chip). HALT (the DMA line the Color Computer's
 * disk controller uses) stops the CPU between instructions while `halt`
 * is set. CWAI and SYNC wait for an interrupt, spending cycles to `until`.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define M6809_HOT   IRAM_ATTR
#else
#define M6809_HOT
#endif

typedef struct m6809 m6809_t;

typedef struct {
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t v);
    void *ctx;
} m6809_bus_t;

/* CC bits */
enum {
    M6809_C = 0x01, M6809_V = 0x02, M6809_Z = 0x04, M6809_N = 0x08,
    M6809_I = 0x10, M6809_H = 0x20, M6809_F = 0x40, M6809_E = 0x80,
};

/* 6309 MD bits */
enum {
    M6309_NM = 0x01,                /* native mode */
    M6309_FM = 0x02,                /* FIRQ stacks everything */
    M6309_IL = 0x40,                /* the last trap was an illegal opcode */
    M6309_DZ = 0x80,                /* ... a division by zero */
};

/* Why the CPU waits */
enum {
    M6809_RUNNING,
    M6809_CWAI,                     /* registers stacked, waiting for an interrupt */
    M6809_SYNC,                     /* waiting for an interrupt line */
};

struct m6809 {
    /* D = A:B and W = E:F, Q = D:W (the 6309's). Little-endian hosts. */
    union {
        uint16_t d;
        struct { uint8_t b, a; };
    };
    union {
        uint16_t w;
        struct { uint8_t f, e; };
    };
    uint16_t x, y, u, s, pc, v;
    uint8_t dp, cc, md;
    bool h6309;                     /* the 6309's instruction set (set before m6809_reset()) */

    uint64_t cycles;                /* CPU cycles since power-on */
    uint64_t until;                 /* m6809_run() stops before an instruction at or after this */

    /* Interrupt inputs */
    uint32_t irq, firq;             /* levels: any bit set = asserted */
    bool nmi_pending;               /* an NMI edge to take */
    bool nmi_armed;                 /* S has been loaded since the reset */
    bool halt;                      /* the HALT line: no instructions while set */
    uint8_t wait;                   /* M6809_RUNNING, _CWAI, _SYNC */

    uint8_t *rd[256];               /* 256-byte pages; NULL: bus.read */
    uint8_t *wr[256];               /* NULL: bus.write */
    m6809_bus_t bus;

    /* The address of the instruction running (for the machine's bus
     * callbacks: the CoCo's floating bus)
     */
    uint16_t ppc;

    /* The debugger (emudbg.h): before an instruction whose address has its
     * bit set in dbg_map, m6809_run() calls emudbg_hit() with dbg (NULL:
     * never). Set by emudbg_arch_6809; m6809_init() clears them.
     */
    const uint8_t *volatile dbg_map;
    void *dbg;
};

void m6809_init(m6809_t *cpu, const m6809_bus_t *bus, bool h6309);

/* RESET: the vector at $FFFE; the 6309 back in emulation mode */
void m6809_reset(m6809_t *cpu);

/* Run instructions (and interrupts) until cpu->until (bus callbacks may
 * lower it to stop earlier)
 */
void m6809_run(m6809_t *cpu);

/* One instruction (or an interrupt, or a wait); its cycles */
int m6809_step(m6809_t *cpu);

static inline void m6809_nmi(m6809_t *cpu)
{
    cpu->nmi_pending = true;
}

/* From bus callbacks during m6809_run(): stop before the next instruction */
static inline void m6809_end_run(m6809_t *cpu)
{
    cpu->until = 0;
}

/* Memory as the CPU sees it, without side effects (I/O reads 0xFF) */
static inline uint8_t m6809_peek(const m6809_t *cpu, uint16_t addr)
{
    const uint8_t *page = cpu->rd[addr >> 8];
    return page ? page[addr & 0xFF] : 0xFF;
}

/* The instruction at addr as text ("LDA $FF00", "LEAX 5,Y"; upper case,
 * $ hex) through m6809_peek(); its length in bytes
 */
int m6809_disassemble(const m6809_t *cpu, uint16_t addr, char *buf, int len);

/* Its length only */
int m6809_insn_length(const m6809_t *cpu, uint16_t addr);
