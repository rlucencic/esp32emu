/*
 * The 6809 / 6309 for the debugger (emudbg.h): registers (the 6309's too),
 * memory through the page tables, the disassembler, calls and returns.
 */
#include "m6809.h"
#include "emudbg.h"

static const emudbg_reg_t s_regs[] = {
    { "PC", 16, EMUDBG_REG_PC, NULL },
    { "A", 8, 0, NULL },
    { "B", 8, 0, NULL },
    { "X", 16, 0, NULL },
    { "Y", 16, 0, NULL },
    { "U", 16, 0, NULL },
    { "S", 16, EMUDBG_REG_SP, NULL },
    { "DP", 8, 0, NULL },
    { "CC", 8, EMUDBG_REG_FLAGS, "EFHINZVC" },
    { "cycles", 32, EMUDBG_REG_RO, NULL },
    /* the 6309's */
    { "E", 8, 0, NULL },
    { "F", 8, 0, NULL },
    { "V", 16, 0, NULL },
    { "MD", 8, EMUDBG_REG_FLAGS, "DI----FN" },
};

enum { R_PC, R_A, R_B, R_X, R_Y, R_U, R_S, R_DP, R_CC, R_CYCLES, R_E, R_F, R_V, R_MD };

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    const m6809_t *c = cpu->core;
    switch (reg) {
    case R_PC: return c->pc;
    case R_A: return c->a;
    case R_B: return c->b;
    case R_X: return c->x;
    case R_Y: return c->y;
    case R_U: return c->u;
    case R_S: return c->s;
    case R_DP: return c->dp;
    case R_CC: return c->cc;
    case R_E: return c->e;
    case R_F: return c->f;
    case R_V: return c->v;
    case R_MD: return c->md;
    default: return (uint32_t)c->cycles;
    }
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    m6809_t *c = cpu->core;
    switch (reg) {
    case R_PC: c->pc = (uint16_t)v; break;
    case R_A: c->a = (uint8_t)v; break;
    case R_B: c->b = (uint8_t)v; break;
    case R_X: c->x = (uint16_t)v; break;
    case R_Y: c->y = (uint16_t)v; break;
    case R_U: c->u = (uint16_t)v; break;
    case R_S: c->s = (uint16_t)v; break;
    case R_DP: c->dp = (uint8_t)v; break;
    case R_CC: c->cc = (uint8_t)v; break;
    case R_E: c->e = (uint8_t)v; break;
    case R_F: c->f = (uint8_t)v; break;
    case R_V: c->v = (uint16_t)v; break;
    case R_MD: c->md = (uint8_t)v; break;
    default: break;
    }
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    return m6809_peek(cpu->core, (uint16_t)addr);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    m6809_t *c = cpu->core;
    uint8_t *page = c->wr[(addr >> 8) & 0xFF];
    if (page) {
        page[addr & 0xFF] = v;
    }
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    return m6809_disassemble(cpu->core, (uint16_t)addr, buf, len);
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    const m6809_t *c = cpu->core;
    uint8_t op = m6809_peek(c, (uint16_t)addr);
    switch (op) {
    case 0x17: case 0x8D: case 0x9D: case 0xAD: case 0xBD: case 0x3F:   /* LBSR BSR JSR SWI */
        return EMUDBG_INSN_CALL;
    case 0x39: case 0x3B:                                               /* RTS RTI */
        return EMUDBG_INSN_RETURN;
    case 0x35: case 0x37:                                               /* PULS/PULU with PC */
        return (m6809_peek(c, (uint16_t)(addr + 1)) & 0x80) ? EMUDBG_INSN_RETURN : EMUDBG_INSN_OTHER;
    case 0x10: case 0x11:                                               /* SWI2 (OS-9's calls), SWI3 */
        return m6809_peek(c, (uint16_t)(addr + 1)) == 0x3F ? EMUDBG_INSN_CALL : EMUDBG_INSN_OTHER;
    default:
        return EMUDBG_INSN_OTHER;
    }
}

static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    const m6809_t *c = cpu->core;
    static const uint16_t vectors[] = { 0xFFFC, 0xFFF6, 0xFFF8 };   /* NMI FIRQ IRQ */
    int n = 0;
    for (int i = 0; i < 3 && n < max; i++) {
        entries[n++] = (uint32_t)(m6809_peek(c, vectors[i]) << 8 | m6809_peek(c, (uint16_t)(vectors[i] + 1)));
    }
    return n;
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    m6809_t *c = cpu->core;
    c->dbg = cpu;
    c->dbg_map = map;
}

const emudbg_arch_t emudbg_arch_6809 = {
    .arch = "6809",
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
