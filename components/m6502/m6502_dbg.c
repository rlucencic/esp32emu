/*
 * The 6502 for the debugger (emudbg.h): registers, memory through the
 * page tables, the disassembler, calls and returns.
 */
#include "m6502.h"
#include "emudbg.h"

static const emudbg_reg_t s_regs[] = {
    { "PC", 16, EMUDBG_REG_PC, NULL },
    { "A", 8, 0, NULL },
    { "X", 8, 0, NULL },
    { "Y", 8, 0, NULL },
    { "S", 8, EMUDBG_REG_SP, NULL },
    { "P", 8, EMUDBG_REG_FLAGS, "NV-BDIZC" },
    { "cycles", 32, EMUDBG_REG_RO, NULL },
};

enum { R_PC, R_A, R_X, R_Y, R_S, R_P, R_CYCLES };

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    const m6502_t *c = cpu->core;
    switch (reg) {
    case R_PC: return c->pc;
    case R_A: return c->a;
    case R_X: return c->x;
    case R_Y: return c->y;
    case R_S: return c->s;
    case R_P: return c->p;
    default: return (uint32_t)c->cycles;
    }
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    m6502_t *c = cpu->core;
    switch (reg) {
    case R_PC: c->pc = (uint16_t)v; break;
    case R_A: c->a = (uint8_t)v; break;
    case R_X: c->x = (uint8_t)v; break;
    case R_Y: c->y = (uint8_t)v; break;
    case R_S: c->s = (uint8_t)v; break;
    case R_P: c->p = (uint8_t)v | M6502_U; break;
    default: break;
    }
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    return m6502_peek(cpu->core, (uint16_t)addr);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    m6502_t *c = cpu->core;
    uint8_t *page = c->wr[(addr >> 8) & 0xFF];
    if (page) {
        page[addr & 0xFF] = v;
    }
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    return m6502_disassemble(cpu->core, (uint16_t)addr, buf, len);
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    switch (m6502_peek(cpu->core, (uint16_t)addr)) {
    case 0x20: return EMUDBG_INSN_CALL;                     /* JSR */
    case 0x60: case 0x40: return EMUDBG_INSN_RETURN;        /* RTS RTI */
    default: return EMUDBG_INSN_OTHER;
    }
}

static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    const m6502_t *c = cpu->core;
    if (max < 2) {
        return 0;
    }
    entries[0] = m6502_peek(c, 0xFFFA) | m6502_peek(c, 0xFFFB) << 8;  /* NMI */
    entries[1] = m6502_peek(c, 0xFFFE) | m6502_peek(c, 0xFFFF) << 8;  /* IRQ */
    return 2;
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    m6502_t *c = cpu->core;
    c->dbg = cpu;
    c->dbg_map = map;
}

const emudbg_arch_t emudbg_arch_6502 = {
    .arch = "6502",
    .addr_bits = 16,
    .regs = s_regs,
    .reg_count = sizeof(s_regs) / sizeof(s_regs[0]),
    .get_reg = get_reg,
    .set_reg = set_reg,
    .peek = peek,
    .poke = poke,
    .disassemble = disassemble,
    .insn = insn,
    .interrupts = interrupts,
    .set_map = set_map,
};
