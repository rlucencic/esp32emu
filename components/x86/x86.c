/*
 * x86 core: the 8088, 8086 and 80286 (see include/x86.h)
 *
 * One function runs an instruction (execute()): prefixes, then a switch on
 * the opcode. The arithmetic flags are lazy: an ALU operation keeps its
 * operands and result (lf_op, lf_res, lf_a, lf_b) and the flags are worked
 * out when something reads them (Jcc, PUSHF, LAHF, an interrupt); the other
 * operations set them in `flags` (lf_op 0).
 */
#include <string.h>
#include "x86.h"
#include "x87.h"
#include "emudbg.h"

#define LIKELY(x)       __builtin_expect(!!(x), 1)
#define UNLIKELY(x)     __builtin_expect(!!(x), 0)
#define INLINE          inline __attribute__((always_inline))
#define NOINLINE        __attribute__((noinline))

enum { ES = X86_ES, CS = X86_CS, SS = X86_SS, DS = X86_DS };

#define AX      c->r.w[X86_AX]
#define CX      c->r.w[X86_CX]
#define DX      c->r.w[X86_DX]
#define BX      c->r.w[X86_BX]
#define SP      c->r.w[X86_SP]
#define BP      c->r.w[X86_BP]
#define SI      c->r.w[X86_SI]
#define DI      c->r.w[X86_DI]
#define AL      c->r.b[0]
#define AH      c->r.b[1]

#define CF      X86_CF
#define PF      X86_PF
#define AF      X86_AF
#define ZF      X86_ZF
#define SF      X86_SF
#define TF      X86_TF
#define IF      X86_IF
#define DF      X86_DF
#define OF      X86_OF
#define ARITH   (CF | PF | AF | ZF | SF | OF)

/* The lazy flags' operations: bit 0 is the width (1: 16 bits) */
enum {
    LF_NONE = 0,
    LF_ADD8 = 2, LF_ADD16,
    LF_SUB8, LF_SUB16,
    LF_LOG8, LF_LOG16,
    LF_INC8, LF_INC16,
    LF_DEC8, LF_DEC16,
};

typedef struct {
    uint8_t mod, reg, rm;
    uint8_t seg;
    uint16_t off;
    uint8_t ea;                     /* the effective address's clocks (8088, 8086) */
} modrm_t;

/* The instruction's bytes, read by execute() through a host pointer (kept
 * in registers: the struct never leaves the inlined code): p the next byte,
 * end where the fast reads stop (the code window's end, the 286's 10th
 * byte), base the host address of IP 0 (IP = p - base). Past end,
 * fetch_slow() opens the next page's window, or reads through the bus.
 */
typedef struct {
    uintptr_t p, end, base;
} code_t;

#define IP(q)   ((uint16_t)((q)->p - (q)->base))

static void interrupt(x86_t *c, uint8_t n);

/* ---- Flags ---- */

static inline bool parity_even(uint32_t v)
{
    v &= 0xFF;
    v ^= v >> 4;
    return !((0x6996 >> (v & 0xF)) & 1);
}

static inline uint32_t lf_mask(int op)
{
    return (op & 1) ? 0xFFFF : 0xFF;
}

static inline uint32_t lf_sign(int op)
{
    return (op & 1) ? 0x8000 : 0x80;
}

/* ADD, SUB: the result's bit 8 or 16 (a logical operation's is 0); INC,
 * DEC and none: the flag as it was
 */
static INLINE bool get_cf(const x86_t *c)
{
    const unsigned op = c->lf_op;
    if (op - LF_ADD8 < LF_INC8 - LF_ADD8) {
        return (c->lf_res >> (8 << (op & 1))) & 1;
    }
    return c->flags & CF;
}

static INLINE bool get_zf(const x86_t *c)
{
    return c->lf_op ? (c->lf_res & lf_mask(c->lf_op)) == 0 : (c->flags & ZF) != 0;
}

static INLINE bool get_sf(const x86_t *c)
{
    return c->lf_op ? (c->lf_res & lf_sign(c->lf_op)) != 0 : (c->flags & SF) != 0;
}

static INLINE bool get_pf(const x86_t *c)
{
    return c->lf_op ? parity_even(c->lf_res) : (c->flags & PF) != 0;
}

static INLINE bool get_of(const x86_t *c)
{
    uint32_t s = lf_sign(c->lf_op);
    switch (c->lf_op) {
    case LF_ADD8: case LF_ADD16: return ((c->lf_a ^ c->lf_res) & (c->lf_b ^ c->lf_res) & s) != 0;
    case LF_SUB8: case LF_SUB16: return ((c->lf_a ^ c->lf_b) & (c->lf_a ^ c->lf_res) & s) != 0;
    case LF_LOG8: case LF_LOG16: return false;
    case LF_INC8: case LF_INC16: return (c->lf_res & lf_mask(c->lf_op)) == s;
    case LF_DEC8: case LF_DEC16: return (c->lf_res & lf_mask(c->lf_op)) == s - 1;
    default: return (c->flags & OF) != 0;
    }
}

static INLINE bool get_af(const x86_t *c)
{
    switch (c->lf_op) {
    case LF_NONE: return (c->flags & AF) != 0;
    case LF_LOG8: case LF_LOG16: return false;
    default: return ((c->lf_a ^ c->lf_b ^ c->lf_res) & 0x10) != 0;
    }
}

static uint16_t arith_flags(const x86_t *c)
{
    if (c->lf_op == LF_NONE) {
        return c->flags & ARITH;
    }
    return (get_cf(c) ? CF : 0) | (get_pf(c) ? PF : 0) | (get_af(c) ? AF : 0) |
           (get_zf(c) ? ZF : 0) | (get_sf(c) ? SF : 0) | (get_of(c) ? OF : 0);
}

/* The lazy flags into `flags` */
static void materialize(x86_t *c)
{
    if (c->lf_op != LF_NONE) {
        c->flags = (c->flags & ~ARITH) | arith_flags(c);
        c->lf_op = LF_NONE;
    }
}

static INLINE void lazy(x86_t *c, int op, uint32_t res, uint32_t a, uint32_t b)
{
    c->lf_op = op;
    c->lf_res = res;
    c->lf_a = a;
    c->lf_b = b;
}

/* SF, ZF and PF from a result, the others as given (into `flags`) */
static void set_szp(x86_t *c, uint32_t v, int w, uint16_t others, uint16_t which)
{
    materialize(c);
    uint16_t f = c->flags & ~(SF | ZF | PF | which);
    v &= w ? 0xFFFF : 0xFF;
    f |= v == 0 ? ZF : 0;
    f |= (v & (w ? 0x8000 : 0x80)) ? SF : 0;
    f |= parity_even(v) ? PF : 0;
    c->flags = f | (others & which);
}

uint16_t x86_get_flags(const x86_t *c)
{
    uint16_t f = (c->flags & ~ARITH) | arith_flags(c);
    if (c->model != X86_80286) {
        return (f & 0x0FD5) | 0xF002;
    }
    return (f & ((c->msw & X86_MSW_PE) ? 0x7FD5 : 0x0FD5)) | 0x0002;
}

void x86_set_flags(x86_t *c, uint16_t f)
{
    c->flags = f & ((c->model == X86_80286 && (c->msw & X86_MSW_PE)) ? 0x7FD5 : 0x0FD5);
    c->lf_op = LF_NONE;
}

static INLINE bool cond(const x86_t *c, int cc)
{
    bool r;
    switch (cc >> 1) {
    case 0: r = get_of(c); break;
    case 1: r = get_cf(c); break;
    case 2: r = get_zf(c); break;
    case 3: r = get_cf(c) || get_zf(c); break;
    case 4: r = get_sf(c); break;
    case 5: r = get_pf(c); break;
    case 6: r = get_sf(c) != get_of(c); break;
    default: r = get_zf(c) || get_sf(c) != get_of(c); break;
    }
    return r ^ (cc & 1);
}

/* ---- Memory ---- */

/* A fault: the instruction left (x86_run() delivers it). In protected
 * mode 8 and 10-13 push an error code: a selector's index and table, EXT
 * (bit 0) while delivering an event from outside the program.
 */
static void __attribute__((noreturn)) fault_err(x86_t *c, int vector, uint16_t error)
{
    c->fault_vector = vector;
    c->fault_error = (uint16_t)(error | (c->ext ? 1 : 0));
    c->fault_has_error = vector == 8 || (vector >= 10 && vector <= 13);
    longjmp(c->fault_jmp, 1);
}

static void __attribute__((noreturn)) fault(x86_t *c, int vector)
{
    fault_err(c, vector, 0);
}

#define PE(c)           ((c)->msw & X86_MSW_PE)
#define SEL(sel)        ((uint16_t)((sel) & 0xFFFC))

/* The descriptors' access byte */
enum {
    A_PRESENT = 0x80, A_SEG = 0x10, A_CODE = 0x08,
    A_CONFORM = 0x04,               /* code: conforming */
    A_DOWN = 0x04,                  /* data: expands down */
    A_RW = 0x02,                    /* code: readable; data: writable */
};
#define A_DPL(a)        (((a) >> 5) & 3)

/* A code segment that may only be executed (CS's reads fault) */
static inline bool exec_only(const x86_seg_t *g)
{
    return (g->access & (A_SEG | A_CODE | A_RW)) == (A_SEG | A_CODE);
}

/* An access beyond a segment's limits: interrupt 13 (in protected mode 12
 * in SS), error code 0
 */
static NOINLINE void __attribute__((noreturn)) limit_fault(x86_t *c, int s)
{
    fault(c, s == X86_SS && PE(c) ? 12 : 13);
}

/* Accesses the page table doesn't allow: the bus */
static NOINLINE uint8_t bus_rd8(x86_t *c, uint32_t lin)
{
    return c->bus.read(c->bus.ctx, lin);
}

static NOINLINE void bus_wr8(x86_t *c, uint32_t lin, uint8_t v)
{
    c->bus.write(c->bus.ctx, lin, v);
}

static INLINE uint8_t mem_rd8(x86_t *c, uint32_t lin)
{
    lin &= c->addr_mask;
    uintptr_t e = c->page[lin >> X86_PAGE_SHIFT];
    if (LIKELY(!(e & X86_NO_READ))) {
        return ((const uint8_t *)(e & ~(uintptr_t)0xFFF))[lin & 0xFFF];
    }
    return bus_rd8(c, lin);
}

static INLINE void mem_wr8(x86_t *c, uint32_t lin, uint8_t v)
{
    lin &= c->addr_mask;
    uintptr_t e = c->page[lin >> X86_PAGE_SHIFT];
    if (LIKELY(!(e & X86_NO_WRITE))) {
        ((uint8_t *)(e & ~(uintptr_t)0xFFF))[lin & 0xFFF] = v;
        return;
    }
    bus_wr8(c, lin, v);
}

/* A word across pages, or through the bus: a byte at a time */
static NOINLINE uint16_t mem_rd16_bytes(x86_t *c, uint32_t lin)
{
    return mem_rd8(c, lin) | mem_rd8(c, lin + 1) << 8;
}

static NOINLINE void mem_wr16_bytes(x86_t *c, uint32_t lin, uint16_t v)
{
    mem_wr8(c, lin, (uint8_t)v);
    mem_wr8(c, lin + 1, (uint8_t)(v >> 8));
}

static INLINE uint16_t mem_rd16(x86_t *c, uint32_t lin)
{
    lin &= c->addr_mask;
    if (LIKELY((lin & 0xFFF) != 0xFFF)) {
        uintptr_t e = c->page[lin >> X86_PAGE_SHIFT];
        if (LIKELY(!(e & X86_NO_READ))) {
            const uint8_t *p = (const uint8_t *)(e & ~(uintptr_t)0xFFF) + (lin & 0xFFF);
            return p[0] | p[1] << 8;
        }
    }
    return mem_rd16_bytes(c, lin);
}

static INLINE void mem_wr16(x86_t *c, uint32_t lin, uint16_t v)
{
    lin &= c->addr_mask;
    if (LIKELY((lin & 0xFFF) != 0xFFF)) {
        uintptr_t e = c->page[lin >> X86_PAGE_SHIFT];
        if (LIKELY(!(e & X86_NO_WRITE))) {
            uint8_t *p = (uint8_t *)(e & ~(uintptr_t)0xFFF) + (lin & 0xFFF);
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            return;
        }
    }
    mem_wr16_bytes(c, lin, v);
}

/* A word at offset FFFF in real mode: the 286 faults (interrupt 13, SS
 * too); the 8088 and 8086 (and accesses from outside a run) wrap it within
 * the segment. (Other limits, protected mode's and LOADALL's, are checked
 * in the slow paths.)
 */
static NOINLINE bool wraps(x86_t *c, int s, uint16_t off)
{
    (void)s;
    if (c->model != X86_80286 || !c->in_run) {
        return off == 0xFFFF;
    }
    fault(c, 13);
}

static NOINLINE uint16_t rd16_edge(x86_t *c, int s, uint16_t off)
{
    const x86_seg_t *g = &c->seg[s];
    if (wraps(c, s, off)) {
        return mem_rd8(c, g->base + off) | mem_rd8(c, g->base) << 8;
    }
    return mem_rd16(c, g->base + off);
}

static NOINLINE void wr16_edge(x86_t *c, int s, uint16_t off, uint16_t v)
{
    const x86_seg_t *g = &c->seg[s];
    if (wraps(c, s, off)) {
        mem_wr8(c, g->base + off, (uint8_t)v);
        mem_wr8(c, g->base, (uint8_t)(v >> 8));
        return;
    }
    mem_wr16(c, g->base + off, v);
}

static void seg_window(x86_t *c, x86_seg_t *g);

/* A write the 8088/8086's queue may have fetched already: noted */
static INLINE void queue_note(x86_t *c, uint32_t lin, const uint8_t *p)
{
    lin &= c->addr_mask;
    if (lin - c->q_lin < 16 && c->q_log_n < 8) {
        c->q_log[c->q_log_n].lin = lin;
        c->q_log[c->q_log_n].old = *p;
        c->q_log_n++;
    }
}

/* Beyond a segment's direct window: its window worked out if it's stale,
 * else through the page table (one copy of it, out of the run loop)
 */
static NOINLINE uint8_t rd8_slow(x86_t *c, int s, uint16_t off)
{
    x86_seg_t *g = &c->seg[s];
    if (g->stale) {
        seg_window(c, g);
        if (off < g->rd_lim) {
            return ((const uint8_t *)g->host)[off];
        }
    }
    if (UNLIKELY(off < g->rlo || off > g->rhi || exec_only(g))) {
        limit_fault(c, s);
    }
    return mem_rd8(c, g->base + off);
}

static NOINLINE void wr8_slow(x86_t *c, int s, uint16_t off, uint8_t v)
{
    x86_seg_t *g = &c->seg[s];
    if (g->stale) {
        seg_window(c, g);
        if (off < g->wr_lim) {
            if (c->model != X86_80286) {
                queue_note(c, g->base + off, (const uint8_t *)g->host + off);
            }
            ((uint8_t *)g->host)[off] = v;
            return;
        }
    }
    if (UNLIKELY(off < g->wlo || off > g->whi)) {
        limit_fault(c, s);
    }
    mem_wr8(c, g->base + off, v);
}

static NOINLINE uint16_t rd16_slow(x86_t *c, int s, uint16_t off)
{
    x86_seg_t *g = &c->seg[s];
    if (g->stale) {
        seg_window(c, g);
        if (off + 1u < g->rd_lim) {
            const uint8_t *p = (const uint8_t *)g->host + off;
            return (uint16_t)(p[0] | p[1] << 8);
        }
    }
    if (off == 0xFFFF && !PE(c)) {
        return rd16_edge(c, s, off);
    }
    if (UNLIKELY(off < g->rlo || off >= g->rhi || exec_only(g))) {
        limit_fault(c, s);          /* (both bytes inside) */
    }
    return mem_rd16(c, g->base + off);
}

static NOINLINE void wr16_slow(x86_t *c, int s, uint16_t off, uint16_t v)
{
    x86_seg_t *g = &c->seg[s];
    if (g->stale) {
        seg_window(c, g);
        if (off + 1u < g->wr_lim) {
            uint8_t *p = (uint8_t *)g->host + off;
            if (c->model != X86_80286) {
                queue_note(c, g->base + off, p);
                queue_note(c, g->base + off + 1, p + 1);
            }
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            return;
        }
    }
    if (off == 0xFFFF && !PE(c)) {
        wr16_edge(c, s, off, v);
        return;
    }
    if (UNLIKELY(off < g->wlo || off >= g->whi)) {
        limit_fault(c, s);
    }
    mem_wr16(c, g->base + off, v);
}

/* The accessors below take the model from their caller's `i286` (a
 * constant in each run loop: the 286's has no bus time to count)
 */
#define rd8(c, s, off)          rd8_(c, s, off, i286)
#define wr8(c, s, off, v)       wr8_(c, s, off, v, i286)
#define rd16(c, s, off)         rd16_(c, s, off, i286)
#define wr16(c, s, off, v)      wr16_(c, s, off, v, i286)
#define push16(c, v)            push16_(c, v, i286)
#define pop16(c)                pop16_(c, i286)
#define get_rm8(c, m)           get_rm8_(c, m, i286)
#define set_rm8(c, m, v)        set_rm8_(c, m, v, i286)
#define get_rm16(c, m)          get_rm16_(c, m, i286)
#define set_rm16(c, m, v)       set_rm16_(c, m, v, i286)

static INLINE uint8_t rd8_(x86_t *c, int s, uint16_t off, const bool i286)
{
    const x86_seg_t *g = &c->seg[s];
    if (!i286) {
        c->busy += c->xfer;
    }
    c->clk += c->mem_wait;
    if (LIKELY(off < g->rd_lim)) {
        return ((const uint8_t *)g->host)[off];
    }
    return rd8_slow(c, s, off);
}

