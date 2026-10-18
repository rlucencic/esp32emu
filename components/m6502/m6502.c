/*
 * MOS 6502: see m6502.h. One switch over the opcodes; the addressing modes
 * are helpers that do the mode's bus cycles (dummy ones included) and
 * return the effective address.
 */
#include <stdio.h>
#include <string.h>
#include "m6502.h"
#include "emudbg.h"

#define C_  M6502_C
#define Z_  M6502_Z
#define I_  M6502_I
#define D_  M6502_D
#define B_  M6502_B
#define U_  M6502_U
#define V_  M6502_V
#define N_  M6502_N

/* The magic constants of ANE and LXA (they vary between chips) */
#define ANE_MAGIC   0xEF
#define LXA_MAGIC   0xEE

/* The helpers are inlined into the opcode switch: GCC would rather call
 * them (they're used hundreds of times), and on the ESP32 a call per bus
 * cycle, and the register window spills of a deep call chain, cost more
 * than the code size
 */
#define INLINE static inline __attribute__((always_inline))

/* ---- Bus cycles ---- */

/* An instruction runs on a copy of the registers and counts its cycles in
 * 32 bits (GCC keeps them in registers: in the m6502_t, every store
 * through a page pointer would make it reload them); the m6502_t is up to
 * date for the slow paths (the bus callbacks) and after the instruction
 */
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    bool quirk;                         /* a taken branch polls a cycle early */
    uint32_t n;                         /* cycles into the instruction */
    uint32_t lim;                       /* reads from cycle n >= lim take the slow path (a stall) */
    uint32_t wlim;                      /* writes likewise (lim with stall_writes, else never) */
    uint32_t until;                     /* no instruction starts from n >= until */
    uint32_t stop_pc;
    uint64_t base;                      /* the cycle the instruction started at */
} regs_t;

/* t - base, in 0 ... 1 << 30 */
static inline uint32_t relative(uint64_t t, uint64_t base)
{
    if (t <= base) {
        return 0;
    }
    uint64_t d = t - base;
    return d > (1u << 30) ? 1u << 30 : (uint32_t)d;
}

INLINE void load(m6502_t *c, regs_t *r)
{
    r->pc = c->pc;
    r->a = c->a;
    r->x = c->x;
    r->y = c->y;
    r->s = c->s;
    r->p = c->p;
    r->quirk = false;
    r->base = c->cycles;
    r->n = 0;
    r->lim = relative(c->stall_from, r->base);
    r->wlim = c->stall_writes ? r->lim : UINT32_MAX;
    r->until = relative(c->until, r->base);
    r->stop_pc = c->stop_pc;
    c->limits_changed = false;
}

INLINE void store(m6502_t *c, const regs_t *r)
{
    c->pc = r->pc;
    c->a = r->a;
    c->x = r->x;
    c->y = r->y;
    c->s = r->s;
    c->p = r->p;
    c->cycles = r->base + r->n;
}

/* A stall that the DMA tables answer: the access moves to the next free
 * cycle and the next stall point is the busy one after it; false when the
 * machine must see it (out of the table, or at dma_stop)
 */
static inline bool dma_skip(m6502_t *c, uint64_t now)
{
    if (!c->dma_free || now >= c->dma_stop) {
        return false;
    }
    uint64_t k = now - c->dma_base;
    if (k >= c->dma_len) {
        return false;
    }
    uint32_t f = c->dma_free[k];
    uint64_t t = c->dma_base + f;
    if (f >= c->dma_len || t >= c->dma_stop) {
        return false;
    }
    uint64_t s = c->dma_base + c->dma_busy[f + 1];
    c->cycles = t;
    c->stall_from = s < c->dma_stop ? s : c->dma_stop;
    c->limits_changed = true;
    return true;
}

/* The slow paths out of line, at cycle 'now': a stall, I/O */
static uint8_t __attribute__((noinline)) M6502_HOT rd_slow(m6502_t *c, uint16_t a, uint64_t now)
{
    c->cycles = now;
    if (now >= c->stall_from && c->bus.stall && !dma_skip(c, now)) {
        c->stall_is_write = false;
        c->bus.stall(c->bus.ctx, c);
    }
    const uint8_t *p = c->rd[a >> 8];
    uint8_t v = p ? p[a & 0xFF] : c->bus.read(c->bus.ctx, a);
    c->cycles++;
    return v;
}

static void __attribute__((noinline)) M6502_HOT wr_slow(m6502_t *c, uint16_t a, uint8_t v, uint64_t now)
{
    c->cycles = now;
    if (c->stall_writes && now >= c->stall_from && c->bus.stall && !dma_skip(c, now)) {
        c->stall_is_write = true;
        c->bus.stall(c->bus.ctx, c);
    }
    uint8_t *p = c->wr[a >> 8];
    if (p) {
        p[a & 0xFF] = v;
    } else {
        c->bus.write(c->bus.ctx, a, v);
    }
    c->cycles++;
}

/* poll_cycle relative to base (way back: a big negative number) */
INLINE int32_t poll_relative(const m6502_t *c, const regs_t *r)
{
    int64_t d = (int64_t)(c->poll_cycle - r->base);
    return d < -(1 << 30) ? -(1 << 30) : (int32_t)d;
}

/* After a slow path: the callbacks may have moved the clock (a stall), the
 * stall point and `until`
 */
