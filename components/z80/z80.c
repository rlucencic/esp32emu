/*
 * Z80 core: see z80.h. One switch per opcode page; the DD/FD forms run
 * through the same code with IX or IY in place of HL.
 */
#include <stdio.h>
#include <string.h>
#include "z80.h"

z80_t z80;
uint8_t *z80_rd[Z80_BANKS];
uint8_t *z80_wr[Z80_BANKS];
#if Z80_MSX
uint8_t z80_ffff_special;
#endif

static z80_bus_t s_bus;

#if Z80_CONTENTION
const uint8_t *z80_contention;
uint8_t z80_contended[4];
static int s_tc;                /* T-states into the instruction, waits included */
static int s_cx;                /* ... of them waits */
#endif

#define A       z80.af.b.h
#define F       z80.af.b.l
#define B       z80.bc.b.h
#define C       z80.bc.b.l
#define D       z80.de.b.h
#define E       z80.de.b.l
#define H       z80.hl.b.h
#define L       z80.hl.b.l
#define AF      z80.af.w
#define BC      z80.bc.w
#define DE      z80.de.w
#define HL      z80.hl.w
#define SP      z80.sp.w
#define PC      z80.pc.w
#define WZ      z80.wz.w

enum { FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08, FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80 };
#define FXY     (FX | FY)

/* ---- Instruction times ----
 *
 * Unprefixed opcodes in t_main (conditional jumps, calls and returns add
 * theirs when taken); DD/FD and ED in t_xy and t_ed, prefix included. AT_*
 * is when an I/O access happens inside its instruction.
 */
#ifdef CONFIG_Z80_TIMING_CPC

/* CPC microseconds ("NOPs"): Caprice32's cycle tables / 4 */
static const uint8_t t_main[256] = {
    1, 3, 2, 2, 1, 1, 2, 1, 1, 3, 2, 2, 1, 1, 2, 1,
    3, 3, 2, 2, 1, 1, 2, 1, 3, 3, 2, 2, 1, 1, 2, 1,
    2, 3, 5, 2, 1, 1, 2, 1, 2, 3, 5, 2, 1, 1, 2, 1,
    2, 3, 4, 2, 3, 3, 3, 1, 2, 3, 4, 2, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    2, 3, 3, 3, 3, 4, 2, 4, 2, 3, 3, 0, 3, 5, 2, 4,
    2, 3, 3, 3, 3, 4, 2, 4, 2, 1, 3, 3, 3, 0, 2, 4,
    2, 3, 3, 6, 3, 4, 2, 4, 2, 1, 3, 1, 3, 0, 2, 4,
    2, 3, 3, 1, 3, 4, 2, 4, 2, 2, 3, 1, 3, 0, 2, 4,
};
#define TI(cpc, t)      (cpc)
#define T_XY_EXTRA      1       /* a DD/FD prefix */

#else

/* T-states */
static const uint8_t t_main[256] = {
    4, 10,  7,  6,  4,  4,  7,  4,  4, 11,  7,  6,  4,  4,  7,  4,
    8, 10,  7,  6,  4,  4,  7,  4, 12, 11,  7,  6,  4,  4,  7,  4,
    7, 10, 16,  6,  4,  4,  7,  4,  7, 11, 16,  6,  4,  4,  7,  4,
    7, 10, 13,  6, 11, 11, 10,  4,  7, 11, 13,  6,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    7,  7,  7,  7,  7,  7,  4,  7,  4,  4,  4,  4,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
    5, 10, 10, 10, 10, 11,  7, 11,  5, 10, 10,  0, 10, 17,  7, 11,
    5, 10, 10, 11, 10, 11,  7, 11,  5,  4, 10, 11, 10,  0,  7, 11,
    5, 10, 10, 19, 10, 11,  7, 11,  5,  4, 10,  4, 10,  0,  7, 11,
    5, 10, 10,  4, 10, 11,  7, 11,  5,  6, 10,  4, 10,  0,  7, 11,
};
#define TI(cpc, t)      (t)
#define T_XY_EXTRA      4

#endif

#define T_JR_TAKEN      TI(1, 5)        /* JR cc, DJNZ */
#define T_RET_TAKEN     TI(2, 6)
#define T_CALL_TAKEN    TI(2, 7)
#define T_LDIR_REPEAT   TI(1, 5)
#define T_CPIR_REPEAT   TI(2, 5)
#define T_IOR_REPEAT    TI(1, 5)        /* INIR, OTIR ... */
#define T_HALTED        TI(1, 4)
#define T_INT_IM2       TI(7, 19)
#define T_INT           TI(5, 13)       /* IM 1, and IM 0 with RST 38 */
#define T_PREFIX        TI(1, 4)        /* a DD/FD that does nothing */
#define T_CB_R          TI(2, 8)
#define T_CB_BIT_HL     TI(3, 12)
#define T_CB_HL         TI(4, 15)
#define T_XYCB_BIT      TI(6, 20)
#define T_XYCB          TI(7, 23)

/* I/O times: the start of the I/O cycle + 1 T-state */
#define AT_OUT_N        TI(2, 8)        /* OUT (n),A */
#define AT_IN_N         TI(3, 8)        /* IN A,(n) */
#define AT_IN_C         TI(4, 9)        /* IN r,(C) */
#define AT_OUT_C        TI(3, 9)        /* OUT (C),r */
#define AT_INI          TI(5, 10)
#define AT_OUTI         TI(4, 13)

/* DD/FD prefixed, prefix included: unprefixed + T_XY_EXTRA, except where
 * (IX+d) or the 16-bit index register is involved
 */
static uint8_t t_xy[256];

/* ED prefixed, prefix included (repeats add theirs) */
static uint8_t t_ed[256];

static uint8_t sz[256];         /* S, Z, X, Y of a result */
static uint8_t szp[256];        /* ... and parity */