static INLINE void wr8_(x86_t *c, int s, uint16_t off, uint8_t v, const bool i286)
{
    const x86_seg_t *g = &c->seg[s];
    if (!i286) {
        c->busy += c->xfer;
    }
    c->clk += c->mem_wait;
    if (LIKELY(off < g->wr_lim)) {
        if (!i286) {
            queue_note(c, g->base + off, (const uint8_t *)g->host + off);
        }
        ((uint8_t *)g->host)[off] = v;
        return;
    }
    wr8_slow(c, s, off, v);
}

static INLINE uint16_t rd16_(x86_t *c, int s, uint16_t off, const bool i286)
{
    const x86_seg_t *g = &c->seg[s];
    const uintptr_t p = g->host + off;      /* (its parity is the linear address's) */
    c->clk += c->wait[p & 1];
    if (!i286) {
        c->busy += 2 * c->xfer;
    }
    if (LIKELY(off + 1u < g->rd_lim)) {
        return (uint16_t)(((const uint8_t *)p)[0] | ((const uint8_t *)p)[1] << 8);
    }
    return rd16_slow(c, s, off);
}

static INLINE void wr16_(x86_t *c, int s, uint16_t off, uint16_t v, const bool i286)
{
    const x86_seg_t *g = &c->seg[s];
    const uintptr_t p = g->host + off;
    c->clk += c->wait[p & 1];
    if (!i286) {
        c->busy += 2 * c->xfer;
    }
    if (LIKELY(off + 1u < g->wr_lim)) {
        if (!i286) {
            queue_note(c, g->base + off, (const uint8_t *)p);
            queue_note(c, g->base + off + 1, (const uint8_t *)p + 1);
        }
        ((uint8_t *)p)[0] = (uint8_t)v;
        ((uint8_t *)p)[1] = (uint8_t)(v >> 8);
        return;
    }
    wr16_slow(c, s, off, v);
}

static INLINE void push16_(x86_t *c, uint16_t v, const bool i286)
{
    uint16_t sp = SP - 2;
    wr16(c, SS, sp, v);
    SP = sp;
}

static INLINE uint16_t pop16_(x86_t *c, const bool i286)
{
    uint16_t v = rd16(c, SS, SP);
    SP += 2;
    return v;
}

/* ---- Instruction fetch ---- */

/* The byte at IP, past the end of the fast reads: the 286's 11th (a
 * fault), or the next page's first (its window opens: to the page's end or
 * the segment's FFFF, IP wrapping there), or one the bus reads (no window).
 * The code pointer's new p, end and base in cpu->fetch[] (q itself stays in
 * the inlined code's registers).
 */
static NOINLINE uint8_t fetch_slow(x86_t *c, uint16_t ip)
{
    const bool i286 = c->model == X86_80286;
    if (UNLIKELY(i286 && (uint16_t)(ip - c->ip0) >= c->max_len)) {
        fault(c, 13);               /* more than 10 bytes */
    }
    const uint32_t end = c->seg[CS].rhi + 1u;  /* (the limit: 10000 in real mode) */
    if (UNLIKELY(ip >= end)) {
        fault(c, 13);
    }
    uint32_t lin = (c->seg[CS].base + ip) & c->addr_mask;
    uintptr_t e = c->page[lin >> X86_PAGE_SHIFT];
    if (UNLIKELY(c->q_old_n)) {
        /* Bytes the 8088/8086's queue holds from before a write: no window
         * (each byte comes here) until they're used up
         */
        c->win_n = 0;
        c->fetch[0] = c->fetch[1] = (uintptr_t)ip + 1;
        c->fetch[2] = 0;
        for (int i = 0; i < c->q_old_n; i++) {
            if (c->q_old[i].lin == lin) {
                uint8_t b = c->q_old[i].old;
                c->q_old[i] = c->q_old[--c->q_old_n];
                return b;
            }
        }
        return (e & X86_NO_READ) ? c->bus.read(c->bus.ctx, lin)
                                 : ((const uint8_t *)(e & ~(uintptr_t)0xFFF))[lin & 0xFFF];
    }
    if (e & X86_NO_READ) {
        c->win_n = 0;
        c->fetch[0] = c->fetch[1] = (uintptr_t)ip + 1;
        c->fetch[2] = 0;
        return c->bus.read(c->bus.ctx, lin);
    }
    uint32_t n = 0x1000 - (lin & 0xFFF);
    if (n > end - ip) {
        n = end - ip;
    }
    uintptr_t host = (e & ~(uintptr_t)0xFFF) + (lin & 0xFFF);
    c->win_lo = ip;
    c->win_n = n;
    c->win_base = host - ip;
    c->win_end = host + n;
    c->fetch[0] = host + 1;
    c->fetch[1] = host + n;
    c->fetch[2] = host - ip;
    if (i286 && c->resync) {
        uintptr_t cap = host + (uint16_t)(c->max_len - (uint16_t)(ip - c->ip0));
        if (c->fetch[1] > cap) {
            c->fetch[1] = cap;
        }
    }
    return *(const uint8_t *)host;
}

static INLINE uint8_t fetch8(x86_t *c, code_t *q)
{
    if (LIKELY(q->p < q->end)) {
        return *(const uint8_t *)q->p++;
    }
    uint8_t b = fetch_slow(c, IP(q));
    q->p = c->fetch[0];
    q->end = c->fetch[1];
    q->base = c->fetch[2];
    return b;
}

static INLINE uint16_t fetch16(x86_t *c, code_t *q)
{
    if (LIKELY(q->p + 1 < q->end)) {
        const uint8_t *b = (const uint8_t *)q->p;
        q->p += 2;
        return (uint16_t)(b[0] | b[1] << 8);
    }
    uint16_t lo = fetch8(c, q);
    return lo | fetch8(c, q) << 8;
}

/* The instruction's start: in the code window, or the slow reads */
static INLINE void code_start(x86_t *c, code_t *q, uint16_t ip)
{
    c->resync = false;
    if (LIKELY(ip - c->win_lo < c->win_n)) {
        q->base = c->win_base;
        q->p = q->base + ip;
        q->end = c->win_end;
    } else {
        q->base = 0;
        q->p = q->end = ip;
    }
}

/* ---- Segments ---- */

static void real_seg(x86_seg_t *g, uint16_t sel)
{
    g->sel = sel;
    g->base = (uint32_t)sel << 4;
}

/* The window the pages allow, cut to the segment's limits (none if they
 * don't start at 0: expand-down; none to read an execute-only CS)
 */
static void seg_clamp(x86_seg_t *g)
{
    if (g->rlo != 0 || exec_only(g)) {
        g->rd_lim = 0;
    } else if (g->rd_lim > g->rhi + 1u) {
        g->rd_lim = g->rhi + 1u;
    }
    if (g->wlo != 0 || g->wlo > g->whi) {
        g->wr_lim = 0;
    } else if (g->wr_lim > g->whi + 1u) {
        g->wr_lim = g->whi + 1u;
    }
}

/* A segment's direct window: from offset 0, as far as its pages are
 * contiguous host memory (not past a hole, a page that goes to the bus, the
 * wrap at 1 MB or the A20 gate) that may be read, written, and its limits
 * allow
 */
static NOINLINE void seg_window(x86_t *c, x86_seg_t *g)
{
    x86_window_t *w = &c->windows[(g->base >> 4) & 63];
    g->stale = false;
    if (w->base == g->base && w->gen == c->window_gen) {
        g->host = w->host;
        g->rd_lim = w->rd_lim;
        g->wr_lim = w->wr_lim;
        seg_clamp(g);
        return;
    }
    uint32_t lin = g->base & c->addr_mask;
    uintptr_t e = c->page[lin >> X86_PAGE_SHIFT];
    const uintptr_t host = (e & ~(uintptr_t)0xFFF) + (lin & 0xFFF);
    uint32_t off = 0, rd = 0, wr = 0;
    bool can_rd = true, can_wr = true;
    while (off < 0x10000 && (can_rd || can_wr)) {
        lin = (g->base + off) & c->addr_mask;
        e = c->page[lin >> X86_PAGE_SHIFT];
        if ((e & ~(uintptr_t)0xFFF) + (lin & 0xFFF) != host + off) {
            break;
        }
        off += 0x1000 - (lin & 0xFFF);
        can_rd &= !(e & X86_NO_READ);
        can_wr &= !(e & X86_NO_WRITE);
        if (can_rd) {
            rd = off;
        }
        if (can_wr) {
            wr = off;
        }
    }
    g->host = host;
    g->rd_lim = rd < 0x10000 ? rd : 0x10000;
    g->wr_lim = wr < 0x10000 ? wr : 0x10000;
    w->base = g->base;
    w->gen = c->window_gen;
    w->host = g->host;
    w->rd_lim = g->rd_lim;
    w->wr_lim = g->wr_lim;
    seg_clamp(g);
}

/* A segment changed: its window waits for an access that misses it (CS's,
 * changing with every far jump and interrupt, is seldom wanted)
 */
static INLINE void seg_stale(x86_seg_t *g)
{
    g->rd_lim = g->wr_lim = 0;
    g->stale = true;
}

static void pm_load_seg(x86_t *c, int s, uint16_t sel);

static INLINE void load_seg(x86_t *c, int s, uint16_t sel)
{
    if (UNLIKELY(PE(c))) {
        pm_load_seg(c, s, sel);
        return;
    }
    /* Real mode: only the base changes (the 286 keeps its limits, which
     * LOADALL may have set: "unreal" mode)
     */
    real_seg(&c->seg[s], sel);
    seg_stale(&c->seg[s]);
    if (s == CS) {
        c->win_n = 0;
        c->resync = true;
    }
}

static void pm_x86_load_seg(x86_t *c, int s, uint16_t sel);

void x86_load_seg(x86_t *c, int s, uint16_t sel)
{
    if (PE(c) && !c->in_run) {
        pm_x86_load_seg(c, s, sel);
        return;
    }
    load_seg(c, s, sel);
}

static void full_seg(x86_seg_t *g, uint16_t sel, uint32_t base)
{
    g->sel = sel;
    g->base = base;
    g->rlo = g->wlo = 0;
    g->rhi = g->whi = 0xFFFF;
    g->access = 0x93;
}

/* ---- ModR/M ---- */

#define decode(c, q, m, so)     decode_(c, q, m, so, i286)

static INLINE void decode_(x86_t *c, code_t *q, modrm_t *m, int so, const bool i286)
{
    uint8_t b = fetch8(c, q);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3) {
        m->ea = 0;
        m->seg = DS;                /* (unused) */
        m->off = 0;
        return;
    }
    uint16_t off;
    uint8_t seg = DS, ea;
    switch (m->rm) {
    case 0: off = BX + SI; ea = 7; break;
    case 1: off = BX + DI; ea = 8; break;
    case 2: off = BP + SI; ea = 8; seg = SS; break;
    case 3: off = BP + DI; ea = 7; seg = SS; break;
    case 4: off = SI; ea = 5; break;
    case 5: off = DI; ea = 5; break;
    case 6:
        if (m->mod == 0) {
            off = fetch16(c, q);
            ea = 6;
        } else {
            off = BP;
            ea = 5;
            seg = SS;
        }
        break;
    default: off = BX; ea = 5; break;
    }
    if (m->mod == 1) {
        off += (uint16_t)(int8_t)fetch8(c, q);
        ea += 4;
    } else if (m->mod == 2) {
        off += fetch16(c, q);
        ea += 4;
    }
    if (so >= 0) {
        seg = (uint8_t)so;
        ea += 2;
    }
    if (i286) {
        ea = m->rm < 4 && m->mod != 0 ? 1 : 0;      /* base + index + displacement */
    }
    m->seg = seg;
    m->off = off;
    m->ea = ea;
}

static INLINE uint8_t get_rm8_(x86_t *c, const modrm_t *m, const bool i286)
{
    return m->mod == 3 ? *x86_reg8(c, m->rm) : rd8(c, m->seg, m->off);
}

static INLINE void set_rm8_(x86_t *c, const modrm_t *m, uint8_t v, const bool i286)
{
    if (m->mod == 3) {
        *x86_reg8(c, m->rm) = v;
    } else {
        wr8(c, m->seg, m->off, v);
    }
}

static INLINE uint16_t get_rm16_(x86_t *c, const modrm_t *m, const bool i286)
{
    return m->mod == 3 ? c->r.w[m->rm] : rd16(c, m->seg, m->off);
}

static INLINE void set_rm16_(x86_t *c, const modrm_t *m, uint16_t v, const bool i286)
{
    if (m->mod == 3) {
        c->r.w[m->rm] = v;
    } else {
        wr16(c, m->seg, m->off, v);
    }
}

/* Clocks of a register form, or a memory form plus its address */
#define T(m, reg, mem)  ((m).mod == 3 ? (reg) : (mem) + (m).ea)

/* ---- Arithmetic ---- */

static INLINE uint32_t alu(x86_t *c, int op, uint32_t a, uint32_t b, int w)
{
    uint32_t r;
    switch (op) {
    case 0: r = a + b; lazy(c, LF_ADD8 + w, r, a, b); break;
    case 1: r = a | b; lazy(c, LF_LOG8 + w, r, a, b); break;
    case 2: r = a + b + get_cf(c); lazy(c, LF_ADD8 + w, r, a, b); break;
    case 3: r = a - b - get_cf(c); lazy(c, LF_SUB8 + w, r, a, b); break;
    case 4: r = a & b; lazy(c, LF_LOG8 + w, r, a, b); break;
    case 6: r = a ^ b; lazy(c, LF_LOG8 + w, r, a, b); break;
    default: r = a - b; lazy(c, LF_SUB8 + w, r, a, b); break;     /* SUB, CMP */
    }
    return r;
}

static INLINE uint32_t inc(x86_t *c, uint32_t a, int w)
{
    c->flags = (c->flags & ~CF) | (get_cf(c) ? CF : 0);
    uint32_t r = a + 1;
    lazy(c, LF_INC8 + w, r, a, 1);
    return r;
}

static INLINE uint32_t dec(x86_t *c, uint32_t a, int w)
{
    c->flags = (c->flags & ~CF) | (get_cf(c) ? CF : 0);
    uint32_t r = a - 1;
    lazy(c, LF_DEC8 + w, r, a, 1);
    return r;
}

/* Shifts and rotates, a bit at a time as the 8086 does them (OF from the
 * last step); count 0 changes nothing
 */
static uint32_t shift(x86_t *c, int op, uint32_t v, unsigned count, int w)
{
    if (count == 0) {
        return v;
    }
    materialize(c);
    const uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    bool cf = c->flags & CF, of = c->flags & OF;
    uint16_t f = c->flags;
    switch (op) {
    case 0:     /* ROL */
        while (count--) {
            cf = (v & sign) != 0;
            v = ((v << 1) | cf) & mask;
        }
        of = ((v & sign) != 0) != cf;
        break;
    case 1:     /* ROR */
        while (count--) {
            cf = v & 1;
            v = (v >> 1) | (cf ? sign : 0);
        }
        of = ((v ^ (v << 1)) & sign) != 0;
        break;
    case 2:     /* RCL */
        while (count--) {
            bool out = (v & sign) != 0;
            v = ((v << 1) | cf) & mask;
            cf = out;
        }
        of = ((v & sign) != 0) != cf;
        break;
    case 3:     /* RCR */
        while (count--) {
            bool out = v & 1;
            v = (v >> 1) | (cf ? sign : 0);
            cf = out;
        }
        of = ((v ^ (v << 1)) & sign) != 0;
        break;
    case 6:     /* the 8088/8086's SETMO: all ones */
        if (c->model != X86_80286) {
            v = mask;
            cf = of = false;
            break;
        }
        __attribute__((fallthrough));       /* the 286's SAL */
    case 4:     /* SHL */
        while (count--) {
            cf = (v & sign) != 0;
            v = (v << 1) & mask;
        }
        of = ((v & sign) != 0) != cf;
        break;
    case 5:     /* SHR */
        while (count--) {
            cf = v & 1;
            of = (v & sign) != 0;
            v >>= 1;
        }
        break;
    default:    /* SAR */
        while (count--) {
            cf = v & 1;
            v = (v >> 1) | (v & sign);
        }
        of = false;
        break;
    }
    f &= ~(CF | OF);
    f |= (cf ? CF : 0) | (of ? OF : 0);
    c->flags = f;
    if (op >= 4) {
        /* AF: SHL's is bit 4 of the result (an ADD of the value to itself) */
        bool af = (op == 4 || (op == 6 && c->model == X86_80286)) && (v & 0x10);
        set_szp(c, v, w, af ? AF : 0, AF);
    }
    return v;
}

static void div_error(x86_t *c)
{
    if (c->model == X86_80286) {
        c->ip = c->ip0;             /* a fault: back to the DIV */
    }
    interrupt(c, 0);
}

/* The flags the 8088/8086's DIV and IDIV leave: their microcode's CORD
 * loop (as MartyPC, MIT, reproduces it) on the magnitudes. The last
 * subtraction that set flags, CF from the quotient's last rotation (DIV;
 * IDIV clears CF and OF afterwards). The results come from C's division.
 */
static void cord_flags(x86_t *c, uint32_t a, uint32_t lo, uint32_t b, int w)
{
    const uint32_t mask = w ? 0xFFFF : 0xFF;
    const int bits = w ? 16 : 8;
    bool carry = a < b;
    alu(c, 7, a, b, w);
    for (int n = bits; n > 0; n--) {
        bool out = (lo >> (bits - 1)) & 1;
        lo = ((lo << 1) | carry) & mask;
        carry = out;
        out = (a >> (bits - 1)) & 1;
        a = ((a << 1) | carry) & mask;
        carry = out;
        if (carry) {
            carry = false;
            a = (a - b) & mask;
        } else {
            alu(c, 7, a, b, w);
            carry = a < b;
            if (!carry) {
                a = (a - b) & mask;
            }
        }
    }
    bool out = (lo >> (bits - 1)) & 1;
    lo = ((lo << 1) | carry) & mask;
    carry = (lo >> (bits - 1)) & 1;
    (void)out;
    materialize(c);
    c->flags = (c->flags & ~CF) | (carry ? CF : 0);
}

/* IMUL's CF, OF and the others on the 8088/8086: the high half plus the
 * low half's sign bit (0 when the product fits), as its microcode adds them
 */