INLINE void resync(m6502_t *c, regs_t *r)
{
    r->n = (uint32_t)c->cycles - (uint32_t)r->base;
    if (__builtin_expect(c->limits_changed, 0)) {
        c->limits_changed = false;
        r->lim = relative(c->stall_from, r->base);
        r->wlim = c->stall_writes ? r->lim : UINT32_MAX;
        r->until = relative(c->until, r->base);
    }
}

INLINE uint8_t rd(m6502_t *c, regs_t *r, uint16_t a)
{
    const uint8_t *p = c->rd[a >> 8];
    if (__builtin_expect(p != NULL && r->n < r->lim, 1)) {
        r->n++;
        return p[a & 0xFF];
    }
    uint8_t v = rd_slow(c, a, r->base + r->n);
    resync(c, r);
    return v;
}

INLINE void wr(m6502_t *c, regs_t *r, uint16_t a, uint8_t v)
{
    uint8_t *p = c->wr[a >> 8];
    if (__builtin_expect(p != NULL && r->n < r->wlim, 1)) {
        p[a & 0xFF] = v;
        r->n++;
        return;
    }
    wr_slow(c, a, v, r->base + r->n);
    resync(c, r);
}

INLINE uint8_t fetch(m6502_t *c, regs_t *r)
{
    return rd(c, r, r->pc++);
}

INLINE void push(m6502_t *c, regs_t *r, uint8_t v)
{
    wr(c, r, 0x100 | r->s, v);
    r->s--;
}

INLINE uint8_t pull(m6502_t *c, regs_t *r)
{
    r->s++;
    return rd(c, r, 0x100 | r->s);
}

INLINE void nz(m6502_t *c, regs_t *r, uint8_t v)
{
    r->p = (r->p & ~(N_ | Z_)) | (v & N_) | (v ? 0 : Z_);
}

/* ---- Addressing modes: the effective address, with the mode's cycles
 * up to (not including) the data access
 */

INLINE uint16_t am_zp(m6502_t *c, regs_t *r)
{
    return fetch(c, r);
}

INLINE uint16_t am_zpi(m6502_t *c, regs_t *r, uint8_t i)
{
    uint8_t z = fetch(c, r);
    rd(c, r, z);                           /* dummy read of the base */
    return (uint8_t)(z + i);
}

INLINE uint16_t am_abs(m6502_t *c, regs_t *r)
{
    uint16_t lo = fetch(c, r);
    return lo | (fetch(c, r) << 8);
}

/* Absolute,X/Y. Reads: the dummy read of the uncorrected address only on
 * a page crossing; writes and read-modify-writes: always
 */
INLINE uint16_t am_absi(m6502_t *c, regs_t *r, uint8_t i, bool always)
{
    uint16_t base = am_abs(c, r);
    uint16_t a = base + i;
    if (always || (a ^ base) & 0xFF00) {
        rd(c, r, (base & 0xFF00) | (a & 0x00FF));
    }
    return a;
}

INLINE uint16_t am_izx(m6502_t *c, regs_t *r)
{
    uint8_t z = fetch(c, r);
    rd(c, r, z);
    z += r->x;
    uint16_t lo = rd(c, r, z);
    return lo | (rd(c, r, (uint8_t)(z + 1)) << 8);
}

INLINE uint16_t am_izy(m6502_t *c, regs_t *r, bool always)
{
    uint8_t z = fetch(c, r);
    uint16_t lo = rd(c, r, z);
    uint16_t base = lo | (rd(c, r, (uint8_t)(z + 1)) << 8);
    uint16_t a = base + r->y;
    if (always || (a ^ base) & 0xFF00) {
        rd(c, r, (base & 0xFF00) | (a & 0x00FF));
    }
    return a;
}

/* ---- ALU ---- */

INLINE void op_adc(m6502_t *c, regs_t *r, uint8_t v)
{
    unsigned carry = r->p & C_;
    if ((r->p & D_) && c->decimal) {
        unsigned bin = r->a + v + carry;
        int al = (r->a & 0x0F) + (v & 0x0F) + carry;
        if (al >= 0x0A) {
            al = ((al + 0x06) & 0x0F) + 0x10;
        }
        int s = (r->a & 0xF0) + (v & 0xF0) + al;
        r->p &= ~(N_ | V_ | Z_ | C_);
        r->p |= (s & N_) | ((~(r->a ^ v) & (r->a ^ s) & 0x80) ? V_ : 0) | ((bin & 0xFF) ? 0 : Z_);
        if (s >= 0xA0) {
            s += 0x60;
        }
        r->p |= s >= 0x100 ? C_ : 0;
        r->a = s;
    } else {
        unsigned s = r->a + v + carry;
        r->p &= ~(V_ | C_);
        r->p |= ((~(r->a ^ v) & (r->a ^ s) & 0x80) ? V_ : 0) | (s >> 8);
        r->a = s;
        nz(c, r, r->a);
    }
}

INLINE void op_sbc(m6502_t *c, regs_t *r, uint8_t v)
{
    unsigned borrow = !(r->p & C_);
    unsigned bin = r->a - v - borrow;
    r->p &= ~(V_ | C_);
    r->p |= (((r->a ^ v) & (r->a ^ bin) & 0x80) ? V_ : 0) | (bin < 0x100 ? C_ : 0);
    if ((r->p & D_) && c->decimal) {
        int al = (r->a & 0x0F) - (v & 0x0F) - (int)borrow;
        if (al < 0) {
            al = ((al - 0x06) & 0x0F) - 0x10;
        }
        int s = (r->a & 0xF0) - (v & 0xF0) + al;
        if (s < 0) {
            s -= 0x60;
        }
        nz(c, r, bin);                     /* NMOS: N and Z from the binary result */
        r->a = s;
    } else {
        r->a = bin;
        nz(c, r, r->a);
    }
}

