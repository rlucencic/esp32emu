/*
 * The x86 for the debugger (emudbg.h): addresses are linear (CS:IP as the
 * CPU forms it, 24 bits), so breakpoints, the memory view and the
 * disassembly work across segments. Registers: the eight general ones,
 * the segment registers, IP, FLAGS, the 286's MSW; PC (CS:IP, linear: the
 * address the debugger stops at, written as IP in the current CS) and SSP
 * (SS:SP, linear: the stack view's) worked out from them; the clocks; the
 * coprocessor's control, status and tag words and its stack registers
 * ST0-ST7 as floats (empty without a coprocessor).
 */
#include <string.h>
#include "x86.h"
#include "x87.h"
#include "emudbg.h"

static const emudbg_reg_t s_regs[] = {
    { "AX", 16, 0, NULL }, { "BX", 16, 0, NULL }, { "CX", 16, 0, NULL }, { "DX", 16, 0, NULL },
    { "SI", 16, 0, NULL }, { "DI", 16, 0, NULL }, { "BP", 16, 0, NULL }, { "SP", 16, 0, NULL },
    { "CS", 16, 0, NULL }, { "DS", 16, 0, NULL }, { "ES", 16, 0, NULL }, { "SS", 16, 0, NULL },
    { "IP", 16, 0, NULL },
    { "FLAGS", 16, EMUDBG_REG_FLAGS, "-N--ODITSZ-A-P-C" },
    { "MSW", 16, 0, NULL },
    { "PC", 24, EMUDBG_REG_PC, NULL },
    { "SSP", 24, EMUDBG_REG_SP | EMUDBG_REG_RO, NULL },
    { "clocks", 32, EMUDBG_REG_RO, NULL },
    { "FCW", 16, 0, NULL }, { "FSW", 16, 0, NULL }, { "FTW", 16, 0, NULL },
    { "ST0", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL }, { "ST1", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL },
    { "ST2", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL }, { "ST3", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL },
    { "ST4", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL }, { "ST5", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL },
    { "ST6", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL }, { "ST7", 32, EMUDBG_REG_FLOAT | EMUDBG_REG_RO, NULL },
};

enum { R_AX, R_BX, R_CX, R_DX, R_SI, R_DI, R_BP, R_SP, R_CS, R_DS, R_ES, R_SS, R_IP, R_FLAGS, R_MSW, R_PC,
       R_SSP, R_CLOCKS, R_FCW, R_FSW, R_FTW, R_ST0 };

/* Our order against the CPU's (AX CX DX BX SP BP SI DI, ES CS SS DS) */
static const uint8_t s_gpr[8] = { X86_AX, X86_BX, X86_CX, X86_DX, X86_SI, X86_DI, X86_BP, X86_SP };
static const uint8_t s_seg[4] = { X86_CS, X86_DS, X86_ES, X86_SS };