static void imul_flags(x86_t *c, uint32_t hi, uint32_t lo, int w)
{
    const uint32_t mask = w ? 0xFFFF : 0xFF;
    uint32_t sum = hi + ((lo >> (w ? 15 : 7)) & 1);
    bool af = ((hi ^ sum) & 0x10) != 0;
    bool big = (sum & mask) != 0;
    set_szp(c, sum, w, (af ? AF : 0) | (big ? CF | OF : 0), AF | CF | OF);
}

/* Clocks of MUL, IMUL, DIV, IDIV: the 8086's (register, memory), the 286's */
#define MD(reg, mem, t286)  (c->model == X86_80286 ? (t286) + (m->mod == 3 ? 0 : 3) : T(*m, reg, mem))

/* MUL, IMUL, DIV, IDIV (F6/F7 /4-/7). On the 8088 and 8086 a REP prefix
 * negates IMUL's product and IDIV's quotient (their microcode's sign flag)
 */
static void muldiv(x86_t *c, int op, uint16_t v, int w, const modrm_t *m, bool rep)
{
    bool big;
    const bool flip = rep && c->model != X86_80286;
    materialize(c);
    if (!w) {
        switch (op) {
        case 4: {
            uint16_t r = (uint16_t)(AL * (uint8_t)v);
            AX = r;
            c->clk += MD(73, 79, 13);
            set_szp(c, AH, 0, AH != 0 ? CF | OF : 0, CF | OF | AF);    /* SZP from the high half */
            return;
        }
        case 5: {
            int16_t r = (int16_t)((int8_t)AL * (int8_t)v);
            if (flip) {
                r = (int16_t)-r;
            }
            AX = (uint16_t)r;
            c->clk += MD(89, 95, 13);
            if (c->model != X86_80286) {
                imul_flags(c, AH, AL, 0);
                return;
            }
            big = r != (int8_t)r;
            break;
        }
        case 6: {
            c->clk += MD(85, 91, 14);
            if (AH >= (uint8_t)v) {
                /* The quotient won't fit (or v is 0): the 8088 found out by
                 * comparing AH with v, and its flags show it
                 */
                alu(c, 7, AH, (uint8_t)v, 0);
                div_error(c);
                return;
            }
            uint16_t a = AX;
            if (c->model != X86_80286) {
                cord_flags(c, AH, AL, (uint8_t)v, 0);
            }
            AL = (uint8_t)(a / (uint8_t)v);
            AH = (uint8_t)(a % (uint8_t)v);
            return;
        }
        default: {
            c->clk += MD(106, 112, 17);
            int16_t a = (int16_t)AX;
            int8_t d = (int8_t)v;
            int q = d ? a / d : 0, r = d ? a % d : 0;
            if (flip) {
                q = -q;
            }
            if (d == 0 || q > 127 || q < (c->model == X86_80286 ? -128 : -127)) {
                if (c->model != X86_80286) {
                    uint32_t ma = (uint32_t)(a < 0 ? -a : a), md = (uint32_t)(d < 0 ? -d : d);
                    if ((ma >> 8) >= md) {
                        alu(c, 7, ma >> 8, md, 0);      /* found by the first comparison */
                    } else {
                        cord_flags(c, ma >> 8, ma & 0xFF, md, 0);
                        c->flags &= ~CF;
                    }
                }
                div_error(c);
                return;
            }
            if (c->model != X86_80286) {
                uint32_t ma = (uint32_t)(a < 0 ? -a : a), md = (uint32_t)(d < 0 ? -d : d);
                cord_flags(c, ma >> 8, ma & 0xFF, md, 0);
                c->flags &= ~(CF | OF);
            }
            AL = (uint8_t)q;
            AH = (uint8_t)r;
            return;
        }
        }
        set_szp(c, AL, 0, big ? CF | OF : 0, CF | OF);
    } else {
        switch (op) {
        case 4: {
            uint32_t r = (uint32_t)AX * v;
            AX = (uint16_t)r;
            DX = (uint16_t)(r >> 16);
            c->clk += MD(125, 131, 21);
            set_szp(c, DX, 1, DX != 0 ? CF | OF : 0, CF | OF | AF);
            return;
        }
        case 5: {
            int32_t r = (int32_t)(int16_t)AX * (int16_t)v;
            if (flip) {
                r = -r;
            }
            AX = (uint16_t)r;
            DX = (uint16_t)((uint32_t)r >> 16);
            c->clk += MD(141, 147, 21);
            if (c->model != X86_80286) {
                imul_flags(c, DX, AX, 1);
                return;
            }
            big = r != (int16_t)r;
            break;
        }
        case 6: {
            c->clk += MD(153, 159, 22);
            uint32_t a = (uint32_t)DX << 16 | AX;
            if (DX >= v) {
                alu(c, 7, DX, v, 1);
                div_error(c);
                return;
            }
            if (c->model != X86_80286) {
                cord_flags(c, DX, AX, v, 1);
            }
            AX = (uint16_t)(a / v);
            DX = (uint16_t)(a % v);
            return;
        }
        default: {
            c->clk += MD(174, 180, 25);
            int32_t a = (int32_t)((uint32_t)DX << 16 | AX);
            int16_t d = (int16_t)v;
            int64_t q = d ? (int64_t)a / d : 0, r = d ? (int64_t)a % d : 0;
            if (flip) {
                q = -q;
            }
            if (d == 0 || q > 32767 || q < (c->model == X86_80286 ? -32768 : -32767)) {
                if (c->model != X86_80286) {
                    uint32_t ma = a < 0 ? (uint32_t)-(int64_t)a : (uint32_t)a;
                    uint32_t md = (uint32_t)(d < 0 ? -d : d);
                    if ((ma >> 16) >= md) {
                        alu(c, 7, ma >> 16, md, 1);
                    } else {
                        cord_flags(c, ma >> 16, ma & 0xFFFF, md, 1);
                        c->flags &= ~CF;
                    }
                }
                div_error(c);
                return;
            }
            if (c->model != X86_80286) {
                uint32_t ma = a < 0 ? (uint32_t)-(int64_t)a : (uint32_t)a;
                uint32_t md = (uint32_t)(d < 0 ? -d : d);
                cord_flags(c, ma >> 16, ma & 0xFFFF, md, 1);
                c->flags &= ~(CF | OF);
            }
            AX = (uint16_t)q;
            DX = (uint16_t)r;
            return;
        }
        }
        set_szp(c, AX, 1, big ? CF | OF : 0, CF | OF);
    }
}

/* DAA, DAS, AAA, AAS as the 8088 does them: the second step of DAA/DAS
 * looks at the original AL against 9F when AF was set (99 otherwise) and
 * CF comes from that step alone; the undefined flags (OF, and SF for
 * AAA/AAS) follow the chip as MartyPC (MIT) charts them by value
 */
static bool in(uint8_t v, uint8_t lo, uint8_t hi)
{
    return v >= lo && v <= hi;
}

static void daa(x86_t *c)
{
    materialize(c);
    uint8_t al = AL, old = AL;
    bool cf = c->flags & CF, af = c->flags & AF;
    if (c->model == X86_80286) {    /* the textbook rule */
        bool old_cf = cf;
        cf = false;
        if ((al & 0x0F) > 9 || af) {
            cf = old_cf || al > 0xF9;
            al += 6;
            af = true;
        } else {
            af = false;
        }
        if (old > 0x99 || old_cf) {
            al += 0x60;
            cf = true;
        } else {
            cf = false;
        }
        AL = al;
        set_szp(c, al, 0, (cf ? CF : 0) | (af ? AF : 0), CF | AF);
        return;
    }
    uint8_t limit = af ? 0x9F : 0x99;
    bool of = cf ? in(old, 0x1A, 0x7F) : in(old, 0x7A, 0x7F);
    if ((al & 0x0F) > 9 || af) {
        al += 6;
        af = true;
    }
    if (old > limit || cf) {
        al += 0x60;
        cf = true;
    } else {
        cf = false;
    }
    AL = al;
    set_szp(c, al, 0, (cf ? CF : 0) | (af ? AF : 0) | (of ? OF : 0), CF | AF | OF);
}

static void das(x86_t *c)
{
    materialize(c);
    uint8_t al = AL, old = AL;
    bool cf = c->flags & CF, af = c->flags & AF;
    if (c->model == X86_80286) {    /* the textbook rule */
        bool old_cf = cf;
        cf = false;
        if ((al & 0x0F) > 9 || af) {
            cf = old_cf || al < 6;
            al -= 6;
            af = true;
        } else {
            af = false;
        }
        if (old > 0x99 || old_cf) {
            al -= 0x60;
            cf = true;
        }
        AL = al;
        set_szp(c, al, 0, (cf ? CF : 0) | (af ? AF : 0), CF | AF);
        return;
    }
    uint8_t limit = af ? 0x9F : 0x99;
    bool of;
    if (!af) {
        of = cf ? in(old, 0x80, 0xDF) : in(old, 0x9A, 0xDF);
    } else {
        of = cf ? in(old, 0x80, 0xE5) : in(old, 0x80, 0x85) || in(old, 0xA0, 0xE5);
    }
    if ((al & 0x0F) > 9 || af) {
        al -= 6;
        af = true;
    }
    if (old > limit || cf) {
        al -= 0x60;
        cf = true;
    } else {
        cf = false;
    }
    AL = al;
    set_szp(c, al, 0, (cf ? CF : 0) | (af ? AF : 0) | (of ? OF : 0), CF | AF | OF);
}

static void aaa(x86_t *c)
{
    materialize(c);
    uint8_t old = AL;
    bool adjust = (AL & 0x0F) > 9 || (c->flags & AF);
    if (adjust) {
        if (c->model == X86_80286) {
            AX += 0x106;
        } else {
            AL += 6;
            AH += 1;
        }
    }
    uint8_t al = c->model == X86_80286 ? AL : (uint8_t)(old + (adjust ? 6 : 0));
    uint16_t f = adjust ? CF | AF : 0;
    f |= in(old, 0x7A, 0x7F) ? OF : 0;
    set_szp(c, al, 0, f, CF | AF | OF);
    if (c->model != X86_80286) {
        c->flags = (c->flags & ~SF) | (in(old, 0x7A, 0xF9) ? SF : 0);
    }
    AL &= 0x0F;
}

static void aas(x86_t *c)
{
    materialize(c);
    uint8_t old = AL;
    bool af = c->flags & AF;
    bool adjust = (AL & 0x0F) > 9 || af;
    if (adjust) {
        if (c->model == X86_80286) {
            AX -= 6;
            AH -= 1;
        } else {
            AL -= 6;
            AH -= 1;
        }
    }
    uint8_t al = c->model == X86_80286 ? AL : (uint8_t)(old - (adjust ? 6 : 0));
    uint16_t f = adjust ? CF | AF : 0;
    f |= af && in(old, 0x80, 0x85) ? OF : 0;
    set_szp(c, al, 0, f, CF | AF | OF);
    if (c->model != X86_80286) {
        bool sf = af ? (old <= 0x05 || old >= 0x86) : old >= 0x80;
        c->flags = (c->flags & ~SF) | (sf ? SF : 0);
    }
    AL &= 0x0F;
}

/* ---- I/O ---- */

#define IOPL_CHECK(c)   do { if (UNLIKELY(PE(c))) iopl_check(c); } while (0)
static void iopl_check(x86_t *c);

static uint8_t in8(x86_t *c, uint16_t port)
{
    IOPL_CHECK(c);
    c->busy += c->xfer;
    return c->bus.in(c->bus.ctx, port);
}

static void out8(x86_t *c, uint16_t port, uint8_t v)
{
    IOPL_CHECK(c);
    c->busy += c->xfer;
    c->bus.out(c->bus.ctx, port, v);
}

static uint16_t in16(x86_t *c, uint16_t port)
{
    IOPL_CHECK(c);
    c->clk += c->wait[port & 1];
    c->busy += 2 * c->xfer;
    if (c->bus.inw == NULL || c->model == X86_8088 || (port & 1)) {
        uint8_t lo = c->bus.in(c->bus.ctx, port);
        return lo | c->bus.in(c->bus.ctx, (uint16_t)(port + 1)) << 8;
    }
    return c->bus.inw(c->bus.ctx, port);
}

static void out16(x86_t *c, uint16_t port, uint16_t v)
{
    IOPL_CHECK(c);
    c->clk += c->wait[port & 1];
    c->busy += 2 * c->xfer;
    if (c->bus.outw == NULL || c->model == X86_8088 || (port & 1)) {
        c->bus.out(c->bus.ctx, port, (uint8_t)v);
        c->bus.out(c->bus.ctx, (uint16_t)(port + 1), (uint8_t)(v >> 8));
        return;
    }
    c->bus.outw(c->bus.ctx, port, v);
}

/* ---- Control transfer ---- */

/* IP set during an instruction: execute() leaves it so; the queue starts
 * empty
 */
static INLINE void set_ip(x86_t *c, uint16_t ip)
{
    c->ip = ip;
    c->flush = true;
    c->ip_set = true;
    c->resync = true;
    c->q_old_n = 0;                 /* (the 8088/8086's queue empties) */
}

/* IP past the instruction's bytes so far, and the bytes counted (before
 * calling what reads IP or sets it: interrupt(), string_op(), the trap)
 */
static INLINE void sync(x86_t *c, const code_t *q)
{
    c->ip = IP(q);
    c->fetched = (uint8_t)(c->ip - c->ip0);
}

/* A jump. The 286's to a place in the code window moves the code pointer
 * there, and the instruction ends as any other (IP from it); the others set
 * IP (the 8088/8086's prefetch queue starts empty)
 */
#define jump(c, q, ip)          jump_(c, q, ip, i286)
#define far_jump(c, q, cs, ip)  far_jump_(c, q, cs, ip, i286)

static INLINE void jump_(x86_t *c, code_t *q, uint16_t ip, const bool i286)
{
    if (i286 && LIKELY(ip - c->win_lo < c->win_n)) {
        q->base = c->win_base;
        q->p = q->base + ip;
        q->end = c->win_end;
        return;
    }
    c->fetched = (uint8_t)(IP(q) - c->ip0);
    set_ip(c, ip);
}

static INLINE void far_jump_(x86_t *c, code_t *q, uint16_t cs, uint16_t ip, const bool i286)
{
    load_seg(c, CS, cs);
    jump_(c, q, ip, i286);
}

static void pm_interrupt(x86_t *c, uint8_t n, bool soft, bool has_error, uint16_t error);

/* Interrupt n (exceptions, hardware, the BIOS's), real mode: FLAGS, CS and
 * IP on the stack, IF and TF off, the vector from the table at IDTR's base
 * (0 but for LIDT on the 286; beyond its limit: interrupt 8, the double
 * fault, and beyond that a shutdown). Protected mode: through the IDT.
 */
static NOINLINE void interrupt(x86_t *c, uint8_t n)
{
    const bool i286 = c->model == X86_80286;
    if (i286 && PE(c)) {
        pm_interrupt(c, n, false, false, 0);
        return;
    }
    if (i286 && c->in_run && n * 4u + 3 > c->idtr.limit) {
        fault(c, 8);
    }
    push16(c, x86_get_flags(c));
    c->flags &= ~(IF | TF);
    push16(c, c->seg[CS].sel);
    push16(c, c->ip);
    uint32_t v = c->idtr.base + n * 4u;
    uint16_t ip = mem_rd16(c, v);
    load_seg(c, CS, mem_rd16(c, v + 2));
    set_ip(c, ip);
}

void x86_interrupt(x86_t *c, uint8_t n)
{
    interrupt(c, n);
}

/* INT n, INT 3, INTO: in protected mode the gate's DPL is checked */
static NOINLINE void soft_interrupt(x86_t *c, uint8_t n)
{
    if (PE(c)) {
        pm_interrupt(c, n, true, false, 0);
        return;
    }
    interrupt(c, n);
}

static INLINE void jcc(x86_t *c, code_t *q, int cc, const bool i286)
{
    int8_t d = (int8_t)fetch8(c, q);
    if (cond(c, cc)) {
        jump(c, q, (uint16_t)(IP(q) + d));
        c->clk += i286 ? 7 : 16;
    } else {
        c->clk += i286 ? 3 : 4;
    }
}

/* An undefined opcode on the 286: #UD, back to the instruction */
static void __attribute__((noreturn)) undefined(x86_t *c)
{
    fault(c, 6);
}

/* ---- Protected mode (the 286) ----
 *
 * Out of the run loops: segment loads, far transfers, interrupts and task
 * switches check everything before they change anything (a fault leaves
 * the instruction as it found it), but a task switch, whose faults after
 * the new task's registers are in belong to the new task.
 */

typedef struct {
    uint32_t addr;                  /* where it is (linear) */
    uint16_t w0;                    /* limit; a gate's offset */
    uint16_t w1;                    /* base 0-15; a gate's selector */
    uint8_t b4;                     /* base 16-23; a call gate's word count */
    uint8_t access;
} desc_t;

/* System descriptor types (access & 1F) */
enum {
    T_TSS = 0x01, T_LDT = 0x02, T_TSS_BUSY = 0x03, T_CALL_GATE = 0x04,
    T_TASK_GATE = 0x05, T_INT_GATE = 0x06, T_TRAP_GATE = 0x07,
};

enum { SW_JMP, SW_CALL, SW_IRET, SW_INT };

static inline uint32_t desc_base(const desc_t *d)
{
    return d->w1 | (uint32_t)d->b4 << 16;
}

static void read_desc_at(x86_t *c, uint32_t addr, desc_t *d)
{
    d->addr = addr;
    d->w0 = mem_rd16(c, addr);
    d->w1 = mem_rd16(c, addr + 2);
    d->b4 = mem_rd8(c, addr + 4);
    d->access = mem_rd8(c, addr + 5);
}

/* The descriptor a selector names, false beyond its table (the GDT, or
 * the LDT: none if LDTR is null)
 */