INLINE void op_cmp(m6502_t *c, regs_t *r, uint8_t reg, uint8_t v)
{
    unsigned d = reg - v;
    r->p = (r->p & ~C_) | (reg >= v ? C_ : 0);
    nz(c, r, d);
}

INLINE uint8_t op_asl(m6502_t *c, regs_t *r, uint8_t v)
{
    r->p = (r->p & ~C_) | (v >> 7);
    v <<= 1;
    nz(c, r, v);
    return v;
}

INLINE uint8_t op_lsr(m6502_t *c, regs_t *r, uint8_t v)
{
    r->p = (r->p & ~C_) | (v & 1);
    v >>= 1;
    nz(c, r, v);
    return v;
}

INLINE uint8_t op_rol(m6502_t *c, regs_t *r, uint8_t v)
{
    uint8_t res = (v << 1) | (r->p & C_);
    r->p = (r->p & ~C_) | (v >> 7);
    nz(c, r, res);
    return res;
}

INLINE uint8_t op_ror(m6502_t *c, regs_t *r, uint8_t v)
{
    uint8_t res = (v >> 1) | ((r->p & C_) << 7);
    r->p = (r->p & ~C_) | (v & 1);
    nz(c, r, res);
    return res;
}

INLINE void op_bit(m6502_t *c, regs_t *r, uint8_t v)
{
    r->p = (r->p & ~(N_ | V_ | Z_)) | (v & (N_ | V_)) | ((r->a & v) ? 0 : Z_);
}

INLINE void op_arr(m6502_t *c, regs_t *r, uint8_t v)
{
    uint8_t t = r->a & v;
    unsigned carry = r->p & C_;
    uint8_t res = (t >> 1) | (carry << 7);
    if ((r->p & D_) && c->decimal) {
        r->p = (r->p & ~(N_ | Z_ | V_ | C_)) | (carry ? N_ : 0) | (res ? 0 : Z_) | (((res ^ t) & 0x40) ? V_ : 0);
        if ((t & 0x0F) + (t & 0x01) > 5) {
            res = (res & 0xF0) | ((res + 6) & 0x0F);
        }
        if ((t & 0xF0) + (t & 0x10) > 0x50) {
            res = (res & 0x0F) | ((res + 0x60) & 0xF0);
            r->p |= C_;
        }
        r->a = res;
    } else {
        r->a = res;
        nz(c, r, res);
        r->p = (r->p & ~(C_ | V_)) | ((res >> 6) & 1) | ((((res >> 6) ^ (res >> 5)) & 1) ? V_ : 0);
    }
}

/* SHA SHX SHY TAS: the value stored is reg & (high byte of the base + 1);
 * when the index crossed a page that value becomes the high byte too
 */
INLINE void op_sh(m6502_t *c, regs_t *r, uint16_t base, uint8_t index, uint8_t reg)
{
    uint16_t a = base + index;
    uint8_t v = reg & ((base >> 8) + 1);
    if ((a ^ base) & 0xFF00) {
        a = (v << 8) | (a & 0xFF);
    }
    wr(c, r, a, v);
}

/* ---- Interrupts ---- */

/* After an IRQ or BRK sequence: no interrupt before the handler's first
 * instruction, but an NMI by its fourth cycle takes over the vector
 */
static void sequence_end(m6502_t *c, uint64_t start)
{
    c->poll_i = I_;
    c->irq_seen = false;
    c->poll_cycle = c->hijack_cycle = start + 3;
    if (c->nmi_pending && c->nmi_cycle <= c->hijack_cycle) {
        c->nmi_pending = false;
        c->pc = m6502_peek(c, 0xFFFA) | (m6502_peek(c, 0xFFFB) << 8);
    }
}

static int interrupt(m6502_t *c, uint16_t vector)
{
    regs_t regs, *r = &regs;
    load(c, r);
    rd(c, r, r->pc);
    rd(c, r, r->pc);
    push(c, r, r->pc >> 8);
    push(c, r, r->pc);
    push(c, r, (r->p & ~B_) | U_);
    r->p |= I_;
    uint16_t lo = rd(c, r, vector);
    r->pc = lo | (rd(c, r, vector + 1) << 8);
    store(c, r);
    if (vector == 0xFFFE) {
        sequence_end(c, r->base);
    } else {
        c->poll_i = I_;
        c->poll_cycle = r->base + 3;
    }
    return r->n;
}

/* The machine brought its devices up to poll_cycle after a step */
void m6502_repoll(m6502_t *c)
{
    c->irq_seen = c->irq && !c->poll_i;
    if (c->nmi_pending && c->poll_cycle == c->hijack_cycle && c->nmi_cycle <= c->hijack_cycle) {
        c->nmi_pending = false;
        c->pc = m6502_peek(c, 0xFFFA) | (m6502_peek(c, 0xFFFB) << 8);
    }
}

void m6502_init(m6502_t *cpu, const m6502_bus_t *bus)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = *bus;
    cpu->decimal = true;
    cpu->stall_from = UINT64_MAX;
    cpu->until = 0;
    cpu->stop_pc = 0x10000;
    cpu->s = 0x00;                      /* the reset's three pulls leave $FD */
    cpu->p = I_ | U_;
}

