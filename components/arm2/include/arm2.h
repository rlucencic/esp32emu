/*
 * ARM2 core (ARMv2: the Acorn Archimedes' CPU, ../esp32arc), and with
 * `swp` the ARM250's and ARM3's SWP.
 *
 * 26-bit addresses: R15 holds the PC (bits 25-2) and the PSR (N Z C V in
 * bits 31-28, I and F in 27-26, the mode in 1-0). Four modes with their
 * banked registers (FIQ: R8-R14, IRQ and SVC: R13-R14); the exceptions,
 * their vectors and return addresses as on the chip; R15 as an operand as
 * the ARM2 reads it (as Rn the PC + 8 alone, as Rm with the PSR, + 12 for a
 * register-specified shift or a store); TEQP/CMPP and friends, MOVS PC and
 * LDM with ^ for the PSR; STM/LDM with ^ for the user registers; LDR/STR
 * with T for user-mode accesses; unaligned loads rotated. Coprocessor
 * instructions go to the machine (none: the undefined instruction trap,
 * where RISC OS's floating point emulator lives).
 *
 * Memory goes through a table of 4 KB pages covering the 64 MB address
 * space: each entry is a host address (4 KB aligned) with the access bits
 * below. Accesses the entry doesn't allow (I/O, writes to ROM, unmapped
 * pages, the wrong privilege) go to the bus callbacks, which may abort them.
 *
 * Time is counted in ticks of the machine's memory clock (the
 * Archimedes' 8 MHz): a sequential cycle 1 tick, a non-sequential one 2,
 * an internal one 1; pages marked slow (ROM) take `slow_s`/`slow_n` ticks
 * for their cycles. arm2_run() runs until the ticks given are used up.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define ARM2_HOT    IRAM_ATTR
#else
#define ARM2_HOT
#endif

#define ARM2_PAGE_SHIFT     12
#define ARM2_PAGE_SIZE      (1u << ARM2_PAGE_SHIFT)
#define ARM2_PAGES          (1u << (26 - ARM2_PAGE_SHIFT))     /* 16384 */
#define ARM2_ADDR_MASK      0x03FFFFFFu

/* Page table entry bits (the host address is 4 KB aligned) */
enum {
    ARM2_USER_R = 0x01,             /* readable in user mode */
    ARM2_USER_W = 0x02,             /* writable in user mode */
    ARM2_PRIV_R = 0x04,             /* readable in the other modes */
    ARM2_PRIV_W = 0x08,             /* writable in the other modes */
    ARM2_SLOW = 0x10,               /* slow memory (ROM): slow_s, slow_n ticks */
    ARM2_WATCH = 0x20,              /* writes set `watch_hit` (the screen's memory) */
};

/* The PSR, in R15's bits */
#define ARM2_N      0x80000000u
#define ARM2_Z      0x40000000u
#define ARM2_C      0x20000000u
#define ARM2_V      0x10000000u
#define ARM2_I      0x08000000u
#define ARM2_F      0x04000000u
#define ARM2_MODE   0x00000003u
#define ARM2_PSR    0xFC000003u
#define ARM2_PC     0x03FFFFFCu

enum { ARM2_USR, ARM2_FIQ, ARM2_IRQ, ARM2_SVC };

/* Exception vectors */
enum {
    ARM2_VEC_RESET = 0x00,
    ARM2_VEC_UNDEFINED = 0x04,
    ARM2_VEC_SWI = 0x08,
    ARM2_VEC_PREFETCH_ABORT = 0x0C,
    ARM2_VEC_DATA_ABORT = 0x10,
    ARM2_VEC_ADDRESS = 0x14,
    ARM2_VEC_IRQ = 0x18,
    ARM2_VEC_FIQ = 0x1C,
};

typedef struct arm2 arm2_t;

typedef struct {
    /* Reads and writes the page table doesn't allow (addresses within 26
     * bits, as the ARM puts them out: bits 1-0 too for a word, which the
     * Archimedes' MEMC decodes in its CAM writes; a word read returns the
     * aligned word, the core rotates it); set *abort for an abort. A
     * write's value is what the bus carries: a byte store repeats its byte
     * four times.
     */
    uint32_t (*read)(void *ctx, uint32_t addr, bool byte, bool *abort);
    void (*write)(void *ctx, uint32_t addr, uint32_t value, bool byte, bool *abort);
    /* An instruction fetch the table doesn't allow: the word, or *abort
     * for a prefetch abort (NULL: read)
     */
    uint32_t (*fetch)(void *ctx, uint32_t addr, bool *abort);
    /* A coprocessor instruction (CDP, LDC, STC, MCR, MRC): true if a
     * coprocessor took it, false for the undefined instruction trap
     */
    bool (*coprocessor)(void *ctx, arm2_t *cpu, uint32_t insn);
    void *ctx;
} arm2_bus_t;

