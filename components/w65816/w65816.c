/*
 * WDC 65C816: see w65816.h. One switch over the opcodes; the 8 and 16-bit
 * cases are tested inline: M and X are bits of P, which stays in a
 * register, so each test is one branch (BBCI on Xtensa). Dispatching on the
 * four m/x combinations would save that one instruction, for three to four
 * times the code (73 KB as it is, against a 32 KB instruction cache) and a
 * trip out of the loop at every REP and SEP. The addressing modes are
 * helpers that make the mode's bus accesses, count its internal cycles and
 * return the 24-bit effective address.
 *
 * Speed, counted in QEMU's ESP32-S3 (-icount: one instruction a ns) on
 * test/speed.c's mix: 38 Xtensa instructions per 65816 instruction (11 per
 * cycle). The first version took 72: the code page pointer (fetches), N
 * and Z apart from P, and one down-counter for the cycles made the
 * difference (see below).
 *
 * The rules (Bruce Clark, "65C816 Opcodes", 6502.org, and the WDC data
 * sheet; the test suite agrees):
 *   - Data addresses run on into the next bank (abs,X; a 16-bit operand's
 *     second byte at $xxFFFF); direct page, stack and the pointers of
 *     JMP (abs) / JML [abs] stay in bank 0, JMP/JSR (abs,X) in the program
 *     bank, and the program counter wraps in its bank.
 *   - Emulation mode keeps S in page 1. The 6502's instructions wrap it
 *     there at each push and pull; the 65816's own (PEA PEI PER PHD PLD PLB
 *     JSL RTL, JSR (abs,X)) let it run over 16 bits during the instruction
 *     and put it back in page 1 at the end.
 *   - With e = 1 and DL = 0 the 6502's direct page modes wrap in the page
 *     (dp,X; the pointers of (dp), (dp,X), (dp),Y); [dp] and PEI never do.
 */
#include <stddef.h>
#include <stdint.h>
#include "w65816.h"
#include "emudbg.h"

#define C_  W65816_C
#define Z_  W65816_Z
#define I_  W65816_I
#define D_  W65816_D
#define X_  W65816_X
#define M_  W65816_M
#define V_  W65816_V
#define N_  W65816_N
#define B_  W65816_B

/* Where a 16-bit operand's second byte is: (ea + 1) & DATA runs on into
 * the next bank, & BANK0 wraps in bank 0 (direct page, stack)
 */
#define DATA    0xFFFFFFu
#define BANK0   0xFFFFu

/* The helpers are left to the compiler: forced into every opcode, the run
 * loop was 72 KB of code, twice the ESP32-S3's instruction cache, and code
 * runs from PSRAM: a cache miss on nearly every instruction (~850 cycles
 * each in the IIGS). Built with -Os it's 18 KB.
 */
#define INLINE  static inline
#define NOINLINE __attribute__((noinline))

/* ---- Registers and cycles ---- */

/* An instruction runs on a copy of the registers (GCC keeps the busy ones
 * in registers: in the w65816_t, every store through a page pointer would
 * make it reload them). The w65816_t is up to date for WDM and after the
 * run; bus callbacks get cycles only.
 *
 * N and Z are kept apart, as the last result: most instructions set them
 * and the branches test them, and both are then a move or a test instead
 * of a change to P.
 *
 * The run counts its cycles down in `left`; the instruction loop runs while
 * it's above 0. To go back to the outer loop after an instruction (a bus
 * callback ran: irq, nmi, end_run may have changed; CLI, WAI, ...) the
 * cycles left move to run_held and left becomes 0.
 *
 * The program counter is a pointer into the code's page (half of the bus
 * accesses are fetches): ip, up to ip_end. The PC itself is
 * cpu->code_pc + (ip - cpu->code_base), worked out where it's needed
 * (calls, PER, interrupts, branches out of the page). At the end of the
 * page, after a jump and after every bus callback (the machine may have
 * changed the page tables) ip == ip_end, and the next fetch looks the page
 * up again (refill()). Code in a page without a pointer (I/O) is fetched
 * through the bus, byte by byte: ip = ip_end = code_base = s_nocode.
 */
typedef struct {
    uint32_t pbr, dbr;              /* the banks, << 16 */
    const uint8_t *ip, *ip_end;
    uint32_t a, x, y, s, d;         /* 16 bits (no truncating at every use) */
    int32_t dl;                     /* DL is not 0: the direct page modes' extra cycle */
    uint8_t p;                      /* N and Z: in nf and zf */
    bool e;
    uint32_t nf;                    /* N is bit 7 */
    uint32_t zf;                    /* Z is zf == 0 */
    int32_t left;                   /* cycles left */
    uint8_t **rdt, **wrt;
} regs_t;

static const uint8_t s_nocode[1];

INLINE uint16_t get_pc(const w65816_t *c, const regs_t *r)
{
    return c->code_pc + (uint32_t)(r->ip - c->code_base);
}

/* A jump: the next fetch looks the page up */
INLINE void set_pc(w65816_t *c, regs_t *r, uint16_t pc)
{
    c->code_pc = pc;
    c->code_base = r->ip = r->ip_end = s_nocode;
}

INLINE uint8_t get_p(const regs_t *r)
{
    return (r->p & ~(N_ | Z_)) | (r->nf & N_) | (r->zf ? 0 : Z_);
}

/* P from the stack or the machine: M and X stay 1 in emulation mode; X = 1
 * clears XH and YH
 */
INLINE void set_p(regs_t *r, uint8_t v)
{
    if (r->e) {
        v |= M_ | X_;
    }
    r->p = v;
    r->nf = v;
    r->zf = !(v & Z_);
    if (v & X_) {
        r->x &= 0xFF;
        r->y &= 0xFF;
    }
}

INLINE void load(w65816_t *c, regs_t *r)
{
    r->pbr = (uint32_t)c->pbr << 16;
    r->dbr = (uint32_t)c->dbr << 16;
    set_pc(c, r, c->pc);
    r->a = c->a;
    r->x = c->x;
    r->y = c->y;
    r->s = c->s;
    r->d = c->d;
    r->dl = (r->d & 0xFF) != 0;
    r->e = c->e;
    r->rdt = c->rd;
    r->wrt = c->wr;
    /* What the chip can't hold otherwise, should the machine (or a test)
     * have set the registers so
     */
    set_p(r, c->p);
    if (r->e) {
        r->s = 0x100 | (r->s & 0xFF);
    }
}

/* The run's cycles so far */
INLINE uint32_t used(const w65816_t *c, const regs_t *r)
{
    return c->run_budget - c->run_held - r->left;
}