static bool read_desc(x86_t *c, uint16_t sel, desc_t *d)
{
    uint32_t base, limit;
    if (sel & 4) {
        if (c->ldtr.rlo > c->ldtr.rhi) {
            return false;
        }
        base = c->ldtr.base;
        limit = c->ldtr.rhi;
    } else {
        base = c->gdtr.base;
        limit = c->gdtr.limit;
    }
    uint32_t i = sel & 0xFFF8u;
    if (i + 7 > limit) {
        return false;
    }
    read_desc_at(c, base + i, d);
    return true;
}

static void null_seg(x86_seg_t *g, uint16_t sel)
{
    g->sel = sel;
    g->base = 0;
    g->rlo = g->wlo = 1;            /* (no offset: every access faults) */
    g->rhi = g->whi = 0;
    g->access = 0;
    seg_stale(g);
}

/* A segment register from its descriptor (marked accessed) */
static void set_seg(x86_t *c, x86_seg_t *g, uint16_t sel, desc_t *d)
{
    if (!(d->access & 1)) {
        d->access |= 1;
        mem_wr8(c, d->addr + 5, d->access);
    }
    g->sel = sel;
    g->base = desc_base(d);
    g->access = d->access;
    uint16_t lim = d->w0;
    if ((d->access & (A_CODE | A_DOWN)) == A_DOWN) {
        g->rlo = lim == 0xFFFF ? 1 : (uint16_t)(lim + 1);  /* expand-down: above the limit */
        g->rhi = lim == 0xFFFF ? 0 : 0xFFFF;
    } else {
        g->rlo = 0;
        g->rhi = lim;
    }
    if (!(d->access & A_CODE) && (d->access & A_RW)) {
        g->wlo = g->rlo;
        g->whi = g->rhi;
    } else {
        g->wlo = 1;
        g->whi = 0;
    }
    seg_stale(g);
}

/* CS, at privilege level rpl (the new CPL) */
static void set_cs(x86_t *c, uint16_t sel, desc_t *d, int rpl)
{
    set_seg(c, &c->seg[CS], (uint16_t)(SEL(sel) | rpl), d);
    c->cpl = (uint8_t)rpl;
    c->win_n = 0;
    c->resync = true;
}

/* Words at off in segment g, inside its limits */
static inline bool can_read16(const x86_seg_t *g, uint16_t off)
{
    return off >= g->rlo && off < g->rhi;
}

static inline bool can_write16(const x86_seg_t *g, uint16_t off)
{
    return off >= g->wlo && off < g->whi;
}

/* n words pushed below sp, popped from sp: inside the stack's limits */
static bool push_room(const x86_seg_t *g, uint16_t sp, int n)
{
    for (int i = 1; i <= n; i++) {
        if (!can_write16(g, (uint16_t)(sp - 2 * i))) {
            return false;
        }
    }
    return true;
}

static bool pop_room(const x86_seg_t *g, uint16_t sp, int n)
{
    for (int i = 0; i < n; i++) {
        if (!can_read16(g, (uint16_t)(sp + 2 * i))) {
            return false;
        }
    }
    return true;
}

static inline uint16_t stack_rd(x86_t *c, uint16_t off)
{
    return mem_rd16(c, c->seg[SS].base + off);
}

/* A selector checked as the stack for privilege level `level` (`vec` its
 * fault: 10 from a TSS, 13 on a return)
 */
static void check_stack(x86_t *c, uint16_t sel, int level, int vec, desc_t *d)
{
    if (SEL(sel) == 0) {
        fault_err(c, vec, 0);
    }
    if (!read_desc(c, sel, d) || (sel & 3) != level || A_DPL(d->access) != level ||
        (d->access & (A_SEG | A_CODE | A_RW)) != (A_SEG | A_RW)) {
        fault_err(c, vec, SEL(sel));
    }
    if (!(d->access & A_PRESENT)) {
        fault_err(c, 12, SEL(sel));
    }
}

/* A code segment to transfer to: its descriptor (vec: 13, or 10 in a task
 * switch), present
 */
static void code_desc(x86_t *c, uint16_t sel, desc_t *d, int vec)
{
    if (SEL(sel) == 0) {
        fault_err(c, vec, 0);
    }
    if (!read_desc(c, sel, d) || (d->access & (A_SEG | A_CODE)) != (A_SEG | A_CODE)) {
        fault_err(c, vec, SEL(sel));
    }
}

static void present(x86_t *c, const desc_t *d, uint16_t sel)
{
    if (!(d->access & A_PRESENT)) {
        fault_err(c, 11, SEL(sel));
    }
}

/* MOV, POP, LDS, LES (and x86_load_seg()) in protected mode. CS: as a far
 * JMP to a code segment would load it (the BIOS's INT 15h AH=89h).
 */
static NOINLINE void pm_load_seg(x86_t *c, int s, uint16_t sel)
{
    x86_seg_t *g = &c->seg[s];
    const int rpl = sel & 3, cpl = c->cpl;
    desc_t d;
    if (SEL(sel) == 0) {
        if (s == SS || s == CS) {
            fault(c, 13);
        }
        null_seg(g, sel);
        return;
    }
    if (!read_desc(c, sel, &d)) {
        fault_err(c, 13, SEL(sel));
    }
    const int dpl = A_DPL(d.access);
    if (s == CS) {
        if ((d.access & (A_SEG | A_CODE)) != (A_SEG | A_CODE) ||
            ((d.access & A_CONFORM) ? dpl > cpl : (rpl > cpl || dpl != cpl))) {
            fault_err(c, 13, SEL(sel));
        }
        present(c, &d, sel);
        set_cs(c, sel, &d, cpl);
        return;
    }
    if (s == SS) {
        if (rpl != cpl || dpl != cpl || (d.access & (A_SEG | A_CODE | A_RW)) != (A_SEG | A_RW)) {
            fault_err(c, 13, SEL(sel));
        }
        if (!(d.access & A_PRESENT)) {
            fault_err(c, 12, SEL(sel));
        }
    } else {
        /* data, or readable code; DPL at least CPL and RPL but for conforming code */
        if (!(d.access & A_SEG) || (d.access & (A_CODE | A_RW)) == A_CODE ||
            ((d.access & (A_CODE | A_CONFORM)) != (A_CODE | A_CONFORM) && (rpl > dpl || cpl > dpl))) {
            fault_err(c, 13, SEL(sel));
        }
        present(c, &d, sel);
    }
    set_seg(c, g, sel, &d);
    c->clk += 15;                   /* (protected mode's 17 against real mode's 2) */
}

/* Returning to an outer level: data segments the new CPL may not use go */
static void null_outer(x86_t *c, int s)
{
    x86_seg_t *g = &c->seg[s];
    bool conforming = (g->access & (A_SEG | A_CODE | A_CONFORM)) == (A_SEG | A_CODE | A_CONFORM);
    if (!conforming && A_DPL(g->access) < c->cpl) {
        null_seg(g, 0);
    }
}

/* FLAGS from POPF and IRET: IOPL changes only at CPL 0, IF only at CPL up
 * to IOPL
 */
static void set_pm_flags(x86_t *c, uint16_t f, int cpl)
{
    uint16_t keep = 0;
    if (cpl > 0) {
        keep |= X86_IOPL;
    }
    if (cpl > ((c->flags & X86_IOPL) >> 12)) {
        keep |= IF;
    }
    c->flags = (uint16_t)((f & 0x7FD5 & ~keep) | (c->flags & keep));
    c->lf_op = LF_NONE;
}

/* The inner stack for privilege level `level`, from the TSS */
static void tss_stack(x86_t *c, int level, uint16_t *ss, uint16_t *sp)
{
    uint16_t at = (uint16_t)(2 + level * 4);
    if (at + 3u > c->tr.rhi || c->tr.rlo > c->tr.rhi) {
        fault_err(c, 10, SEL(c->tr.sel));
    }
    *sp = mem_rd16(c, c->tr.base + at);
    *ss = mem_rd16(c, c->tr.base + at + 2);
}

static void load_ldt(x86_t *c, uint16_t sel, int vec)
{
    if (SEL(sel) == 0) {
        null_seg(&c->ldtr, sel);    /* no LDT */
        return;
    }
    desc_t d;
    if ((sel & 4) || !read_desc(c, sel, &d) || (d.access & 0x1F) != T_LDT) {
        fault_err(c, vec, SEL(sel));
    }
    if (!(d.access & A_PRESENT)) {
        fault_err(c, vec == 13 ? 11 : 10, SEL(sel));
    }
    c->ldtr.sel = sel;
    c->ldtr.base = desc_base(&d);
    c->ldtr.rlo = 0;
    c->ldtr.rhi = d.w0;
    c->ldtr.access = d.access;
}

/* A task switch to the TSS sel describes (JMP, CALL, IRET, an interrupt):
 * the state into the current TSS (IP: c->ip), then the new one's. Faults
 * after the new registers are in are the new task's.
 */
static NOINLINE void task_switch(x86_t *c, uint16_t sel, desc_t *d, int kind)
{
    const int type = d->access & 0x1F;
    if ((sel & 4) || type != (kind == SW_IRET ? T_TSS_BUSY : T_TSS)) {
        fault_err(c, kind == SW_IRET ? 10 : 13, SEL(sel));
    }
    present(c, d, sel);
    if (d->w0 < 0x2B) {
        fault_err(c, 10, SEL(sel));
    }
    if (c->tr.rhi < 0x29 || c->tr.rlo > c->tr.rhi) {
        fault_err(c, 10, SEL(c->tr.sel));       /* the current TSS can't hold the state */
    }
    const uint32_t ob = c->tr.base, nb = desc_base(d);
    uint16_t fl = x86_get_flags(c);
    if (kind == SW_IRET) {
        fl &= ~X86_NT;
    }
    mem_wr16(c, ob + 0x0E, c->ip);
    mem_wr16(c, ob + 0x10, fl);
    for (int i = 0; i < 8; i++) {
        mem_wr16(c, ob + 0x12 + 2u * i, c->r.w[i]);
    }
    for (int s = 0; s < 4; s++) {
        mem_wr16(c, ob + 0x22 + 2u * s, c->seg[s].sel);
    }
    if (kind == SW_JMP || kind == SW_IRET) {
        desc_t od;
        if (read_desc(c, c->tr.sel, &od)) {
            mem_wr8(c, od.addr + 5, (uint8_t)(od.access & ~0x02));  /* not busy */
        }
    }
    if (kind == SW_CALL || kind == SW_INT) {
        mem_wr16(c, nb, c->tr.sel);                 /* the back link */
    }
    if (kind != SW_IRET) {
        d->access |= 0x02;
        mem_wr8(c, d->addr + 5, d->access);         /* busy */
    }
    c->tr.sel = sel;
    c->tr.base = nb;
    c->tr.rlo = 0;
    c->tr.rhi = d->w0;
    c->tr.access = d->access;
    c->msw |= X86_MSW_TS;

    /* The new task */
    uint16_t nfl = mem_rd16(c, nb + 0x10);
    if (kind == SW_CALL || kind == SW_INT) {
        nfl |= X86_NT;
    }
    for (int i = 0; i < 8; i++) {
        c->r.w[i] = mem_rd16(c, nb + 0x12 + 2u * i);
    }
    uint16_t sels[4];
    for (int s = 0; s < 4; s++) {
        sels[s] = mem_rd16(c, nb + 0x22 + 2u * s);
    }
    uint16_t ldt = mem_rd16(c, nb + 0x2A);
    c->flags = nfl & 0x7FD5;
    c->lf_op = LF_NONE;
    set_ip(c, mem_rd16(c, nb + 0x0E));
    c->ip0 = c->ip;
    c->sp0 = SP;
    for (int s = 0; s < 4; s++) {
        c->seg[s].sel = sels[s];
    }
    load_ldt(c, ldt, 10);
    desc_t cd;
    code_desc(c, sels[CS], &cd, 10);
    int rpl = sels[CS] & 3, dpl = A_DPL(cd.access);
    if ((cd.access & A_CONFORM) ? dpl > rpl : dpl != rpl) {
        fault_err(c, 10, SEL(sels[CS]));
    }
    present(c, &cd, sels[CS]);
    set_cs(c, sels[CS], &cd, rpl);
    desc_t sd;
    check_stack(c, sels[SS], rpl, 10, &sd);
    set_seg(c, &c->seg[SS], sels[SS], &sd);
    for (int s = 0; s < 4; s += 3) {                /* ES, DS */
        if (SEL(sels[s]) == 0) {
            null_seg(&c->seg[s], sels[s]);
            continue;
        }
        desc_t dd;
        int drpl = sels[s] & 3, ddpl;
        if (!read_desc(c, sels[s], &dd) || !(dd.access & A_SEG) || (dd.access & (A_CODE | A_RW)) == A_CODE) {
            fault_err(c, 10, SEL(sels[s]));
        }
        ddpl = A_DPL(dd.access);
        if ((dd.access & (A_CODE | A_CONFORM)) != (A_CODE | A_CONFORM) && (drpl > ddpl || rpl > ddpl)) {
            fault_err(c, 10, SEL(sels[s]));
        }
        present(c, &dd, sels[s]);
        set_seg(c, &c->seg[s], sels[s], &dd);
    }
    if (c->ip > c->seg[CS].rhi) {
        fault(c, 13);
    }
}

/* A gate's or TSS's target task, for JMP, CALL and interrupts */
static void task_gate(x86_t *c, const desc_t *gate, int kind)
{
    uint16_t sel = gate->w1;
    desc_t d;
    if ((sel & 4) || !read_desc(c, sel, &d)) {
        fault_err(c, 13, SEL(sel));
    }
    task_switch(c, sel, &d, kind);
}

/* JMP far and CALL far: to a code segment, through a call gate, or to a
 * task (a TSS, a task gate). `call`: CS and the return IP (`ret`) pushed.
 */
static NOINLINE void pm_far(x86_t *c, uint16_t sel, uint16_t off, bool call, uint16_t ret)
{
    const int cpl = c->cpl, rpl = sel & 3;
    desc_t d;
    if (SEL(sel) == 0) {
        fault(c, 13);
    }
    if (!read_desc(c, sel, &d)) {
        fault_err(c, 13, SEL(sel));
    }
    const int dpl = A_DPL(d.access);
    x86_seg_t *ss = &c->seg[SS];
    if (d.access & A_SEG) {
        if (!(d.access & A_CODE) || ((d.access & A_CONFORM) ? dpl > cpl : (rpl > cpl || dpl != cpl))) {
            fault_err(c, 13, SEL(sel));
        }
        present(c, &d, sel);
        if (call && !push_room(ss, SP, 2)) {
            fault(c, 12);
        }
        if (off > d.w0) {
            fault(c, 13);
        }
        if (call) {
            mem_wr16(c, ss->base + (uint16_t)(SP - 2), c->seg[CS].sel);
            mem_wr16(c, ss->base + (uint16_t)(SP - 4), ret);
            SP -= 4;
        }
        set_cs(c, sel, &d, cpl);
        set_ip(c, off);
        c->clk += call ? 26 : 23;
        return;
    }
    const int type = d.access & 0x1F;
    if (type != T_CALL_GATE && type != T_TASK_GATE && type != T_TSS) {
        fault_err(c, 13, SEL(sel));
    }
    if (dpl < cpl || dpl < rpl) {
        fault_err(c, 13, SEL(sel));
    }
    present(c, &d, sel);
    if (type != T_CALL_GATE) {
        c->ip = call ? ret : c->ip;
        if (type == T_TSS) {
            task_switch(c, sel, &d, call ? SW_CALL : SW_JMP);
        } else {
            task_gate(c, &d, call ? SW_CALL : SW_JMP);
        }
        c->clk += 180;
        return;
    }
    /* The call gate's code segment */
    const uint16_t tsel = d.w1, toff = d.w0;
    desc_t t;
    code_desc(c, tsel, &t, 13);
    const int tdpl = A_DPL(t.access);
    const bool conforming = t.access & A_CONFORM;
    if (call ? tdpl > cpl : (conforming ? tdpl > cpl : tdpl != cpl)) {
        fault_err(c, 13, SEL(tsel));
    }
    present(c, &t, tsel);
    if (!call || conforming || tdpl == cpl) {
        if (call && !push_room(ss, SP, 2)) {
            fault(c, 12);
        }
        if (toff > t.w0) {
            fault(c, 13);
        }
        if (call) {
            mem_wr16(c, ss->base + (uint16_t)(SP - 2), c->seg[CS].sel);
            mem_wr16(c, ss->base + (uint16_t)(SP - 4), ret);
            SP -= 4;
        }
        set_cs(c, tsel, &t, cpl);
        set_ip(c, toff);
        c->clk += call ? 41 : 38;
        return;
    }
    /* To a more privileged level: its stack from the TSS, the parameters
     * copied over
     */
    uint16_t nss, nsp;
    tss_stack(c, tdpl, &nss, &nsp);
    desc_t sd;
    check_stack(c, nss, tdpl, 10, &sd);
    const int words = d.b4 & 0x1F;
    x86_seg_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.access = sd.access;
    {
        desc_t tmp = sd;
        tmp.access |= 1;            /* (limits only: set_seg() marks it later) */
        uint16_t lim = tmp.w0;
        if (tmp.access & A_DOWN) {
            ns.wlo = lim == 0xFFFF ? 1 : (uint16_t)(lim + 1);
            ns.whi = lim == 0xFFFF ? 0 : 0xFFFF;
        } else {
            ns.wlo = 0;
            ns.whi = lim;
        }
    }
    if (!push_room(&ns, nsp, 4 + words)) {
        fault_err(c, 12, SEL(nss));
    }
    if (!pop_room(ss, SP, words)) {
        fault(c, 12);
    }
    if (toff > t.w0) {
        fault(c, 13);
    }
    uint16_t params[32];
    for (int i = 0; i < words; i++) {
        params[i] = stack_rd(c, (uint16_t)(SP + 2 * i));
    }
    const uint16_t oss = ss->sel, osp = SP, ocs = c->seg[CS].sel;
    const uint32_t nb = desc_base(&sd);
    uint16_t p = nsp;
    p -= 2; mem_wr16(c, nb + p, oss);
    p -= 2; mem_wr16(c, nb + p, osp);
    for (int i = words - 1; i >= 0; i--) {
        p -= 2;
        mem_wr16(c, nb + p, params[i]);
    }
    p -= 2; mem_wr16(c, nb + p, ocs);
    p -= 2; mem_wr16(c, nb + p, ret);
    set_seg(c, ss, nss, &sd);
    SP = p;
    set_cs(c, tsel, &t, tdpl);
    set_ip(c, toff);
    c->clk += 82 + 4 * words;
}