void m6502_reset(m6502_t *cpu)
{
    cpu->jammed = false;
    cpu->nmi_pending = false;
    cpu->irq_seen = false;
    cpu->poll_i = I_;
    regs_t regs, *r = &regs;
    load(cpu, r);
    /* three stack reads, no writes */
    rd(cpu, r, r->pc);
    rd(cpu, r, r->pc);
    rd(cpu, r, 0x100 | r->s--);
    rd(cpu, r, 0x100 | r->s--);
    rd(cpu, r, 0x100 | r->s--);
    r->p |= I_ | U_;
    uint16_t lo = rd(cpu, r, 0xFFFC);
    r->pc = lo | (rd(cpu, r, 0xFFFD) << 8);
    store(cpu, r);
}

uint8_t m6502_peek(const m6502_t *cpu, uint16_t addr)
{
    const uint8_t *p = cpu->rd[addr >> 8];
    return p ? p[addr & 0xFF] : 0xFF;
}

/* ---- Instructions ---- */

/* Branch: 2 cycles, 3 if taken, 4 if it crosses a page */
/* spin: the branch back that a wait loop takes (stop_on_spin) */
INLINE void branch(m6502_t *c, regs_t *r, bool take, int spin)
{
    int8_t off = fetch(c, r);
    if (!take) {
        return;
    }
    if (__builtin_expect(off == spin, 0) && c->stop_on_spin) {
        r->until = r->n;                /* (after this instruction) */
    }
    rd(c, r, r->pc);
    uint16_t a = r->pc + off;
    if ((a ^ r->pc) & 0xFF00) {
        rd(c, r, (r->pc & 0xFF00) | (a & 0xFF));
    } else {
        r->quirk = true;         /* polls IRQ as if not taken */
    }
    r->pc = a;
}

#define READ(mode_addr, body) do { uint16_t ea = (mode_addr); uint8_t v = rd(c, r, ea); body; } while (0)
#define WRITE(mode_addr, value) do { uint16_t ea = (mode_addr); wr(c, r, ea, (value)); } while (0)
/* read, write the old value back, write the new one (stmt changes v) */
#define RMW(mode_addr, stmt) do { uint16_t ea = (mode_addr); uint8_t v = rd(c, r, ea); wr(c, r, ea, v); \
                                  stmt; wr(c, r, ea, v); } while (0)

#ifdef M6502_HOST_HOOK
void m6502_host_hook(m6502_t *c);       /* host builds: tracing */
#endif

/* Not in IRAM (64 KB of code): the cache serves it about as fast, and
 * internal RAM is scarce
 */
