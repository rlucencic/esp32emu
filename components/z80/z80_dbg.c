/*
 * The Z80 for the debugger (emudbg.h): registers, memory through the
 * banks, the disassembler, calls and returns. One Z80 (the core's state
 * is global).
 */
#include "z80.h"
#include "emudbg.h"

static emudbg_cpu_t *s_cpu;

static const emudbg_reg_t s_regs[] = {
    { "PC", 16, EMUDBG_REG_PC, NULL },
    { "SP", 16, EMUDBG_REG_SP, NULL },
    { "AF", 16, 0, NULL },
    { "BC", 16, 0, NULL },
    { "DE", 16, 0, NULL },
    { "HL", 16, 0, NULL },
    { "IX", 16, 0, NULL },
    { "IY", 16, 0, NULL },
    { "F", 8, EMUDBG_REG_FLAGS, "SZ5H3PNC" },
    { "AF'", 16, 0, NULL },
    { "BC'", 16, 0, NULL },
    { "DE'", 16, 0, NULL },
    { "HL'", 16, 0, NULL },
    { "I", 8, 0, NULL },
    { "R", 8, 0, NULL },
    { "IM", 2, 0, NULL },
    { "IFF1", 1, 0, NULL },
    { "IFF2", 1, 0, NULL },
};

enum { R_PC, R_SP, R_AF, R_BC, R_DE, R_HL, R_IX, R_IY, R_F, R_AF2, R_BC2, R_DE2, R_HL2,
       R_I, R_R, R_IM, R_IFF1, R_IFF2 };

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    (void)cpu;
    switch (reg) {
    case R_PC: return z80.pc.w;
    case R_SP: return z80.sp.w;
    case R_AF: return z80.af.w;
    case R_BC: return z80.bc.w;
    case R_DE: return z80.de.w;
    case R_HL: return z80.hl.w;
    case R_IX: return z80.ix.w;
    case R_IY: return z80.iy.w;
    case R_F: return z80.af.b.l;
    case R_AF2: return z80.af2;
    case R_BC2: return z80.bc2;
    case R_DE2: return z80.de2;
    case R_HL2: return z80.hl2;
    case R_I: return z80.i;
    case R_R: return z80.r;
    case R_IM: return z80.im;
    case R_IFF1: return z80.iff1;
    default: return z80.iff2;
    }
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    (void)cpu;
    uint16_t w = (uint16_t)v;
    switch (reg) {
    case R_PC: z80.pc.w = w; break;
    case R_SP: z80.sp.w = w; break;
    case R_AF: z80.af.w = w; break;
    case R_BC: z80.bc.w = w; break;
    case R_DE: z80.de.w = w; break;
    case R_HL: z80.hl.w = w; break;
    case R_IX: z80.ix.w = w; break;
    case R_IY: z80.iy.w = w; break;
    case R_F: z80.af.b.l = (uint8_t)v; break;
    case R_AF2: z80.af2 = w; break;
    case R_BC2: z80.bc2 = w; break;
    case R_DE2: z80.de2 = w; break;
    case R_HL2: z80.hl2 = w; break;
    case R_I: z80.i = (uint8_t)v; break;
    case R_R: z80.r = (uint8_t)v; break;
    case R_IM: z80.im = (uint8_t)(v > 2 ? 2 : v); break;
    case R_IFF1: z80.iff1 = v & 1; break;
    default: z80.iff2 = v & 1; break;
    }
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    (void)cpu;
    return z80_peek((uint16_t)addr);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    (void)cpu;
    uint8_t *bank = z80_wr[(addr & 0xFFFF) >> Z80_BANK_SHIFT];
    if (bank) {
        bank[addr & Z80_BANK_MASK] = v;
    }
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    (void)cpu;
    return z80_disassemble((uint16_t)addr, buf, len);
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    (void)cpu;
    uint8_t op = z80_peek((uint16_t)addr);
    if (op == 0xCD || (op & 0xC7) == 0xC4 || (op & 0xC7) == 0xC7) {
        return EMUDBG_INSN_CALL;                            /* CALL, CALL cc, RST */
    }
    if (op == 0xC9 || (op & 0xC7) == 0xC0) {
        return EMUDBG_INSN_RETURN;                          /* RET, RET cc */
    }
    if (op == 0xED && (z80_peek((uint16_t)(addr + 1)) & 0xC7) == 0x45) {
        return EMUDBG_INSN_RETURN;                          /* RETN, RETI */
    }
    return EMUDBG_INSN_OTHER;
}

static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    (void)cpu;
    if (max < 3) {
        return 0;
    }
    entries[0] = 0x0066;                                    /* NMI */
    entries[1] = 0x0038;                                    /* IM 1 */
    /* IM 2: the vector the bus gives isn't known here; $FF is the usual */
    uint16_t v = (uint16_t)(z80.i << 8 | 0xFF);
    entries[2] = z80_peek(v) | z80_peek((uint16_t)(v + 1)) << 8;
    return z80.im == 2 ? 3 : 2;
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    s_cpu = cpu;
    z80_dbg_map = map;
}

void z80_dbg_hit(void)
{
    emudbg_hit(s_cpu, z80.pc.w);
}

const emudbg_arch_t emudbg_arch_z80 = {
    .arch = "z80",
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
