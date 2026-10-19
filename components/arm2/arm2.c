/*
 * ARM2 (ARMv2) core: see include/arm2.h.
 */
#include <string.h>
#include "arm2.h"
#include "emudbg.h"

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define INLINE      inline __attribute__((always_inline))

/* What arm2_run() keeps in registers while it runs (the instructions are
 * inlined into it): the next instruction's address and the ticks left.
 * The arm2_t has them (pc, left) only around the bus callbacks, which may
 * look (arm2_now()) and change them (wait states, arm2_end_run()), the
 * debugger and the end of the run.
 *
 * `page`: the page an instruction is fetched from without a look at
 * anything else, the PC's (code_page) unless an interrupt is pending or
 * the debugger is on (then ~0: every instruction takes the slow way).
 * refresh() after whatever may change those: the bus callbacks, PSR
 * writes, exceptions.
 */
typedef struct {
    uint32_t pc;
    int32_t left;
    uint32_t page;
} run_t;

#define NZCV        0xF0000000u
#define IF          (ARM2_I | ARM2_F)

/* Bit c of s_pass[f]: condition c passes with N Z C V = f */
static uint16_t s_pass[16];

static inline void flags_changed(arm2_t *c)
{
    c->cond_mask = s_pass[c->psr >> 28];
}

static inline uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* ---- Modes and the PSR ---- */

/* Bank the registers of mode `from` and bring in those of `to` */
static void switch_mode(arm2_t *c, uint32_t from, uint32_t to)
{
    switch (from) {
    case ARM2_FIQ:
        memcpy(c->fiq, &c->r[8], 7 * sizeof(uint32_t));
        break;
    case ARM2_IRQ:
        memcpy(c->usr, &c->r[8], 5 * sizeof(uint32_t));
        c->irq[0] = c->r[13];
        c->irq[1] = c->r[14];
        break;
    case ARM2_SVC:
        memcpy(c->usr, &c->r[8], 5 * sizeof(uint32_t));
        c->svc[0] = c->r[13];
        c->svc[1] = c->r[14];
        break;
    default:
        memcpy(c->usr, &c->r[8], 7 * sizeof(uint32_t));
        break;
    }
    switch (to) {
    case ARM2_FIQ:
        memcpy(&c->r[8], c->fiq, 7 * sizeof(uint32_t));
        break;
    case ARM2_IRQ:
        memcpy(&c->r[8], c->usr, 5 * sizeof(uint32_t));
        c->r[13] = c->irq[0];
        c->r[14] = c->irq[1];
        break;
    case ARM2_SVC:
        memcpy(&c->r[8], c->usr, 5 * sizeof(uint32_t));
        c->r[13] = c->svc[0];
        c->r[14] = c->svc[1];
        break;
    default:
        memcpy(&c->r[8], c->usr, 7 * sizeof(uint32_t));
        break;
    }
}

static inline void update_pending(arm2_t *c)
{
    c->pending = (c->fiq_line && !(c->psr & ARM2_F)) || (c->irq_line && !(c->psr & ARM2_I));
}

static inline void set_psr(arm2_t *c, uint32_t psr)
{
    uint32_t old = c->psr;
    psr &= ARM2_PSR;
    c->psr = psr;
    if (UNLIKELY((old ^ psr) & ARM2_MODE)) {
        switch_mode(c, old & ARM2_MODE, psr & ARM2_MODE);
        if (!(old & ARM2_MODE) || !(psr & ARM2_MODE)) {
            c->code_page = ~0u;             /* privilege changed: the PC's page again */
            c->rd_bit = (psr & ARM2_MODE) ? ARM2_PRIV_R : ARM2_USER_R;
            c->wr_bit = (psr & ARM2_MODE) ? ARM2_PRIV_W : ARM2_USER_W;
        }
    }
    if ((old ^ psr) & IF) {
        update_pending(c);
    }
    flags_changed(c);
}

/* A write to R15's PSR bits by the program: all of them in a privileged
 * mode, the flags alone in user mode
 */
static inline void write_psr(arm2_t *c, uint32_t v)
{
    if (c->psr & ARM2_MODE) {
        set_psr(c, v);
    } else {
        c->psr = (c->psr & ~NZCV) | (v & NZCV);
        flags_changed(c);
    }
}

static INLINE void refresh(const arm2_t *c, run_t *r)
{
#if EMUDBG_ENABLED
    r->page = (c->pending || c->dbg_map) ? ~0u : c->code_page;
#else
    r->page = c->pending ? ~0u : c->code_page;
#endif
}

/* write_psr() inside a run */
static INLINE void run_write_psr(arm2_t *c, run_t *r, uint32_t v)
{
    write_psr(c, v);
    refresh(c, r);
}

/* An exception's mode: out of line (inlined, the exceptions every
 * transfer may take spread its code through the instructions' own)
 */
static void ARM2_HOT __attribute__((noinline)) enter_mode(arm2_t *c, uint32_t mode, uint32_t r14)
{
    uint32_t psr = (c->psr & ~ARM2_MODE) | mode | ARM2_I;
    if (mode == ARM2_FIQ) {
        psr |= ARM2_F;
    }
    set_psr(c, psr);
    c->r[14] = r14;
}

static INLINE void exception(arm2_t *c, run_t *r, uint32_t vector, uint32_t mode, uint32_t r14)
{
    enter_mode(c, mode, r14);
    r->pc = vector;
    r->left -= 4;                           /* 2S + 1N */
    refresh(c, r);
}

void arm2_set_psr(arm2_t *c, uint32_t psr)
{
    set_psr(c, psr);
}

/* Taken before the next instruction (arm2_run() checks `pending`) */
void arm2_set_irq(arm2_t *c, bool level)
{
    c->irq_line = level;
    update_pending(c);
}