INLINE void store(w65816_t *c, const regs_t *r)
{
    c->pbr = r->pbr >> 16;
    c->dbr = r->dbr >> 16;
    c->pc = get_pc(c, r);
    c->a = r->a;
    c->x = r->x;
    c->y = r->y;
    c->s = r->s;
    c->d = r->d;
    c->p = get_p(r);
    c->e = r->e;
    c->cycles = c->run_base + used(c, r);
}

/* Back to the outer loop after this instruction */
INLINE void yield(w65816_t *c, regs_t *r)
{
    c->run_held += r->left;
    r->left = 0;
}

/* ---- Bus accesses ---- */

/* The slow paths, out of line: the machine's callbacks, at the cycle of
 * the access; they may add wait states. Then they yield (a callback may
 * have changed irq, nmi or the CPU's speed).
 */
static uint8_t NOINLINE W65816_HOT bus_read(w65816_t *c, uint32_t a, int32_t left)
{
    c->cycles = c->run_base + (uint32_t)(c->run_budget - c->run_held - left);
    uint8_t v = c->bus.read(c->bus.ctx, a);
    c->run_held += left - 1 - c->penalty;
    c->penalty = 0;
    return v;
}

static void NOINLINE W65816_HOT bus_write(w65816_t *c, uint32_t a, uint8_t v, int32_t left)
{
    c->cycles = c->run_base + (uint32_t)(c->run_budget - c->run_held - left);
    c->bus.write(c->bus.ctx, a, v);
    c->run_held += left - 1 - c->penalty;
    c->penalty = 0;
}

/* After a callback the code page is looked up again (ip_end = ip) */
INLINE uint8_t rd(w65816_t *c, regs_t *r, uint32_t a)
{
    const uint8_t *pg = r->rdt[a >> 8];
    if (__builtin_expect(pg != NULL, 1)) {
        r->left--;
        return pg[a & 0xFF];
    }
    uint8_t v = bus_read(c, a, r->left);
    r->left = 0;
    r->ip_end = r->ip;
    return v;
}

INLINE void wr(w65816_t *c, regs_t *r, uint32_t a, uint8_t v)
{
    uint8_t *pg = r->wrt[a >> 8];
    if (__builtin_expect(pg != NULL, 1)) {
        pg[a & 0xFF] = v;
        r->left--;
        return;
    }
    bus_write(c, a, v, r->left);
    r->left = 0;
    r->ip_end = r->ip;
}

/* A fetch at the end of the code page (or after a jump, a callback):
 * look the PC's page up and fetch from it, or through the bus. The new ip
 * and ip_end, the byte, the cycles left
 */
typedef struct {
    const uint8_t *ip, *ip_end;
    uint32_t v;
    int32_t left;
} refill_t;

static refill_t NOINLINE W65816_HOT refill(w65816_t *c, const uint8_t *ip, uint32_t pbr, int32_t left)
{
    uint16_t pc = c->code_pc + (uint32_t)(ip - c->code_base);
    const uint8_t *pg = c->rd[(pbr | pc) >> 8];
    if (pg != NULL) {
        c->code_pc = pc & 0xFF00;
        c->code_base = pg;
        return (refill_t){ pg + (pc & 0xFF) + 1, pg + 256, pg[pc & 0xFF], left - 1 };
    }
    c->code_pc = pc + 1;
    c->code_base = s_nocode;
    uint8_t v = bus_read(c, pbr | pc, left);
    return (refill_t){ s_nocode, s_nocode, v, 0 };
}

/* The program counter wraps in its bank (refill() does it) */
INLINE uint8_t fetch(w65816_t *c, regs_t *r)
{
    if (__builtin_expect(r->ip != r->ip_end, 1)) {
        r->left--;
        return *r->ip++;
    }
    refill_t f = refill(c, r->ip, r->pbr, r->left);
    r->ip = f.ip;
    r->ip_end = f.ip_end;
    r->left = f.left;
    return f.v;
}

INLINE uint16_t fetch16(w65816_t *c, regs_t *r)
{
    uint32_t lo = fetch(c, r);
    return lo | (uint32_t)fetch(c, r) << 8;
}

/* The 6502's pushes and pulls: in emulation mode S wraps in page 1 */
INLINE void push(w65816_t *c, regs_t *r, uint8_t v)
{
    wr(c, r, r->s, v);
    r->s = r->e ? 0x100 | ((r->s - 1) & 0xFF) : (r->s - 1) & 0xFFFF;
}

INLINE uint8_t pull(w65816_t *c, regs_t *r)
{
    r->s = r->e ? 0x100 | ((r->s + 1) & 0xFF) : (r->s + 1) & 0xFFFF;
    return rd(c, r, r->s);
}

/* The 65816's own: S runs over 16 bits, stack_end() puts it back in page 1 */
INLINE void push_n(w65816_t *c, regs_t *r, uint8_t v)
{
    wr(c, r, r->s, v);
    r->s = (r->s - 1) & 0xFFFF;
}

INLINE uint8_t pull_n(w65816_t *c, regs_t *r)
{
    r->s = (r->s + 1) & 0xFFFF;
    return rd(c, r, r->s);
}

INLINE void stack_end(regs_t *r)
{
    if (r->e) {
        r->s = 0x100 | (r->s & 0xFF);
    }
}

/* ---- N and Z from a result ---- */

INLINE void nz8(regs_t *r, uint32_t v)
{
    r->nf = v;
    r->zf = v & 0xFF;
}

INLINE void nz16(regs_t *r, uint32_t v)
{
    r->nf = v >> 8;
    r->zf = v & 0xFFFF;
}

INLINE void nz(regs_t *r, uint32_t v, bool w)
{
    if (w) {
        nz16(r, v);
    } else {
        nz8(r, v);
    }
}

/* ---- Addressing modes: the effective address, after the mode's cycles
 * up to (not including) the data access
 */

/* D + o in bank 0; with e = 1 and DL = 0 the 6502's wrap in the page */
INLINE uint32_t dp_at(const regs_t *r, uint32_t o)
{
    if (r->e && !r->dl) {
        return r->d | (o & 0xFF);
    }
    return (r->d + o) & 0xFFFF;
}

/* The direct page modes take a cycle more when DL is not 0 */
INLINE int32_t dl_cycle(const regs_t *r)
{
    return r->dl;
}

INLINE void set_d(regs_t *r, uint32_t d)
{
    r->d = d;
    r->dl = (d & 0xFF) != 0;
}

INLINE uint32_t am_dp(w65816_t *c, regs_t *r)
{
    uint32_t o = fetch(c, r);
    r->left -= dl_cycle(r);
    return (r->d + o) & 0xFFFF;
}

