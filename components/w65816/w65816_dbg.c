/*
 * The 65C816 for the debugger (emudbg.h): registers, memory through the
 * page tables (a machine with memory behind its bus callbacks gives its
 * own peek and poke), the disassembler, calls and returns.
 */
#include "w65816.h"
#include "emudbg.h"

static const emudbg_reg_t s_regs[] = {
    { "PC", 24, EMUDBG_REG_PC, NULL },          /* PBR:PC */
    { "A", 16, 0, NULL },
    { "X", 16, 0, NULL },
    { "Y", 16, 0, NULL },
    { "S", 16, EMUDBG_REG_SP, NULL },
    { "D", 16, 0, NULL },
    { "DBR", 8, 0, NULL },
    { "PBR", 8, 0, NULL },
    { "P", 8, EMUDBG_REG_FLAGS, "NVMXDIZC" },
    { "E", 1, 0, NULL },
    { "cycles", 32, EMUDBG_REG_RO, NULL },
};

enum { R_PC, R_A, R_X, R_Y, R_S, R_D, R_DBR, R_PBR, R_P, R_E, R_CYCLES };

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    const w65816_t *c = cpu->core;
    switch (reg) {
    case R_PC: return (uint32_t)c->pbr << 16 | c->pc;
    case R_A: return c->a;
    case R_X: return c->x;
    case R_Y: return c->y;
    case R_S: return c->s;
    case R_D: return c->d;
    case R_DBR: return c->dbr;
    case R_PBR: return c->pbr;
    case R_P: return c->p;
    case R_E: return c->e;
    default: return (uint32_t)c->cycles;
    }
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    w65816_t *c = cpu->core;
    switch (reg) {
    case R_PC: c->pbr = (uint8_t)(v >> 16); c->pc = (uint16_t)v; break;
    case R_A: c->a = (uint16_t)v; break;
    case R_X: c->x = (uint16_t)v; break;
    case R_Y: c->y = (uint16_t)v; break;
    case R_S: c->s = (uint16_t)v; break;
    case R_D: c->d = (uint16_t)v; break;
    case R_DBR: c->dbr = (uint8_t)v; break;
    case R_PBR: c->pbr = (uint8_t)v; break;
    case R_P: c->p = (uint8_t)v; break;         /* the run's load() keeps M and X in emulation mode */
    case R_E: c->e = v & 1; break;
    default: break;
    }
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    return w65816_peek(cpu->core, addr);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    w65816_t *c = cpu->core;
    uint8_t *page = c->wr[(addr >> 8) & 0xFFFF];
    if (page) {
        page[addr & 0xFF] = v;
    }
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    return w65816_disassemble(cpu->core, addr & 0xFFFFFF, buf, len);
}

static uint8_t peek_cpu(emudbg_cpu_t *cpu, uint32_t addr)
{
    return cpu->peek ? cpu->peek(cpu->ctx, addr & 0xFFFFFF) : peek(cpu, addr);
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    switch (peek_cpu(cpu, addr)) {
    case 0x20: case 0x22: case 0xFC: return EMUDBG_INSN_CALL;              /* JSR JSL JSR (a,X) */
    case 0x60: case 0x6B: case 0x40: return EMUDBG_INSN_RETURN;            /* RTS RTL RTI */
    default: return EMUDBG_INSN_OTHER;
    }
}

static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    const w65816_t *c = cpu->core;
    uint32_t bank = (uint32_t)c->vector_bank << 16;
    /* NMI and IRQ, in emulation mode and native; handlers are in bank 0 */
    static const uint16_t vectors[] = { 0xFFFA, 0xFFFE, 0xFFEA, 0xFFEE };
    int n = 0;
    for (int k = 0; k < 4 && n < max; k++) {
        uint32_t v = bank | vectors[k];
        entries[n++] = peek_cpu(cpu, v) | peek_cpu(cpu, v + 1) << 8;
    }
    return n;
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    w65816_t *c = cpu->core;
    c->dbg = cpu;
    c->dbg_map = map;
}

const emudbg_arch_t emudbg_arch_65c816 = {
    .arch = "65c816",
    .addr_bits = 24,
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