struct arm2 {
    uint32_t r[16];                 /* the current mode's registers; r[15] unused (see pc) */
    uint32_t pc;                    /* the next instruction's address (bits 25-2): between runs
                                       and for the debugger (arm2_run() keeps it in a register) */
    uint32_t psr;                   /* N Z C V I F and the mode, in R15's bits */
    uint16_t cond_mask;             /* bit c: condition c passes with the flags now (arm2_run()
                                       keeps it; changes to psr between runs are seen) */
    uint32_t usr[7];                /* R8-R14 of the user mode (and IRQ/SVC's R8-R12) */
    uint32_t fiq[7];                /* R8-R14 of FIQ mode */
    uint32_t irq[2], svc[2];        /* R13-R14 */

    bool swp;                       /* the ARM250's and ARM3's SWP (else undefined) */

    /* Interrupt inputs (levels) */
    bool irq_line, fiq_line;
    bool pending;                   /* an interrupt to take (arm2_update_interrupts()) */

    /* Memory */
    const uintptr_t *page;          /* ARM2_PAGES entries */
    arm2_bus_t bus;
    uint8_t slow_s, slow_n;         /* ticks of slow pages' sequential, non-sequential cycles */
    uintptr_t rd_bit, wr_bit;       /* the table's bits for this mode's reads and writes */
    bool watch_hit;                 /* a write to an ARM2_WATCH page (the machine clears it) */

    /* Time */
    int32_t left;                   /* ticks left in this arm2_run(): up to date in the bus
                                       callbacks, which may lower it (wait states) */
    int32_t run_start;              /* `left` when this run began */
    uint64_t ticks;                 /* ticks since power-on, up to this run (the machine may add
                                       ticks between runs: time the CPU didn't get) */
    uint32_t insns;                 /* instructions run (statistics) */

    /* The page the PC is in (refetched when it leaves it, or the table
     * changes: arm2_flush())
     */
    uint32_t code_page;             /* PC >> 12, or ~0 */
    const uint8_t *code;            /* host address of that page less its own (+ PC: the word) */
    uint8_t code_s;                 /* its sequential cycle's ticks */

    /* The debugger (emudbg.h): before an instruction whose address has its
     * bit set in dbg_map, arm2_run() calls emudbg_hit() with dbg (NULL:
     * never). Set by emudbg_arch_arm2; arm2_init() clears them.
     */
    const uint8_t *volatile dbg_map;
    void *dbg;
};

void arm2_init(arm2_t *cpu, const arm2_bus_t *bus, const uintptr_t *page);

/* RESET: SVC mode, IRQ and FIQ disabled, PC 0 */
void arm2_reset(arm2_t *cpu);

/* Run until `ticks` more are used up (the last instruction may overrun:
 * `ticks` counts what ran); bus callbacks may end it early with
 * arm2_end_run(). The ticks actually run.
 */
int32_t arm2_run(arm2_t *cpu, int32_t ticks);

/* Ticks since power-on, inside a run too (for the bus callbacks) */
static inline uint64_t arm2_now(const arm2_t *cpu)
{
    return cpu->ticks + (uint64_t)(int64_t)(cpu->run_start - cpu->left);
}

/* From bus callbacks during arm2_run(): stop after this instruction (the
 * run then counts the ticks it used, and arm2_now() goes on from there)
 */
void arm2_end_run(arm2_t *cpu);

/* The interrupt lines changed */
void arm2_set_irq(arm2_t *cpu, bool level);
void arm2_set_fiq(arm2_t *cpu, bool level);

/* The page table changed (forget the PC's page) */
static inline void arm2_flush(arm2_t *cpu)
{
    cpu->code_page = ~0u;
}

/* R15 as the program sees it: the PC and the PSR */
static inline uint32_t arm2_r15(const arm2_t *cpu)
{
    return (cpu->pc & ARM2_PC) | cpu->psr;
}

/* A register of a mode (0-15: R0-R15, the banked ones of that mode) */
uint32_t arm2_get_reg(const arm2_t *cpu, int mode, int reg);
void arm2_set_reg(arm2_t *cpu, int mode, int reg, uint32_t v);

/* Change the PSR from outside (the mode's registers swap) */
void arm2_set_psr(arm2_t *cpu, uint32_t psr);

/* Memory as the CPU in a privileged mode sees it, without side effects
 * (what the table doesn't map reads 0)
 */
uint32_t arm2_peek32(const arm2_t *cpu, uint32_t addr);
uint8_t arm2_peek8(const arm2_t *cpu, uint32_t addr);

/* The instruction at addr as text ("ADDS R0,R1,#&10"; upper case, & hex as
 * RISC OS writes it) through arm2_peek32()
 */
void arm2_disassemble(const arm2_t *cpu, uint32_t addr, char *buf, int len);
void arm2_disassemble_word(uint32_t insn, uint32_t addr, char *buf, int len);