/* dp,X and dp,Y */
INLINE uint32_t am_dpi(w65816_t *c, regs_t *r, uint32_t i)
{
    uint32_t o = fetch(c, r);
    r->left -= 1 + dl_cycle(r);
    return dp_at(r, o + i);
}

/* (dp) */
INLINE uint32_t am_dpind(w65816_t *c, regs_t *r)
{
    uint32_t o = fetch(c, r);
    r->left -= dl_cycle(r);
    uint32_t lo = rd(c, r, dp_at(r, o));
    uint32_t hi = rd(c, r, dp_at(r, o + 1));
    return r->dbr | hi << 8 | lo;
}

/* (dp,X) */
INLINE uint32_t am_dpxind(w65816_t *c, regs_t *r)
{
    uint32_t o = fetch(c, r) + r->x;
    r->left -= 1 + dl_cycle(r);
    uint32_t lo = rd(c, r, dp_at(r, o));
    uint32_t hi = rd(c, r, dp_at(r, o + 1));
    return r->dbr | hi << 8 | lo;
}

/* (dp),Y: a cycle more for writes, a 16-bit Y or a page crossing */
INLINE uint32_t am_dpindy(w65816_t *c, regs_t *r, bool always)
{
    uint32_t o = fetch(c, r);
    r->left -= dl_cycle(r);
    uint32_t lo = rd(c, r, dp_at(r, o));
    uint32_t hi = rd(c, r, dp_at(r, o + 1));
    if (always || !(r->p & X_) || lo + r->y > 0xFF) {
        r->left--;
    }
    return (r->dbr + (hi << 8 | lo) + r->y) & DATA;
}

/* [dp] and [dp],Y: a 65816 mode, never wraps in the page */
INLINE uint32_t am_dplong(w65816_t *c, regs_t *r, uint32_t i)
{
    uint32_t o = r->d + fetch(c, r);
    r->left -= dl_cycle(r);
    uint32_t lo = rd(c, r, o & 0xFFFF);
    uint32_t hi = rd(c, r, (o + 1) & 0xFFFF);
    uint32_t bank = rd(c, r, (o + 2) & 0xFFFF);
    return ((bank << 16 | hi << 8 | lo) + i) & DATA;
}

/* sr,S */
INLINE uint32_t am_sr(w65816_t *c, regs_t *r)
{
    uint32_t o = fetch(c, r);
    r->left--;
    return (r->s + o) & 0xFFFF;
}

/* (sr,S),Y */
INLINE uint32_t am_sriy(w65816_t *c, regs_t *r)
{
    uint32_t o = r->s + fetch(c, r);
    r->left--;
    uint32_t lo = rd(c, r, o & 0xFFFF);
    uint32_t hi = rd(c, r, (o + 1) & 0xFFFF);
    r->left--;
    return (r->dbr + (hi << 8 | lo) + r->y) & DATA;
}

INLINE uint32_t am_abs(w65816_t *c, regs_t *r)
{
    return r->dbr | fetch16(c, r);
}

/* abs,X and abs,Y: like (dp),Y */
INLINE uint32_t am_absi(w65816_t *c, regs_t *r, uint32_t i, bool always)
{
    uint32_t a = fetch16(c, r);
    if (always || !(r->p & X_) || (a & 0xFF) + i > 0xFF) {
        r->left--;
    }
    return (r->dbr + a + i) & DATA;
}

/* long and long,X */
INLINE uint32_t am_long(w65816_t *c, regs_t *r, uint32_t i)
{
    uint32_t a = fetch16(c, r);
    uint32_t bank = fetch(c, r);
    return ((bank << 16 | a) + i) & DATA;
}

/* ---- ALU: v is the operand, w a 16-bit operation ---- */

INLINE void op_ora(regs_t *r, uint32_t v, bool w)
{
    r->a |= v;
    nz(r, r->a, w);
}

INLINE void op_and(regs_t *r, uint32_t v, bool w)
{
    r->a &= w ? v : v | 0xFF00;
    nz(r, r->a, w);
}

INLINE void op_eor(regs_t *r, uint32_t v, bool w)
{
    r->a ^= v;
    nz(r, r->a, w);
}

INLINE void op_lda(regs_t *r, uint32_t v, bool w)
{
    r->a = w ? v : (r->a & 0xFF00) | v;
    nz(r, v, w);
}

INLINE void op_ldx(regs_t *r, uint32_t v, bool w)
{
    r->x = v;
    nz(r, v, w);
}

INLINE void op_ldy(regs_t *r, uint32_t v, bool w)
{
    r->y = v;
    nz(r, v, w);
}

INLINE void compare(regs_t *r, uint32_t reg, uint32_t v, bool w)
{
    reg &= w ? 0xFFFF : 0xFF;
    r->p = (r->p & ~C_) | (reg >= v ? C_ : 0);
    nz(r, reg - v, w);
}

INLINE void op_cmp(regs_t *r, uint32_t v, bool w)
{
    compare(r, r->a, v, w);
}

INLINE void op_cpx(regs_t *r, uint32_t v, bool w)
{
    compare(r, r->x, v, w);
}

INLINE void op_cpy(regs_t *r, uint32_t v, bool w)
{
    compare(r, r->y, v, w);
}

INLINE void op_bit(regs_t *r, uint32_t v, bool w)
{
    uint32_t top = w ? v >> 8 : v;
    r->p = (r->p & ~V_) | (top & V_);
    r->nf = top;
    r->zf = r->a & v;
}

/* BIT #: Z only */
INLINE void op_bit_imm(regs_t *r, uint32_t v, bool w)
{
    r->zf = r->a & v;
}

/* Decimal ADC and SBC, digit by digit (bsnes's, checked on the SNES's
 * 65816, invalid BCD included): a digit over 9 (ADC) or without a carry
 * out (SBC) is adjusted by 6, the top digit after V is taken. The result,
 * C in bit 16 and V in bit 17
 */
static uint32_t NOINLINE decimal(uint32_t a, uint32_t v, uint32_t carry, bool w, bool sub)
{
    int top = w ? 12 : 4;
    int mask = w ? 0xFFFF : 0xFF;
    if (sub) {
        v = ~v & mask;
    }
    int res = 0;
    for (int sh = 0; sh < top; sh += 4) {
        int digit = ((a >> sh) & 0xF) + ((v >> sh) & 0xF) + carry;
        if (sub ? digit <= 0xF : digit > 9) {
            digit += sub ? -6 : 6;
        }
        carry = digit > 0xF;
        res |= (digit & 0xF) << sh;
    }
    res += (a & (0xF << top)) + (v & (0xF << top)) + (carry << top);
    uint32_t vf = (~(a ^ v) & (a ^ res) & (8u << top)) != 0;
    if (sub ? res <= mask : res >= (0xA << top)) {
        res += sub ? -(6 << top) : 6 << top;
    }
    carry = res > mask;
    return (res & mask) | carry << 16 | vf << 17;
}