/* The return CS checked (RETF, IRET): its descriptor */
static void return_cs(x86_t *c, uint16_t csel, desc_t *d)
{
    code_desc(c, csel, d, 13);
    const int rpl = csel & 3, dpl = A_DPL(d->access);
    if (rpl < c->cpl || ((d->access & A_CONFORM) ? dpl > rpl : dpl != rpl)) {
        fault_err(c, 13, SEL(csel));
    }
    present(c, d, csel);
}

/* RETF n, and IRET (the flags) in protected mode */
static NOINLINE void pm_return(x86_t *c, uint16_t imm, bool iret)
{
    x86_seg_t *ss = &c->seg[SS];
    if (iret && (c->flags & X86_NT)) {
        /* Back to the task that called this one */
        uint16_t link = mem_rd16(c, c->tr.base);
        desc_t d;
        if ((link & 4) || !read_desc(c, link, &d)) {
            fault_err(c, 10, SEL(link));
        }
        task_switch(c, link, &d, SW_IRET);
        c->clk += 169;
        return;
    }
    const int n = iret ? 3 : 2;
    if (!pop_room(ss, SP, n)) {
        fault(c, 12);
    }
    const uint16_t ip = stack_rd(c, SP), csel = stack_rd(c, (uint16_t)(SP + 2));
    const uint16_t fl = iret ? stack_rd(c, (uint16_t)(SP + 4)) : 0;
    desc_t d;
    return_cs(c, csel, &d);
    const int cpl = c->cpl, rpl = csel & 3;
    if (rpl == cpl) {
        if (ip > d.w0) {
            fault(c, 13);
        }
        SP += (uint16_t)(2 * n + imm);
        set_cs(c, csel, &d, rpl);
        set_ip(c, ip);
        if (iret) {
            set_pm_flags(c, fl, cpl);
        }
        c->clk += iret ? 31 : 25;
        return;
    }
    /* To an outer level: its SS:SP from the stack too */
    const uint16_t at = (uint16_t)(SP + 2 * n + imm);
    if (!pop_room(ss, at, 2)) {
        fault(c, 12);
    }
    const uint16_t nsp = stack_rd(c, at), nss = stack_rd(c, (uint16_t)(at + 2));
    desc_t sd;
    check_stack(c, nss, rpl, 13, &sd);
    if (ip > d.w0) {
        fault(c, 13);
    }
    if (iret) {
        set_pm_flags(c, fl, cpl);
    }
    set_cs(c, csel, &d, rpl);
    set_seg(c, ss, nss, &sd);
    SP = (uint16_t)(nsp + (iret ? 0 : imm));
    set_ip(c, ip);
    null_outer(c, ES);
    null_outer(c, DS);
    c->clk += 55;
}

/* An interrupt in protected mode, through the IDT's gate: software ones
 * (INT n, INT 3, INTO) check the gate's DPL; exceptions may push an error
 * code. The return IP is c->ip.
 */
static NOINLINE void pm_interrupt(x86_t *c, uint8_t n, bool soft, bool has_error, uint16_t error)
{
    const uint16_t idt_err = (uint16_t)(n * 8 + 2);
    if (n * 8u + 7 > c->idtr.limit) {
        fault_err(c, 13, idt_err);
    }
    desc_t g;
    read_desc_at(c, c->idtr.base + n * 8u, &g);
    const int type = g.access & 0x1F, cpl = c->cpl;
    if (type != T_TASK_GATE && type != T_INT_GATE && type != T_TRAP_GATE) {
        fault_err(c, 13, idt_err);
    }
    if (soft && A_DPL(g.access) < cpl) {
        fault_err(c, 13, idt_err);
    }
    if (!(g.access & A_PRESENT)) {
        fault_err(c, 11, idt_err);
    }
    if (type == T_TASK_GATE) {
        task_gate(c, &g, SW_INT);
        if (has_error) {
            const bool i286 = true;
            push16(c, error);
        }
        c->clk += 167;
        return;
    }
    const uint16_t tsel = g.w1, toff = g.w0;
    desc_t t;
    code_desc(c, tsel, &t, 13);
    const int tdpl = A_DPL(t.access);
    if (tdpl > cpl) {
        fault_err(c, 13, SEL(tsel));
    }
    present(c, &t, tsel);
    const uint16_t fl = x86_get_flags(c);
    const int words = 3 + (has_error ? 1 : 0);
    x86_seg_t *ss = &c->seg[SS];
    if ((t.access & A_CONFORM) || tdpl == cpl) {
        if (!push_room(ss, SP, words)) {
            fault(c, 12);
        }
        if (toff > t.w0) {
            fault(c, 13);
        }
        uint16_t p = SP;
        p -= 2; mem_wr16(c, ss->base + p, fl);
        p -= 2; mem_wr16(c, ss->base + p, c->seg[CS].sel);
        p -= 2; mem_wr16(c, ss->base + p, c->ip);
        if (has_error) {
            p -= 2;
            mem_wr16(c, ss->base + p, error);
        }
        SP = p;
        set_cs(c, tsel, &t, cpl);
        c->clk += 40;
    } else {
        uint16_t nss, nsp;
        tss_stack(c, tdpl, &nss, &nsp);
        desc_t sd;
        check_stack(c, nss, tdpl, 10, &sd);
        uint16_t lim = sd.w0, wlo, whi;
        if (sd.access & A_DOWN) {
            wlo = lim == 0xFFFF ? 1 : (uint16_t)(lim + 1);
            whi = lim == 0xFFFF ? 0 : 0xFFFF;
        } else {
            wlo = 0;
            whi = lim;
        }
        for (int i = 1; i <= words + 2; i++) {
            uint16_t o = (uint16_t)(nsp - 2 * i);
            if (o < wlo || o >= whi) {
                fault_err(c, 12, SEL(nss));
            }
        }
        if (toff > t.w0) {
            fault(c, 13);
        }
        const uint32_t nb = desc_base(&sd);
        uint16_t p = nsp;
        p -= 2; mem_wr16(c, nb + p, ss->sel);
        p -= 2; mem_wr16(c, nb + p, SP);
        p -= 2; mem_wr16(c, nb + p, fl);
        p -= 2; mem_wr16(c, nb + p, c->seg[CS].sel);
        p -= 2; mem_wr16(c, nb + p, c->ip);
        if (has_error) {
            p -= 2;
            mem_wr16(c, nb + p, error);
        }
        set_seg(c, ss, nss, &sd);
        SP = p;
        set_cs(c, tsel, &t, tdpl);
        c->clk += 78;
    }
    c->flags &= ~(TF | X86_NT);
    if (type == T_INT_GATE) {
        c->flags &= ~IF;
    }
    set_ip(c, toff);
}

/* IN, OUT, INS, OUTS, CLI, STI in protected mode: CPL up to IOPL */
static NOINLINE void iopl_check(x86_t *c)
{
    if (c->cpl > ((c->flags & X86_IOPL) >> 12)) {
        fault(c, 13);
    }
}

/* The system instructions: CPL 0 in protected mode */
static void cpl0(x86_t *c)
{
    if (PE(c) && c->cpl != 0) {
        fault(c, 13);
    }
}

/* LAR, LSL, VERR, VERW: the descriptor if the selector may see it (ZF) */
static bool visible(x86_t *c, uint16_t sel, desc_t *d, int which)
{
    if (SEL(sel) == 0 || !read_desc(c, sel, d)) {
        return false;
    }
    const uint8_t a = d->access;
    if (a & A_SEG) {
        bool conforming = (a & (A_CODE | A_CONFORM)) == (A_CODE | A_CONFORM);
        if (!conforming && (A_DPL(a) < c->cpl || A_DPL(a) < (sel & 3))) {
            return false;
        }
        switch (which) {
        case 2: return !(a & A_CODE) || (a & A_RW);                 /* VERR */
        case 3: return !(a & A_CODE) && (a & A_RW);                 /* VERW */
        default: return true;
        }
    }
    if (which >= 2 || A_DPL(a) < c->cpl || A_DPL(a) < (sel & 3)) {
        return false;
    }
    int type = a & 0x1F;
    if (which == 1) {                                               /* LSL */
        return type == T_TSS || type == T_LDT || type == T_TSS_BUSY;
    }
    return type >= T_TSS && type <= T_TASK_GATE;                    /* LAR */
}

static void set_zf(x86_t *c, bool on)
{
    materialize(c);
    c->flags = (uint16_t)((c->flags & ~ZF) | (on ? ZF : 0));
}

/* A descriptor cache as LOADALL has it: base (24 bits), access, limit */
static void loadall_seg(x86_t *c, x86_seg_t *g, uint32_t at)
{
    desc_t d;
    d.addr = 0;
    d.w1 = mem_rd16(c, at);
    d.b4 = mem_rd8(c, at + 2);
    d.access = (uint8_t)(mem_rd8(c, at + 3) | 1);   /* (accessed: set_seg() writes nothing) */
    d.w0 = mem_rd16(c, at + 4);
    uint16_t sel = g->sel;
    set_seg(c, g, sel, &d);
}

/* x86_load_seg() between runs (the debugger): no faults */
static void pm_x86_load_seg(x86_t *c, int s, uint16_t sel)
{
    desc_t d;
    if (SEL(sel) != 0 && read_desc(c, sel, &d) && (d.access & A_SEG)) {
        if (s == CS) {
            set_cs(c, sel, &d, sel & 3);
        } else {
            set_seg(c, &c->seg[s], sel, &d);
        }
    } else {
        null_seg(&c->seg[s], sel);
    }
}

/* LOADALL (0F 05): every register, the descriptor caches too, from 800h */
static NOINLINE void loadall(x86_t *c)
{
    c->msw = (uint16_t)((c->msw & X86_MSW_PE) | mem_rd16(c, 0x806));
    c->tr.sel = mem_rd16(c, 0x816);
    uint16_t fl = mem_rd16(c, 0x818);
    uint16_t ip = mem_rd16(c, 0x81A);
    c->ldtr.sel = mem_rd16(c, 0x81C);
    c->seg[DS].sel = mem_rd16(c, 0x81E);
    c->seg[SS].sel = mem_rd16(c, 0x820);
    c->seg[CS].sel = mem_rd16(c, 0x822);
    c->seg[ES].sel = mem_rd16(c, 0x824);
    static const uint8_t order[8] = { X86_DI, X86_SI, X86_BP, X86_SP, X86_BX, X86_DX, X86_CX, X86_AX };
    for (int i = 0; i < 8; i++) {
        c->r.w[order[i]] = mem_rd16(c, 0x826 + 2u * i);
    }
    loadall_seg(c, &c->seg[ES], 0x836);
    loadall_seg(c, &c->seg[CS], 0x83C);
    loadall_seg(c, &c->seg[SS], 0x842);
    loadall_seg(c, &c->seg[DS], 0x848);
    c->gdtr.base = mem_rd16(c, 0x84E) | (uint32_t)mem_rd8(c, 0x850) << 16;
    c->gdtr.limit = mem_rd16(c, 0x852);
    loadall_seg(c, &c->ldtr, 0x854);
    c->idtr.base = mem_rd16(c, 0x85A) | (uint32_t)mem_rd8(c, 0x85C) << 16;
    c->idtr.limit = mem_rd16(c, 0x85E);
    loadall_seg(c, &c->tr, 0x860);
    c->flags = fl & (PE(c) ? 0x7FD5 : 0x0FD5);
    c->lf_op = LF_NONE;
    c->cpl = PE(c) ? (uint8_t)A_DPL(c->seg[CS].access) : 0;
    c->win_n = 0;
    set_ip(c, ip);
}

/* ---- String instructions ---- */

/* An interrupt to take now (the string instructions look between steps) */
static inline bool interrupt_due(const x86_t *c)
{
    return c->nmi || (c->intr && (c->flags & IF) && !c->inhibit);
}

/* REP MOVS and REP STOS on the 286, forward, inside the segments' direct
 * windows: many elements at once, each with the clocks string_op()'s loop
 * counts (none of them can fault or reach the bus, and no interrupt comes
 * due between them), up to the run's end (x86_step(): all). A move whose
 * destination overlaps its source further on stays with the loop, which
 * also takes what's left beyond a window.
 */
static void string_bulk(x86_t *c, uint8_t op, int src)
{
    const bool movs = op <= 0xA5;
    const uint32_t size = 1 + (op & 1);
    x86_seg_t *gs = &c->seg[src], *gd = &c->seg[ES];
    if (gd->stale) {
        seg_window(c, gd);
    }
    if (movs && gs->stale) {
        seg_window(c, gs);
    }
    uint32_t n = CX;
    uint32_t room = gd->wr_lim > DI ? (gd->wr_lim - DI) / size : 0;
    if (room < n) {
        n = room;
    }
    if (movs) {
        room = gs->rd_lim > SI ? (gs->rd_lim - SI) / size : 0;
        if (room < n) {
            n = room;
        }
    }
    if (n == 0) {
        return;
    }
    int32_t per = movs ? 4 : 3;
    if (size == 2) {
        per += c->wait[(gd->host + DI) & 1] + (movs ? c->wait[(gs->host + SI) & 1] : 0);
    } else {
        per += c->mem_wait * (movs ? 2 : 1);
    }
    if (!c->single && (int64_t)n * per > c->budget - c->clk) {
        int32_t left = c->budget - c->clk;
        uint32_t k = left <= 0 ? 1 : (uint32_t)((left + per - 1) / per);
        if (k < n) {
            n = k;
        }
    }
    uint8_t *dst = (uint8_t *)gd->host + DI;
    const uint32_t bytes = n * size;
    if (movs) {
        const uint8_t *from = (const uint8_t *)gs->host + SI;
        if (dst > from && dst < from + bytes) {
            return;
        }
        memmove(dst, from, bytes);
        SI += bytes;
    } else if (size == 1) {
        memset(dst, AL, bytes);
    } else {
        for (uint32_t i = 0; i < bytes; i += 2) {
            dst[i] = AL;
            dst[i + 1] = AH;
        }
    }
    DI += bytes;
    CX -= n;
    c->clk += (int32_t)n * per;
}

/* After sync(): a repeat cut short sets IP back to the instruction */
static NOINLINE void string_op(x86_t *c, uint8_t op, int rep, int so)
{
    const int src = so >= 0 ? so : DS;
    const int w = op & 1;
    const int16_t d = (c->flags & DF) ? -(1 + w) : (1 + w);
    const bool i286 = c->model == X86_80286;
    uint8_t once, each;
    switch (op) {
    case 0xA4: case 0xA5: once = i286 ? 5 : 18; each = i286 ? 4 : 17; break;   /* MOVS */
    case 0xA6: case 0xA7: once = i286 ? 8 : 22; each = i286 ? 9 : 22; break;   /* CMPS */
    case 0xAA: case 0xAB: once = i286 ? 3 : 11; each = i286 ? 3 : 10; break;   /* STOS */
    case 0xAC: case 0xAD: once = i286 ? 5 : 12; each = i286 ? 4 : 13; break;   /* LODS */
    case 0xAE: case 0xAF: once = i286 ? 7 : 15; each = i286 ? 8 : 15; break;   /* SCAS */
    default: once = 5; each = 4; break;                                         /* INS, OUTS */
    }
    if (rep) {
        c->clk += i286 ? 5 : 9;
        if (CX == 0) {
            return;
        }
        if (i286 && (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB) && !(c->flags & DF) &&
            !interrupt_due(c)) {
            string_bulk(c, op, src);
            if (CX == 0) {
                return;
            }
            if (c->clk >= c->budget && !c->single) {
                c->ip = c->ip0;
                c->ip_set = true;
                c->resync = true;
                return;
            }
        }
    } else {
        c->clk += once;
    }
    /* A repeat's CX counts down first; a write that faults on the 286 has
     * taken one more (the suite's results: CX two down, one for a read)
     */
    const bool extra = rep && i286;
#define WRITE(x)    do { if (extra) { CX--; x; CX++; } else { x; } } while (0)
    for (;;) {
        /* Each index steps before its access, in the 286's order (MOVS
         * reads at SI then writes at DI, CMPS reads at DI first): when a
         * word at FFFF faults, the ones before it have stepped
         */
        uint16_t s = SI, t = DI;
        if (rep && (op & 0xFE) != 0xA6) {
            CX--;                   /* (CMPS: after its first read) */
        }
        switch (op) {
        case 0xA4: { SI += d; uint8_t v = rd8(c, src, s); DI += d; WRITE(wr8(c, ES, t, v)); break; }
        case 0xA5: { SI += d; uint16_t v = rd16(c, src, s); DI += d; WRITE(wr16(c, ES, t, v)); break; }
        case 0xA6: {
            DI += d;
            uint8_t b = rd8(c, ES, t);
            CX -= rep ? 1 : 0;
            SI += d;
            alu(c, 7, rd8(c, src, s), b, 0);
            break;
        }
        case 0xA7: {
            DI += d;
            uint16_t b = rd16(c, ES, t);
            CX -= rep ? 1 : 0;
            SI += d;
            alu(c, 7, rd16(c, src, s), b, 1);
            break;
        }
        case 0xAA: DI += d; WRITE(wr8(c, ES, t, AL)); break;
        case 0xAB: DI += d; WRITE(wr16(c, ES, t, AX)); break;
        case 0xAC: SI += d; AL = rd8(c, src, s); break;
        case 0xAD: SI += d; AX = rd16(c, src, s); break;
        case 0xAE: DI += d; alu(c, 7, AL, rd8(c, ES, t), 0); break;
        case 0xAF: DI += d; alu(c, 7, AX, rd16(c, ES, t), 1); break;
        case 0x6C: DI += d; WRITE(wr8(c, ES, t, in8(c, DX))); break;
        case 0x6D: DI += d; WRITE(wr16(c, ES, t, in16(c, DX))); break;
        case 0x6E: SI += d; out8(c, DX, rd8(c, src, s)); break;
        default: SI += d; out16(c, DX, rd16(c, src, s)); break;
        }
#undef WRITE
        if (!rep) {
            return;
        }
        c->clk += each;
        if ((op & 0xF6) == 0xA6 && get_zf(c) != (rep == 0xF3)) {
            return;     /* CMPS, SCAS: REPE stops at a difference, REPNE at a match */
        }
        if (CX == 0 || c->single) {
            if (CX == 0) {
                return;
            }
            continue;
        }
        if (UNLIKELY(interrupt_due(c))) {
            /* Resumed after the interrupt; the 8086 only with its last prefix */
            c->ip = i286 ? c->ip0 : c->last_prefix;
            c->ip_set = true;
            c->resync = true;
            return;
        }
        if (c->clk >= c->budget) {
            c->ip = c->ip0;
            c->ip_set = true;
            c->resync = true;
            return;
        }
    }
}