void m6502_run(m6502_t *c)
{
    regs_t regs, *r = &regs;
    load(c, r);
    uint8_t i_before = c->poll_i;
    bool irq_seen = c->irq_seen;
    /* the previous instruction's poll cycle, relative to base */
    int32_t poll = poll_relative(c, r);
    while (r->n < r->until) {
#ifdef M6502_HOST_HOOK
        store(c, r);
        m6502_host_hook(c);
#endif
#if EMUDBG_ENABLED
        const uint8_t *map = c->dbg_map;
        if (__builtin_expect(map != NULL, 0) && emudbg_map16(map, r->pc)) {
            /* The debugger may stop here and change the registers */
            c->poll_i = i_before;
            c->poll_cycle = r->base + (int64_t)poll;
            c->irq_seen = irq_seen;
            store(c, r);
            emudbg_hit(c->dbg, c->pc);
            load(c, r);
            i_before = c->poll_i;
            irq_seen = c->irq_seen;
            poll = poll_relative(c, r);
        }
#endif
        if (__builtin_expect(c->jammed || c->nmi_pending || irq_seen, 0)) {
            if (c->jammed) {
                r->n++;
                i_before = I_;
                poll = r->n;
                continue;
            }
            uint16_t vector = 0;
            if (c->nmi_pending && (int64_t)(c->nmi_cycle - r->base) <= (int64_t)poll) {
                c->nmi_pending = false;
                vector = 0xFFFA;
            } else if (irq_seen) {
                vector = 0xFFFE;
            }
            if (vector) {
                irq_seen = false;
                store(c, r);
                interrupt(c, vector);
                load(c, r);
                i_before = I_;
                poll = poll_relative(c, r);
                continue;
            }
        }
        /* IRQ is polled with the I flag as it was before this instruction
         * (CLI, SEI and PLP act one instruction late; RTI at once)
         */
        uint32_t n0 = r->n;
        i_before = r->p & I_;
        uint8_t op = fetch(c, r);
        switch (op) {
        /* ---- Loads and stores ---- */
        case 0xA9: r->a = fetch(c, r); nz(c, r, r->a); break;
        case 0xA5: READ(am_zp(c, r), r->a = v; nz(c, r, v)); break;
        case 0xB5: READ(am_zpi(c, r, r->x), r->a = v; nz(c, r, v)); break;
        case 0xAD: READ(am_abs(c, r), r->a = v; nz(c, r, v)); break;
        case 0xBD: READ(am_absi(c, r, r->x, false), r->a = v; nz(c, r, v)); break;
        case 0xB9: READ(am_absi(c, r, r->y, false), r->a = v; nz(c, r, v)); break;
        case 0xA1: READ(am_izx(c, r), r->a = v; nz(c, r, v)); break;
        case 0xB1: READ(am_izy(c, r, false), r->a = v; nz(c, r, v)); break;
        case 0xA2: r->x = fetch(c, r); nz(c, r, r->x); break;
        case 0xA6: READ(am_zp(c, r), r->x = v; nz(c, r, v)); break;
        case 0xB6: READ(am_zpi(c, r, r->y), r->x = v; nz(c, r, v)); break;
        case 0xAE: READ(am_abs(c, r), r->x = v; nz(c, r, v)); break;
        case 0xBE: READ(am_absi(c, r, r->y, false), r->x = v; nz(c, r, v)); break;
        case 0xA0: r->y = fetch(c, r); nz(c, r, r->y); break;
        case 0xA4: READ(am_zp(c, r), r->y = v; nz(c, r, v)); break;
        case 0xB4: READ(am_zpi(c, r, r->x), r->y = v; nz(c, r, v)); break;
        case 0xAC: READ(am_abs(c, r), r->y = v; nz(c, r, v)); break;
        case 0xBC: READ(am_absi(c, r, r->x, false), r->y = v; nz(c, r, v)); break;
        case 0x85: WRITE(am_zp(c, r), r->a); break;
        case 0x95: WRITE(am_zpi(c, r, r->x), r->a); break;
        case 0x8D: WRITE(am_abs(c, r), r->a); break;
        case 0x9D: WRITE(am_absi(c, r, r->x, true), r->a); break;
        case 0x99: WRITE(am_absi(c, r, r->y, true), r->a); break;
        case 0x81: WRITE(am_izx(c, r), r->a); break;
        case 0x91: WRITE(am_izy(c, r, true), r->a); break;
        case 0x86: WRITE(am_zp(c, r), r->x); break;
        case 0x96: WRITE(am_zpi(c, r, r->y), r->x); break;
        case 0x8E: WRITE(am_abs(c, r), r->x); break;
        case 0x84: WRITE(am_zp(c, r), r->y); break;
        case 0x94: WRITE(am_zpi(c, r, r->x), r->y); break;
        case 0x8C: WRITE(am_abs(c, r), r->y); break;

        /* ---- Transfers, stack ---- */
        case 0xAA: rd(c, r, r->pc); r->x = r->a; nz(c, r, r->x); break;
        case 0xA8: rd(c, r, r->pc); r->y = r->a; nz(c, r, r->y); break;
        case 0x8A: rd(c, r, r->pc); r->a = r->x; nz(c, r, r->a); break;
        case 0x98: rd(c, r, r->pc); r->a = r->y; nz(c, r, r->a); break;
        case 0xBA: rd(c, r, r->pc); r->x = r->s; nz(c, r, r->x); break;
        case 0x9A: rd(c, r, r->pc); r->s = r->x; break;
        case 0x48: rd(c, r, r->pc); push(c, r, r->a); break;
        case 0x08: rd(c, r, r->pc); push(c, r, r->p | B_ | U_); break;
        case 0x68: rd(c, r, r->pc); rd(c, r, 0x100 | r->s); r->a = pull(c, r); nz(c, r, r->a); break;
        case 0x28: rd(c, r, r->pc); rd(c, r, 0x100 | r->s); r->p = (pull(c, r) & ~B_) | U_; break;

        /* ---- ALU ---- */
    #define ALU_OPS(base, fn) \
        case base + 0x09: { uint8_t v = fetch(c, r); fn; break; } \
        case base + 0x05: READ(am_zp(c, r), fn); break; \
        case base + 0x15: READ(am_zpi(c, r, r->x), fn); break; \
        case base + 0x0D: READ(am_abs(c, r), fn); break; \
        case base + 0x1D: READ(am_absi(c, r, r->x, false), fn); break; \
        case base + 0x19: READ(am_absi(c, r, r->y, false), fn); break; \
        case base + 0x01: READ(am_izx(c, r), fn); break; \
        case base + 0x11: READ(am_izy(c, r, false), fn); break;
        ALU_OPS(0x00, r->a |= v; nz(c, r, r->a))
        ALU_OPS(0x20, r->a &= v; nz(c, r, r->a))
        ALU_OPS(0x40, r->a ^= v; nz(c, r, r->a))
        ALU_OPS(0x60, op_adc(c, r, v))
        ALU_OPS(0xC0, op_cmp(c, r, r->a, v))
        ALU_OPS(0xE0, op_sbc(c, r, v))
    #undef ALU_OPS
        case 0xEB: op_sbc(c, r, fetch(c, r)); break;
        case 0xE0: op_cmp(c, r, r->x, fetch(c, r)); break;
        case 0xE4: READ(am_zp(c, r), op_cmp(c, r, r->x, v)); break;
        case 0xEC: READ(am_abs(c, r), op_cmp(c, r, r->x, v)); break;
        case 0xC0: op_cmp(c, r, r->y, fetch(c, r)); break;
        case 0xC4: READ(am_zp(c, r), op_cmp(c, r, r->y, v)); break;
        case 0xCC: READ(am_abs(c, r), op_cmp(c, r, r->y, v)); break;
        case 0x24: READ(am_zp(c, r), op_bit(c, r, v)); break;
        case 0x2C: READ(am_abs(c, r), op_bit(c, r, v)); break;

        /* ---- Shifts and increments ---- */
    #define RMW_OPS(base, fn) \
        case base + 0x06: RMW(am_zp(c, r), fn); break; \
        case base + 0x16: RMW(am_zpi(c, r, r->x), fn); break; \
        case base + 0x0E: RMW(am_abs(c, r), fn); break; \
        case base + 0x1E: RMW(am_absi(c, r, r->x, true), fn); break;
        RMW_OPS(0x00, v = op_asl(c, r, v))
        RMW_OPS(0x20, v = op_rol(c, r, v))
        RMW_OPS(0x40, v = op_lsr(c, r, v))
        RMW_OPS(0x60, v = op_ror(c, r, v))
        RMW_OPS(0xC0, v--; nz(c, r, v))
        RMW_OPS(0xE0, v++; nz(c, r, v))
    #undef RMW_OPS
        case 0x0A: rd(c, r, r->pc); r->a = op_asl(c, r, r->a); break;
        case 0x2A: rd(c, r, r->pc); r->a = op_rol(c, r, r->a); break;
        case 0x4A: rd(c, r, r->pc); r->a = op_lsr(c, r, r->a); break;
        case 0x6A: rd(c, r, r->pc); r->a = op_ror(c, r, r->a); break;
        case 0xE8: rd(c, r, r->pc); r->x++; nz(c, r, r->x); break;
        case 0xC8: rd(c, r, r->pc); r->y++; nz(c, r, r->y); break;
        case 0xCA: rd(c, r, r->pc); r->x--; nz(c, r, r->x); break;
        case 0x88: rd(c, r, r->pc); r->y--; nz(c, r, r->y); break;

        /* ---- Flags ---- */
        case 0x18: rd(c, r, r->pc); r->p &= ~C_; break;
        case 0x38: rd(c, r, r->pc); r->p |= C_; break;
        case 0x58: rd(c, r, r->pc); r->p &= ~I_; break;
        case 0x78: rd(c, r, r->pc); r->p |= I_; break;
        case 0xB8: rd(c, r, r->pc); r->p &= ~V_; break;
        case 0xD8: rd(c, r, r->pc); r->p &= ~D_; break;
        case 0xF8: rd(c, r, r->pc); r->p |= D_; break;

        /* ---- Jumps ---- */
        case 0x4C: r->pc = am_abs(c, r); break;
        case 0x6C: {
            uint16_t a = am_abs(c, r);
            uint16_t lo = rd(c, r, a);
            r->pc = lo | (rd(c, r, (a & 0xFF00) | ((a + 1) & 0xFF)) << 8);    /* no carry into the page */
            break;
        }
        case 0x20: {
            uint16_t lo = fetch(c, r);
            rd(c, r, 0x100 | r->s);
            push(c, r, r->pc >> 8);
            push(c, r, r->pc);
            r->pc = lo | (fetch(c, r) << 8);
            break;
        }
        case 0x60: {
            rd(c, r, r->pc);
            rd(c, r, 0x100 | r->s);
            uint16_t lo = pull(c, r);
            r->pc = lo | (pull(c, r) << 8);
            rd(c, r, r->pc++);
            break;
        }
        case 0x40: {
            rd(c, r, r->pc);
            rd(c, r, 0x100 | r->s);
            r->p = (pull(c, r) & ~B_) | U_;
            uint16_t lo = pull(c, r);
            r->pc = lo | (pull(c, r) << 8);
            i_before = r->p & I_;           /* RTI's I counts at once */
            break;
        }
        case 0x00: {
            fetch(c, r);                       /* the signature byte */
            push(c, r, r->pc >> 8);
            push(c, r, r->pc);
            push(c, r, r->p | B_ | U_);
            r->p |= I_;
            uint16_t lo = rd(c, r, 0xFFFE);
            r->pc = lo | (rd(c, r, 0xFFFF) << 8);
            store(c, r);
            sequence_end(c, r->base + n0);
            load(c, r);
            i_before = I_;
            irq_seen = false;
            poll = poll_relative(c, r);
            goto next;
        }
        case 0x10: branch(c, r, !(r->p & N_), -5); break;
        case 0x30: branch(c, r, r->p & N_, -5); break;
        case 0x50: branch(c, r, !(r->p & V_), -2); break;
        case 0x70: branch(c, r, r->p & V_, -2); break;
        case 0x90: branch(c, r, !(r->p & C_), 0x100); break;
        case 0xB0: branch(c, r, r->p & C_, 0x100); break;
        case 0xD0: branch(c, r, !(r->p & Z_), 0x100); break;
        case 0xF0: branch(c, r, r->p & Z_, 0x100); break;

        /* ---- NOPs ---- */
        case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
            rd(c, r, r->pc);
            break;
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
            fetch(c, r);
            break;
        case 0x04: case 0x44: case 0x64: READ(am_zp(c, r), (void)v); break;
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: READ(am_zpi(c, r, r->x), (void)v); break;
        case 0x0C: READ(am_abs(c, r), (void)v); break;
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
            READ(am_absi(c, r, r->x, false), (void)v);
            break;

        /* ---- Undocumented: read-modify-write combinations ---- */
    #define UND_RMW(base, fn) \
        case base + 0x07: RMW(am_zp(c, r), fn); break; \
        case base + 0x17: RMW(am_zpi(c, r, r->x), fn); break; \
        case base + 0x0F: RMW(am_abs(c, r), fn); break; \
        case base + 0x1F: RMW(am_absi(c, r, r->x, true), fn); break; \
        case base + 0x1B: RMW(am_absi(c, r, r->y, true), fn); break; \
        case base + 0x03: RMW(am_izx(c, r), fn); break; \
        case base + 0x13: RMW(am_izy(c, r, true), fn); break;
        UND_RMW(0x00, v = op_asl(c, r, v); r->a |= v; nz(c, r, r->a))                 /* SLO */
        UND_RMW(0x20, v = op_rol(c, r, v); r->a &= v; nz(c, r, r->a))                 /* RLA */
        UND_RMW(0x40, v = op_lsr(c, r, v); r->a ^= v; nz(c, r, r->a))                 /* SRE */
        UND_RMW(0x60, v = op_ror(c, r, v); op_adc(c, r, v))                           /* RRA */
        UND_RMW(0xC0, v--; op_cmp(c, r, r->a, v))                                  /* DCP */
        UND_RMW(0xE0, v++; op_sbc(c, r, v))                                        /* ISC */
    #undef UND_RMW
        /* SAX, LAX */
        case 0x87: WRITE(am_zp(c, r), r->a & r->x); break;
        case 0x97: WRITE(am_zpi(c, r, r->y), r->a & r->x); break;
        case 0x8F: WRITE(am_abs(c, r), r->a & r->x); break;
        case 0x83: WRITE(am_izx(c, r), r->a & r->x); break;
        case 0xA7: READ(am_zp(c, r), r->a = r->x = v; nz(c, r, v)); break;
        case 0xB7: READ(am_zpi(c, r, r->y), r->a = r->x = v; nz(c, r, v)); break;
        case 0xAF: READ(am_abs(c, r), r->a = r->x = v; nz(c, r, v)); break;
        case 0xBF: READ(am_absi(c, r, r->y, false), r->a = r->x = v; nz(c, r, v)); break;
        case 0xA3: READ(am_izx(c, r), r->a = r->x = v; nz(c, r, v)); break;
        case 0xB3: READ(am_izy(c, r, false), r->a = r->x = v; nz(c, r, v)); break;
        /* Immediate ones */
        case 0x0B: case 0x2B:                                                   /* ANC */
            r->a &= fetch(c, r);
            nz(c, r, r->a);
            r->p = (r->p & ~C_) | (r->a >> 7);
            break;
        case 0x4B:                                                              /* ALR */
            r->a &= fetch(c, r);
            r->a = op_lsr(c, r, r->a);
            break;
        case 0x6B: op_arr(c, r, fetch(c, r)); break;                                  /* ARR */
        case 0xCB: {                                                            /* SBX */
            uint8_t v = fetch(c, r);
            unsigned res = (r->a & r->x) - v;
            r->p = (r->p & ~C_) | (res < 0x100 ? C_ : 0);
            r->x = res;
            nz(c, r, r->x);
            break;
        }
        case 0x8B: {                                                            /* ANE */
            uint8_t v = fetch(c, r);
            r->a = (r->a | ANE_MAGIC) & r->x & v;
            nz(c, r, r->a);
            break;
        }
        case 0xAB: {                                                            /* LXA */
            uint8_t v = fetch(c, r);
            r->a = r->x = (r->a | LXA_MAGIC) & v;
            nz(c, r, r->a);
            break;
        }
        case 0xBB: READ(am_absi(c, r, r->y, false), r->a = r->x = r->s = v & r->s; nz(c, r, r->a)); break;    /* LAS */
        /* The unstable stores */
        case 0x9F: { uint16_t b = am_abs(c, r); rd(c, r, (b & 0xFF00) | ((b + r->y) & 0xFF)); op_sh(c, r, b, r->y, r->a & r->x); break; }
        case 0x93: {
            uint8_t z = fetch(c, r);
            uint16_t lo = rd(c, r, z);
            uint16_t b = lo | (rd(c, r, (uint8_t)(z + 1)) << 8);
            rd(c, r, (b & 0xFF00) | ((b + r->y) & 0xFF));
            op_sh(c, r, b, r->y, r->a & r->x);
            break;
        }
        case 0x9E: { uint16_t b = am_abs(c, r); rd(c, r, (b & 0xFF00) | ((b + r->y) & 0xFF)); op_sh(c, r, b, r->y, r->x); break; }
        case 0x9C: { uint16_t b = am_abs(c, r); rd(c, r, (b & 0xFF00) | ((b + r->x) & 0xFF)); op_sh(c, r, b, r->x, r->y); break; }
        case 0x9B: {
            uint16_t b = am_abs(c, r);
            rd(c, r, (b & 0xFF00) | ((b + r->y) & 0xFF));
            r->s = r->a & r->x;
            op_sh(c, r, b, r->y, r->s);
            break;
        }

        /* ---- JAM ---- */
        default:
            c->jammed = true;
            r->pc--;
            break;
        }
        poll = (int32_t)r->n - 2 - r->quirk;
        r->quirk = false;
        irq_seen = c->irq && !i_before;
    next:
        if (r->pc == r->stop_pc) {
            break;
        }
    }
    store(c, r);
    c->poll_i = i_before;
    c->poll_cycle = r->base + (int64_t)poll;
    c->irq_seen = irq_seen;
}

