/*
 * Z80 core, shared by the Z80 machines (../esp32cpc, ../esp32zx).
 *
 * Instruction-stepped, with all documented and undocumented behaviour that
 * matters (flags bits 3 and 5, MEMPTR for BIT n,(HL), IXH/IXL, SLL, the
 * DDCB forms that also copy to a register, block I/O flags); passes zexdoc
 * and zexall.
 *
 * Instruction times (menuconfig "Z80 core", or CONFIG_Z80_* on a host
 * build):
 *   - T-states (the default; ZX Spectrum), optionally with the Spectrum's
 *     memory contention (CONFIG_Z80_CONTENTION): each memory access to a
 *     contended 16 KB bank waits z80_contention[time of the access]
 *   - CPC microseconds (CONFIG_Z80_TIMING_CPC): the Gate Array stretches
 *     every memory and I/O access to a 1 us grid, so each instruction takes
 *     a whole number of them
 *
 * Memory goes through 16 KB banks for reading and writing (z80_rd, z80_wr),
 * I/O through the machine's functions (z80_set_bus()), which get the time
 * inside the instruction at which the access happens.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_attr.h"
#define Z80_HOT     IRAM_ATTR
#else
#define Z80_HOT
#endif

#if defined(CONFIG_Z80_CONTENTION) && !defined(CONFIG_Z80_TIMING_CPC)
#define Z80_CONTENTION 1
#else
#define Z80_CONTENTION 0
#endif

typedef union {
    uint16_t w;
    struct {
        uint8_t l, h;       /* little-endian hosts (Xtensa, x86, ARM) */
    } b;
} z80_pair_t;

typedef struct {
    z80_pair_t af, bc, de, hl, ix, iy, sp, pc;
    z80_pair_t wz;                      /* MEMPTR */
    uint16_t af2, bc2, de2, hl2;
    uint8_t i, r;                       /* r: bit 7 as written, 0-6 counting */
    uint8_t iff1, iff2, im;
    uint8_t halted;
    uint8_t ei_delay;                   /* EI just executed: no interrupt yet */
    volatile uint8_t int_line;          /* /INT asserted */
    /* Contention: the machine's clock at the start of the instruction (an
     * index into z80_contention), and wait states its I/O adds to it
     */
    int32_t t;
    int wait;
} z80_t;

extern z80_t z80;

/* ---- The machine's side ---- */

/* 16 KB banks as the CPU sees them; a NULL write bank goes to the bus's
 * write_special()
 */
extern uint8_t *z80_rd[4];
extern uint8_t *z80_wr[4];

typedef struct {
    /* I/O at time `at` of the current instruction (us or T-states) */
    uint8_t (*in)(uint16_t port, int at);
    void (*out)(uint16_t port, uint8_t v, int at);
    /* Interrupt acknowledge: the byte on the data bus (the IM 2 vector) */
    uint8_t (*int_ack)(void);
    void (*write_special)(uint16_t addr, uint8_t v);    /* may be NULL */
} z80_bus_t;

void z80_set_bus(const z80_bus_t *bus);

#if Z80_CONTENTION
/* Wait states by machine time (NULL: none), and which banks wait */
extern const uint8_t *z80_contention;
extern uint8_t z80_contended[4];
#endif

/* Memory as the CPU sees it, without side effects */
static inline uint8_t z80_peek(uint16_t a)
{
    return z80_rd[a >> 14][a & 0x3FFF];
}

/* ---- Running ---- */

void z80_reset(void);

/* Execute one instruction (or accept an interrupt); its time */
int z80_step(void);

/* Disassemble the instruction at pc into buf; its length */
int z80_disassemble(uint16_t pc, char *buf, int len);