/* ---- 286 ---- */

static void ltr(x86_t *c, uint16_t sel)
{
    desc_t d;
    if (SEL(sel) == 0) {
        fault(c, 13);
    }
    if ((sel & 4) || !read_desc(c, sel, &d) || (d.access & 0x1F) != T_TSS) {
        fault_err(c, 13, SEL(sel));
    }
    present(c, &d, sel);
    d.access |= 0x02;               /* busy */
    mem_wr8(c, d.addr + 5, d.access);
    c->tr.sel = sel;
    c->tr.base = desc_base(&d);
    c->tr.rlo = 0;
    c->tr.rhi = d.w0;
    c->tr.access = d.access;
}

static NOINLINE void op_0f(x86_t *c, code_t *q, int so)
{
    const bool i286 = true;
    modrm_t m;
    uint8_t op = fetch8(c, q);
    switch (op) {
    case 0x00:      /* SLDT STR LLDT LTR VERR VERW: protected mode */
        if (!PE(c)) {
            undefined(c);
        }
        decode(c, q, &m, so);
        switch (m.reg) {
        case 0:
            set_rm16(c, &m, c->ldtr.sel);
            c->clk += T(m, 2, 3);
            break;
        case 1:
            set_rm16(c, &m, c->tr.sel);
            c->clk += T(m, 2, 3);
            break;
        case 2:
            cpl0(c);
            load_ldt(c, get_rm16(c, &m), 13);
            c->clk += T(m, 17, 19);
            break;
        case 3:
            cpl0(c);
            ltr(c, get_rm16(c, &m));
            c->clk += T(m, 17, 19);
            break;
        case 4:
        case 5: {
            desc_t d;
            set_zf(c, visible(c, get_rm16(c, &m), &d, m.reg == 4 ? 2 : 3));
            c->clk += T(m, 14, 16);
            break;
        }
        default:
            undefined(c);
        }
        break;
    case 0x02:      /* LAR */
    case 0x03: {    /* LSL */
        if (!PE(c)) {
            undefined(c);
        }
        decode(c, q, &m, so);
        desc_t d;
        bool ok = visible(c, get_rm16(c, &m), &d, op == 0x02 ? 0 : 1);
        if (ok) {
            c->r.w[m.reg] = op == 0x02 ? (uint16_t)(d.access << 8) : d.w0;
        }
        set_zf(c, ok);
        c->clk += T(m, 14, 16);
        break;
    }
    case 0x05:      /* LOADALL */
        cpl0(c);
        loadall(c);
        c->clk += 195;
        break;
    case 0x01:
        decode(c, q, &m, so);
        switch (m.reg) {
        case 0:     /* SGDT */
        case 1:     /* SIDT */
            if (m.mod == 3) {
                undefined(c);
            } else {
                uint32_t base = m.reg ? c->idtr.base : c->gdtr.base;
                wr16(c, m.seg, m.off, m.reg ? c->idtr.limit : c->gdtr.limit);
                wr16(c, m.seg, (uint16_t)(m.off + 2), (uint16_t)base);
                wr16(c, m.seg, (uint16_t)(m.off + 4), (uint16_t)(0xFF00 | ((base >> 16) & 0xFF)));
                c->clk += 11;
            }
            break;
        case 2:     /* LGDT */
        case 3:     /* LIDT */
            if (m.mod == 3) {
                undefined(c);
            } else {
                cpl0(c);
                uint16_t limit = rd16(c, m.seg, m.off);
                uint32_t base = rd16(c, m.seg, (uint16_t)(m.off + 2));
                base |= (uint32_t)(rd16(c, m.seg, (uint16_t)(m.off + 4)) & 0xFF) << 16;
                if (m.reg == 2) {
                    c->gdtr.base = base;
                    c->gdtr.limit = limit;
                } else {
                    c->idtr.base = base;
                    c->idtr.limit = limit;
                }
                c->clk += 11;
            }
            break;
        case 4:     /* SMSW */
            set_rm16(c, &m, c->msw);
            c->clk += T(m, 2, 3);
            break;
        case 6: {   /* LMSW: PE can't be cleared */
            cpl0(c);
            uint16_t v = get_rm16(c, &m);
            c->clk += T(m, 3, 6);
            c->msw = (c->msw & (0xFFF0 | X86_MSW_PE)) | (v & 0x000F);
            break;
        }
        default:
            undefined(c);
        }
        break;
    case 0x06:      /* CLTS */
        cpl0(c);
        c->msw &= ~X86_MSW_TS;
        c->clk += 2;
        break;
    default:
        undefined(c);
    }
}