int m6502_step(m6502_t *c)
{
    uint64_t start = c->cycles, until = c->until;
    uint32_t stop = c->stop_pc;
    c->until = start + 1;
    c->stop_pc = 0x10000;
    m6502_run(c);
    c->until = until;
    c->stop_pc = stop;
    return (int)(c->cycles - start);
}

/* ---- Disassembler (debugging) ---- */

enum { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IZX, IZY, IND, REL };

static const char s_names[256][4] = {
    "BRK","ORA","JAM","SLO","NOP","ORA","ASL","SLO","PHP","ORA","ASL","ANC","NOP","ORA","ASL","SLO",
    "BPL","ORA","JAM","SLO","NOP","ORA","ASL","SLO","CLC","ORA","NOP","SLO","NOP","ORA","ASL","SLO",
    "JSR","AND","JAM","RLA","BIT","AND","ROL","RLA","PLP","AND","ROL","ANC","BIT","AND","ROL","RLA",
    "BMI","AND","JAM","RLA","NOP","AND","ROL","RLA","SEC","AND","NOP","RLA","NOP","AND","ROL","RLA",
    "RTI","EOR","JAM","SRE","NOP","EOR","LSR","SRE","PHA","EOR","LSR","ALR","JMP","EOR","LSR","SRE",
    "BVC","EOR","JAM","SRE","NOP","EOR","LSR","SRE","CLI","EOR","NOP","SRE","NOP","EOR","LSR","SRE",
    "RTS","ADC","JAM","RRA","NOP","ADC","ROR","RRA","PLA","ADC","ROR","ARR","JMP","ADC","ROR","RRA",
    "BVS","ADC","JAM","RRA","NOP","ADC","ROR","RRA","SEI","ADC","NOP","RRA","NOP","ADC","ROR","RRA",
    "NOP","STA","NOP","SAX","STY","STA","STX","SAX","DEY","NOP","TXA","ANE","STY","STA","STX","SAX",
    "BCC","STA","JAM","SHA","STY","STA","STX","SAX","TYA","STA","TXS","TAS","SHY","STA","SHX","SHA",
    "LDY","LDA","LDX","LAX","LDY","LDA","LDX","LAX","TAY","LDA","TAX","LXA","LDY","LDA","LDX","LAX",
    "BCS","LDA","JAM","LAX","LDY","LDA","LDX","LAX","CLV","LDA","TSX","LAS","LDY","LDA","LDX","LAX",
    "CPY","CMP","NOP","DCP","CPY","CMP","DEC","DCP","INY","CMP","DEX","SBX","CPY","CMP","DEC","DCP",
    "BNE","CMP","JAM","DCP","NOP","CMP","DEC","DCP","CLD","CMP","NOP","DCP","NOP","CMP","DEC","DCP",
    "CPX","SBC","NOP","ISC","CPX","SBC","INC","ISC","INX","SBC","NOP","SBC","CPX","SBC","INC","ISC",
    "BEQ","SBC","JAM","ISC","NOP","SBC","INC","ISC","SED","SBC","NOP","ISC","NOP","SBC","INC","ISC",
};