void arm2_set_fiq(arm2_t *c, bool level)
{
    c->fiq_line = level;
    update_pending(c);
}

/* The run ends with the ticks it used: what's left comes off its length
 * (not left = 0 alone: that counted the rest as run, time jumped ahead)
 */
void arm2_end_run(arm2_t *c)
{
    if (c->left > 0) {
        c->run_start -= c->left;
        c->left = 0;
    }
}

/* ---- Registers from outside ---- */

uint32_t arm2_get_reg(const arm2_t *c, int mode, int reg)
{
    uint32_t cur = c->psr & ARM2_MODE;
    if (reg == 15) {
        return arm2_r15(c);
    }
    if (reg < 8 || mode == (int)cur || (reg < 13 && mode != ARM2_FIQ && cur != ARM2_FIQ)) {
        return c->r[reg];
    }
    switch (mode) {
    case ARM2_FIQ: return c->fiq[reg - 8];
    case ARM2_IRQ: return reg < 13 ? c->usr[reg - 8] : c->irq[reg - 13];
    case ARM2_SVC: return reg < 13 ? c->usr[reg - 8] : c->svc[reg - 13];
    default: return c->usr[reg - 8];
    }
}

void arm2_set_reg(arm2_t *c, int mode, int reg, uint32_t v)
{
    uint32_t cur = c->psr & ARM2_MODE;
    if (reg == 15) {
        c->pc = v & ARM2_PC;
        return;
    }
    if (reg < 8 || mode == (int)cur || (reg < 13 && mode != ARM2_FIQ && cur != ARM2_FIQ)) {
        c->r[reg] = v;
        return;
    }
    switch (mode) {
    case ARM2_FIQ: c->fiq[reg - 8] = v; break;
    case ARM2_IRQ: if (reg < 13) c->usr[reg - 8] = v; else c->irq[reg - 13] = v; break;
    case ARM2_SVC: if (reg < 13) c->usr[reg - 8] = v; else c->svc[reg - 13] = v; break;
    default: c->usr[reg - 8] = v; break;
    }
}

/* The user mode's register (STM/LDM with ^) */
static inline uint32_t *user_reg(arm2_t *c, int reg)
{
    uint32_t mode = c->psr & ARM2_MODE;
    if (reg < 8 || mode == ARM2_USR || (reg < 13 && mode != ARM2_FIQ)) {
        return &c->r[reg];
    }
    return &c->usr[reg - 8];
}

/* ---- Memory ---- */

static inline uint32_t rd_bit(const arm2_t *c)
{
    return (uint32_t)c->rd_bit;
}

static inline uint32_t wr_bit(const arm2_t *c)
{
    return (uint32_t)c->wr_bit;
}

/* The bus callbacks, with the run's state in the arm2_t meanwhile (all
 * that takes the run_t is inlined: it stays in registers). Their abort
 * flag is a variable of their own: the caller's stays in a register.
 */
static INLINE uint32_t bus_read(arm2_t *c, run_t *r, uint32_t addr, bool byte, bool *abort)
{
    bool aborted = false;
    c->left = r->left;
    uint32_t v = c->bus.read(c->bus.ctx, addr, byte, &aborted);
    r->left = c->left;
    refresh(c, r);
    if (aborted) {
        *abort = true;
    }
    return v;
}

static INLINE void bus_write(arm2_t *c, run_t *r, uint32_t addr, uint32_t v, bool byte, bool *abort)
{
    bool aborted = false;
    c->left = r->left;
    c->bus.write(c->bus.ctx, addr, v, byte, &aborted);
    r->left = c->left;
    refresh(c, r);
    if (aborted) {
        *abort = true;
    }
}

/* A word (rotated for unaligned addresses, as the ARM2 does); `n` the
 * ticks of a RAM cycle (2 non-sequential, 1 sequential)
 */
static INLINE uint32_t read_word(arm2_t *c, run_t *r, uint32_t addr, uint32_t bit, int n, bool *abort)
{
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    uint32_t v;
    if (LIKELY(e & bit)) {
        v = *(const uint32_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFC));
        r->left -= (e & ARM2_SLOW) ? (n > 1 ? c->slow_n : c->slow_s) : n;
    } else {
        r->left -= n;
        v = bus_read(c, r, addr, false, abort);
    }
    return (addr & 3) ? ror32(v, (addr & 3) * 8) : v;
}

static INLINE uint32_t read_byte(arm2_t *c, run_t *r, uint32_t addr, uint32_t bit, bool *abort)
{
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    if (LIKELY(e & bit)) {
        r->left -= (e & ARM2_SLOW) ? c->slow_n : 2;
        return *(const uint8_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFF));
    }
    r->left -= 2;
    return bus_read(c, r, addr, true, abort) & 0xFF;
}

static INLINE void write_word(arm2_t *c, run_t *r, uint32_t addr, uint32_t v, uint32_t bit, int n, bool *abort)
{
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    r->left -= n;
    if (LIKELY(e & bit)) {
        *(uint32_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFC)) = v;
        if (e & ARM2_WATCH) {
            c->watch_hit = true;
        }
        return;
    }
    bus_write(c, r, addr, v, false, abort);
}

static INLINE void write_byte(arm2_t *c, run_t *r, uint32_t addr, uint32_t v, uint32_t bit, bool *abort)
{
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    r->left -= 2;
    if (LIKELY(e & bit)) {
        *(uint8_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFF)) = (uint8_t)v;
        if (e & ARM2_WATCH) {
            c->watch_hit = true;
        }
        return;
    }
    bus_write(c, r, addr, (v & 0xFF) * 0x01010101u, true, abort);
}