static void init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) {
            p += (i >> b) & 1;
        }
        sz[i] = (i & (FS | FXY)) | (i == 0 ? FZ : 0);
        szp[i] = sz[i] | ((p & 1) ? 0 : FP);
        t_xy[i] = t_main[i] + T_XY_EXTRA;
        t_ed[i] = TI(2, 8);
    }
    static const uint8_t xy_special[][3] = {     /* opcode, CPC, T-states */
        { 0x09, 4, 15 }, { 0x19, 4, 15 }, { 0x29, 4, 15 }, { 0x39, 4, 15 },
        { 0x21, 4, 14 }, { 0x22, 6, 20 }, { 0x23, 3, 10 }, { 0x2A, 6, 20 }, { 0x2B, 3, 10 },
        { 0x34, 6, 23 }, { 0x35, 6, 23 }, { 0x36, 6, 19 },
        { 0x46, 5, 19 }, { 0x4E, 5, 19 }, { 0x56, 5, 19 }, { 0x5E, 5, 19 }, { 0x66, 5, 19 },
        { 0x6E, 5, 19 }, { 0x7E, 5, 19 },
        { 0x70, 5, 19 }, { 0x71, 5, 19 }, { 0x72, 5, 19 }, { 0x73, 5, 19 }, { 0x74, 5, 19 },
        { 0x75, 5, 19 }, { 0x77, 5, 19 },
        { 0x86, 5, 19 }, { 0x8E, 5, 19 }, { 0x96, 5, 19 }, { 0x9E, 5, 19 }, { 0xA6, 5, 19 },
        { 0xAE, 5, 19 }, { 0xB6, 5, 19 }, { 0xBE, 5, 19 },
        { 0xE1, 4, 14 }, { 0xE3, 7, 23 }, { 0xE5, 5, 15 }, { 0xE9, 2, 8 }, { 0xF9, 3, 10 },
    };
    for (size_t i = 0; i < sizeof(xy_special) / sizeof(xy_special[0]); i++) {
        t_xy[xy_special[i][0]] = TI(xy_special[i][1], xy_special[i][2]);
    }
    static const uint8_t ed_row[2][16] = {
        { 4, 4, 4, 6, 2, 4, 2, 3, 4, 4, 4, 6, 2, 4, 2, 3 },
        { 12, 12, 15, 20, 8, 14, 8, 9, 12, 12, 15, 20, 8, 14, 8, 9 },
    };
    for (int op = 0x40; op < 0x80; op++) {
        t_ed[op] = TI(ed_row[0][op & 15], ed_row[1][op & 15]);
    }
    t_ed[0x67] = t_ed[0x6F] = TI(5, 18);        /* RRD, RLD */
    t_ed[0x77] = t_ed[0x7F] = TI(2, 8);
    static const uint8_t block_us[4] = { 5, 4, 5, 5 };  /* LDx, CPx, INx, OUTx */
    for (int i = 0; i < 4; i++) {
        uint8_t t = TI(block_us[i], 16);
        (void)block_us;
        t_ed[0xA0 + i] = t_ed[0xA8 + i] = t_ed[0xB0 + i] = t_ed[0xB8 + i] = t;
    }
}

void z80_set_bus(const z80_bus_t *bus)
{
    s_bus = *bus;
}

void z80_reset(void)
{
    static bool tables;
    if (!tables) {
        init_tables();
        tables = true;
    }
    uint8_t line = z80.int_line;
    memset(&z80, 0, sizeof(z80));
    z80.int_line = line;
    AF = 0xFFFF;
    SP = 0xFFFF;
    z80.ix.w = z80.iy.w = 0xFFFF;
}

/* ---- Memory and I/O ----
 *
 * With contention, every access first waits what the machine's table says
 * for that moment (if its bank is contended), then takes its 3 T-states
 * (4 for an opcode fetch). Internal cycles between accesses aren't counted:
 * accesses after them are seen a little early.
 */

#if Z80_CONTENTION
static inline void contend(uint16_t a, int cycles)
{
    if (z80_contended[a >> 14] && z80_contention) {
        int w = z80_contention[z80.t + s_tc];
        s_tc += w;
        s_cx += w;
    }
    s_tc += cycles;
}
#define IO_AT(at)       ((at) + s_cx)
#else
#define contend(a, cycles)
#define IO_AT(at)       (at)
#endif