static int mode_of(uint8_t op)
{
    int lo = op & 0x1F;
    if (op == 0x20) return ABS;
    if (op == 0x6C) return IND;
    if (op == 0x4C) return ABS;
    if ((op & 0x9F) == 0x00) return op == 0x00 || op == 0x40 || op == 0x60 ? IMP : IMM;
    switch (lo) {
    case 0x00: return op & 0x80 ? IMM : IMP;
    case 0x01: case 0x03: return IZX;
    case 0x02: return op >= 0x80 ? IMM : IMP;
    case 0x04: case 0x05: case 0x06: case 0x07: return ZP;
    case 0x08: return IMP;
    case 0x09: case 0x0B: return IMM;
    case 0x0A: return op < 0x80 ? ACC : IMP;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: return ABS;
    case 0x10: return REL;
    case 0x11: case 0x13: return IZY;
    case 0x12: return IMP;
    case 0x14: case 0x15: return ZPX;
    case 0x16: case 0x17: return (op & 0xC0) == 0x80 ? ZPY : ZPX;
    case 0x18: case 0x1A: return IMP;
    case 0x19: case 0x1B: return ABY;
    case 0x1C: case 0x1D: return ABX;
    default: return (op & 0xC0) == 0x80 ? ABY : ABX;
    }
}