INLINE void op_adc(regs_t *r, uint32_t v, bool w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF;
    uint32_t a = r->a & mask, res;
    if (__builtin_expect(r->p & D_, 0)) {
        uint32_t t = decimal(a, v, r->p & C_, w, false);
        res = t & 0xFFFF;
        r->p = (r->p & ~(C_ | V_)) | ((t >> 16) & C_) | ((t >> 11) & V_);
    } else {
        res = a + v + (r->p & C_);
        uint32_t ov = ~(a ^ v) & (a ^ res) & (w ? 0x8000 : 0x80);
        r->p = (r->p & ~(C_ | V_)) | (res > mask ? C_ : 0) | (ov ? V_ : 0);
        res &= mask;
    }
    r->a = w ? res : (r->a & 0xFF00) | res;
    nz(r, res, w);
}

INLINE void op_sbc(regs_t *r, uint32_t v, bool w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF;
    uint32_t a = r->a & mask, res;
    if (__builtin_expect(r->p & D_, 0)) {
        uint32_t t = decimal(a, v, r->p & C_, w, true);
        res = t & 0xFFFF;
        r->p = (r->p & ~(C_ | V_)) | ((t >> 16) & C_) | ((t >> 11) & V_);
    } else {
        uint32_t nv = ~v & mask;
        res = a + nv + (r->p & C_);
        uint32_t ov = ~(a ^ nv) & (a ^ res) & (w ? 0x8000 : 0x80);
        r->p = (r->p & ~(C_ | V_)) | (res > mask ? C_ : 0) | (ov ? V_ : 0);
        res &= mask;
    }
    r->a = w ? res : (r->a & 0xFF00) | res;
    nz(r, res, w);
}

/* Read-modify-write: the new value */
INLINE uint32_t op_asl(regs_t *r, uint32_t v, bool w)
{
    r->p = (r->p & ~C_) | ((v >> (w ? 15 : 7)) & 1);
    v = (v << 1) & (w ? 0xFFFF : 0xFF);
    nz(r, v, w);
    return v;
}

INLINE uint32_t op_lsr(regs_t *r, uint32_t v, bool w)
{
    r->p = (r->p & ~C_) | (v & 1);
    v >>= 1;
    nz(r, v, w);
    return v;
}

INLINE uint32_t op_rol(regs_t *r, uint32_t v, bool w)
{
    uint32_t res = ((v << 1) | (r->p & C_)) & (w ? 0xFFFF : 0xFF);
    r->p = (r->p & ~C_) | ((v >> (w ? 15 : 7)) & 1);
    nz(r, res, w);
    return res;
}

INLINE uint32_t op_ror(regs_t *r, uint32_t v, bool w)
{
    uint32_t res = (v >> 1) | ((uint32_t)(r->p & C_) << (w ? 15 : 7));
    r->p = (r->p & ~C_) | (v & 1);
    nz(r, res, w);
    return res;
}

INLINE uint32_t op_inc(regs_t *r, uint32_t v, bool w)
{
    v = (v + 1) & (w ? 0xFFFF : 0xFF);
    nz(r, v, w);
    return v;
}

INLINE uint32_t op_dec(regs_t *r, uint32_t v, bool w)
{
    v = (v - 1) & (w ? 0xFFFF : 0xFF);
    nz(r, v, w);
    return v;
}

INLINE uint32_t op_tsb(regs_t *r, uint32_t v, bool w)
{
    r->zf = r->a & v;
    return v | (r->a & (w ? 0xFFFF : 0xFF));
}

INLINE uint32_t op_trb(regs_t *r, uint32_t v, bool w)
{
    r->zf = r->a & v;
    return v & ~r->a & (w ? 0xFFFF : 0xFF);
}

/* ---- Operand access, by the width that M_ (A, memory) or X_ (X, Y) gives ---- */

/* Read the operand at ea (its second byte at (ea + 1) & mask) and apply fn */
#define READ(flag, ea_expr, mask, fn) do { \
        uint32_t ea_ = (ea_expr); \
        if (r->p & (flag)) { \
            fn(r, rd(c, r, ea_), false); \
        } else { \
            uint32_t lo_ = rd(c, r, ea_); \
            fn(r, lo_ | (uint32_t)rd(c, r, (ea_ + 1) & (mask)) << 8, true); \
        } \
    } while (0)

#define IMM(flag, fn) do { \
        if (r->p & (flag)) { \
            fn(r, fetch(c, r), false); \
        } else { \
            fn(r, fetch16(c, r), true); \
        } \
    } while (0)

#define STORE(flag, ea_expr, mask, value) do { \
        uint32_t ea_ = (ea_expr), v_ = (value); \
        wr(c, r, ea_, v_); \
        if (!(r->p & (flag))) { \
            wr(c, r, (ea_ + 1) & (mask), v_ >> 8); \
        } \
    } while (0)

/* Read, an internal cycle, write: 16-bit ones write the high byte first */
#define RMW(ea_expr, mask, fn) do { \
        uint32_t ea_ = (ea_expr); \
        if (r->p & M_) { \
            uint32_t v_ = rd(c, r, ea_); \
            r->left--; \
            wr(c, r, ea_, fn(r, v_, false)); \
        } else { \
            uint32_t ea1_ = (ea_ + 1) & (mask); \
            uint32_t v_ = rd(c, r, ea_); \
            v_ |= (uint32_t)rd(c, r, ea1_) << 8; \
            r->left--; \
            v_ = fn(r, v_, true); \
            wr(c, r, ea1_, v_ >> 8); \
            wr(c, r, ea_, v_); \
        } \
    } while (0)

/* ASL A and the like: B stays when M is 1 */
#define ACC(fn) do { \
        r->left--; \
        if (r->p & M_) { \
            r->a = (r->a & 0xFF00) | fn(r, r->a & 0xFF, false); \
        } else { \
            r->a = fn(r, r->a, true); \
        } \
    } while (0)

/* ---- Instructions ---- */

/* Branch: 2 cycles, 3 if taken, 4 if it crosses a page in emulation mode.
 * In the code page (that of the next instruction: ip != ip_end) just ip
 * moves
 */