uint32_t arm2_peek32(const arm2_t *c, uint32_t addr)
{
    addr &= ARM2_ADDR_MASK & ~3u;
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    if (e & ARM2_PRIV_R) {
        return *(const uint32_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFC));
    }
    return 0;
}

uint8_t arm2_peek8(const arm2_t *c, uint32_t addr)
{
    return (uint8_t)(arm2_peek32(c, addr) >> ((addr & 3) * 8));
}

/* A register by its field (Rn bits 19-16, Rd 15-12, Rs 11-8, Rm 3-0)
 * as a byte offset into r[]: the field times 4 takes one shift fewer
 * than an index. 15 * 4 is R15.
 */
#define RN4(op)         (((op) >> 14) & 0x3C)
#define RD4(op)         (((op) >> 10) & 0x3C)
#define RS4(op)         (((op) >> 6) & 0x3C)
#define RM4(op)         (((op) << 2) & 0x3C)
#define R15_4           0x3C
#define REG(c, off)     (*(uint32_t *)((uint8_t *)(c)->r + (off)))

/* ---- The barrel shifter ---- */

/* Operand 2's forms: an immediate (bit 25), a register shifted by a
 * constant, or by a register (bit 4)
 */
enum { OP2_IMM, OP2_SHIFT_IMM, OP2_SHIFT_REG };

/* Operand 2 of a data processing instruction (`form` a constant where
 * it's inlined); *carry its carry out. `pc` the instruction's address.
 */