int m6502_disassemble(const m6502_t *cpu, uint16_t pc, char *buf, int len)
{
    uint8_t op = m6502_peek(cpu, pc);
    uint8_t b1 = m6502_peek(cpu, pc + 1), b2 = m6502_peek(cpu, pc + 2);
    uint16_t w = b1 | (b2 << 8);
    const char *n = s_names[op];
    switch (mode_of(op)) {
    case IMP: snprintf(buf, len, "%s", n); return 1;
    case ACC: snprintf(buf, len, "%s A", n); return 1;
    case IMM: snprintf(buf, len, "%s #$%02X", n, b1); return 2;
    case ZP: snprintf(buf, len, "%s $%02X", n, b1); return 2;
    case ZPX: snprintf(buf, len, "%s $%02X,X", n, b1); return 2;
    case ZPY: snprintf(buf, len, "%s $%02X,Y", n, b1); return 2;
    case IZX: snprintf(buf, len, "%s ($%02X,X)", n, b1); return 2;
    case IZY: snprintf(buf, len, "%s ($%02X),Y", n, b1); return 2;
    case REL: snprintf(buf, len, "%s $%04X", n, (uint16_t)(pc + 2 + (int8_t)b1)); return 2;
    case ABS: snprintf(buf, len, "%s $%04X", n, w); return 3;
    case ABX: snprintf(buf, len, "%s $%04X,X", n, w); return 3;
    case ABY: snprintf(buf, len, "%s $%04X,Y", n, w); return 3;
    default: snprintf(buf, len, "%s ($%04X)", n, w); return 3;
    }
}