INLINE void branch(w65816_t *c, regs_t *r, bool take)
{
    int8_t o = fetch(c, r);
    if (take) {
        r->left--;
        uintptr_t t = (uintptr_t)r->ip + o;
        if (__builtin_expect(r->ip != r->ip_end && (uintptr_t)r->ip_end - 1 - t < 256, 1)) {
            r->ip = (const uint8_t *)t;
        } else {
            uint16_t pc = get_pc(c, r), to = pc + o;
            r->left -= r->e && ((to ^ pc) & 0xFF00);
            set_pc(c, r, to);
        }
    }
}

/* Interrupt entry from its third cycle on (BRK and COP have fetched their
 * signature byte, IRQ and NMI spent two internal cycles): p is the copy to
 * push (B clear for IRQ and NMI in emulation mode)
 */
INLINE void interrupt(w65816_t *c, regs_t *r, uint16_t vector, uint8_t p)
{
    if (!r->e) {
        push(c, r, r->pbr >> 16);
    }
    uint16_t pc = get_pc(c, r);
    push(c, r, pc >> 8);
    push(c, r, pc);
    push(c, r, p);
    r->p = (r->p | I_) & ~D_;
    r->pbr = 0;
    uint32_t v = (uint32_t)c->vector_bank << 16 | vector;
    uint32_t lo = rd(c, r, v);
    set_pc(c, r, lo | (uint32_t)rd(c, r, v + 1) << 8);
}

/* MVN (+1) and MVP (-1): one byte per step, the instruction again until
 * C runs out (to $FFFF); X and Y wrap in their width, DBR becomes the
 * destination bank. Back to the opcode: in the code page by ip alone, if
 * the page is still valid (ip_end at its end, and not given up: ip_end = ip
 * looks the same when the instruction ends the page)
 */
INLINE void block_move(w65816_t *c, regs_t *r, int step)
{
    uint32_t dst = (uint32_t)fetch(c, r) << 16;
    uint32_t src = (uint32_t)fetch(c, r) << 16;
    r->dbr = dst;
    wr(c, r, dst | r->y, rd(c, r, src | r->x));
    r->left -= 2;
    uint16_t mask = (r->p & X_) ? 0xFF : 0xFFFF;
    r->x = (r->x + step) & mask;
    r->y = (r->y + step) & mask;
    bool more = r->a != 0;
    r->a = (r->a - 1) & 0xFFFF;
    if (more) {
        uintptr_t base = (uintptr_t)c->code_base;
        if (r->ip != r->ip_end && (uintptr_t)r->ip_end == base + 256 && (uintptr_t)r->ip - 3 >= base) {
            r->ip -= 3;
        } else {
            set_pc(c, r, get_pc(c, r) - 3);
        }
    }
}

#define ALU_GROUP(base, fn) \
    case base + 0x01: READ(M_, am_dpxind(c, r), DATA, fn); break; \
    case base + 0x03: READ(M_, am_sr(c, r), BANK0, fn); break; \
    case base + 0x05: READ(M_, am_dp(c, r), BANK0, fn); break; \
    case base + 0x07: READ(M_, am_dplong(c, r, 0), DATA, fn); break; \
    case base + 0x09: IMM(M_, fn); break; \
    case base + 0x0D: READ(M_, am_abs(c, r), DATA, fn); break; \
    case base + 0x0F: READ(M_, am_long(c, r, 0), DATA, fn); break; \
    case base + 0x11: READ(M_, am_dpindy(c, r, false), DATA, fn); break; \
    case base + 0x12: READ(M_, am_dpind(c, r), DATA, fn); break; \
    case base + 0x13: READ(M_, am_sriy(c, r), DATA, fn); break; \
    case base + 0x15: READ(M_, am_dpi(c, r, r->x), BANK0, fn); break; \
    case base + 0x17: READ(M_, am_dplong(c, r, r->y), DATA, fn); break; \
    case base + 0x19: READ(M_, am_absi(c, r, r->y, false), DATA, fn); break; \
    case base + 0x1D: READ(M_, am_absi(c, r, r->x, false), DATA, fn); break; \
    case base + 0x1F: READ(M_, am_long(c, r, r->x), DATA, fn); break;

#define RMW_GROUP(base, fn) \
    case base + 0x06: RMW(am_dp(c, r), BANK0, fn); break; \
    case base + 0x0E: RMW(am_abs(c, r), DATA, fn); break; \
    case base + 0x16: RMW(am_dpi(c, r, r->x), BANK0, fn); break; \
    case base + 0x1E: RMW(am_absi(c, r, r->x, true), DATA, fn); break;