static INLINE uint32_t operand2(arm2_t *c, run_t *r, uint32_t op, uint32_t pc, uint32_t *carry, const int form)
{
    if (form == OP2_IMM) {
        unsigned rot = (op >> 7) & 0x1E;
        uint32_t v = ror32(op & 0xFF, rot);
        *carry = rot ? v >> 31 : (c->psr >> 29) & 1;
        return v;
    }
    unsigned rm = RM4(op);
    unsigned type = (op >> 5) & 3;
    uint32_t v;
    unsigned amount;
    if (form == OP2_SHIFT_REG) {
        /* An internal cycle, and R15 reads 4 more */
        r->left -= 1;
        v = rm == R15_4 ? ((pc + 12) & ARM2_PC) | c->psr : REG(c, rm);
        unsigned rs = RS4(op);
        amount = (rs == R15_4 ? pc + 12 : REG(c, rs)) & 0xFF;
        if (amount == 0) {
            *carry = (c->psr >> 29) & 1;
            return v;
        }
        switch (type) {
        case 0:     /* LSL */
            if (amount < 32) { *carry = (v >> (32 - amount)) & 1; return v << amount; }
            *carry = amount == 32 ? v & 1 : 0;
            return 0;
        case 1:     /* LSR */
            if (amount < 32) { *carry = (v >> (amount - 1)) & 1; return v >> amount; }
            *carry = amount == 32 ? v >> 31 : 0;
            return 0;
        case 2:     /* ASR */
            if (amount < 32) { *carry = (v >> (amount - 1)) & 1; return (uint32_t)((int32_t)v >> amount); }
            *carry = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        default:    /* ROR */
            amount &= 31;
            if (amount == 0) { *carry = v >> 31; return v; }
            *carry = (v >> (amount - 1)) & 1;
            return ror32(v, amount);
        }
    }
    v = rm == R15_4 ? ((pc + 8) & ARM2_PC) | c->psr : REG(c, rm);
    amount = (op >> 7) & 31;
    if (LIKELY(type == 0)) {
        /* LSL, the commonest (and a plain register) first */
        if (amount == 0) { *carry = (c->psr >> 29) & 1; return v; }
        *carry = (v >> (32 - amount)) & 1;
        return v << amount;
    }
    if (type == 1) {
        /* LSR (#0 is #32) */
        if (amount == 0) { *carry = v >> 31; return 0; }
        *carry = (v >> (amount - 1)) & 1;
        return v >> amount;
    }
    if (type == 2) {
        /* ASR (#0 is #32) */
        if (amount == 0) { *carry = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *carry = (v >> (amount - 1)) & 1;
        return (uint32_t)((int32_t)v >> amount);
    }
    /* ROR (#0 is RRX) */
    if (amount == 0) { *carry = v & 1; return (((c->psr >> 29) & 1) << 31) | (v >> 1); }
    *carry = (v >> (amount - 1)) & 1;
    return ror32(v, amount);
}

/* LDR/STR's register offset (shifts by an immediate only) */
static inline uint32_t offset_register(const arm2_t *c, uint32_t op, uint32_t pc)
{
    unsigned rm = RM4(op);
    uint32_t v = rm == R15_4 ? ((pc + 8) & ARM2_PC) | c->psr : REG(c, rm);
    unsigned amount = (op >> 7) & 31;
    switch ((op >> 5) & 3) {
    case 0: return v << amount;
    case 1: return amount ? v >> amount : 0;
    case 2: return (uint32_t)((int32_t)v >> (amount ? amount : 31));
    default: return amount ? ror32(v, amount) : (((c->psr >> 29) & 1) << 31) | (v >> 1);
    }
}

/* ---- Instructions ---- */

/* The flags of an addition a + b (+ carry) giving r */
static inline uint32_t add_flags(uint32_t a, uint32_t b, uint32_t r, uint32_t carry)
{
    return (r & ARM2_N) | (r ? 0 : ARM2_Z) | (carry ? ARM2_C : 0) |
           ((((a ^ r) & (b ^ r)) >> 31) ? ARM2_V : 0);
}

/* ... of a subtraction a - b (- borrow) giving r */
static inline uint32_t sub_flags(uint32_t a, uint32_t b, uint32_t r, uint32_t carry)
{
    return (r & ARM2_N) | (r ? 0 : ARM2_Z) | (carry ? ARM2_C : 0) |
           ((((a ^ b) & (a ^ r)) >> 31) ? ARM2_V : 0);
}

/* A data processing instruction: `opcode` (bits 24-21), operand 2's
 * `form` and `s` (bit 20) are constants where it's inlined (arm2_run()
 * has one for each, but for the rare shifts by a register)
 */
static INLINE void data_processing(arm2_t *c, run_t *run, uint32_t op, uint32_t pc, const unsigned opcode,
                                   const int form, const bool s)
{
    unsigned rd = RD4(op);
    unsigned rn = RN4(op);
    uint32_t carry;
    uint32_t b = operand2(c, run, op, pc, &carry, form);
    /* As Rn, R15 is the PC alone (+ 12 with a register-specified shift) */
    uint32_t a = rn == R15_4 ? (pc + (form == OP2_SHIFT_REG ? 12 : 8)) & ARM2_PC : REG(c, rn);
    uint32_t r, flags = 0;
    uint32_t cin = (c->psr >> 29) & 1;

    switch (opcode) {
    case 0x0: r = a & b; goto logical;              /* AND */
    case 0x1: r = a ^ b; goto logical;              /* EOR */
    case 0x2: r = a - b; flags = sub_flags(a, b, r, a >= b); break;            /* SUB */
    case 0x3: r = b - a; flags = sub_flags(b, a, r, b >= a); break;            /* RSB */
    case 0x4: r = a + b; flags = add_flags(a, b, r, r < a); break;             /* ADD */
    case 0x5: r = a + b + cin; flags = add_flags(a, b, r, cin ? r <= a : r < a); break;    /* ADC */
    case 0x6: r = a - b - !cin; flags = sub_flags(a, b, r, cin ? a >= b : a > b); break;   /* SBC */
    case 0x7: r = b - a - !cin; flags = sub_flags(b, a, r, cin ? b >= a : b > a); break;   /* RSC */
    case 0x8: r = a & b; goto test_logical;         /* TST */
    case 0x9: r = a ^ b; goto test_logical;         /* TEQ */
    case 0xA: r = a - b; flags = sub_flags(a, b, r, a >= b); goto test;        /* CMP */
    case 0xB: r = a + b; flags = add_flags(a, b, r, r < a); goto test;         /* CMN */
    case 0xC: r = a | b; goto logical;              /* ORR */
    case 0xD: r = b; goto logical;                  /* MOV */
    case 0xE: r = a & ~b; goto logical;             /* BIC */
    default:  r = ~b; goto logical;                 /* MVN */
    }

    /* Arithmetic */
    if (rd == R15_4) {
        goto to_pc;
    }
    REG(c, rd) = r;
    if (s) {
        c->psr = (c->psr & ~NZCV) | flags;
        flags_changed(c);
    }
    return;

logical:
    if (rd == R15_4) {
        goto to_pc;
    }
    REG(c, rd) = r;
    if (s) {
        c->psr = (c->psr & ~NZCV) | (r & ARM2_N) | (r ? 0 : ARM2_Z) | (carry ? ARM2_C : 0) |
                 (c->psr & ARM2_V);
        flags_changed(c);
    }
    return;

test_logical:
    flags = (r & ARM2_N) | (r ? 0 : ARM2_Z) | (carry ? ARM2_C : 0) | (c->psr & ARM2_V);
test:
    if (!s) {
        return;                             /* not an ARM2 instruction: nothing happens */
    }
    if (rd == R15_4) {
        run_write_psr(c, run, r);           /* TEQP, TSTP, CMPP, CMNP */
    } else {
        c->psr = (c->psr & ~NZCV) | flags;
        flags_changed(c);
    }
    return;

to_pc:
    /* Rd = R15: the PC, and with S the PSR (MOVS PC, R14) */
    if (s) {
        run_write_psr(c, run, r);
    }
    run->pc = r & ARM2_PC;
    run->left -= 3;                         /* refill: + 1S + 1N */
}

static INLINE void multiply(arm2_t *c, run_t *run, uint32_t op)
{
    unsigned rd = (op >> 16) & 15;
    uint32_t rs = c->r[(op >> 8) & 15];
    uint32_t r = c->r[op & 15] * rs;
    if (op & (1u << 21)) {
        r += c->r[(op >> 12) & 15];         /* MLA */
    }
    /* Booth's algorithm, two bits of Rs a cycle, ending early */
    int m = rs ? (33 - __builtin_clz(rs)) >> 1 : 1;
    run->left -= m > 16 ? 16 : m;
    if (rd != 15) {
        c->r[rd] = r;
    }
    if (op & (1u << 20)) {
        c->psr = (c->psr & ~(ARM2_N | ARM2_Z)) | (r & ARM2_N) | (r ? 0 : ARM2_Z);
        flags_changed(c);
    }
}

/* SWP (ARM250, ARM3) */
static INLINE void swap(arm2_t *c, run_t *r, uint32_t op, uint32_t pc)
{
    unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    uint32_t addr = c->r[rn];
    bool abort = false;
    if (addr & ~ARM2_ADDR_MASK) {
        exception(c, r, ARM2_VEC_ADDRESS, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
        return;
    }
    uint32_t v;
    if (op & (1u << 22)) {
        v = read_byte(c, r, addr, rd_bit(c), &abort);
        if (!abort) {
            write_byte(c, r, addr, c->r[rm], wr_bit(c), &abort);
        }
    } else {
        v = read_word(c, r, addr, rd_bit(c), 2, &abort);
        if (!abort) {
            write_word(c, r, addr, c->r[rm], wr_bit(c), 2, &abort);
        }
    }
    r->left -= 1;
    if (abort) {
        exception(c, r, ARM2_VEC_DATA_ABORT, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
        return;
    }
    if (rd != 15) {
        c->r[rd] = v;
    }
}

/* LDR, STR: `load` (bit 20), `byte` (22) and `reg` (25, the offset a
 * register) constants where it's inlined
 */
static INLINE void single_transfer(arm2_t *c, run_t *r, uint32_t op, uint32_t pc, const bool load, const bool byte,
                                   const bool reg)
{
    unsigned rn = RN4(op);
    unsigned rd = RD4(op);
    bool pre = (op >> 24) & 1, up = (op >> 23) & 1;
    bool wb = (op >> 21) & 1;
    uint32_t off = reg ? offset_register(c, op, pc) : op & 0xFFF;
    uint32_t base = rn == R15_4 ? (pc + 8) & ARM2_PC : REG(c, rn);
    uint32_t moved = up ? base + off : base - off;
    uint32_t addr = pre ? moved : base;
    /* Post-indexed with W: the access as in user mode (LDRT, STRT) */
    bool user = !pre && wb;
    uint32_t rbit = user ? ARM2_USER_R : rd_bit(c);
    uint32_t wbit = user ? ARM2_USER_W : wr_bit(c);
    bool abort = false;

    if (UNLIKELY(addr & ~ARM2_ADDR_MASK)) {
        exception(c, r, ARM2_VEC_ADDRESS, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
        return;
    }
    if (load) {
        uint32_t v = byte ? read_byte(c, r, addr, rbit, &abort) : read_word(c, r, addr, rbit, 2, &abort);
        r->left -= 1;                       /* + 1I */
        if ((!pre || wb) && rn != R15_4) {
            REG(c, rn) = moved;             /* the base is updated even if it aborts */
        }
        if (UNLIKELY(abort)) {
            exception(c, r, ARM2_VEC_DATA_ABORT, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
            return;
        }
        if (rd == R15_4) {
            r->pc = v & ARM2_PC;            /* the PC alone */
            r->left -= 3;
        } else {
            REG(c, rd) = v;
        }
    } else {
        /* R15 stored is the PC + 12 with the PSR */
        uint32_t v = rd == R15_4 ? ((pc + 12) & ARM2_PC) | c->psr : REG(c, rd);
        if (byte) {
            write_byte(c, r, addr, v, wbit, &abort);
        } else {
            write_word(c, r, addr, v, wbit, 2, &abort);
        }
        if ((!pre || wb) && rn != R15_4) {
            REG(c, rn) = moved;
        }
        if (UNLIKELY(abort)) {
            exception(c, r, ARM2_VEC_DATA_ABORT, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
        }
    }
}

/* The registers in a list (the board's RISC-V has no instruction for it:
 * __builtin_popcount() is a library call there)
 */
static inline int count16(uint32_t v)
{
    v = v - ((v >> 1) & 0x5555);
    v = (v & 0x3333) + ((v >> 2) & 0x3333);
    v = (v + (v >> 4)) & 0x0F0F;
    return (int)((v + (v >> 8)) & 0x1F);
}

/* LDM, STM (`load`, bit 20, a constant where it's inlined). The words are
 * the aligned ones (only LDR rotates); a block in one page the table
 * allows goes straight through, the rest a word at a time.
 */
static INLINE void block_transfer(arm2_t *c, run_t *r, uint32_t op, uint32_t pc, const bool load)
{
    unsigned rn = (op >> 16) & 15;
    bool pre = (op >> 24) & 1, up = (op >> 23) & 1, psr_or_user = (op >> 22) & 1;
    bool wb = (op >> 21) & 1;
    uint32_t list = op & 0xFFFF;
    int n = count16(list);
    uint32_t base = rn == 15 ? (pc + 8) & ARM2_PC : c->r[rn];
    uint32_t addr, final;
    bool abort = false;

    if (up) {
        addr = base + (pre ? 4 : 0);
        final = base + 4 * n;
    } else {
        addr = base - 4 * n + (pre ? 0 : 4);
        final = base - 4 * n;
    }
    if (UNLIKELY(n == 0)) {
        return;
    }
    if (UNLIKELY(addr & ~ARM2_ADDR_MASK)) {
        exception(c, r, ARM2_VEC_ADDRESS, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
        return;
    }
    uintptr_t e = c->page[addr >> ARM2_PAGE_SHIFT];
    bool one_page = ((addr + 4 * (uint32_t)n - 4) >> ARM2_PAGE_SHIFT) == (addr >> ARM2_PAGE_SHIFT);

    if (!load) {
        uint32_t wbit = wr_bit(c);
        /* The base written back after the first register: a base that
         * comes first in the list is stored as it was, later the new one
         */
        bool wb_first = wb && rn != 15 && (list & ((1u << rn) - 1));
        if (LIKELY(one_page && (e & wbit) && !psr_or_user)) {
            uint32_t *p = (uint32_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFC));
            r->left -= n + 2;               /* 1N + (n-1)S + 1I */
            if (e & ARM2_WATCH) {
                c->watch_hit = true;
            }
            if (wb_first) {
                c->r[rn] = final;
            }
#pragma GCC unroll 15
            for (int i = 0; i < 15; i++) {
                if (list & (1u << i)) {
                    *p++ = c->r[i];
                }
            }
            if (list & 0x8000) {
                *p = ((pc + 12) & ARM2_PC) | c->psr;
            }
            if (wb && rn != 15) {
                c->r[rn] = final;
            }
            return;
        }
        int cost = 2;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) {
                continue;
            }
            uint32_t v;
            if (i == 15) {
                v = ((pc + 12) & ARM2_PC) | c->psr;
            } else if (psr_or_user) {
                v = *user_reg(c, i);        /* STM ^: the user registers */
            } else {
                v = c->r[i];
            }
            write_word(c, r, addr & ARM2_ADDR_MASK, v, wbit, cost, &abort);
            if (cost == 2 && wb && rn != 15) {
                c->r[rn] = final;
            }
            cost = 1;
            addr += 4;
        }
        r->left -= 1;
        if (UNLIKELY(abort)) {
            exception(c, r, ARM2_VEC_DATA_ABORT, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
        }
        return;
    }

    /* LDM: the base written back first, so that a loaded base wins */
    if (wb && rn != 15) {
        c->r[rn] = final;
    }
    uint32_t rbit = rd_bit(c);
    bool user = psr_or_user && !(list & 0x8000);    /* LDM ^ without the PC: user registers */
    uint32_t v[16];
    if (LIKELY(one_page && (e & rbit))) {
        const uint32_t *p = (const uint32_t *)((e & ~(uintptr_t)0xFFF) + (addr & 0xFFC));
        r->left -= ((e & ARM2_SLOW) ? c->slow_n + (n - 1) * c->slow_s : n + 1) + 1;     /* + 1I */
        if (UNLIKELY(user)) {
            for (int i = 0; i < 15; i++) {
                if (list & (1u << i)) {
                    *user_reg(c, i) = *p++;
                }
            }
            return;
        }
#pragma GCC unroll 15
        for (int i = 0; i < 15; i++) {
            if (list & (1u << i)) {
                c->r[i] = *p++;
            }
        }
        if (!(list & 0x8000)) {
            return;
        }
        v[15] = *p;
    } else {
        int cost = 2;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) {
                continue;
            }
            v[i] = read_word(c, r, addr & (ARM2_ADDR_MASK & ~3u), rbit, cost, &abort);
            cost = 1;
            addr += 4;
        }
        r->left -= 1;
        if (UNLIKELY(abort)) {
            exception(c, r, ARM2_VEC_DATA_ABORT, ARM2_SVC, ((pc + 8) & ARM2_PC) | c->psr);
            return;
        }
        for (int i = 0; i < 15; i++) {
            if (list & (1u << i)) {
                if (user) {
                    *user_reg(c, i) = v[i];
                } else {
                    c->r[i] = v[i];
                }
            }
        }
        if (!(list & 0x8000)) {
            return;
        }
    }
    if (psr_or_user) {
        run_write_psr(c, r, v[15]);         /* LDM ^ with the PC: the PSR too */
    }
    r->pc = v[15] & ARM2_PC;
    r->left -= 3;
}

/* ---- Running ---- */

static INLINE void take_interrupt(arm2_t *c, run_t *r)
{
    uint32_t r14 = ((r->pc + 4) & ARM2_PC) | c->psr;
    if (c->fiq_line && !(c->psr & ARM2_F)) {
        exception(c, r, ARM2_VEC_FIQ, ARM2_FIQ, r14);
    } else if (c->irq_line && !(c->psr & ARM2_I)) {
        exception(c, r, ARM2_VEC_IRQ, ARM2_IRQ, r14);
    }
}

/* The PC's page, into *code and *code_s too: false if the table doesn't
 * allow it. `code` less the page's address: code + pc is the instruction.
 */
static INLINE bool code_page(arm2_t *c, uint32_t pc, const uint8_t **code, int32_t *code_s)
{
    uintptr_t e = c->page[pc >> ARM2_PAGE_SHIFT];
    if (!(e & rd_bit(c))) {
        return false;
    }
    c->code_page = pc >> ARM2_PAGE_SHIFT;
    c->code = (const uint8_t *)((e & ~(uintptr_t)0xFFF) - (pc & ~(uintptr_t)0xFFF));
    c->code_s = (e & ARM2_SLOW) ? c->slow_s : 1;
    *code = c->code;
    *code_s = c->code_s;
    return true;
}

/* A fetch the table doesn't allow, through the bus */
static INLINE uint32_t fetch_bus(arm2_t *c, run_t *r, uint32_t pc, bool *abort)
{
    c->left = r->left;
    uint32_t op = c->bus.fetch ? c->bus.fetch(c->bus.ctx, pc, abort) : c->bus.read(c->bus.ctx, pc, false, abort);
    r->left = c->left;
    return op;
}

/* A coprocessor instruction, or the undefined instruction trap */
static INLINE void coprocessor(arm2_t *c, run_t *r, uint32_t op, uint32_t pc)
{
    c->left = r->left;
    c->pc = r->pc & ARM2_ADDR_MASK;
    bool taken = c->bus.coprocessor && c->bus.coprocessor(c->bus.ctx, c, op);
    r->left = c->left;
    r->pc = c->pc;
    refresh(c, r);
    if (!taken) {
        exception(c, r, ARM2_VEC_UNDEFINED, ARM2_SVC, ((pc + 4) & ARM2_PC) | c->psr);
    }
}

/* The instructions by their bits 31-20, as arm2_run()'s labels: those
 * that always run (AL) by bits 27-20 (data processing by opcode, with its
 * operand a register (r) or an immediate (i); LDR, STR by size and offset,
 * i an immediate, r a register), the other conditions to their check
 * first, never (NV) to the next instruction
 */
#define DP_PAIR(name, form)     &&dp_##name##_##form, &&dp_##name##_##form
#define DP_ROW(form)    DP_PAIR(and, form), DP_PAIR(eor, form), DP_PAIR(sub, form), DP_PAIR(rsb, form), \
                        DP_PAIR(add, form), DP_PAIR(adc, form), DP_PAIR(sbc, form), DP_PAIR(rsc, form), \
                        DP_PAIR(tst, form), DP_PAIR(teq, form), DP_PAIR(cmp, form), DP_PAIR(cmn, form), \
                        DP_PAIR(orr, form), DP_PAIR(mov, form), DP_PAIR(bic, form), DP_PAIR(mvn, form)
#define ST_8(form)      &&str_##form, &&ldr_##form, &&str_##form, &&ldr_##form, \
                        &&strb_##form, &&ldrb_##form, &&strb_##form, &&ldrb_##form
#define ST_ROW(form)    ST_8(form), ST_8(form), ST_8(form), ST_8(form)
#define X2(l)           &&l, &&l
#define X4(l)           X2(l), X2(l)
#define X8(l)           X4(l), X4(l)
#define X16(l)          X8(l), X8(l)
#define X64(l)          X16(l), X16(l), X16(l), X16(l)
#define X256(l)         X64(l), X64(l), X64(l), X64(l)
#define COND_ROW        X64(cond), X64(cond), X16(cond), X16(cond), X16(cond_b), X16(cond_bl), X64(cond)
#define BLK_ROW        X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), \
                        X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), \
                        X2(stm_ldm), X2(stm_ldm), X2(stm_ldm), X2(stm_ldm)

#ifdef ARM2_HOST_HOOK
void arm2_host_hook(arm2_t *c);         /* host builds: tracing */
#define HOST_HOOK()     do { c->pc = pc; arm2_host_hook(c); } while (0)
#else
#define HOST_HOOK()     do { } while (0)
#endif

#if EMUDBG_ENABLED
#define DEBUG_CHECK()   do { if (UNLIKELY(c->dbg_map != NULL) && emudbg_map24(c->dbg_map, pc)) goto debug; } while (0)
#else
#define DEBUG_CHECK()   do { } while (0)
#endif

/* The next instruction, at the end of each one: a dispatch of its own
 * (the host's branch predictor learns what tends to follow each). An
 * interrupt, the debugger and a new page take the slow way (run_t). The
 * PC isn't masked: past the address space's end it's in no page, and
 * the slow way wraps it.
 */
#define NEXT()                                                                  \
    do {                                                                        \
        if (UNLIKELY(r->left <= 0)) goto done;                                  \
        pc = r->pc;                                                             \
        if (UNLIKELY((pc >> ARM2_PAGE_SHIFT) != r->page)) goto slow;            \
        op = *(const uint32_t *)(code + pc);                                    \
        r->left -= code_s;                                                      \
        HOST_HOOK();                                                            \
        r->pc = pc + 4;                                                         \
        insns++;                                                                \
        goto *ops[op >> 20];                                                    \
    } while (0)

/* The instruction's address, in the instructions: from the next one's
 * (then the dispatch keeps no copy of it)
 */
#define PC              (r->pc - 4)

#define DP(name, opcode)                                                        \
    dp_##name##_i:                                                              \
        data_processing(c, r, op, PC, opcode, OP2_IMM, (op >> 20) & 1); NEXT(); \
    dp_##name##_r:                                                              \
        if (UNLIKELY(op & 0x10)) goto dp_shift_reg;                             \
        data_processing(c, r, op, PC, opcode, OP2_SHIFT_IMM, (op >> 20) & 1); NEXT();

#define ST(name, load, byte)                                                    \
    name##_i: single_transfer(c, r, op, PC, load, byte, false); NEXT();         \
    name##_r:                                                                   \
        if (UNLIKELY(op & 0x10)) goto undefined;                                \
        single_transfer(c, r, op, PC, load, byte, true); NEXT();

#define COND_PASSES()   ((c->cond_mask >> (op >> 28)) & 1)

int32_t ARM2_HOT arm2_run(arm2_t *c, int32_t ticks)
{
    static const void *const ops[4096] = {
        COND_ROW, COND_ROW, COND_ROW, COND_ROW, COND_ROW, COND_ROW, COND_ROW,   /* EQ-VC */
        COND_ROW, COND_ROW, COND_ROW, COND_ROW, COND_ROW, COND_ROW, COND_ROW,   /* HI-LE */
        DP_ROW(r), DP_ROW(i),                   /* E00-E3F data processing (and multiplies) */
        ST_ROW(i), ST_ROW(r),                   /* E40-E7F LDR, STR */
        BLK_ROW,                                /* E80-E9F LDM, STM */
        X16(b), X16(bl),                        /* EA0-EBF */
        X16(cop), X16(cop), X16(cop),           /* EC0-EEF coprocessors */
        X16(swi),                               /* EF0-EFF */
        X256(next),                             /* NV */
    };
    /* An overrun of the last run is in `ticks` already: start afresh */
    run_t run = { c->pc & ARM2_PC, ticks, 0 };
    run_t *r = &run;
    c->left = ticks;
    c->run_start = ticks;
    refresh(c, r);
    flags_changed(c);
    /* The PC's page (valid while its number is c->code_page: the table's
     * changes and mode switches set that to ~0)
     */
    const uint8_t *code = c->code;
    int32_t code_s = c->code_s;
    uint32_t insns = 0;
    uint32_t pc, op;

next:
    NEXT();

slow:
    if (c->pending) {
        take_interrupt(c, r);               /* a step of its own (for the debugger) */
        goto next;
    }
    pc &= ARM2_PC;
    if ((pc >> ARM2_PAGE_SHIFT) == c->code_page) {
        op = *(const uint32_t *)(code + pc);
        r->left -= code_s;
    } else if (code_page(c, pc, &code, &code_s)) {
        op = *(const uint32_t *)(code + pc);
        r->left -= code_s;
    } else {
        bool abort = false;
        op = fetch_bus(c, r, pc, &abort);
        r->left -= 2;
        if (abort) {
            exception(c, r, ARM2_VEC_PREFETCH_ABORT, ARM2_SVC, ((pc + 4) & ARM2_PC) | c->psr);
            goto next;
        }
    }
    refresh(c, r);
    HOST_HOOK();
    DEBUG_CHECK();
decoded: __attribute__((unused));       /* the debugger's way back in */
    r->pc = pc + 4;
    insns++;
    goto *ops[op >> 20];

cond:
    if (!COND_PASSES()) {
        goto next;
    }
    goto *ops[0xE00 | ((op >> 20) & 0xFF)];

#if EMUDBG_ENABLED
debug:
    c->pc = pc;
    c->left = r->left;
    emudbg_hit(c->dbg, pc);
    r->left = c->left;
    flags_changed(c);
    pc = c->pc;                             /* the debugger may have moved it */
    if ((pc >> ARM2_PAGE_SHIFT) != c->code_page && !code_page(c, pc, &code, &code_s)) {
        r->pc = pc;
        refresh(c, r);
        goto next;
    }
    refresh(c, r);                          /* it may have gone */
    op = *(const uint32_t *)(code + pc);
    goto decoded;
#endif

    DP(and, 0x0) DP(eor, 0x1) DP(sub, 0x2) DP(rsb, 0x3) DP(add, 0x4) DP(adc, 0x5) DP(sbc, 0x6) DP(rsc, 0x7)
    DP(tst, 0x8) DP(teq, 0x9) DP(cmp, 0xA) DP(cmn, 0xB) DP(orr, 0xC) DP(mov, 0xD) DP(bic, 0xE) DP(mvn, 0xF)

    /* Shifts by a register, and in their space multiplies and SWP */
dp_shift_reg:
    if (UNLIKELY((op & 0x90) == 0x90)) {
        goto multiply_swap;
    }
    if (op & (1u << 20)) {
        data_processing(c, r, op, PC, (op >> 21) & 15, OP2_SHIFT_REG, true);
    } else {
        data_processing(c, r, op, PC, (op >> 21) & 15, OP2_SHIFT_REG, false);
    }
    NEXT();

multiply_swap:
    if ((op & 0x0FC000F0) == 0x00000090) {
        multiply(c, r, op);
    } else if (c->swp && (op & 0x0FB00FF0) == 0x01000090) {
        swap(c, r, op, PC);
    } else {
        goto undefined;
    }
    NEXT();

    ST(ldr, true, false) ST(str, false, false) ST(ldrb, true, true) ST(strb, false, true)

stm_ldm:
    if (op & (1u << 20)) {
        block_transfer(c, r, op, PC, true);
    } else {
        block_transfer(c, r, op, PC, false);
    }
    NEXT();

    /* B, BL: R14 gets the next instruction's address and the PSR. The
     * target isn't masked (as the PC in NEXT()).
     */
cond_bl:
    if (!COND_PASSES()) {
        goto next;
    }
bl:
    c->r[14] = (r->pc & ARM2_PC) | c->psr;
    goto b;
cond_b:
    if (!COND_PASSES()) {
        goto next;
    }
b:
    r->pc += 4 + (uint32_t)((int32_t)(op << 8) >> 6);
    r->left -= 3;
    NEXT();

swi:
    exception(c, r, ARM2_VEC_SWI, ARM2_SVC, (r->pc & ARM2_PC) | c->psr);
    NEXT();

cop:
    coprocessor(c, r, op, PC);
    NEXT();

undefined:
    exception(c, r, ARM2_VEC_UNDEFINED, ARM2_SVC, (r->pc & ARM2_PC) | c->psr);
    NEXT();

done:
    c->pc = r->pc & ARM2_ADDR_MASK;
    c->left = r->left;
    c->insns += insns;
    int32_t ran = c->run_start - c->left;
    c->ticks += (uint64_t)ran;
    c->run_start = c->left;
    return ran;
}

/* ---- Setting up ---- */

static void make_conditions(void)
{
    memset(s_pass, 0, sizeof(s_pass));
    for (int cond = 0; cond < 16; cond++) {
        for (int f = 0; f < 16; f++) {
            bool n = f & 8, z = f & 4, cf = f & 2, v = f & 1, ok;
            switch (cond) {
            case 0x0: ok = z; break;                    /* EQ */
            case 0x1: ok = !z; break;                   /* NE */
            case 0x2: ok = cf; break;                   /* CS */
            case 0x3: ok = !cf; break;                  /* CC */
            case 0x4: ok = n; break;                    /* MI */
            case 0x5: ok = !n; break;                   /* PL */
            case 0x6: ok = v; break;                    /* VS */
            case 0x7: ok = !v; break;                   /* VC */
            case 0x8: ok = cf && !z; break;             /* HI */
            case 0x9: ok = !cf || z; break;             /* LS */
            case 0xA: ok = n == v; break;               /* GE */
            case 0xB: ok = n != v; break;               /* LT */
            case 0xC: ok = !z && n == v; break;         /* GT */
            case 0xD: ok = z || n != v; break;          /* LE */
            case 0xE: ok = true; break;                 /* AL */
            default: ok = false; break;                 /* NV */
            }
            if (ok) {
                s_pass[f] |= (uint16_t)(1u << cond);
            }
        }
    }
}

void arm2_init(arm2_t *c, const arm2_bus_t *bus, const uintptr_t *page)
{
    bool swp = c->swp;
    memset(c, 0, sizeof(*c));
    c->swp = swp;
    c->bus = *bus;
    c->page = page;
    c->slow_s = c->slow_n = 4;
    c->rd_bit = ARM2_USER_R;                /* user mode */
    c->wr_bit = ARM2_USER_W;
    c->code_page = ~0u;
    make_conditions();
}

void arm2_reset(arm2_t *c)
{
    /* SVC mode, IRQ and FIQ off; R14_svc gets the old PC and PSR */
    uint32_t r14 = arm2_r15(c);
    set_psr(c, (c->psr & NZCV) | ARM2_I | ARM2_F | ARM2_SVC);
    c->r[14] = r14;
    c->pc = 0;
    c->code_page = ~0u;
    update_pending(c);
}