static uint32_t linear(const x86_t *c, int seg, uint16_t off)
{
    return (c->seg[seg].base + off) & c->addr_mask;
}

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    const x86_t *c = cpu->core;
    if (reg <= R_SP) {
        return c->r.w[s_gpr[reg]];
    }
    if (reg <= R_SS) {
        return c->seg[s_seg[reg - R_CS]].sel;
    }
    switch (reg) {
    case R_IP: return c->ip;
    case R_FLAGS: return x86_get_flags(c);
    case R_MSW: return c->msw;
    case R_PC: return linear(c, X86_CS, c->ip);
    case R_SSP: return linear(c, X86_SS, c->r.w[X86_SP]);
    case R_CLOCKS: return (uint32_t)(c->cycles + (uint64_t)(int64_t)c->clk);
    case R_FCW: return c->has_fpu ? c->fpu.cw : 0;
    case R_FSW: return c->has_fpu ? c->fpu.sw : 0;
    case R_FTW: return c->has_fpu ? c->fpu.tw : 0xFFFF;
    default: {
        /* ST(i): the physical register TOP + i */
        int p = (((c->fpu.sw >> 11) & 7) + reg - R_ST0) & 7;
        if (!c->has_fpu || ((c->fpu.tw >> (2 * p)) & 3) == 3) {
            return EMUDBG_FLOAT_EMPTY;
        }
        float v = (float)x87_to_double(c->fpu.st[p]);
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        return bits;
    }
    }
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    x86_t *c = cpu->core;
    if (reg <= R_SP) {
        c->r.w[s_gpr[reg]] = (uint16_t)v;
    } else if (reg <= R_SS) {
        x86_load_seg(c, s_seg[reg - R_CS], (uint16_t)v);
    } else if (reg == R_IP) {
        c->ip = (uint16_t)v;
    } else if (reg == R_FLAGS) {
        x86_set_flags(c, (uint16_t)v);
    } else if (reg == R_MSW) {
        c->msw = (uint16_t)((c->msw & X86_MSW_PE) | (v & 0xFFFE));     /* PE can't be cleared */
    } else if (reg == R_PC) {
        c->ip = (uint16_t)(v - c->seg[X86_CS].base);                    /* in the current CS */
    } else if (reg == R_FCW && c->has_fpu) {
        c->fpu.cw = (uint16_t)v;
    } else if (reg == R_FSW && c->has_fpu) {
        c->fpu.sw = (uint16_t)v;
    } else if (reg == R_FTW && c->has_fpu) {
        c->fpu.tw = (uint16_t)v;
    }
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    return x86_peek8(cpu->core, addr);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    const x86_t *c = cpu->core;
    addr &= c->addr_mask;
    uintptr_t e = c->page[addr >> X86_PAGE_SHIFT];
    if (!(e & X86_NO_WRITE)) {
        ((uint8_t *)(e & ~(uintptr_t)0xFFF))[addr & 0xFFF] = v;
    }
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    const x86_t *c = cpu->core;
    uint8_t bytes[16];
    for (int i = 0; i < 16; i++) {
        bytes[i] = x86_peek8(c, addr + (uint32_t)i);
    }
    /* jump targets as offsets in the current code segment */
    return x86_disassemble_bytes(c->model, bytes, (uint16_t)(addr - c->seg[X86_CS].base), buf, len);
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    const x86_t *c = cpu->core;
    uint8_t op = x86_peek8(c, addr);
    for (int i = 0; i < 8 && ((op & 0xE7) == 0x26 || (op & 0xFC) == 0xF0); i++) {
        op = x86_peek8(c, ++addr);  /* past the prefixes */
    }
    switch (op) {
    case 0xE8: case 0x9A:                           /* CALL near, far */
    case 0xCC: case 0xCD: case 0xCE:                /* INT 3, INT n, INTO */
        return EMUDBG_INSN_CALL;
    case 0xFF: {
        uint8_t reg = (x86_peek8(c, addr + 1) >> 3) & 7;
        return reg == 2 || reg == 3 ? EMUDBG_INSN_CALL : EMUDBG_INSN_OTHER;
    }
    case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCF:  /* RET, RETF, IRET */
        return EMUDBG_INSN_RETURN;
    default:
        return EMUDBG_INSN_OTHER;
    }
}

/* The hardware interrupts' handlers: IRQ 0-7 (vectors 08h-0Fh) and on the
 * AT 8-15 (70h-77h), from the vector table
 */
static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    const x86_t *c = cpu->core;
    int n = 0;
    for (int i = 0; i < 16 && n < max; i++) {
        if (i >= 8 && c->model != X86_80286) {
            break;
        }
        uint32_t v = c->idtr.base + (uint32_t)(i < 8 ? 0x08 + i : 0x70 + i - 8) * 4;
        uint16_t off = (uint16_t)(x86_peek8(c, v) | x86_peek8(c, v + 1) << 8);
        uint16_t seg = (uint16_t)(x86_peek8(c, v + 2) | x86_peek8(c, v + 3) << 8);
        entries[n++] = (((uint32_t)seg << 4) + off) & c->addr_mask;
    }
    return n;
}

static uint32_t stack(emudbg_cpu_t *cpu)
{
    const x86_t *c = cpu->core;
    return linear(c, X86_SS, c->r.w[X86_SP]);
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    x86_t *c = cpu->core;
    c->dbg = cpu;
    c->dbg_map = map;
}

const emudbg_arch_t emudbg_arch_x86 = {
    .arch = "x86",
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
    .stack = stack,
    .set_map = set_map,
};