/* Not in IRAM (too big): the cache serves it, and internal RAM is scarce */
int w65816_run(w65816_t *c, int cycles)
{
    regs_t regs, *r = &regs;
    int32_t budget = cycles > 0 ? cycles : 0;
    c->run_base = c->cycles;
    c->run_budget = budget;
    c->run_held = 0;
    c->end_run = false;
    load(c, r);
    r->left = budget;
    for (;;) {
        if (c->stopped || c->end_run) {
            break;
        }
        /* Interrupts, between instructions: here at the start, after a bus
         * callback and after an instruction that may have cleared I
         */
        if (c->nmi) {
            c->nmi = false;
            c->waiting = false;
            r->left -= 2;
            interrupt(c, r, r->e ? 0xFFFA : 0xFFEA, r->e ? get_p(r) & ~B_ : get_p(r));
        } else if (c->irq && (c->waiting || !(r->p & I_))) {
            c->waiting = false;             /* with I set, WAI just ends */
            if (!(r->p & I_)) {
                r->left -= 2;
                interrupt(c, r, r->e ? 0xFFFE : 0xFFEE, r->e ? get_p(r) & ~B_ : get_p(r));
            }
        } else if (c->waiting) {
            break;
        }
        r->left += c->run_held;
        c->run_held = 0;
        if (r->left <= 0) {
            break;
        }
#if EMUDBG_ENABLED
        /* While the debugger looks (breakpoints, steps), one instruction
         * at a time and back here: the instruction loop stays as it is
         */
        const uint8_t *map = c->dbg_map;
        if (__builtin_expect(map != NULL, 0)) {
            uint32_t pc = r->pbr | get_pc(c, r);
            if (emudbg_map24(map, pc)) {
                store(c, r);
                emudbg_hit(c->dbg, pc);
                load(c, r);
            }
            c->run_held += r->left - 1;
            r->left = 1;
        }
#endif
        do {
            uint8_t op = fetch(c, r);
            switch (op) {
            /* ---- The accumulator's ALU operations, loads and stores ---- */
            ALU_GROUP(0x00, op_ora)
            ALU_GROUP(0x20, op_and)
            ALU_GROUP(0x40, op_eor)
            ALU_GROUP(0x60, op_adc)
            ALU_GROUP(0xA0, op_lda)
            ALU_GROUP(0xC0, op_cmp)
            ALU_GROUP(0xE0, op_sbc)
            case 0x81: STORE(M_, am_dpxind(c, r), DATA, r->a); break;
            case 0x83: STORE(M_, am_sr(c, r), BANK0, r->a); break;
            case 0x85: STORE(M_, am_dp(c, r), BANK0, r->a); break;
            case 0x87: STORE(M_, am_dplong(c, r, 0), DATA, r->a); break;
            case 0x8D: STORE(M_, am_abs(c, r), DATA, r->a); break;
            case 0x8F: STORE(M_, am_long(c, r, 0), DATA, r->a); break;
            case 0x91: STORE(M_, am_dpindy(c, r, true), DATA, r->a); break;
            case 0x92: STORE(M_, am_dpind(c, r), DATA, r->a); break;
            case 0x93: STORE(M_, am_sriy(c, r), DATA, r->a); break;
            case 0x95: STORE(M_, am_dpi(c, r, r->x), BANK0, r->a); break;
            case 0x97: STORE(M_, am_dplong(c, r, r->y), DATA, r->a); break;
            case 0x99: STORE(M_, am_absi(c, r, r->y, true), DATA, r->a); break;
            case 0x9D: STORE(M_, am_absi(c, r, r->x, true), DATA, r->a); break;
            case 0x9F: STORE(M_, am_long(c, r, r->x), DATA, r->a); break;
            case 0x64: STORE(M_, am_dp(c, r), BANK0, 0); break;                 /* STZ */
            case 0x74: STORE(M_, am_dpi(c, r, r->x), BANK0, 0); break;
            case 0x9C: STORE(M_, am_abs(c, r), DATA, 0); break;
            case 0x9E: STORE(M_, am_absi(c, r, r->x, true), DATA, 0); break;
            case 0x24: READ(M_, am_dp(c, r), BANK0, op_bit); break;             /* BIT */
            case 0x2C: READ(M_, am_abs(c, r), DATA, op_bit); break;
            case 0x34: READ(M_, am_dpi(c, r, r->x), BANK0, op_bit); break;
            case 0x3C: READ(M_, am_absi(c, r, r->x, false), DATA, op_bit); break;
            case 0x89: IMM(M_, op_bit_imm); break;

            /* ---- The index registers ---- */
            case 0xA2: IMM(X_, op_ldx); break;
            case 0xA6: READ(X_, am_dp(c, r), BANK0, op_ldx); break;
            case 0xAE: READ(X_, am_abs(c, r), DATA, op_ldx); break;
            case 0xB6: READ(X_, am_dpi(c, r, r->y), BANK0, op_ldx); break;
            case 0xBE: READ(X_, am_absi(c, r, r->y, false), DATA, op_ldx); break;
            case 0xA0: IMM(X_, op_ldy); break;
            case 0xA4: READ(X_, am_dp(c, r), BANK0, op_ldy); break;
            case 0xAC: READ(X_, am_abs(c, r), DATA, op_ldy); break;
            case 0xB4: READ(X_, am_dpi(c, r, r->x), BANK0, op_ldy); break;
            case 0xBC: READ(X_, am_absi(c, r, r->x, false), DATA, op_ldy); break;
            case 0x86: STORE(X_, am_dp(c, r), BANK0, r->x); break;
            case 0x8E: STORE(X_, am_abs(c, r), DATA, r->x); break;
            case 0x96: STORE(X_, am_dpi(c, r, r->y), BANK0, r->x); break;
            case 0x84: STORE(X_, am_dp(c, r), BANK0, r->y); break;
            case 0x8C: STORE(X_, am_abs(c, r), DATA, r->y); break;
            case 0x94: STORE(X_, am_dpi(c, r, r->x), BANK0, r->y); break;
            case 0xE0: IMM(X_, op_cpx); break;
            case 0xE4: READ(X_, am_dp(c, r), BANK0, op_cpx); break;
            case 0xEC: READ(X_, am_abs(c, r), DATA, op_cpx); break;
            case 0xC0: IMM(X_, op_cpy); break;
            case 0xC4: READ(X_, am_dp(c, r), BANK0, op_cpy); break;
            case 0xCC: READ(X_, am_abs(c, r), DATA, op_cpy); break;
            case 0xE8: r->left--; r->x = (r->x + 1) & ((r->p & X_) ? 0xFF : 0xFFFF); nz(r, r->x, !(r->p & X_)); break;
            case 0xC8: r->left--; r->y = (r->y + 1) & ((r->p & X_) ? 0xFF : 0xFFFF); nz(r, r->y, !(r->p & X_)); break;
            case 0xCA: r->left--; r->x = (r->x - 1) & ((r->p & X_) ? 0xFF : 0xFFFF); nz(r, r->x, !(r->p & X_)); break;
            case 0x88: r->left--; r->y = (r->y - 1) & ((r->p & X_) ? 0xFF : 0xFFFF); nz(r, r->y, !(r->p & X_)); break;

            /* ---- Read-modify-write ---- */
            RMW_GROUP(0x00, op_asl)
            RMW_GROUP(0x20, op_rol)
            RMW_GROUP(0x40, op_lsr)
            RMW_GROUP(0x60, op_ror)
            RMW_GROUP(0xC0, op_dec)
            RMW_GROUP(0xE0, op_inc)
            case 0x04: RMW(am_dp(c, r), BANK0, op_tsb); break;
            case 0x0C: RMW(am_abs(c, r), DATA, op_tsb); break;
            case 0x14: RMW(am_dp(c, r), BANK0, op_trb); break;
            case 0x1C: RMW(am_abs(c, r), DATA, op_trb); break;
            case 0x0A: ACC(op_asl); break;
            case 0x2A: ACC(op_rol); break;
            case 0x4A: ACC(op_lsr); break;
            case 0x6A: ACC(op_ror); break;
            case 0x1A: ACC(op_inc); break;
            case 0x3A: ACC(op_dec); break;

            /* ---- Transfers: the destination's width ---- */
            case 0xAA:                                                          /* TAX */
                r->left--;
                r->x = (r->p & X_) ? r->a & 0xFF : r->a;
                nz(r, r->x, !(r->p & X_));
                break;
            case 0xA8:                                                          /* TAY */
                r->left--;
                r->y = (r->p & X_) ? r->a & 0xFF : r->a;
                nz(r, r->y, !(r->p & X_));
                break;
            case 0xBA:                                                          /* TSX */
                r->left--;
                r->x = (r->p & X_) ? r->s & 0xFF : r->s;
                nz(r, r->x, !(r->p & X_));
                break;
            case 0x8A:                                                          /* TXA */
                r->left--;
                r->a = (r->p & M_) ? (r->a & 0xFF00) | (r->x & 0xFF) : r->x;
                nz(r, r->a, !(r->p & M_));
                break;
            case 0x98:                                                          /* TYA */
                r->left--;
                r->a = (r->p & M_) ? (r->a & 0xFF00) | (r->y & 0xFF) : r->y;
                nz(r, r->a, !(r->p & M_));
                break;
            case 0x9B: r->left--; r->y = r->x; nz(r, r->y, !(r->p & X_)); break;   /* TXY */
            case 0xBB: r->left--; r->x = r->y; nz(r, r->x, !(r->p & X_)); break;   /* TYX */
            case 0x9A: r->left--; r->s = r->e ? 0x100 | (r->x & 0xFF) : r->x; break; /* TXS */
            case 0x1B: r->left--; r->s = r->e ? 0x100 | (r->a & 0xFF) : r->a; break; /* TCS */
            case 0x3B: r->left--; r->a = r->s; nz16(r, r->a); break;               /* TSC */
            case 0x5B: r->left--; set_d(r, r->a); nz16(r, r->d); break;             /* TCD */
            case 0x7B: r->left--; r->a = r->d; nz16(r, r->a); break;               /* TDC */
            case 0xEB: r->left -= 2; r->a = ((r->a >> 8) | (r->a << 8)) & 0xFFFF; nz8(r, r->a); break; /* XBA */

            /* ---- Flags and modes ---- */
            case 0x18: r->left--; r->p &= ~C_; break;
            case 0x38: r->left--; r->p |= C_; break;
            case 0x58: r->left--; r->p &= ~I_; yield(c, r); break;              /* CLI: look at IRQ */
            case 0x78: r->left--; r->p |= I_; break;
            case 0xB8: r->left--; r->p &= ~V_; break;
            case 0xD8: r->left--; r->p &= ~D_; break;
            case 0xF8: r->left--; r->p |= D_; break;
            case 0xC2: {                                                        /* REP */
                uint8_t v = fetch(c, r);
                r->left--;
                set_p(r, get_p(r) & ~v);
                if (v & I_) {
                    yield(c, r);
                }
                break;
            }
            case 0xE2: {                                                        /* SEP */
                uint8_t v = fetch(c, r);
                r->left--;
                set_p(r, get_p(r) | v);
                break;
            }
            case 0xFB: {                                                        /* XCE */
                r->left--;
                bool carry = r->p & C_;
                r->p = (r->p & ~C_) | (r->e ? C_ : 0);
                r->e = carry;
                if (r->e) {
                    set_p(r, get_p(r));
                    r->s = 0x100 | (r->s & 0xFF);
                }
                break;
            }

            /* ---- Stack ---- */
            case 0x48:                                                          /* PHA */
                r->left--;
                if (!(r->p & M_)) {
                    push(c, r, r->a >> 8);
                }
                push(c, r, r->a);
                break;
            case 0xDA:                                                          /* PHX */
                r->left--;
                if (!(r->p & X_)) {
                    push(c, r, r->x >> 8);
                }
                push(c, r, r->x);
                break;
            case 0x5A:                                                          /* PHY */
                r->left--;
                if (!(r->p & X_)) {
                    push(c, r, r->y >> 8);
                }
                push(c, r, r->y);
                break;
            case 0x08: r->left--; push(c, r, get_p(r)); break;                  /* PHP */
            case 0x8B: r->left--; push(c, r, r->dbr >> 16); break;              /* PHB */
            case 0x4B: r->left--; push(c, r, r->pbr >> 16); break;              /* PHK */
            case 0x0B:                                                          /* PHD */
                r->left--;
                push_n(c, r, r->d >> 8);
                push_n(c, r, r->d);
                stack_end(r);
                break;
            case 0x68:                                                          /* PLA */
                r->left -= 2;
                if (r->p & M_) {
                    r->a = (r->a & 0xFF00) | pull(c, r);
                    nz8(r, r->a);
                } else {
                    uint32_t lo = pull(c, r);
                    r->a = lo | (uint32_t)pull(c, r) << 8;
                    nz16(r, r->a);
                }
                break;
            case 0xFA:                                                          /* PLX */
                r->left -= 2;
                if (r->p & X_) {
                    r->x = pull(c, r);
                    nz8(r, r->x);
                } else {
                    uint32_t lo = pull(c, r);
                    r->x = lo | (uint32_t)pull(c, r) << 8;
                    nz16(r, r->x);
                }
                break;
            case 0x7A:                                                          /* PLY */
                r->left -= 2;
                if (r->p & X_) {
                    r->y = pull(c, r);
                    nz8(r, r->y);
                } else {
                    uint32_t lo = pull(c, r);
                    r->y = lo | (uint32_t)pull(c, r) << 8;
                    nz16(r, r->y);
                }
                break;
            case 0x28: r->left -= 2; set_p(r, pull(c, r)); yield(c, r); break;  /* PLP */
            case 0xAB: {                                                        /* PLB */
                r->left -= 2;
                uint32_t v = pull_n(c, r);
                stack_end(r);
                r->dbr = v << 16;
                nz8(r, v);
                break;
            }
            case 0x2B: {                                                        /* PLD */
                r->left -= 2;
                uint32_t lo = pull_n(c, r);
                set_d(r, lo | (uint32_t)pull_n(c, r) << 8);
                stack_end(r);
                nz16(r, r->d);
                break;
            }
            case 0xF4: {                                                        /* PEA */
                uint16_t v = fetch16(c, r);
                push_n(c, r, v >> 8);
                push_n(c, r, v);
                stack_end(r);
                break;
            }
            case 0xD4: {                                                        /* PEI */
                uint32_t o = r->d + fetch(c, r);
                r->left -= dl_cycle(r);
                uint32_t lo = rd(c, r, o & 0xFFFF);
                uint32_t hi = rd(c, r, (o + 1) & 0xFFFF);
                push_n(c, r, hi);
                push_n(c, r, lo);
                stack_end(r);
                break;
            }
            case 0x62: {                                                        /* PER */
                uint16_t v = fetch16(c, r);
                r->left--;
                v += get_pc(c, r);
                push_n(c, r, v >> 8);
                push_n(c, r, v);
                stack_end(r);
                break;
            }

            /* ---- Jumps, calls, returns ---- */
            case 0x4C: set_pc(c, r, fetch16(c, r)); break;                      /* JMP abs */
            case 0x5C: {                                                        /* JML long */
                uint16_t a = fetch16(c, r);
                r->pbr = (uint32_t)fetch(c, r) << 16;
                set_pc(c, r, a);
                break;
            }
            case 0x6C: {                                                        /* JMP (abs) */
                uint16_t a = fetch16(c, r);
                uint32_t lo = rd(c, r, a);
                set_pc(c, r, lo | (uint32_t)rd(c, r, (uint16_t)(a + 1)) << 8);
                break;
            }
            case 0x7C: {                                                        /* JMP (abs,X) */
                uint16_t a = fetch16(c, r) + r->x;
                r->left--;
                uint32_t lo = rd(c, r, r->pbr | a);
                set_pc(c, r, lo | (uint32_t)rd(c, r, r->pbr | (uint16_t)(a + 1)) << 8);
                break;
            }
            case 0xDC: {                                                        /* JML [abs] */
                uint16_t a = fetch16(c, r);
                uint32_t lo = rd(c, r, a);
                uint32_t hi = rd(c, r, (uint16_t)(a + 1));
                r->pbr = (uint32_t)rd(c, r, (uint16_t)(a + 2)) << 16;
                set_pc(c, r, lo | hi << 8);
                break;
            }
            case 0x20: {                                                        /* JSR abs */
                uint16_t a = fetch16(c, r);
                uint16_t ret = get_pc(c, r) - 1;
                r->left--;
                push(c, r, ret >> 8);
                push(c, r, ret);
                set_pc(c, r, a);
                break;
            }
            case 0xFC: {                                                        /* JSR (abs,X) */
                uint32_t lo = fetch(c, r);
                uint16_t ret = get_pc(c, r);
                push_n(c, r, ret >> 8);
                push_n(c, r, ret);
                uint16_t a = (lo | (uint32_t)fetch(c, r) << 8) + r->x;
                r->left--;
                uint32_t tlo = rd(c, r, r->pbr | a);
                set_pc(c, r, tlo | (uint32_t)rd(c, r, r->pbr | (uint16_t)(a + 1)) << 8);
                stack_end(r);
                break;
            }
            case 0x22: {                                                        /* JSL */
                uint16_t a = fetch16(c, r);
                push_n(c, r, r->pbr >> 16);
                r->left--;
                uint32_t bank = fetch(c, r);
                uint16_t ret = get_pc(c, r) - 1;
                push_n(c, r, ret >> 8);
                push_n(c, r, ret);
                stack_end(r);
                r->pbr = bank << 16;
                set_pc(c, r, a);
                break;
            }
            case 0x60: {                                                        /* RTS */
                r->left -= 2;
                uint32_t lo = pull(c, r);
                uint16_t a = lo | (uint32_t)pull(c, r) << 8;
                r->left--;
                set_pc(c, r, a + 1);
                break;
            }
            case 0x6B: {                                                        /* RTL */
                r->left -= 2;
                uint32_t lo = pull_n(c, r);
                uint32_t hi = pull_n(c, r);
                r->pbr = (uint32_t)pull_n(c, r) << 16;
                stack_end(r);
                set_pc(c, r, (lo | hi << 8) + 1);
                break;
            }
            case 0x40: {                                                        /* RTI */
                r->left -= 2;
                set_p(r, pull(c, r));
                uint32_t lo = pull(c, r);
                set_pc(c, r, lo | (uint32_t)pull(c, r) << 8);
                if (!r->e) {
                    r->pbr = (uint32_t)pull(c, r) << 16;
                }
                yield(c, r);
                break;
            }
            case 0x00:                                                          /* BRK */
                fetch(c, r);
                interrupt(c, r, r->e ? 0xFFFE : 0xFFE6, get_p(r));
                break;
            case 0x02:                                                          /* COP */
                fetch(c, r);
                interrupt(c, r, r->e ? 0xFFF4 : 0xFFE4, get_p(r));
                break;

            /* ---- Branches ---- */
            case 0x10: branch(c, r, !(r->nf & N_)); break;
            case 0x30: branch(c, r, r->nf & N_); break;
            case 0x50: branch(c, r, !(r->p & V_)); break;
            case 0x70: branch(c, r, r->p & V_); break;
            case 0x90: branch(c, r, !(r->p & C_)); break;
            case 0xB0: branch(c, r, r->p & C_); break;
            case 0xD0: branch(c, r, r->zf != 0); break;
            case 0xF0: branch(c, r, r->zf == 0); break;
            case 0x80: branch(c, r, true); break;                               /* BRA */
            case 0x82: {                                                        /* BRL */
                uint16_t o = fetch16(c, r);
                r->left--;
                set_pc(c, r, get_pc(c, r) + o);
                break;
            }

            /* ---- The rest ---- */
            case 0x54: block_move(c, r, 1); break;                              /* MVN */
            case 0x44: block_move(c, r, -1); break;                             /* MVP */
            case 0xEA: r->left--; break;                                        /* NOP */
            case 0x42:                                                          /* WDM */
                if (c->bus.wdm) {
                    uint8_t v = fetch(c, r);
                    store(c, r);
                    c->bus.wdm(c->bus.ctx, c, v);
                    uint32_t n = (uint32_t)(c->cycles - c->run_base) + c->penalty;
                    c->penalty = 0;
                    load(c, r);
                    c->run_held = c->run_budget - (int32_t)n;
                    r->left = 0;
                } else {
                    set_pc(c, r, get_pc(c, r) + 1);     /* an internal cycle on the chip */
                    r->left--;
                }
                break;
            case 0xCB: r->left -= 2; c->waiting = true; yield(c, r); break;     /* WAI */
            case 0xDB: r->left -= 2; c->stopped = true; yield(c, r); break;     /* STP */
            default: __builtin_unreachable();                                   /* (all 256 are above) */
            }
        } while (r->left > 0);
        r->left += c->run_held;
        c->run_held = 0;
        if (r->left <= 0) {
            break;
        }
    }
    store(c, r);
    return budget - r->left;
}

int w65816_step(w65816_t *cpu)
{
    return w65816_run(cpu, 1);
}

void w65816_reset(w65816_t *c)
{
    c->e = true;
    c->p = (c->p | M_ | X_ | I_) & ~D_;
    c->d = 0;
    c->dbr = 0;
    c->pbr = 0;
    c->stopped = false;
    c->waiting = false;
    c->nmi = false;
    c->penalty = 0;
    regs_t regs, *r = &regs;
    c->run_base = c->cycles;
    c->run_budget = 0;
    c->run_held = 0;
    load(c, r);
    r->left = -5;                       /* two internal cycles, three stack reads */
    uint32_t v = (uint32_t)c->vector_bank << 16 | 0xFFFC;
    uint32_t lo = rd(c, r, v);
    set_pc(c, r, lo | (uint32_t)rd(c, r, v + 1) << 8);
    store(c, r);
}