/* 60-6F on the 286 */
static NOINLINE void op_186(x86_t *c, code_t *q, uint8_t op, int rep, int so)
{
    const bool i286 = true;
    modrm_t m;
    switch (op) {
    case 0x60: {    /* PUSHA: a word at FFFF faults before any is written */
        uint16_t sp = SP;
        for (int i = 1; i <= 8; i++) {
            if ((uint16_t)(sp - 2 * i) == 0xFFFF) {
                fault(c, 13);
            }
        }
        push16(c, AX);
        push16(c, CX);
        push16(c, DX);
        push16(c, BX);
        push16(c, sp);
        push16(c, BP);
        push16(c, SI);
        push16(c, DI);
        c->clk += 17;
        break;
    }
    case 0x61:      /* POPA */
        DI = pop16(c);
        SI = pop16(c);
        BP = pop16(c);
        SP += 2;
        BX = pop16(c);
        DX = pop16(c);
        CX = pop16(c);
        AX = pop16(c);
        c->clk += 19;
        break;
    case 0x62: {    /* BOUND */
        decode(c, q, &m, so);
        if (m.mod == 3) {
            undefined(c);
        }
        int16_t lo = (int16_t)rd16(c, m.seg, m.off);
        int16_t hi = (int16_t)rd16(c, m.seg, (uint16_t)(m.off + 2));
        int16_t i = (int16_t)c->r.w[m.reg];
        c->clk += 13;
        if (i < lo || i > hi) {
            c->ip = c->ip0;
            interrupt(c, 5);
        }
        break;
    }
    case 0x68:      /* PUSH imm16 */
        push16(c, fetch16(c, q));
        c->clk += 3;
        break;
    case 0x6A:      /* PUSH imm8 */
        push16(c, (uint16_t)(int8_t)fetch8(c, q));
        c->clk += 3;
        break;
    case 0x69:      /* IMUL r16, r/m16, imm */
    case 0x6B: {
        decode(c, q, &m, so);
        int16_t a = (int16_t)get_rm16(c, &m);
        int16_t b = op == 0x69 ? (int16_t)fetch16(c, q) : (int8_t)fetch8(c, q);
        int32_t r = (int32_t)a * b;
        c->r.w[m.reg] = (uint16_t)r;
        set_szp(c, (uint16_t)r, 1, r != (int16_t)r ? CF | OF : 0, CF | OF);
        c->clk += T(m, 21, 24);
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        IOPL_CHECK(c);
        sync(c, q);
        string_op(c, op, rep, so);
        break;
    case 0x63: {    /* ARPL: protected mode */
        if (!PE(c)) {
            undefined(c);
        }
        decode(c, q, &m, so);
        uint16_t d = get_rm16(c, &m), sr = c->r.w[m.reg];
        if ((d & 3) < (sr & 3)) {
            set_rm16(c, &m, (uint16_t)((d & ~3) | (sr & 3)));
            set_zf(c, true);
        } else {
            set_zf(c, false);
        }
        c->clk += T(m, 10, 11);
        break;
    }
    default:        /* 64-67 */
        undefined(c);
    }
}

/* ---- The instruction ---- */

static INLINE void alu_group(x86_t *c, code_t *q, uint8_t op, const int form, int so, const bool i286)
{
    const int aop = op >> 3;
    const bool cmp = aop == 7;
    modrm_t m;
    uint32_t r;
    switch (form) {
    case 0:         /* Eb, Gb */
        decode(c, q, &m, so);
        r = alu(c, aop, get_rm8(c, &m), *x86_reg8(c, m.reg), 0);
        if (!cmp) {
            set_rm8(c, &m, (uint8_t)r);
        }
        c->clk += i286 ? T(m, 2, cmp ? 6 : 7) : T(m, 3, cmp ? 9 : 16);
        break;
    case 1:         /* Ev, Gv */
        decode(c, q, &m, so);
        r = alu(c, aop, get_rm16(c, &m), c->r.w[m.reg], 1);
        if (!cmp) {
            set_rm16(c, &m, (uint16_t)r);
        }
        c->clk += i286 ? T(m, 2, cmp ? 6 : 7) : T(m, 3, cmp ? 9 : 16);
        break;
    case 2:         /* Gb, Eb */
        decode(c, q, &m, so);
        r = alu(c, aop, *x86_reg8(c, m.reg), get_rm8(c, &m), 0);
        if (!cmp) {
            *x86_reg8(c, m.reg) = (uint8_t)r;
        }
        c->clk += i286 ? T(m, 2, 7) : T(m, 3, 9);
        break;
    case 3:         /* Gv, Ev */
        decode(c, q, &m, so);
        r = alu(c, aop, c->r.w[m.reg], get_rm16(c, &m), 1);
        if (!cmp) {
            c->r.w[m.reg] = (uint16_t)r;
        }
        c->clk += i286 ? T(m, 2, 7) : T(m, 3, 9);
        break;
    case 4:         /* AL, imm8 */
        r = alu(c, aop, AL, fetch8(c, q), 0);
        if (!cmp) {
            AL = (uint8_t)r;
        }
        c->clk += i286 ? 3 : 4;
        break;
    default:        /* AX, imm16 */
        r = alu(c, aop, AX, fetch16(c, q), 1);
        if (!cmp) {
            AX = (uint16_t)r;
        }
        c->clk += i286 ? 3 : 4;
        break;
    }
}

/* An ESC instruction for the coprocessor: its memory operand's bytes read
 * (those it writes too: the faults come first, and an instruction that
 * stops writes them back as they were), run, written
 */
static NOINLINE void esc_fpu(x86_t *c, uint8_t op, const modrm_t *m, const bool i286)
{
    uint8_t buf[94];
    int rd = 0, wr = 0;
    if (m->mod != 3) {
        x87_operand(op, m->reg, &rd, &wr);
        for (int i = 0; i < rd || i < wr; i++) {
            buf[i] = rd8(c, m->seg, (uint16_t)(m->off + i));
        }
    }
    bool error = c->fpu.error;
    c->clk += x87_exec(c, op, m->mod, m->reg, m->rm, buf, m->off, c->seg[m->seg].sel);
    for (int i = 0; i < wr; i++) {
        wr8(c, m->seg, (uint16_t)(m->off + i), buf[i]);
    }
    if (c->fpu.error != error && c->bus.fpu_error) {
        c->bus.fpu_error(c->bus.ctx, c->fpu.error);
    }
}

static INLINE void execute_op(x86_t *c, code_t *q, const bool i286)
{
    modrm_t m;
    uint16_t v, t;
    uint32_t r;
    int so = -1, rep = 0;

    bool capped = false;
#define ds  (so >= 0 ? so : DS)

    uint8_t op = fetch8(c, q);
dispatch:
    switch (op) {
    /* Prefixes: noted, and the next byte dispatched (the instructions
     * without one aren't tested for them)
     */
    case 0x26: case 0x2E: case 0x36: case 0x3E:
        so = (op >> 3) & 3;
        goto prefix;
    case 0xF2: case 0xF3:
        rep = op;
        __attribute__((fallthrough));
    case 0xF0: case 0xF1:
    prefix:
        if (i286 && !capped) {
            /* Only prefixes make an instruction longer than 10 bytes: the
             * window shortened to its 10th byte (back after it: resync)
             */
            uintptr_t cap = q->base + c->ip0 + c->max_len;
            if (q->end > cap) {
                q->end = cap;
            }
            c->resync = true;
            capped = true;
        }
        if (!i286) {
            c->last_prefix = (uint16_t)(IP(q) - 1);
            c->clk += 2;
        }
        op = fetch8(c, q);
        goto dispatch;

    /* ADD OR ADC SBB AND SUB XOR CMP: each form its own case (one
     * dispatch, then the operation's)
     */
#define ALU8(f) \
    case 0x00 + f: case 0x08 + f: case 0x10 + f: case 0x18 + f: \
    case 0x20 + f: case 0x28 + f: case 0x30 + f: case 0x38 + f: \
        alu_group(c, q, op, f, so, i286); break;
    ALU8(0) ALU8(1) ALU8(2) ALU8(3) ALU8(4) ALU8(5)
#undef ALU8

    case 0x06: case 0x0E: case 0x16: case 0x1E:     /* PUSH seg */
        push16(c, c->seg[op >> 3].sel);
        c->clk += i286 ? 3 : 10;
        break;
    case 0x07: case 0x17: case 0x1F:                /* POP seg */
        load_seg(c, op >> 3, pop16(c));
        if (op == 0x17) {
            c->inhibit = true;
        }
        c->clk += i286 ? 5 : 8;
        break;
    case 0x0F:
        if (i286) {
            code_t t = *q;              /* (q stays in registers) */
            op_0f(c, &t, so);
            *q = t;
        } else {                                    /* POP CS */
            far_jump(c, q, pop16(c), IP(q));
            c->clk += 8;
        }
        break;
    case 0x27: daa(c); c->clk += 4; break;
    case 0x2F: das(c); c->clk += 4; break;
    case 0x37: aaa(c); c->clk += i286 ? 3 : 4; break;
    case 0x3F: aas(c); c->clk += i286 ? 3 : 4; break;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->r.w[op & 7] = (uint16_t)inc(c, c->r.w[op & 7], 1);
        c->clk += 2;
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->r.w[op & 7] = (uint16_t)dec(c, c->r.w[op & 7], 1);
        c->clk += 2;
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
        push16(c, c->r.w[op & 7]);
        c->clk += i286 ? 3 : 11;
        break;
    case 0x54:                                      /* PUSH SP */
        if (i286) {
            push16(c, SP);
        } else {
            t = (uint16_t)(SP - 2);
            wr16(c, SS, t, t);
            SP = t;
        }
        c->clk += i286 ? 3 : 11;
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        v = pop16(c);
        c->r.w[op & 7] = v;
        c->clk += i286 ? 5 : 8;
        break;

    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        if (i286) {
            code_t t = *q;
            op_186(c, &t, op, rep, so);
            *q = t;
        } else {
            jcc(c, q, op & 0x0F, i286);                   /* the 8086: 70-7F again */
        }
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        jcc(c, q, op & 0x0F, i286);
        break;

    case 0x80: case 0x82:                           /* ALU Eb, imm8 (82: the same) */
        decode(c, q, &m, so);
        r = alu(c, m.reg, get_rm8(c, &m), fetch8(c, q), 0);
        if (m.reg != 7) {
            set_rm8(c, &m, (uint8_t)r);
        }
        c->clk += i286 ? T(m, 3, m.reg == 7 ? 6 : 7) : T(m, 4, m.reg == 7 ? 10 : 17);
        break;
    case 0x81:                                      /* ALU Ev, imm16 */
    case 0x83:                                      /* ALU Ev, imm8 sign-extended */
        decode(c, q, &m, so);
        v = get_rm16(c, &m);
        t = op == 0x81 ? fetch16(c, q) : (uint16_t)(int8_t)fetch8(c, q);
        r = alu(c, m.reg, v, t, 1);
        if (m.reg != 7) {
            set_rm16(c, &m, (uint16_t)r);
        }
        c->clk += i286 ? T(m, 3, m.reg == 7 ? 6 : 7) : T(m, 4, m.reg == 7 ? 10 : 17);
        break;
    case 0x84:                                      /* TEST Eb, Gb */
        decode(c, q, &m, so);
        alu(c, 4, get_rm8(c, &m), *x86_reg8(c, m.reg), 0);
        c->clk += i286 ? T(m, 2, 6) : T(m, 3, 9);
        break;
    case 0x85:                                      /* TEST Ev, Gv */
        decode(c, q, &m, so);
        alu(c, 4, get_rm16(c, &m), c->r.w[m.reg], 1);
        c->clk += i286 ? T(m, 2, 6) : T(m, 3, 9);
        break;
    case 0x86: {                                    /* XCHG Eb, Gb */
        decode(c, q, &m, so);
        uint8_t a = get_rm8(c, &m);
        set_rm8(c, &m, *x86_reg8(c, m.reg));
        *x86_reg8(c, m.reg) = a;
        c->clk += i286 ? T(m, 3, 5) : T(m, 4, 17);
        break;
    }
    case 0x87:                                      /* XCHG Ev, Gv */
        decode(c, q, &m, so);
        v = get_rm16(c, &m);
        set_rm16(c, &m, c->r.w[m.reg]);
        c->r.w[m.reg] = v;
        c->clk += i286 ? T(m, 3, 5) : T(m, 4, 17);
        break;
    case 0x88:                                      /* MOV Eb, Gb */
        decode(c, q, &m, so);
        set_rm8(c, &m, *x86_reg8(c, m.reg));
        c->clk += i286 ? T(m, 2, 3) : T(m, 2, 9);
        break;
    case 0x89:                                      /* MOV Ev, Gv */
        decode(c, q, &m, so);
        set_rm16(c, &m, c->r.w[m.reg]);
        c->clk += i286 ? T(m, 2, 3) : T(m, 2, 9);
        break;
    case 0x8A:                                      /* MOV Gb, Eb */
        decode(c, q, &m, so);
        *x86_reg8(c, m.reg) = get_rm8(c, &m);
        c->clk += i286 ? T(m, 2, 5) : T(m, 2, 8);
        break;
    case 0x8B:                                      /* MOV Gv, Ev */
        decode(c, q, &m, so);
        c->r.w[m.reg] = get_rm16(c, &m);
        c->clk += i286 ? T(m, 2, 5) : T(m, 2, 8);
        break;
    case 0x8C:                                      /* MOV Ew, Sw */
        decode(c, q, &m, so);
        if (i286 && m.reg > 3) {
            undefined(c);
        }
        set_rm16(c, &m, c->seg[m.reg & 3].sel);
        c->clk += i286 ? T(m, 2, 3) : T(m, 2, 9);
        break;
    case 0x8D:                                      /* LEA */
        decode(c, q, &m, so);
        if (m.mod == 3) {
            if (i286) {
                undefined(c);
            }
        } else {
            c->r.w[m.reg] = m.off;
        }
        c->clk += i286 ? 3 : 2 + m.ea;
        break;
    case 0x8E:                                      /* MOV Sw, Ew */
        decode(c, q, &m, so);
        v = get_rm16(c, &m);
        if (i286 && ((m.reg & 3) == CS || m.reg > 3)) {
            undefined(c);                           /* after reading the operand */
        }
        if ((m.reg & 3) == CS) {
            far_jump(c, q, v, IP(q));               /* the 8086's MOV CS */
        } else {
            load_seg(c, m.reg & 3, v);
        }
        if ((m.reg & 3) == SS) {
            c->inhibit = true;
        }
        c->clk += i286 ? T(m, 2, 5) : T(m, 2, 8);
        break;
    case 0x8F:                                      /* POP Ev */
        decode(c, q, &m, so);
        if (i286 && m.reg != 0) {
            undefined(c);
        }
        v = pop16(c);
        c->sp0 = SP;                                /* a fault writing it: popped (the 286) */
        set_rm16(c, &m, v);
        c->clk += i286 ? T(m, 5, 5) : T(m, 8, 17);
        break;

    case 0x90:                                      /* NOP */
        c->clk += 3;
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        v = AX;
        AX = c->r.w[op & 7];
        c->r.w[op & 7] = v;
        c->clk += 3;
        break;
    case 0x98:                                      /* CBW */
        AH = (AL & 0x80) ? 0xFF : 0x00;
        c->clk += 2;
        break;
    case 0x99:                                      /* CWD */
        DX = (AX & 0x8000) ? 0xFFFF : 0x0000;
        c->clk += i286 ? 2 : 5;
        break;
    case 0x9A:                                      /* CALL far */
        v = fetch16(c, q);
        t = fetch16(c, q);
        if (i286 && PE(c)) {
            sync(c, q);
            pm_far(c, t, v, true, c->ip);
            break;
        }
        push16(c, c->seg[CS].sel);
        push16(c, IP(q));
        far_jump(c, q, t, v);
        c->clk += i286 ? 13 : 28;
        break;
    case 0x9B:                                      /* WAIT: no coprocessor */
        if (i286 && (c->msw & (X86_MSW_MP | X86_MSW_TS)) == (X86_MSW_MP | X86_MSW_TS)) {
            fault(c, 7);
        }
        c->clk += 3;
        break;
    case 0x9C:                                      /* PUSHF */
        push16(c, x86_get_flags(c));
        c->clk += i286 ? 3 : 10;
        break;
    case 0x9D:                                      /* POPF */
        if (i286 && PE(c)) {
            set_pm_flags(c, pop16(c), c->cpl);
        } else {
            x86_set_flags(c, pop16(c));
        }
        c->clk += i286 ? 5 : 8;
        break;
    case 0x9E:                                      /* SAHF */
        materialize(c);
        c->flags = (c->flags & ~(SF | ZF | AF | PF | CF)) | (AH & (SF | ZF | AF | PF | CF));
        c->clk += i286 ? 2 : 4;
        break;
    case 0x9F:                                      /* LAHF */
        AH = (uint8_t)x86_get_flags(c);
        c->clk += i286 ? 2 : 4;
        break;

    case 0xA0:                                      /* MOV AL, [moffs] */
        AL = rd8(c, ds, fetch16(c, q));
        c->clk += i286 ? 5 : 10;
        break;
    case 0xA1:                                      /* MOV AX, [moffs] */
        AX = rd16(c, ds, fetch16(c, q));
        c->clk += i286 ? 5 : 10;
        break;
    case 0xA2:                                      /* MOV [moffs], AL */
        wr8(c, ds, fetch16(c, q), AL);
        c->clk += i286 ? 3 : 10;
        break;
    case 0xA3:                                      /* MOV [moffs], AX */
        wr16(c, ds, fetch16(c, q), AX);
        c->clk += i286 ? 3 : 10;
        break;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        if (LIKELY(!rep)) {             /* MOVS, STOS, LODS once: as string_op() does them */
            const int w = op & 1;
            const uint16_t d = (c->flags & DF) ? (uint16_t)-(1 + w) : (uint16_t)(1 + w);
            const uint16_t s = SI, di = DI;
            switch (op) {
            case 0xA4: SI += d; { uint8_t b = rd8(c, ds, s); DI += d; wr8(c, ES, di, b); }
                c->clk += i286 ? 5 : 18; break;
            case 0xA5: SI += d; v = rd16(c, ds, s); DI += d; wr16(c, ES, di, v);
                c->clk += i286 ? 5 : 18; break;
            case 0xAA: DI += d; wr8(c, ES, di, AL); c->clk += i286 ? 3 : 11; break;
            case 0xAB: DI += d; wr16(c, ES, di, AX); c->clk += i286 ? 3 : 11; break;
            case 0xAC: SI += d; AL = rd8(c, ds, s); c->clk += i286 ? 5 : 12; break;
            default: SI += d; AX = rd16(c, ds, s); c->clk += i286 ? 5 : 12; break;
            }
            break;
        }
        __attribute__((fallthrough));
    case 0xA6: case 0xA7: case 0xAE: case 0xAF:
        sync(c, q);
        string_op(c, op, rep, so);
        break;
    case 0xA8:                                      /* TEST AL, imm8 */
        alu(c, 4, AL, fetch8(c, q), 0);
        c->clk += i286 ? 3 : 4;
        break;
    case 0xA9:                                      /* TEST AX, imm16 */
        alu(c, 4, AX, fetch16(c, q), 1);
        c->clk += i286 ? 3 : 4;
        break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        *x86_reg8(c, op & 7) = fetch8(c, q);
        c->clk += i286 ? 2 : 4;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->r.w[op & 7] = fetch16(c, q);
        c->clk += i286 ? 2 : 4;
        break;

    case 0xC0: case 0xC1:
        if (i286) {                                 /* shift by imm8 */
            decode(c, q, &m, so);
            if (op == 0xC0) {
                uint8_t a = get_rm8(c, &m);
                uint8_t n = fetch8(c, q) & 31;
                set_rm8(c, &m, (uint8_t)shift(c, m.reg, a, n, 0));
                c->clk += T(m, 5, 8) + n;
            } else {
                v = get_rm16(c, &m);
                uint8_t n = fetch8(c, q) & 31;
                set_rm16(c, &m, (uint16_t)shift(c, m.reg, v, n, 1));
                c->clk += T(m, 5, 8) + n;
            }
            break;
        }
        op += 2;                                    /* the 8086: C2, C3 */
        __attribute__((fallthrough));
    case 0xC2: case 0xC3:                           /* RET (imm16) */
        t = op == 0xC2 ? fetch16(c, q) : 0;
        jump(c, q, pop16(c));
        SP += t;
        c->clk += i286 ? 11 : (op == 0xC2 ? 12 : 8);
        break;
    case 0xC4: case 0xC5:                           /* LES, LDS */
        decode(c, q, &m, so);
        if (m.mod == 3) {
            if (i286) {
                undefined(c);
            }
            c->clk += 2;
            break;
        }
        v = rd16(c, m.seg, m.off);
        t = rd16(c, m.seg, (uint16_t)(m.off + 2));
        c->r.w[m.reg] = v;
        load_seg(c, op == 0xC4 ? ES : DS, t);
        c->clk += i286 ? 7 : 16 + m.ea;
        break;
    case 0xC6:                                      /* MOV Eb, imm8 */
        decode(c, q, &m, so);
        if (i286 && m.reg != 0) {
            fetch8(c, q);                              /* the whole instruction first (its length) */
            undefined(c);
        }
        set_rm8(c, &m, fetch8(c, q));
        c->clk += i286 ? T(m, 2, 3) : T(m, 4, 10);
        break;
    case 0xC7:                                      /* MOV Ev, imm16 */
        decode(c, q, &m, so);
        if (i286 && m.reg != 0) {
            fetch16(c, q);
            undefined(c);
        }
        set_rm16(c, &m, fetch16(c, q));
        c->clk += i286 ? T(m, 2, 3) : T(m, 4, 10);
        break;
    case 0xC8: case 0xC9:
        if (i286) {
            if (op == 0xC8) {                       /* ENTER */
                uint16_t size = fetch16(c, q);
                uint8_t level = fetch8(c, q) & 31;
                push16(c, BP);
                uint16_t frame = SP, bp = BP;     /* BP as it was if a push faults */
                for (int i = 1; i < level; i++) {
                    bp -= 2;
                    push16(c, rd16(c, SS, bp));
                }
                if (level > 0) {
                    push16(c, frame);
                }
                BP = frame;
                SP -= size;
                c->clk += level == 0 ? 11 : level == 1 ? 15 : 12 + 4 * (level - 1);
            } else {                                /* LEAVE */
                SP = BP;
                BP = pop16(c);
                c->clk += 5;
            }
            break;
        }
        op += 2;                                    /* the 8086: CA, CB */
        __attribute__((fallthrough));
    case 0xCA: case 0xCB:                           /* RETF (imm16) */
        t = op == 0xCA ? fetch16(c, q) : 0;
        if (i286 && PE(c)) {
            sync(c, q);
            pm_return(c, t, false);
            break;
        }
        v = pop16(c);
        far_jump(c, q, pop16(c), v);
        SP += t;
        c->clk += i286 ? 15 : (op == 0xCA ? 17 : 18);
        break;
    case 0xCC:                                      /* INT 3 */
        sync(c, q);
        soft_interrupt(c, 3);
        c->clk += i286 ? 23 : 52;
        break;
    case 0xCD:                                      /* INT n */
        v = fetch8(c, q);
        sync(c, q);
        soft_interrupt(c, (uint8_t)v);
        c->clk += i286 ? 23 : 51;
        break;
    case 0xCE:                                      /* INTO */
        if (get_of(c)) {
            sync(c, q);
            soft_interrupt(c, 4);
            c->clk += i286 ? 24 : 53;
        } else {
            c->clk += i286 ? 3 : 4;
        }
        break;
    case 0xCF:                                      /* IRET */
        if (i286 && PE(c)) {
            sync(c, q);
            pm_return(c, 0, true);
            break;
        }
        v = pop16(c);
        t = pop16(c);
        far_jump(c, q, t, v);
        x86_set_flags(c, pop16(c));
        c->clk += i286 ? 17 : 24;
        break;

    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {   /* shifts by 1, by CL */
        decode(c, q, &m, so);
        unsigned n = op < 0xD2 ? 1 : (i286 ? (c->r.b[2] & 31) : c->r.b[2]);
        if (op & 1) {
            v = get_rm16(c, &m);
            set_rm16(c, &m, (uint16_t)shift(c, m.reg, v, n, 1));
        } else {
            uint8_t a = get_rm8(c, &m);
            set_rm8(c, &m, (uint8_t)shift(c, m.reg, a, n, 0));
        }
        if (op < 0xD2) {
            c->clk += i286 ? T(m, 2, 7) : T(m, 2, 15);
        } else {
            c->clk += i286 ? T(m, 5, 8) + n : T(m, 8, 20) + 4 * n;
        }
        break;
    }
    case 0xD4: {                                    /* AAM */
        uint8_t d = fetch8(c, q);
        c->clk += i286 ? 16 : 83;
        if (d == 0) {
            if (i286) {
                set_szp(c, AL, 0, 0, CF | OF | AF);
                c->flags &= ~(SF | ZF);         /* the 286's: AL's parity alone */
            } else {
                set_szp(c, 0, 0, 0, CF | OF | AF);  /* the 8088's: a zero result */
            }
            sync(c, q);
            div_error(c);
            break;
        }
        uint8_t a = AL;
        AH = a / d;
        AL = a % d;
        set_szp(c, AL, 0, 0, CF | OF | AF);
        break;
    }
    case 0xD5: {                                    /* AAD */
        uint8_t d = fetch8(c, q);
        uint8_t a = AL, b = (uint8_t)(AH * d);
        r = alu(c, 0, a, b, 0);
        AL = (uint8_t)r;
        AH = 0;
        c->clk += i286 ? 14 : 60;
        break;
    }
    case 0xD6:                                      /* SALC (undocumented) */
        AL = get_cf(c) ? 0xFF : 0x00;
        c->clk += 3;
        break;
    case 0xD7:                                      /* XLAT */
        AL = rd8(c, ds, (uint16_t)(BX + AL));
        c->clk += i286 ? 5 : 11;
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        if (i286 && (c->msw & (X86_MSW_EM | X86_MSW_TS))) {
            fault(c, 7);                            /* emulated, or another task's */
        }
        decode(c, q, &m, so);
        if (c->has_fpu) {
            esc_fpu(c, op, &m, i286);
            break;
        }
        if (i286 && m.mod != 3) {                   /* ESC: no coprocessor */
            rd16(c, m.seg, m.off);                  /* the 286 reads it (and faults at FFFF) */
        }
        c->clk += T(m, 2, 8);
        break;

    case 0xE0:                                      /* LOOPNZ */
    case 0xE1:                                      /* LOOPZ */
    case 0xE2: {                                    /* LOOP */
        int8_t d = (int8_t)fetch8(c, q);
        CX--;
        bool go = CX != 0 && (op == 0xE2 || get_zf(c) == (op == 0xE1));
        if (go) {
            jump(c, q, (uint16_t)(IP(q) + d));
        }
        if (i286) {
            c->clk += go ? 8 : 4;
        } else {
            c->clk += op == 0xE2 ? (go ? 17 : 5) : op == 0xE1 ? (go ? 18 : 6) : (go ? 19 : 5);
        }
        break;
    }
    case 0xE3: {                                    /* JCXZ */
        int8_t d = (int8_t)fetch8(c, q);
        if (CX == 0) {
            jump(c, q, (uint16_t)(IP(q) + d));
            c->clk += i286 ? 8 : 18;
        } else {
            c->clk += i286 ? 4 : 6;
        }
        break;
    }
    case 0xE4:                                      /* IN AL, imm8 */
        AL = in8(c, fetch8(c, q));
        c->clk += i286 ? 5 : 10;
        break;
    case 0xE5:                                      /* IN AX, imm8 */
        AX = in16(c, fetch8(c, q));
        c->clk += i286 ? 5 : 10;
        break;
    case 0xE6:                                      /* OUT imm8, AL */
        out8(c, fetch8(c, q), AL);
        c->clk += i286 ? 3 : 10;
        break;
    case 0xE7:                                      /* OUT imm8, AX */
        out16(c, fetch8(c, q), AX);
        c->clk += i286 ? 3 : 10;
        break;
    case 0xE8:                                      /* CALL rel16 */
        v = fetch16(c, q);
        push16(c, IP(q));
        jump(c, q, (uint16_t)(IP(q) + v));
        c->clk += i286 ? 7 : 19;
        break;
    case 0xE9:                                      /* JMP rel16 */
        v = fetch16(c, q);
        jump(c, q, (uint16_t)(IP(q) + v));
        c->clk += i286 ? 7 : 15;
        break;
    case 0xEA:                                      /* JMP far */
        v = fetch16(c, q);
        t = fetch16(c, q);
        if (i286 && PE(c)) {
            sync(c, q);
            pm_far(c, t, v, false, c->ip);
            break;
        }
        far_jump(c, q, t, v);
        c->clk += i286 ? 11 : 15;
        break;
    case 0xEB: {                                    /* JMP rel8 */
        int8_t d = (int8_t)fetch8(c, q);
        jump(c, q, (uint16_t)(IP(q) + d));
        c->clk += i286 ? 7 : 15;
        break;
    }
    case 0xEC:                                      /* IN AL, DX */
        AL = in8(c, DX);
        c->clk += i286 ? 5 : 8;
        break;
    case 0xED:                                      /* IN AX, DX */
        AX = in16(c, DX);
        c->clk += i286 ? 5 : 8;
        break;
    case 0xEE:                                      /* OUT DX, AL */
        out8(c, DX, AL);
        c->clk += i286 ? 3 : 8;
        break;
    case 0xEF:                                      /* OUT DX, AX */
        out16(c, DX, AX);
        c->clk += i286 ? 3 : 8;
        break;

    case 0xF4:                                      /* HLT */
        if (i286 && PE(c) && c->cpl != 0) {
            fault(c, 13);
        }
        c->halted = true;
        c->clk += 2;
        break;
    case 0xF5:                                      /* CMC */
        materialize(c);
        c->flags ^= CF;
        c->clk += 2;
        break;
    case 0xF6: case 0xF7: {                         /* group 3 */
        const int w = op & 1;
        decode(c, q, &m, so);
        v = w ? get_rm16(c, &m) : get_rm8(c, &m);
        switch (m.reg) {
        case 0: case 1:                             /* TEST (1: the same) */
            t = w ? fetch16(c, q) : fetch8(c, q);
            alu(c, 4, v, t, w);
            c->clk += i286 ? T(m, 3, 6) : T(m, 5, 11);
            break;
        case 2:                                     /* NOT */
            v = (uint16_t)~v;
            if (w) {
                set_rm16(c, &m, v);
            } else {
                set_rm8(c, &m, (uint8_t)v);
            }
            c->clk += i286 ? T(m, 2, 7) : T(m, 3, 16);
            break;
        case 3:                                     /* NEG */
            r = alu(c, 5, 0, v, w);
            if (w) {
                set_rm16(c, &m, (uint16_t)r);
            } else {
                set_rm8(c, &m, (uint8_t)r);
            }
            c->clk += i286 ? T(m, 2, 7) : T(m, 3, 16);
            break;
        default:
            sync(c, q);
            muldiv(c, m.reg, v, w, &m, rep != 0);
            break;
        }
        break;
    }
    case 0xF8: materialize(c); c->flags &= ~CF; c->clk += 2; break;        /* CLC */
    case 0xF9: materialize(c); c->flags |= CF; c->clk += 2; break;         /* STC */
    case 0xFA:                                                              /* CLI */
        IOPL_CHECK(c);
        c->flags &= ~IF;
        c->clk += i286 ? 3 : 2;
        break;
    case 0xFB:                                                              /* STI */
        IOPL_CHECK(c);
        if (!(c->flags & IF)) {
            c->inhibit = true;
        }
        c->flags |= IF;
        c->clk += 2;
        break;
    case 0xFC: c->flags &= ~DF; c->clk += 2; break;                        /* CLD */
    case 0xFD: c->flags |= DF; c->clk += 2; break;                         /* STD */

    case 0xFE: {                                    /* group 4 (and the BIOS trap) */
        uint32_t at = (c->seg[CS].base + (uint16_t)(IP(q) - 1)) & c->addr_mask;
        if (c->bus.trap && at >= c->trap_lo && at < c->trap_hi &&
            x86_peek8(c, (c->seg[CS].base + IP(q)) & c->addr_mask) == 0x38) {
            fetch8(c, q);
            uint8_t n = fetch8(c, q);
            c->clk += 10;
            sync(c, q);
            c->ip_set = true;               /* the trap may set CS:IP */
            c->resync = true;
            c->bus.trap(c->bus.ctx, c, n);
            break;
        }
        decode(c, q, &m, so);
        if (m.reg > 1 && i286) {
            undefined(c);
        }
        uint8_t a = get_rm8(c, &m);
        set_rm8(c, &m, (uint8_t)(m.reg & 1 ? dec(c, a, 0) : inc(c, a, 0)));
        c->clk += i286 ? T(m, 2, 7) : T(m, 3, 15);
        break;
    }
    case 0xFF:                                      /* group 5 */
        decode(c, q, &m, so);
        switch (m.reg) {
        case 0: case 1:                             /* INC, DEC */
            v = get_rm16(c, &m);
            set_rm16(c, &m, (uint16_t)(m.reg ? dec(c, v, 1) : inc(c, v, 1)));
            c->clk += i286 ? T(m, 2, 7) : T(m, 3, 15);
            break;
        case 2:                                     /* CALL near */
            v = get_rm16(c, &m);
            push16(c, IP(q));
            jump(c, q, v);
            c->clk += i286 ? T(m, 7, 11) : T(m, 16, 21);
            break;
        case 3:                                     /* CALL far */
            if (m.mod == 3) {
                if (i286) {
                    undefined(c);
                }
                break;
            }
            v = rd16(c, m.seg, m.off);
            t = rd16(c, m.seg, (uint16_t)(m.off + 2));
            if (i286 && PE(c)) {
                sync(c, q);
                pm_far(c, t, v, true, c->ip);
                break;
            }
            push16(c, c->seg[CS].sel);
            push16(c, IP(q));
            far_jump(c, q, t, v);
            c->clk += i286 ? 16 : 37 + m.ea;
            break;
        case 4:                                     /* JMP near */
            jump(c, q, get_rm16(c, &m));
            c->clk += i286 ? T(m, 7, 11) : T(m, 11, 18);
            break;
        case 5:                                     /* JMP far */
            if (m.mod == 3) {
                if (i286) {
                    undefined(c);
                }
                break;
            }
            v = rd16(c, m.seg, m.off);
            t = rd16(c, m.seg, (uint16_t)(m.off + 2));
            if (i286 && PE(c)) {
                sync(c, q);
                pm_far(c, t, v, false, c->ip);
                break;
            }
            far_jump(c, q, t, v);
            c->clk += i286 ? 15 : 24 + m.ea;
            break;
        case 7:
            if (i286) {
                undefined(c);
            }
            __attribute__((fallthrough));   /* the 8086's PUSH again */
        default:                                    /* PUSH */
            if (m.mod == 3 && m.rm == X86_SP && !i286) {
                t = (uint16_t)(SP - 2);             /* PUSH SP: the new SP, as 54 */
                wr16(c, SS, t, t);
                SP = t;
            } else {
                push16(c, get_rm16(c, &m));
            }
            c->clk += i286 ? T(m, 3, 5) : T(m, 11, 16);
            break;
        }
        break;

    default:
        /* Every opcode is handled above */
        break;
    }
#undef ds
}

/* One instruction, its bytes from q (at IP): IP past them at the end,
 * unless it set IP
 */
static INLINE void execute(x86_t *c, code_t *q, const bool i286)
{
    const uint16_t ip = IP(q);
    c->ip0 = ip;
    c->sp0 = SP;
    if (!i286) {
        c->last_prefix = ip;
        c->busy = 0;
        c->q_lin = (c->seg[CS].base + ip) & c->addr_mask;
        c->q_log_n = 0;
    }
    c->flush = false;
    c->ip_set = false;
    execute_op(c, q, i286);
    if (LIKELY(!c->ip_set)) {
        c->ip = IP(q);
        if (!i286) {
            c->fetched = (uint8_t)(c->ip - ip);
        }
    }
}

/* ---- Running ---- */

/* The 8088/8086's prefetch queue: the bus fetches code (4 clocks a byte on
 * the 8088, 2 on the 8086) while the instruction doesn't use it for data;
 * an instruction whose bytes aren't there yet waits for them
 */
static inline int32_t queue_time(x86_t *c, int32_t t)
{
    if (c->model == X86_80286) {
        return t;
    }
    const int per = c->model == X86_8088 ? 4 : 2;
    const int full = c->model == X86_8088 ? 16 : 12;
    int cost = c->fetched * per;
    int avail = c->bank + t - c->busy;
    if (avail < cost) {
        t += cost - avail;
        avail = cost;
    }
    int bank = avail - cost;
    c->bank = (int16_t)(c->flush ? 0 : bank > full ? full : bank);
    return t;
}

static void take_interrupt(x86_t *c)
{
    c->busy = 0;
    c->fetched = 0;
    c->halted = false;
    c->ip0 = c->ip;                 /* (a fault delivering it: back here) */
    c->sp0 = SP;
    c->ext = true;
    if (c->nmi) {
        c->nmi = false;
        interrupt(c, 2);
        c->clk += 50;
    } else {
        uint8_t vector = c->bus.inta(c->bus.ctx);
        interrupt(c, vector);
        c->clk += 61;
    }
    c->ext = false;
    c->bank = 0;
}

/* Writes during an 8088/8086 instruction against the bytes its queue had
 * already fetched past it (as many as its fill says: 4 at most on the
 * 8088, 6 on the 8086): their old bytes run next
 */
static NOINLINE void queue_check(x86_t *c)
{
    int n = c->q_log_n;
    c->q_log_n = 0;
    if (c->ip_set) {
        return;                     /* it jumped: the queue starts empty */
    }
    const int per = c->model == X86_8088 ? 4 : 2, len = c->model == X86_8088 ? 4 : 6;
    int held = c->bank / per;
    if (held > len) {
        held = len;
    }
    uint32_t end = (c->seg[CS].base + c->ip) & c->addr_mask;
    for (int i = 0; i < n; i++) {
        uint32_t lin = c->q_log[i].lin;
        if (lin - end >= (uint32_t)held) {
            continue;
        }
        bool known = false;         /* written twice: the first one's old byte */
        for (int k = 0; k < c->q_old_n; k++) {
            known |= c->q_old[k].lin == lin;
        }
        if (!known && c->q_old_n < 6) {
            c->q_old[c->q_old_n].lin = lin;
            c->q_old[c->q_old_n].old = c->q_log[i].old;
            c->q_old_n++;
        }
    }
    if (c->q_old_n) {
        c->win_n = 0;
        c->resync = true;
    }
}

/* A fault left the instruction (the 286): back to its start and SP, then
 * its interrupt (in protected mode with its error code). A fault while
 * taking it is the double fault (interrupt 8, error code 0), and one while
 * taking that a shutdown.
 */
static void deliver_fault(x86_t *c)
{
    c->ip = c->ip0;
    SP = c->sp0;
    int vector = c->fault_vector;
    bool has_error = c->fault_has_error;
    uint16_t error = c->fault_error;
    if (c->faulting >= 2) {
        c->faulting = 0;
        c->ext = false;
        if (c->bus.shutdown) {
            c->bus.shutdown(c->bus.ctx, c);
        } else {
            c->stuck = true;
            c->halted = true;
        }
        return;
    }
    if (c->faulting == 1) {
        vector = 8;
        has_error = true;
        error = 0;
    }
    c->faulting++;
    c->ext = true;                  /* (faults while delivering it) */
    if (PE(c)) {
        pm_interrupt(c, (uint8_t)vector, false, has_error, error);
    } else {
        interrupt(c, (uint8_t)vector);
    }
    c->ext = false;
    c->faulting = 0;
}

#if EMUDBG_ENABLED
#define DEBUGGER_ON(c)  ((c)->dbg_map != NULL)
#else
#define DEBUGGER_ON(c)  0
#endif

/* The instructions of a run, the model a constant (a fault leaves it for
 * x86_run(), which delivers it and comes back). The code pointer carries
 * from one instruction to the next; a jump, an interrupt or a new window
 * starts it again from IP.
 */
static INLINE void run_body(x86_t *c, const bool i286, const bool dbg)
{
    code_t q;
    uint32_t n = 0;
    int32_t clk = c->clk;               /* (cpu->clk, in a register for the test) */
    code_start(c, &q, c->ip);
    while (LIKELY(clk < c->budget)) {
        /* Interrupts, HLT, the instruction after STI or MOV SS, TF, the
         * debugger: one test
         */
        bool trace = false;
        if (UNLIKELY(c->lines | (c->flags & TF) | (dbg && DEBUGGER_ON(c)))) {
            if (c->inhibit) {
                c->inhibit = false;
            } else if ((c->nmi || (c->intr && (c->flags & IF))) && !c->stuck) {
                take_interrupt(c);
                clk = c->clk;
                n++;
                code_start(c, &q, c->ip);
                continue;
            }
            if (c->halted) {
                if (clk < c->budget) {
                    c->clk = c->budget;     /* the rest of the run passes idle */
                }
                break;
            }
#if EMUDBG_ENABLED
            if (dbg && c->dbg_map != NULL) {
                uint32_t lin = (c->seg[CS].base + c->ip) & c->addr_mask;
                if (emudbg_map24(c->dbg_map, lin)) {
                    emudbg_hit((emudbg_cpu_t *)c->dbg, lin);
                    code_start(c, &q, c->ip);   /* (it may have changed IP) */
                }
            }
#endif
            trace = c->flags & TF;
        }
#ifdef X86_HOST_HOOK
        x86_host_hook(c);
#endif
        execute(c, &q, i286);
        if (i286) {
            clk = c->clk;
        } else {
            clk += queue_time(c, c->clk - clk);
            c->clk = clk;
            if (UNLIKELY(c->q_log_n)) {
                queue_check(c);
            }
        }
        n++;
        if (UNLIKELY(trace) && !c->inhibit) {
            c->ip0 = c->ip;
            c->sp0 = SP;
            interrupt(c, 1);                /* TF: after the instruction */
            c->clk += 50;
            clk = c->clk;
            c->bank = 0;
            n++;
        }
        if (UNLIKELY(c->resync)) {
            code_start(c, &q, c->ip);
        }
    }
    c->insns += n;
}

static X86_HOT NOINLINE void run_286(x86_t *c)
{
    run_body(c, true, false);
}

/* The 8088 and 8086 (the XT's 4.77 MHz need far less): not in internal RAM */
static NOINLINE void run_8086(x86_t *c)
{
    run_body(c, false, false);
}

#if EMUDBG_ENABLED
/* With the debugger's breakpoints (set before the run) */
static NOINLINE void run_debug(x86_t *c)
{
    run_body(c, c->model == X86_80286, true);
}
#endif

X86_HOT int32_t x86_run(x86_t *c, int32_t clocks)
{
    c->budget = clocks;
    c->clk = 0;
    c->in_run = true;
    if (setjmp(c->fault_jmp)) {
        deliver_fault(c);
        c->insns++;
    }
#if EMUDBG_ENABLED
    if (c->dbg_map != NULL) {
        run_debug(c);
    } else
#endif
    if (c->model == X86_80286) {
        run_286(c);
    } else {
        run_8086(c);
    }
    c->in_run = false;
    int32_t ran = c->clk;
    c->cycles += (uint64_t)ran;
    c->clk = c->budget = 0;
    return ran;
}

static NOINLINE void step_one(x86_t *c)
{
    code_t q;
    code_start(c, &q, c->ip);
    if (c->model == X86_80286) {
        execute(c, &q, true);
    } else {
        execute(c, &q, false);
    }
}

int32_t x86_step(x86_t *c)
{
    c->clk = c->budget = 0;
    c->single = true;
    c->in_run = true;
    if (setjmp(c->fault_jmp)) {
        deliver_fault(c);
    } else {
        step_one(c);
    }
    int32_t t = queue_time(c, c->clk);
    c->single = false;
    c->in_run = false;
    c->clk = 0;
    c->cycles += (uint64_t)t;
    c->insns++;
    return t;
}

/* ---- Outside ---- */

void x86_init(x86_t *c, x86_model_t model, const x86_bus_t *bus, const uintptr_t *page)
{
    memset(c, 0, sizeof(*c));
    c->model = model;
    c->bus = *bus;
    c->page = page;
    c->addr_mask = model == X86_80286 ? 0xFFFFFF : 0xFFFFF;
    c->max_len = model == X86_80286 ? 10 : 255;
    switch (model) {
    case X86_8088: c->wait[0] = c->wait[1] = 4; c->xfer = 4; break;
    case X86_8086: c->wait[0] = 0; c->wait[1] = 4; c->xfer = 4; break;
    default: c->wait[0] = 0; c->wait[1] = 2; c->xfer = 2; break;
    }
}

void x86_reset(x86_t *c)
{
    if (c->has_fpu) {
        bool error = c->fpu.error;
        x87_reset(&c->fpu, c->model != X86_80286);      /* (RESET reaches it too) */
        if (error && c->bus.fpu_error) {
            c->bus.fpu_error(c->bus.ctx, false);
        }
    }
    memset(&c->r, 0, sizeof(c->r));
    for (int s = 0; s < 4; s++) {
        full_seg(&c->seg[s], 0, 0);
    }
    c->flags = 0;
    c->lf_op = LF_NONE;
    if (c->model == X86_80286) {
        full_seg(&c->seg[CS], 0xF000, 0xFF0000);
        c->ip = 0xFFF0;
        c->msw = 0xFFF0;
    } else {
        full_seg(&c->seg[CS], 0xFFFF, 0xFFFF0);
        c->ip = 0;
    }
    c->idtr.base = 0;
    c->idtr.limit = 0x3FF;
    c->gdtr.base = 0;
    c->gdtr.limit = 0xFFFF;
    full_seg(&c->ldtr, 0, 0);
    full_seg(&c->tr, 0, 0);
    c->nmi = false;
    c->halted = false;
    c->inhibit = false;
    c->stuck = false;
    c->cpl = 0;
    c->ext = false;
    c->bank = 0;
    c->faulting = 0;
    c->q_log_n = c->q_old_n = 0;
    x86_flush(c);
    /* (from a bus callback: the instruction leaves IP so) */
    c->fetched = 0;
    c->flush = true;
    c->ip_set = true;
    c->resync = true;
}

void x86_set_fpu(x86_t *c, bool present)
{
    c->has_fpu = present;
    x87_reset(&c->fpu, c->model != X86_80286);
}

void x86_set_wait(x86_t *c, int waits)
{
    c->mem_wait = (uint8_t)waits;
    c->wait[0] = (uint8_t)(c->model == X86_8088 ? 4 + waits : waits);
    c->wait[1] = (uint8_t)(c->model == X86_80286 ? 2 + 2 * waits : 4 + 2 * waits);
}

void x86_flush(x86_t *c)
{
    c->win_n = 0;
    c->resync = true;
    if (++c->window_gen == 0) {         /* (0: the never-filled entries') */
        c->window_gen = 1;
        memset(c->windows, 0, sizeof(c->windows));
    }
    for (int s = 0; s < 4; s++) {
        seg_stale(&c->seg[s]);
    }
}

void x86_set_a20(x86_t *c, bool open)
{
    if (c->model == X86_80286) {
        c->addr_mask = open ? 0xFFFFFF : 0xEFFFFF;
        x86_flush(c);
    }
}

uint8_t x86_read8(x86_t *c, uint32_t addr)
{
    return mem_rd8(c, addr);
}

uint16_t x86_read16(x86_t *c, uint32_t addr)
{
    return mem_rd16(c, addr);
}

void x86_write8(x86_t *c, uint32_t addr, uint8_t v)
{
    mem_wr8(c, addr, v);
}

void x86_write16(x86_t *c, uint32_t addr, uint16_t v)
{
    mem_wr16(c, addr, v);
}

uint8_t x86_peek8(const x86_t *c, uint32_t addr)
{
    addr &= c->addr_mask;
    uintptr_t e = c->page[addr >> X86_PAGE_SHIFT];
    if (e & X86_NO_READ) {
        return 0xFF;
    }
    return ((const uint8_t *)(e & ~(uintptr_t)0xFFF))[addr & 0xFFF];
}