/* A bank's byte (or the machine's) */
static inline uint8_t mem(uint16_t a)
{
#if Z80_READ_SPECIAL
    const uint8_t *p = z80_rd[a >> Z80_BANK_SHIFT];
#if Z80_MSX
    if (__builtin_expect(p == NULL || (a == 0xFFFF && z80_ffff_special), 0)) {
#else
    if (__builtin_expect(p == NULL, 0)) {
#endif
        return s_bus.read_special(a);
    }
    return p[a & Z80_BANK_MASK];
#else
    return z80_rd[a >> Z80_BANK_SHIFT][a & Z80_BANK_MASK];
#endif
}

static inline uint8_t rd(uint16_t a)
{
    contend(a, 3);
    return mem(a);
}

static inline void wr(uint16_t a, uint8_t v)
{
    contend(a, 3);
    uint8_t *p = z80_wr[a >> Z80_BANK_SHIFT];
#if Z80_MSX
    if (__builtin_expect(a == 0xFFFF && z80_ffff_special, 0)) {
        p = NULL;
    }
#endif
    if (p) {
        p[a & Z80_BANK_MASK] = v;
    } else if (s_bus.write_special) {
        s_bus.write_special(a, v);
    }
}

static inline uint8_t io_in(uint16_t port, int at)
{
    return s_bus.in ? s_bus.in(port, IO_AT(at)) : 0xFF;
}

static inline void io_out(uint16_t port, uint8_t v, int at)
{
    if (s_bus.out) {
        s_bus.out(port, v, IO_AT(at));
    }
}

/* ---- Helpers ---- */

/* An opcode fetch (M1) */
static inline uint8_t fetch_op(void)
{
    contend(PC, 4);
    uint8_t op = mem(PC);
    PC++;
    return op;
}

static inline uint8_t fetch(void)
{
    return rd(PC++);
}

static inline uint16_t fetch16(void)
{
    uint16_t v = rd(PC);
    v |= rd(PC + 1) << 8;
    PC += 2;
    return v;
}

static inline uint16_t rd16(uint16_t a)
{
    return rd(a) | (rd(a + 1) << 8);
}

static inline void wr16(uint16_t a, uint16_t v)
{
    wr(a, v);
    wr(a + 1, v >> 8);
}

static inline void inc_r(void)
{
    z80.r = (z80.r & 0x80) | ((z80.r + 1) & 0x7F);
}

static inline void push(uint16_t v)
{
    SP -= 2;
    wr16(SP, v);
}

static inline uint16_t pop(void)
{
    uint16_t v = rd16(SP);
    SP += 2;
    return v;
}

static inline bool cond(int n)
{
    switch (n) {
    case 0: return !(F & FZ);
    case 1: return F & FZ;
    case 2: return !(F & FC);
    case 3: return F & FC;
    case 4: return !(F & FP);
    case 5: return F & FP;
    case 6: return !(F & FS);
    default: return F & FS;
    }
}

/* 8-bit registers by number (6 = (HL), not handled here); H and L come
 * from xy (HL, IX or IY)
 */
static inline uint8_t get_r(int n, const z80_pair_t *xy)
{
    switch (n) {
    case 0: return B;
    case 1: return C;
    case 2: return D;
    case 3: return E;
    case 4: return xy->b.h;
    case 5: return xy->b.l;
    default: return A;
    }
}

static inline void set_r(int n, z80_pair_t *xy, uint8_t v)
{
    switch (n) {
    case 0: B = v; break;
    case 1: C = v; break;
    case 2: D = v; break;
    case 3: E = v; break;
    case 4: xy->b.h = v; break;
    case 5: xy->b.l = v; break;
    default: A = v; break;
    }
}

/* ---- ALU ---- */

static inline void add8(uint8_t v, int carry)
{
    unsigned r = A + v + carry;
    F = sz[r & 0xFF] | ((A ^ v ^ r) & FH) | ((((A ^ ~v) & (A ^ r)) & 0x80) >> 5) | (r >> 8);
    A = r;
}

static inline void sub8(uint8_t v, int carry)
{
    unsigned r = A - v - carry;
    F = sz[r & 0xFF] | FN | ((A ^ v ^ r) & FH) | ((((A ^ v) & (A ^ r)) & 0x80) >> 5) | ((r >> 8) & FC);
    A = r;
}

static inline void cp8(uint8_t v)
{
    unsigned r = A - v;
    F = (sz[r & 0xFF] & ~FXY) | (v & FXY) | FN | ((A ^ v ^ r) & FH) |
        ((((A ^ v) & (A ^ r)) & 0x80) >> 5) | ((r >> 8) & FC);
}

static inline void alu(int n, uint8_t v)
{
    switch (n) {
    case 0: add8(v, 0); break;
    case 1: add8(v, F & FC); break;
    case 2: sub8(v, 0); break;
    case 3: sub8(v, F & FC); break;
    case 4: A &= v; F = szp[A] | FH; break;
    case 5: A ^= v; F = szp[A]; break;
    case 6: A |= v; F = szp[A]; break;
    default: cp8(v); break;
    }
}

static inline uint8_t inc8(uint8_t v)
{
    uint8_t r = v + 1;
    F = (F & FC) | sz[r] | ((r & 0x0F) == 0 ? FH : 0) | (r == 0x80 ? FP : 0);
    return r;
}

static inline uint8_t dec8(uint8_t v)
{
    uint8_t r = v - 1;
    F = (F & FC) | FN | sz[r] | ((r & 0x0F) == 0x0F ? FH : 0) | (r == 0x7F ? FP : 0);
    return r;
}

static inline uint16_t add16(uint16_t a, uint16_t v)
{
    uint32_t r = a + v;
    WZ = a + 1;
    F = (F & (FS | FZ | FP)) | ((r >> 8) & FXY) | (((a ^ v ^ r) >> 8) & FH) | (r >> 16);
    return r;
}

static inline void adc16(uint16_t v)
{
    uint32_t r = HL + v + (F & FC);
    WZ = HL + 1;
    F = ((r >> 8) & (FS | FXY)) | ((r & 0xFFFF) ? 0 : FZ) | (((HL ^ v ^ r) >> 8) & FH) |
        ((((HL ^ ~v) & (HL ^ r)) & 0x8000) >> 13) | (r >> 16);
    HL = r;
}

static inline void sbc16(uint16_t v)
{
    uint32_t r = HL - v - (F & FC);
    WZ = HL + 1;
    F = FN | ((r >> 8) & (FS | FXY)) | ((r & 0xFFFF) ? 0 : FZ) | (((HL ^ v ^ r) >> 8) & FH) |
        ((((HL ^ v) & (HL ^ r)) & 0x8000) >> 13) | ((r >> 16) & FC);
    HL = r;
}

/* CB rotates and shifts by number (RLC RRC RL RR SLA SRA SLL SRL) */
static inline uint8_t rot(int n, uint8_t v)
{
    uint8_t r, c;
    switch (n) {
    case 0: c = v >> 7; r = (v << 1) | c; break;
    case 1: c = v & 1; r = (v >> 1) | (c << 7); break;
    case 2: c = v >> 7; r = (v << 1) | (F & FC); break;
    case 3: c = v & 1; r = (v >> 1) | ((F & FC) << 7); break;
    case 4: c = v >> 7; r = v << 1; break;
    case 5: c = v & 1; r = (v >> 1) | (v & 0x80); break;
    case 6: c = v >> 7; r = (v << 1) | 1; break;
    default: c = v & 1; r = v >> 1; break;
    }
    F = szp[r] | c;
    return r;
}

static inline void bit(int n, uint8_t v, uint8_t xy_from)
{
    uint8_t r = v & (1u << n);
    F = (F & FC) | FH | (xy_from & FXY) | (r & FS) | (r ? 0 : FZ | FP);
}

static void daa(void)
{
    uint8_t a = A, corr = 0, carry = F & FC;
    if ((F & FH) || (a & 0x0F) > 9) {
        corr = 0x06;
    }
    if (carry || a > 0x99) {
        corr |= 0x60;
        carry = FC;
    }
    A = (F & FN) ? a - corr : a + corr;
    F = szp[A] | ((a ^ A) & FH) | (F & FN) | carry;
}

/* ---- Interrupts ---- */

static int interrupt(void)
{
    z80.halted = 0;
    z80.iff1 = z80.iff2 = 0;
    inc_r();
    uint8_t vector = s_bus.int_ack ? s_bus.int_ack() : 0xFF;
#if Z80_CONTENTION
    s_tc = 7;           /* the acknowledge cycle, then the pushes */
#endif
    push(PC);
    if (z80.im == 2) {
        PC = rd16((z80.i << 8) | vector);
        WZ = PC;
        return T_INT_IM2;
    }
    PC = 0x38;          /* IM 0 reads $FF from the bus: RST 38 */
    WZ = PC;
    return T_INT;
}

/* ---- CB page ---- */

static int exec_cb(void)
{
    uint8_t op = fetch_op();
    inc_r();
    int r = op & 7, n = (op >> 3) & 7;
    uint8_t v;
    if (r == 6) {
        v = rd(HL);
        switch (op >> 6) {
        case 0: wr(HL, rot(n, v)); return T_CB_HL;
        case 1: bit(n, v, z80.wz.b.h); return T_CB_BIT_HL;
        case 2: wr(HL, v & ~(1u << n)); return T_CB_HL;
        default: wr(HL, v | (1u << n)); return T_CB_HL;
        }
    }
    v = get_r(r, &z80.hl);
    switch (op >> 6) {
    case 0: set_r(r, &z80.hl, rot(n, v)); break;
    case 1: bit(n, v, v); break;
    case 2: set_r(r, &z80.hl, v & ~(1u << n)); break;
    default: set_r(r, &z80.hl, v | (1u << n)); break;
    }
    return T_CB_R;
}

/* DD CB d op / FD CB d op: always on (IX+d); the result also goes into
 * a register unless that is (HL)
 */
static int exec_xycb(z80_pair_t *xy)
{
    uint16_t addr = xy->w + (int8_t)fetch();
    uint8_t op = fetch();
    int r = op & 7, n = (op >> 3) & 7;
    uint8_t v = rd(addr), res;
    WZ = addr;
    switch (op >> 6) {
    case 0: res = rot(n, v); break;
    case 1: bit(n, v, addr >> 8); return T_XYCB_BIT;
    case 2: res = v & ~(1u << n); break;
    default: res = v | (1u << n); break;
    }
    wr(addr, res);
    if (r != 6) {
        set_r(r, &z80.hl, res);
    }
    return T_XYCB;
}

/* ---- ED page ---- */

static inline void block_io_flags(uint8_t v, unsigned k)
{
    F = sz[B] | ((v & 0x80) ? FN : 0) | (k > 255 ? FH | FC : 0) | (szp[(k & 7) ^ B] & FP);
}

static int exec_ed(void)
{
    uint8_t op = fetch_op();
    inc_r();
    int t = t_ed[op];
    uint8_t v;
    switch (op) {
    case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x70: case 0x78:
        v = io_in(BC, AT_IN_C);
        WZ = BC + 1;
        F = (F & FC) | szp[v];
        if (op != 0x70) {
            set_r((op >> 3) & 7, &z80.hl, v);
        }
        break;
    case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x71: case 0x79:
        io_out(BC, op == 0x71 ? 0 : get_r((op >> 3) & 7, &z80.hl), AT_OUT_C);
        WZ = BC + 1;
        break;
    case 0x42: sbc16(BC); break;
    case 0x52: sbc16(DE); break;
    case 0x62: sbc16(HL); break;
    case 0x72: sbc16(SP); break;
    case 0x4A: adc16(BC); break;
    case 0x5A: adc16(DE); break;
    case 0x6A: adc16(HL); break;
    case 0x7A: adc16(SP); break;
    case 0x43: case 0x53: case 0x63: case 0x73: {
        uint16_t a = fetch16();
        uint16_t r = op == 0x43 ? BC : op == 0x53 ? DE : op == 0x63 ? HL : SP;
        wr16(a, r);
        WZ = a + 1;
        break;
    }
    case 0x4B: case 0x5B: case 0x6B: case 0x7B: {
        uint16_t a = fetch16();
        uint16_t r = rd16(a);
        WZ = a + 1;
        if (op == 0x4B) BC = r;
        else if (op == 0x5B) DE = r;
        else if (op == 0x6B) HL = r;
        else SP = r;
        break;
    }
    case 0x44: case 0x4C: case 0x54: case 0x5C: case 0x64: case 0x6C: case 0x74: case 0x7C:
        v = A;
        A = 0;
        sub8(v, 0);
        break;
    case 0x45: case 0x55: case 0x65: case 0x75:
    case 0x4D: case 0x5D: case 0x6D: case 0x7D:
        z80.iff1 = z80.iff2;
        PC = pop();
        WZ = PC;
        break;
    case 0x46: case 0x4E: case 0x66: case 0x6E: z80.im = 0; break;
    case 0x56: case 0x76: z80.im = 1; break;
    case 0x5E: case 0x7E: z80.im = 2; break;
    case 0x47: z80.i = A; break;
    case 0x4F: z80.r = A; break;
    case 0x57:
        A = z80.i;
        F = (F & FC) | sz[A] | (z80.iff2 ? FP : 0);
        break;
    case 0x5F:
        A = z80.r;
        F = (F & FC) | sz[A] | (z80.iff2 ? FP : 0);
        break;
    case 0x67:      /* RRD */
        v = rd(HL);
        wr(HL, (A << 4) | (v >> 4));
        A = (A & 0xF0) | (v & 0x0F);
        F = (F & FC) | szp[A];
        WZ = HL + 1;
        break;
    case 0x6F:      /* RLD */
        v = rd(HL);
        wr(HL, (v << 4) | (A & 0x0F));
        A = (A & 0xF0) | (v >> 4);
        F = (F & FC) | szp[A];
        WZ = HL + 1;
        break;

    case 0xA0: case 0xA8: case 0xB0: case 0xB8: {     /* LDI LDD LDIR LDDR */
        v = rd(HL);
        wr(DE, v);
        int step = (op & 8) ? -1 : 1;
        HL += step;
        DE += step;
        BC--;
        uint8_t n = v + A;
        F = (F & (FS | FZ | FC)) | (BC ? FP : 0) | (n & FX) | ((n << 4) & FY);
        if ((op & 0x10) && BC) {
            PC -= 2;
            WZ = PC + 1;
            t += T_LDIR_REPEAT;
        }
        break;
    }
    case 0xA1: case 0xA9: case 0xB1: case 0xB9: {     /* CPI CPD CPIR CPDR */
        v = rd(HL);
        uint8_t r = A - v;
        int step = (op & 8) ? -1 : 1;
        HL += step;
        WZ += step;
        BC--;
        F = (F & FC) | FN | (sz[r] & (FS | FZ)) | ((A ^ v ^ r) & FH) | (BC ? FP : 0);
        uint8_t n = r - ((F & FH) ? 1 : 0);
        F |= (n & FX) | ((n << 4) & FY);
        if ((op & 0x10) && BC && !(F & FZ)) {
            PC -= 2;
            WZ = PC + 1;
            t += T_CPIR_REPEAT;
        }
        break;
    }
    case 0xA2: case 0xAA: case 0xB2: case 0xBA: {     /* INI IND INIR INDR */
        v = io_in(BC, AT_INI);
        int step = (op & 8) ? -1 : 1;
        WZ = BC + step;
        wr(HL, v);
        HL += step;
        B--;
        block_io_flags(v, v + (uint8_t)(C + step));
        if ((op & 0x10) && B) {
            PC -= 2;
            t += T_IOR_REPEAT;
        }
        break;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {     /* OUTI OUTD OTIR OTDR */
        v = rd(HL);
        B--;
        int step = (op & 8) ? -1 : 1;
        io_out(BC, v, AT_OUTI);
        WZ = BC + step;
        HL += step;
        block_io_flags(v, v + L);
        if ((op & 0x10) && B) {
            PC -= 2;
            t += T_IOR_REPEAT;
        }
        break;
    }
    default:
        break;      /* the other ED codes do nothing */
    }
    return t;
}

/* ---- Unprefixed and DD/FD ---- */

/* (HL), or (IX+d) with the displacement fetched */
static inline uint16_t ea(const z80_pair_t *xy, int idx)
{
    if (!idx) {
        return HL;
    }
    uint16_t a = xy->w + (int8_t)fetch();
    WZ = a;
    return a;
}

static int exec_op(uint8_t op, z80_pair_t *xy, int idx)
{
    int t = idx ? t_xy[op] : t_main[op];
    uint8_t v;
    uint16_t a;

    switch (op) {
    case 0x00: break;
    case 0x01: BC = fetch16(); break;
    case 0x11: DE = fetch16(); break;
    case 0x21: xy->w = fetch16(); break;
    case 0x31: SP = fetch16(); break;
    case 0x02: wr(BC, A); z80.wz.b.l = BC + 1; z80.wz.b.h = A; break;
    case 0x12: wr(DE, A); z80.wz.b.l = DE + 1; z80.wz.b.h = A; break;
    case 0x0A: A = rd(BC); WZ = BC + 1; break;
    case 0x1A: A = rd(DE); WZ = DE + 1; break;
    case 0x22: a = fetch16(); wr16(a, xy->w); WZ = a + 1; break;
    case 0x2A: a = fetch16(); xy->w = rd16(a); WZ = a + 1; break;
    case 0x32: a = fetch16(); wr(a, A); z80.wz.b.l = a + 1; z80.wz.b.h = A; break;
    case 0x3A: a = fetch16(); A = rd(a); WZ = a + 1; break;
    case 0x03: BC++; break;
    case 0x13: DE++; break;
    case 0x23: xy->w++; break;
    case 0x33: SP++; break;
    case 0x0B: BC--; break;
    case 0x1B: DE--; break;
    case 0x2B: xy->w--; break;
    case 0x3B: SP--; break;
    case 0x04: B = inc8(B); break;
    case 0x0C: C = inc8(C); break;
    case 0x14: D = inc8(D); break;
    case 0x1C: E = inc8(E); break;
    case 0x24: xy->b.h = inc8(xy->b.h); break;
    case 0x2C: xy->b.l = inc8(xy->b.l); break;
    case 0x3C: A = inc8(A); break;
    case 0x34: a = ea(xy, idx); wr(a, inc8(rd(a))); break;
    case 0x05: B = dec8(B); break;
    case 0x0D: C = dec8(C); break;
    case 0x15: D = dec8(D); break;
    case 0x1D: E = dec8(E); break;
    case 0x25: xy->b.h = dec8(xy->b.h); break;
    case 0x2D: xy->b.l = dec8(xy->b.l); break;
    case 0x3D: A = dec8(A); break;
    case 0x35: a = ea(xy, idx); wr(a, dec8(rd(a))); break;
    case 0x06: B = fetch(); break;
    case 0x0E: C = fetch(); break;
    case 0x16: D = fetch(); break;
    case 0x1E: E = fetch(); break;
    case 0x26: xy->b.h = fetch(); break;
    case 0x2E: xy->b.l = fetch(); break;
    case 0x3E: A = fetch(); break;
    case 0x36: a = ea(xy, idx); wr(a, fetch()); break;
    case 0x07:
        A = (A << 1) | (A >> 7);
        F = (F & (FS | FZ | FP)) | (A & (FXY | FC));
        break;
    case 0x0F:
        F = (F & (FS | FZ | FP)) | (A & FC);
        A = (A >> 1) | (A << 7);
        F |= A & FXY;
        break;
    case 0x17:
        v = A >> 7;
        A = (A << 1) | (F & FC);
        F = (F & (FS | FZ | FP)) | (A & FXY) | v;
        break;
    case 0x1F:
        v = A & 1;
        A = (A >> 1) | ((F & FC) << 7);
        F = (F & (FS | FZ | FP)) | (A & FXY) | v;
        break;
    case 0x08: { uint16_t x = AF; AF = z80.af2; z80.af2 = x; break; }
    case 0x09: xy->w = add16(xy->w, BC); break;
    case 0x19: xy->w = add16(xy->w, DE); break;
    case 0x29: xy->w = add16(xy->w, xy->w); break;
    case 0x39: xy->w = add16(xy->w, SP); break;
    case 0x10:
        v = fetch();
        if (--B) {
            PC += (int8_t)v;
            WZ = PC;
            t += T_JR_TAKEN;
        }
        break;
    case 0x18: PC += (int8_t)fetch(); WZ = PC; break;
    case 0x20: case 0x28: case 0x30: case 0x38:
        v = fetch();
        if (cond((op >> 3) & 3)) {
            PC += (int8_t)v;
            WZ = PC;
            t += T_JR_TAKEN;
        }
        break;
    case 0x27: daa(); break;
    case 0x2F: A = ~A; F = (F & (FS | FZ | FP | FC)) | FH | FN | (A & FXY); break;
    case 0x37: F = (F & (FS | FZ | FP)) | FC | (A & FXY); break;
    case 0x3F: F = ((F & (FS | FZ | FP | FC)) | ((F & FC) << 4) | (A & FXY)) ^ FC; break;

    case 0x76:              /* HALT: NOPs until an interrupt */
        z80.halted = 1;
        break;
    case 0x40 ... 0x75:
    case 0x77 ... 0x7F: {
        int dst = (op >> 3) & 7, src = op & 7;
        if (src == 6) {
            set_r(dst, &z80.hl, rd(ea(xy, idx)));       /* LD H,(IX+d) loads H */
        } else if (dst == 6) {
            a = ea(xy, idx);
            wr(a, get_r(src, &z80.hl));
        } else {
            set_r(dst, xy, get_r(src, xy));
        }
        break;
    }
    case 0x80 ... 0xBF:
        if ((op & 7) == 6) {
            v = rd(ea(xy, idx));
        } else {
            v = get_r(op & 7, xy);
        }
        alu((op >> 3) & 7, v);
        break;

    case 0xC0: case 0xC8: case 0xD0: case 0xD8: case 0xE0: case 0xE8: case 0xF0: case 0xF8:
        if (cond((op >> 3) & 7)) {
            PC = pop();
            WZ = PC;
            t += T_RET_TAKEN;
        }
        break;
    case 0xC9: PC = pop(); WZ = PC; break;
    case 0xC1: BC = pop(); break;
    case 0xD1: DE = pop(); break;
    case 0xE1: xy->w = pop(); break;
    case 0xF1: AF = pop(); break;
    case 0xC5: push(BC); break;
    case 0xD5: push(DE); break;
    case 0xE5: push(xy->w); break;
    case 0xF5: push(AF); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA:
        a = fetch16();
        WZ = a;
        if (cond((op >> 3) & 7)) {
            PC = a;
        }
        break;
    case 0xC3: PC = WZ = fetch16(); break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xE4: case 0xEC: case 0xF4: case 0xFC:
        a = fetch16();
        WZ = a;
        if (cond((op >> 3) & 7)) {
            push(PC);
            PC = a;
            t += T_CALL_TAKEN;
        }
        break;
    case 0xCD: a = fetch16(); WZ = a; push(PC); PC = a; break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu((op >> 3) & 7, fetch());
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push(PC);
        PC = WZ = op & 0x38;
        break;
    case 0xD3: {
        v = fetch();
        io_out(v | (A << 8), A, AT_OUT_N);
        z80.wz.b.l = v + 1;
        z80.wz.b.h = A;
        break;
    }
    case 0xDB: {
        uint16_t port = fetch() | (A << 8);
        A = io_in(port, AT_IN_N);
        WZ = port + 1;
        break;
    }
    case 0xD9: {
        uint16_t x;
        x = BC; BC = z80.bc2; z80.bc2 = x;
        x = DE; DE = z80.de2; z80.de2 = x;
        x = HL; HL = z80.hl2; z80.hl2 = x;
        break;
    }
    case 0xE3: a = rd16(SP); wr16(SP, xy->w); xy->w = a; WZ = a; break;
    case 0xE9: PC = xy->w; break;
    case 0xEB: { uint16_t x = DE; DE = HL; HL = x; break; }     /* never IX/IY */
    case 0xF3: z80.iff1 = z80.iff2 = 0; break;
    case 0xFB: z80.iff1 = z80.iff2 = 1; z80.ei_delay = 1; break;
    case 0xF9: SP = xy->w; break;
    default:
        break;      /* CB, DD, ED, FD: handled by the caller */
    }
    return t;
}

static int exec_xy(z80_pair_t *xy)
{
    uint8_t op = mem(PC);                   /* (a look ahead: not z80_peek(), blind to read_special()) */
    if (op == 0xDD || op == 0xFD || op == 0xED) {
        return T_PREFIX;    /* this prefix does nothing; the next one counts */
    }
    fetch_op();
    inc_r();
    if (op == 0xCB) {
        return exec_xycb(xy);
    }
    return exec_op(op, xy, 1);
}

#ifdef Z80_HOST_HOOK
void z80_host_hook(void);       /* host build: tracing */
#endif

static inline int execute(void)
{
    if (z80.int_line && z80.iff1 && !z80.ei_delay) {
        return interrupt();
    }
    z80.ei_delay = 0;
    if (z80.halted) {
        inc_r();
        return T_HALTED;
    }
    uint8_t op = fetch_op();
    inc_r();
    switch (op) {
    case 0xCB: return exec_cb();
    case 0xED: return exec_ed();
    case 0xDD: return exec_xy(&z80.ix);
    case 0xFD: return exec_xy(&z80.iy);
    default: return exec_op(op, &z80.hl, 0);
    }
}

int Z80_HOT z80_step(void)
{
#ifdef Z80_HOST_HOOK
    z80_host_hook();
#endif
    z80.wait = 0;
#if Z80_CONTENTION
    s_tc = s_cx = 0;
    int t = execute();
    return t + s_cx + z80.wait;
#else
    int t = execute();
    return t + z80.wait;
#endif
}

/* ---- Disassembler (debugging) ---- */

static const char *const r8n[8] = { "B", "C", "D", "E", "H", "L", "(HL)", "A" };
static const char *const r16n[4] = { "BC", "DE", "HL", "SP" };
static const char *const r16p[4] = { "BC", "DE", "HL", "AF" };
static const char *const ccn[8] = { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" };
static const char *const alun[8] = { "ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP " };
static const char *const rotn[8] = { "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL" };

int z80_disassemble(uint16_t pc, char *buf, int len)
{
    uint16_t start = pc;
    uint8_t op = mem(pc++);
    const char *xyn = NULL;
    int8_t d = 0;
    if (op == 0xDD || op == 0xFD) {
        xyn = op == 0xDD ? "IX" : "IY";
        op = mem(pc++);
    }
    char hl[12], mhl[16], h[8], l[8];
    snprintf(hl, sizeof(hl), "%s", xyn ? xyn : "HL");
    snprintf(h, sizeof(h), "%s", xyn ? (xyn[1] == 'X' ? "IXH" : "IYH") : "H");
    snprintf(l, sizeof(l), "%s", xyn ? (xyn[1] == 'X' ? "IXL" : "IYL") : "L");

    if (op == 0xCB) {
        if (xyn) {
            d = mem(pc++);
        }
        uint8_t o = mem(pc++);
        int r = o & 7, n = (o >> 3) & 7;
        if (xyn) {
            snprintf(mhl, sizeof(mhl), "(%s%+d)", xyn, d);
        } else {
            snprintf(mhl, sizeof(mhl), "%s", r8n[r]);
        }
        switch (o >> 6) {
        case 0: snprintf(buf, len, "%s %s", rotn[n], mhl); break;
        case 1: snprintf(buf, len, "BIT %d,%s", n, mhl); break;
        case 2: snprintf(buf, len, "RES %d,%s", n, mhl); break;
        default: snprintf(buf, len, "SET %d,%s", n, mhl); break;
        }
        return (uint16_t)(pc - start);
    }
    if (op == 0xED) {
        uint8_t o = mem(pc++);
        int r = (o >> 3) & 7;
        uint16_t nn;
        switch (o) {
        case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x70: case 0x78:
            snprintf(buf, len, "IN %s,(C)", o == 0x70 ? "F" : r8n[r]); break;
        case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x71: case 0x79:
            snprintf(buf, len, "OUT (C),%s", o == 0x71 ? "0" : r8n[r]); break;
        case 0x42: case 0x52: case 0x62: case 0x72: snprintf(buf, len, "SBC HL,%s", r16n[r >> 1]); break;
        case 0x4A: case 0x5A: case 0x6A: case 0x7A: snprintf(buf, len, "ADC HL,%s", r16n[r >> 1]); break;
        case 0x43: case 0x53: case 0x63: case 0x73:
            nn = mem(pc) | (mem(pc + 1) << 8); pc += 2;
            snprintf(buf, len, "LD ($%04X),%s", nn, r16n[r >> 1]); break;
        case 0x4B: case 0x5B: case 0x6B: case 0x7B:
            nn = mem(pc) | (mem(pc + 1) << 8); pc += 2;
            snprintf(buf, len, "LD %s,($%04X)", r16n[r >> 1], nn); break;
        case 0x44: snprintf(buf, len, "NEG"); break;
        case 0x45: snprintf(buf, len, "RETN"); break;
        case 0x4D: snprintf(buf, len, "RETI"); break;
        case 0x46: snprintf(buf, len, "IM 0"); break;
        case 0x56: snprintf(buf, len, "IM 1"); break;
        case 0x5E: snprintf(buf, len, "IM 2"); break;
        case 0x47: snprintf(buf, len, "LD I,A"); break;
        case 0x4F: snprintf(buf, len, "LD R,A"); break;
        case 0x57: snprintf(buf, len, "LD A,I"); break;
        case 0x5F: snprintf(buf, len, "LD A,R"); break;
        case 0x67: snprintf(buf, len, "RRD"); break;
        case 0x6F: snprintf(buf, len, "RLD"); break;
        default: {
            static const char *const blk[4][4] = {
                { "LDI", "CPI", "INI", "OUTI" }, { "LDD", "CPD", "IND", "OUTD" },
                { "LDIR", "CPIR", "INIR", "OTIR" }, { "LDDR", "CPDR", "INDR", "OTDR" },
            };
            if (o >= 0xA0 && o < 0xC0 && (o & 7) < 4) {
                snprintf(buf, len, "%s", blk[(o >> 3) & 3][o & 3]);
            } else {
                snprintf(buf, len, "ED %02X", o);
            }
        }
        }
        return (uint16_t)(pc - start);
    }

    /* (HL) or (IX+d) */
    bool uses_m = (op >= 0x40 && op < 0xC0 && ((op & 7) == 6 || (op >= 0x70 && op < 0x78))) ||
                  op == 0x34 || op == 0x35 || op == 0x36;
    if (op == 0x76) {
        uses_m = false;
    }
    if (xyn && uses_m) {
        d = mem(pc++);
        snprintf(mhl, sizeof(mhl), "(%s%+d)", xyn, d);
    } else {
        snprintf(mhl, sizeof(mhl), "(HL)");
    }
    /* With an (IX+d) operand, H and L stay H and L */
    const char *rn[8] = { "B", "C", "D", "E", uses_m ? "H" : h, uses_m ? "L" : l, mhl, "A" };
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    uint16_t nn;
    uint8_t n;
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) snprintf(buf, len, "NOP");
            else if (y == 1) snprintf(buf, len, "EX AF,AF'");
            else {
                d = mem(pc++);
                uint16_t target = pc + d;
                if (y == 2) snprintf(buf, len, "DJNZ $%04X", target);
                else if (y == 3) snprintf(buf, len, "JR $%04X", target);
                else snprintf(buf, len, "JR %s,$%04X", ccn[y - 4], target);
            }
            break;
        case 1:
            if (!q) {
                nn = mem(pc) | (mem(pc + 1) << 8); pc += 2;
                snprintf(buf, len, "LD %s,$%04X", p == 2 ? hl : r16n[p], nn);
            } else {
                snprintf(buf, len, "ADD %s,%s", hl, p == 2 ? hl : r16n[p]);
            }
            break;
        case 2:
            if (p < 2) {
                snprintf(buf, len, q ? "LD A,(%s)" : "LD (%s),A", r16n[p]);
            } else {
                nn = mem(pc) | (mem(pc + 1) << 8); pc += 2;
                if (p == 2 && q) snprintf(buf, len, "LD %s,($%04X)", hl, nn);
                if (p == 2 && !q) snprintf(buf, len, "LD ($%04X),%s", nn, hl);
                if (p == 3) snprintf(buf, len, q ? "LD A,($%04X)" : "LD ($%04X),A", nn);
            }
            break;
        case 3: snprintf(buf, len, "%s %s", q ? "DEC" : "INC", p == 2 ? hl : r16n[p]); break;
        case 4: snprintf(buf, len, "INC %s", rn[y]); break;
        case 5: snprintf(buf, len, "DEC %s", rn[y]); break;
        case 6: n = mem(pc++); snprintf(buf, len, "LD %s,$%02X", rn[y], n); break;
        default: {
            static const char *const m[8] = { "RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF" };
            snprintf(buf, len, "%s", m[y]);
        }
        }
        break;
    case 1:
        if (op == 0x76) snprintf(buf, len, "HALT");
        else snprintf(buf, len, "LD %s,%s", rn[y], rn[z]);
        break;
    case 2:
        snprintf(buf, len, "%s%s", alun[y], rn[z]);
        break;
    default:
        switch (z) {
        case 0: snprintf(buf, len, "RET %s", ccn[y]); break;
        case 1:
            if (!q) snprintf(buf, len, "POP %s", p == 2 ? hl : r16p[p]);
            else if (p == 0) snprintf(buf, len, "RET");
            else if (p == 1) snprintf(buf, len, "EXX");
            else if (p == 2) snprintf(buf, len, "JP (%s)", hl);
            else snprintf(buf, len, "LD SP,%s", hl);
            break;
        case 2: nn = mem(pc) | (mem(pc + 1) << 8); pc += 2; snprintf(buf, len, "JP %s,$%04X", ccn[y], nn); break;
        case 3:
            if (y == 0) { nn = mem(pc) | (mem(pc + 1) << 8); pc += 2; snprintf(buf, len, "JP $%04X", nn); }
            else if (y == 2) { n = mem(pc++); snprintf(buf, len, "OUT ($%02X),A", n); }
            else if (y == 3) { n = mem(pc++); snprintf(buf, len, "IN A,($%02X)", n); }
            else if (y == 4) snprintf(buf, len, "EX (SP),%s", hl);
            else if (y == 5) snprintf(buf, len, "EX DE,HL");
            else if (y == 6) snprintf(buf, len, "DI");
            else snprintf(buf, len, "EI");
            break;
        case 4: nn = mem(pc) | (mem(pc + 1) << 8); pc += 2; snprintf(buf, len, "CALL %s,$%04X", ccn[y], nn); break;
        case 5:
            if (!q) snprintf(buf, len, "PUSH %s", p == 2 ? hl : r16p[p]);
            else { nn = mem(pc) | (mem(pc + 1) << 8); pc += 2; snprintf(buf, len, "CALL $%04X", nn); }
            break;
        case 6: n = mem(pc++); snprintf(buf, len, "%s$%02X", alun[y], n); break;
        default: snprintf(buf, len, "RST $%02X", y * 8); break;
        }
    }
    return (uint16_t)(pc - start);
}
