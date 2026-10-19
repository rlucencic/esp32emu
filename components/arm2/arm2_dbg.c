/*
 * The ARM2 for the debugger (emudbg.h): registers (with the user, SVC and
 * IRQ modes' R13 and R14), memory through the page table, the
 * disassembler, calls and returns.
 */
#include "arm2.h"
#include "emudbg.h"

static const emudbg_reg_t s_regs[] = {
    { "R0", 32, 0, NULL }, { "R1", 32, 0, NULL }, { "R2", 32, 0, NULL }, { "R3", 32, 0, NULL },
    { "R4", 32, 0, NULL }, { "R5", 32, 0, NULL }, { "R6", 32, 0, NULL }, { "R7", 32, 0, NULL },
    { "R8", 32, 0, NULL }, { "R9", 32, 0, NULL }, { "R10", 32, 0, NULL }, { "R11", 32, 0, NULL },
    { "R12", 32, 0, NULL },
    { "R13", 32, EMUDBG_REG_SP, NULL },
    { "R14", 32, 0, NULL },
    { "PC", 26, EMUDBG_REG_PC, NULL },
    { "PSR", 6, EMUDBG_REG_FLAGS, "NZCVIF" },
    { "Mode", 2, 0, NULL },                 /* 0 USR, 1 FIQ, 2 IRQ, 3 SVC */
    { "R13_usr", 32, 0, NULL }, { "R14_usr", 32, 0, NULL },
    { "R13_svc", 32, 0, NULL }, { "R14_svc", 32, 0, NULL },
    { "R13_irq", 32, 0, NULL }, { "R14_irq", 32, 0, NULL },
    { "ticks", 32, EMUDBG_REG_RO, NULL },
};

enum { R_PC = 15, R_PSR, R_MODE, R_USR13, R_USR14, R_SVC13, R_SVC14, R_IRQ13, R_IRQ14, R_TICKS };

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    const arm2_t *c = cpu->core;
    switch (reg) {
    case R_PC: return c->pc;
    case R_PSR: return c->psr >> 26;
    case R_MODE: return c->psr & ARM2_MODE;
    case R_USR13: case R_USR14: return arm2_get_reg(c, ARM2_USR, 13 + reg - R_USR13);
    case R_SVC13: case R_SVC14: return arm2_get_reg(c, ARM2_SVC, 13 + reg - R_SVC13);
    case R_IRQ13: case R_IRQ14: return arm2_get_reg(c, ARM2_IRQ, 13 + reg - R_IRQ13);
    case R_TICKS: return (uint32_t)c->ticks;
    default: return c->r[reg];
    }
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    arm2_t *c = cpu->core;
    switch (reg) {
    case R_PC: c->pc = v & ARM2_PC; arm2_flush(c); break;
    case R_PSR: arm2_set_psr(c, (c->psr & ARM2_MODE) | ((v & 0x3F) << 26)); break;
    case R_MODE: arm2_set_psr(c, (c->psr & ~ARM2_MODE) | (v & ARM2_MODE)); break;
    case R_USR13: case R_USR14: arm2_set_reg(c, ARM2_USR, 13 + reg - R_USR13, v); break;
    case R_SVC13: case R_SVC14: arm2_set_reg(c, ARM2_SVC, 13 + reg - R_SVC13, v); break;
    case R_IRQ13: case R_IRQ14: arm2_set_reg(c, ARM2_IRQ, 13 + reg - R_IRQ13, v); break;
    case R_TICKS: break;
    default: c->r[reg] = v; break;
    }
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    return arm2_peek8(cpu->core, addr);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    const arm2_t *c = cpu->core;
    addr &= ARM2_ADDR_MASK;
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    if (e & ARM2_PRIV_W) {
        ((uint8_t *)(e & ~(uintptr_t)0xFFF))[addr & 0xFFF] = v;
    }
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    arm2_disassemble(cpu->core, addr, buf, len);
    return 4;
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    uint32_t op = arm2_peek32(cpu->core, addr);
    switch ((op >> 25) & 7) {
    case 0:
    case 1:
        /* Writing the PC from R14: MOV PC,R14, MOVS PC,R14, SUBS PC,R14,#4 */
        if (((op >> 12) & 15) == 15 && !((op & 0x0E000090) == 0x00000090)) {
            unsigned opc = (op >> 21) & 15;
            bool from_r14 = (opc == 13 || opc == 15) ? !(op & (1u << 25)) && (op & 15) == 14
                                                     : ((op >> 16) & 15) == 14;
            if (from_r14 && !(opc >= 8 && opc <= 11)) {
                return EMUDBG_INSN_RETURN;
            }
        }
        return EMUDBG_INSN_OTHER;
    case 2:
    case 3:
        /* LDR PC,[R13],#4 */
        return (op & 0x0010F000) == 0x0010F000 && ((op >> 16) & 15) == 13 ? EMUDBG_INSN_RETURN : EMUDBG_INSN_OTHER;
    case 4:
        /* LDM with the PC */
        return (op & 0x00108000) == 0x00108000 ? EMUDBG_INSN_RETURN : EMUDBG_INSN_OTHER;
    case 5:
        return (op & (1u << 24)) ? EMUDBG_INSN_CALL : EMUDBG_INSN_OTHER;     /* BL */
    default:
        /* SWI, and the coprocessor instructions the floating point
         * emulator runs from the undefined instruction vector
         */
        return EMUDBG_INSN_CALL;
    }
}

static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    (void)cpu;
    int n = 0;
    if (n < max) {
        entries[n++] = ARM2_VEC_IRQ;
    }
    if (n < max) {
        entries[n++] = ARM2_VEC_FIQ;
    }
    return n;
}

/* The privileged modes' stacks count as deeper than the user mode's: a
 * SWI from user mode returns there
 */
static uint32_t stack(emudbg_cpu_t *cpu)
{
    const arm2_t *c = cpu->core;
    return ((c->psr & ARM2_MODE) == ARM2_USR ? 0x80000000u : 0) | (c->r[13] & ARM2_ADDR_MASK);
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    arm2_t *c = cpu->core;
    c->dbg = cpu;
    c->dbg_map = map;
}

const emudbg_arch_t emudbg_arch_arm2 = {
    .arch = "arm2",
    .addr_bits = 26,
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
