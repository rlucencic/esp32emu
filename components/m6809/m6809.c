/*
 * Motorola 6809 / Hitachi 6309 core (see include/m6809.h).
 *
 * One instruction at a time: m6809_step() takes an interrupt or runs an
 * instruction and adds its cycles. Cycle counts: the 6809 data sheet's
 * (the 6309 in emulation mode: the same), the 6309 Technical Reference's
 * native counts (CYC(e, n): emulation, native). Indexed modes add theirs
 * (indexed()).
 *
 * References: the MC6809E data sheet, Chris Burke's HD6309 Technical
 * Reference, and MAME's m6809/hd6309 (BSD-3, N. Woods and others; read for
 * the corner cases: TFR/EXG between register sizes, the 6309's register
 * operations, DIVD/DIVQ).
 */
#include <string.h>
#include "m6809.h"
#include "emudbg.h"

#define NATIVE      (c->md & M6309_NM)
#define CYC(e, n)   (NATIVE ? (n) : (e))

static const uint16_t VEC_TRAP = 0xFFF0, VEC_SWI3 = 0xFFF2, VEC_SWI2 = 0xFFF4, VEC_FIRQ = 0xFFF6,
                      VEC_IRQ = 0xFFF8, VEC_SWI = 0xFFFA, VEC_NMI = 0xFFFC, VEC_RESET = 0xFFFE;

/* ---- Memory ---- */

static inline uint8_t rd8(m6809_t *c, uint16_t a)
{
    const uint8_t *p = c->rd[a >> 8];
    return p ? p[a & 0xFF] : c->bus.read(c->bus.ctx, a);
}

static inline void wr8(m6809_t *c, uint16_t a, uint8_t v)
{
    uint8_t *p = c->wr[a >> 8];
    if (p) {
        p[a & 0xFF] = v;
    } else {
        c->bus.write(c->bus.ctx, a, v);
    }
}

static inline uint16_t rd16(m6809_t *c, uint16_t a)
{
    uint16_t hi = rd8(c, a);
    return (uint16_t)(hi << 8 | rd8(c, (uint16_t)(a + 1)));
}

static inline void wr16(m6809_t *c, uint16_t a, uint16_t v)
{
    wr8(c, a, (uint8_t)(v >> 8));
    wr8(c, (uint16_t)(a + 1), (uint8_t)v);
}

static inline uint8_t fetch(m6809_t *c)
{
    return rd8(c, c->pc++);
}

static inline uint16_t fetch16(m6809_t *c)
{
    uint16_t v = rd16(c, c->pc);
    c->pc += 2;
    return v;
}

static inline void push8(m6809_t *c, uint16_t *sp, uint8_t v)
{
    wr8(c, --*sp, v);
}

static inline void push16(m6809_t *c, uint16_t *sp, uint16_t v)
{
    wr8(c, --*sp, (uint8_t)v);
    wr8(c, --*sp, (uint8_t)(v >> 8));
}

static inline uint8_t pull8(m6809_t *c, uint16_t *sp)
{
    return rd8(c, (*sp)++);
}

static inline uint16_t pull16(m6809_t *c, uint16_t *sp)
{
    uint16_t hi = rd8(c, (*sp)++);
    return (uint16_t)(hi << 8 | rd8(c, (*sp)++));
}

/* ---- Flags ---- */

#define CLR(f)      (c->cc &= (uint8_t)~(f))
#define SET_IF(f, x) do { if (x) c->cc |= (f); } while (0)

static inline void nz8(m6809_t *c, uint8_t v)
{
    c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_Z)) | ((v & 0x80) ? M6809_N : 0) | (v ? 0 : M6809_Z));
}

static inline void nz16(m6809_t *c, uint16_t v)
{
    c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_Z)) | ((v & 0x8000) ? M6809_N : 0) | (v ? 0 : M6809_Z));
}

/* Loads, stores, logic: N Z, V cleared */
static inline uint8_t ld8(m6809_t *c, uint8_t v)
{
    CLR(M6809_V);
    nz8(c, v);
    return v;
}

static inline uint16_t ld16(m6809_t *c, uint16_t v)
{
    CLR(M6809_V);
    nz16(c, v);
    return v;
}

static inline uint8_t add8(m6809_t *c, uint8_t a, uint8_t b, unsigned carry)
{
    unsigned r = a + b + carry;
    c->cc &= (uint8_t)~(M6809_H | M6809_N | M6809_Z | M6809_V | M6809_C);
    SET_IF(M6809_H, (a ^ b ^ r) & 0x10);
    SET_IF(M6809_N, r & 0x80);
    SET_IF(M6809_Z, !(r & 0xFF));
    SET_IF(M6809_V, (a ^ r) & (b ^ r) & 0x80);
    SET_IF(M6809_C, r & 0x100);
    return (uint8_t)r;
}

static inline uint8_t sub8(m6809_t *c, uint8_t a, uint8_t b, unsigned borrow)
{
    unsigned r = (unsigned)a - b - borrow;
    c->cc &= (uint8_t)~(M6809_N | M6809_Z | M6809_V | M6809_C);
    SET_IF(M6809_N, r & 0x80);
    SET_IF(M6809_Z, !(r & 0xFF));
    SET_IF(M6809_V, (a ^ b) & (a ^ r) & 0x80);
    SET_IF(M6809_C, r & 0x100);
    return (uint8_t)r;
}

static inline uint16_t add16(m6809_t *c, uint16_t a, uint16_t b, unsigned carry)
{
    uint32_t r = (uint32_t)a + b + carry;
    c->cc &= (uint8_t)~(M6809_N | M6809_Z | M6809_V | M6809_C);
    SET_IF(M6809_N, r & 0x8000);
    SET_IF(M6809_Z, !(r & 0xFFFF));
    SET_IF(M6809_V, (a ^ r) & (b ^ r) & 0x8000);
    SET_IF(M6809_C, r & 0x10000);
    return (uint16_t)r;
}

static inline uint16_t sub16(m6809_t *c, uint16_t a, uint16_t b, unsigned borrow)
{
    uint32_t r = (uint32_t)a - b - borrow;
    c->cc &= (uint8_t)~(M6809_N | M6809_Z | M6809_V | M6809_C);
    SET_IF(M6809_N, r & 0x8000);
    SET_IF(M6809_Z, !(r & 0xFFFF));
    SET_IF(M6809_V, (a ^ b) & (a ^ r) & 0x8000);
    SET_IF(M6809_C, r & 0x10000);
    return (uint16_t)r;
}

#define CARRY   ((unsigned)(c->cc & M6809_C))

/* ---- Read-modify-write operations (8 and 16 bits) ---- */

enum { RMW_NEG, RMW_XNC, RMW_COM, RMW_LSR, RMW_ROR, RMW_ASR, RMW_ASL, RMW_ROL, RMW_DEC, RMW_INC, RMW_TST, RMW_CLR };

/* Opcode low nibble ($x0-$xF of rows 0, 4-7) to the operation; -1: none */
static const int8_t rmw_op[16] = {
    RMW_NEG, RMW_NEG, RMW_XNC, RMW_COM, RMW_LSR, RMW_LSR, RMW_ROR, RMW_ASR,
    RMW_ASL, RMW_ROL, RMW_DEC, RMW_DEC, RMW_INC, RMW_TST, -1, RMW_CLR,
};

static uint8_t rmw8(m6809_t *c, int op, uint8_t v)
{
    uint8_t r;
    switch (op) {
    case RMW_XNC:
        /* The 6809's undocumented $x2: COM with carry set, else NEG */
        if (c->cc & M6809_C) {
            goto com;
        }
        /* fall through */
    case RMW_NEG:
        return sub8(c, 0, v, 0);
    case RMW_COM:
    com:
        r = (uint8_t)~v;
        c->cc = (uint8_t)((c->cc & ~M6809_V) | M6809_C);
        nz8(c, r);
        return r;
    case RMW_LSR:
        c->cc = (uint8_t)((c->cc & ~(M6809_C | M6809_N)) | (v & 1));
        r = v >> 1;
        SET_IF(M6809_Z, !r);
        if (r) CLR(M6809_Z);
        return r;
    case RMW_ROR:
        r = (uint8_t)((v >> 1) | (CARRY << 7));
        c->cc = (uint8_t)((c->cc & ~M6809_C) | (v & 1));
        nz8(c, r);
        return r;
    case RMW_ASR:
        r = (uint8_t)((v >> 1) | (v & 0x80));
        c->cc = (uint8_t)((c->cc & ~M6809_C) | (v & 1));
        nz8(c, r);
        return r;
    case RMW_ASL:
        r = (uint8_t)(v << 1);
        c->cc &= (uint8_t)~(M6809_C | M6809_V);
        SET_IF(M6809_C, v & 0x80);
        SET_IF(M6809_V, (v ^ (v << 1)) & 0x80);
        nz8(c, r);
        return r;
    case RMW_ROL:
        r = (uint8_t)((v << 1) | CARRY);
        c->cc &= (uint8_t)~(M6809_C | M6809_V);
        SET_IF(M6809_C, v & 0x80);
        SET_IF(M6809_V, (v ^ (v << 1)) & 0x80);
        nz8(c, r);
        return r;
    case RMW_DEC:
        r = (uint8_t)(v - 1);
        c->cc = (uint8_t)((c->cc & ~M6809_V) | (v == 0x80 ? M6809_V : 0));
        nz8(c, r);
        return r;
    case RMW_INC:
        r = (uint8_t)(v + 1);
        c->cc = (uint8_t)((c->cc & ~M6809_V) | (v == 0x7F ? M6809_V : 0));
        nz8(c, r);
        return r;
    case RMW_TST:
        return ld8(c, v);
    default:        /* CLR */
        c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_V | M6809_C)) | M6809_Z);
        return 0;
    }
}

static uint16_t rmw16(m6809_t *c, int op, uint16_t v)
{
    uint16_t r;
    switch (op) {
    case RMW_NEG:
        return sub16(c, 0, v, 0);
    case RMW_COM:
        r = (uint16_t)~v;
        c->cc = (uint8_t)((c->cc & ~M6809_V) | M6809_C);
        nz16(c, r);
        return r;
    case RMW_LSR:
        c->cc = (uint8_t)((c->cc & ~(M6809_C | M6809_N | M6809_Z)) | (v & 1));
        r = v >> 1;
        SET_IF(M6809_Z, !r);
        return r;
    case RMW_ROR:
        r = (uint16_t)((v >> 1) | (CARRY << 15));
        c->cc = (uint8_t)((c->cc & ~M6809_C) | (v & 1));
        nz16(c, r);
        return r;
    case RMW_ASR:
        r = (uint16_t)((v >> 1) | (v & 0x8000));
        c->cc = (uint8_t)((c->cc & ~M6809_C) | (v & 1));
        nz16(c, r);
        return r;
    case RMW_ASL:
        r = (uint16_t)(v << 1);
        c->cc &= (uint8_t)~(M6809_C | M6809_V);
        SET_IF(M6809_C, v & 0x8000);
        SET_IF(M6809_V, (v ^ (v << 1)) & 0x8000);
        nz16(c, r);
        return r;
    case RMW_ROL:
        r = (uint16_t)((v << 1) | CARRY);
        c->cc &= (uint8_t)~(M6809_C | M6809_V);
        SET_IF(M6809_C, v & 0x8000);
        SET_IF(M6809_V, (v ^ (v << 1)) & 0x8000);
        nz16(c, r);
        return r;
    case RMW_DEC:
        r = (uint16_t)(v - 1);
        c->cc = (uint8_t)((c->cc & ~M6809_V) | (v == 0x8000 ? M6809_V : 0));
        nz16(c, r);
        return r;
    case RMW_INC:
        r = (uint16_t)(v + 1);
        c->cc = (uint8_t)((c->cc & ~M6809_V) | (v == 0x7FFF ? M6809_V : 0));
        nz16(c, r);
        return r;
    case RMW_TST:
        return ld16(c, v);
    default:        /* CLR */
        c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_V | M6809_C)) | M6809_Z);
        return 0;
    }
}

/* ---- Addressing ---- */

static inline uint16_t direct(m6809_t *c)
{
    return (uint16_t)(c->dp << 8 | fetch(c));
}

static inline uint16_t *index_reg(m6809_t *c, uint8_t post)
{
    switch (post & 0x60) {
    case 0x00: return &c->x;
    case 0x20: return &c->y;
    case 0x40: return &c->u;
    default:   return &c->s;
    }
}

/* The indexed mode's address; its extra cycles to *cyc */
static uint16_t indexed(m6809_t *c, int *cyc)
{
    uint8_t post = fetch(c);
    uint16_t *r = index_reg(c, post);
    uint16_t ea;
    if (!(post & 0x80)) {
        /* n5,R */
        *cyc += 1;
        return (uint16_t)(*r + (int16_t)((int8_t)(post << 3) >> 3));
    }
    bool native = c->h6309 && NATIVE;
    switch (post & 0x0F) {
    case 0x0:               /* ,R+ (6309 with I: [,W] and friends) */
        if ((post & 0x10) && c->h6309) {
            switch (post & 0x60) {
            case 0x00:      /* [,W] */
                ea = c->w;
                *cyc += 3;
                break;
            case 0x20:      /* [n16,W] */
                ea = (uint16_t)(c->w + fetch16(c));
                *cyc += native ? 6 : 7;
                break;
            case 0x40:      /* [,W++] */
                ea = c->w;
                c->w += 2;
                *cyc += native ? 5 : 6;
                break;
            default:        /* [,--W] */
                c->w -= 2;
                ea = c->w;
                *cyc += native ? 5 : 6;
                break;
            }
            return rd16(c, ea);
        }
        ea = *r;
        *r += 1;
        *cyc += native ? 1 : 2;
        break;
    case 0x1:               /* ,R++ */
        ea = *r;
        *r += 2;
        *cyc += native ? 2 : 3;
        break;
    case 0x2:               /* ,-R */
        *r -= 1;
        ea = *r;
        *cyc += native ? 1 : 2;
        break;
    case 0x3:               /* ,--R */
        *r -= 2;
        ea = *r;
        *cyc += native ? 2 : 3;
        break;
    case 0x4:               /* ,R */
        ea = *r;
        break;
    case 0x5:               /* B,R */
        ea = (uint16_t)(*r + (int8_t)c->b);
        *cyc += 1;
        break;
    case 0x6:               /* A,R */
        ea = (uint16_t)(*r + (int8_t)c->a);
        *cyc += 1;
        break;
    case 0x7:               /* E,R (6309) */
        ea = (uint16_t)(*r + (c->h6309 ? (int8_t)c->e : 0));
        *cyc += 1;
        break;
    case 0x8:               /* n8,R */
        ea = (uint16_t)(*r + (int8_t)fetch(c));
        *cyc += 1;
        break;
    case 0x9:               /* n16,R */
        ea = (uint16_t)(*r + fetch16(c));
        *cyc += native ? 3 : 4;
        break;
    case 0xA:               /* F,R (6309) */
        ea = (uint16_t)(*r + (c->h6309 ? (int8_t)c->f : 0));
        *cyc += 1;
        break;
    case 0xB:               /* D,R */
        ea = (uint16_t)(*r + c->d);
        *cyc += native ? 2 : 4;
        break;
    case 0xC: {             /* n8,PCR */
        int8_t off = (int8_t)fetch(c);
        ea = (uint16_t)(c->pc + off);
        *cyc += 1;
        break;
    }
    case 0xD: {             /* n16,PCR */
        uint16_t off = fetch16(c);
        ea = (uint16_t)(c->pc + off);
        *cyc += native ? 3 : 5;
        break;
    }
    case 0xE:               /* W,R (6309) */
        ea = (uint16_t)(*r + (c->h6309 ? c->w : 0));
        *cyc += native ? 2 : 4;
        break;
    default:                /* $xF */
        if (post & 0x10) {
            /* [n16]: its cycles include the indirection */
            ea = fetch16(c);
            *cyc += native ? 4 : 5;
            return rd16(c, ea);
        }
        if (!c->h6309) {
            ea = fetch16(c);    /* (undefined on the 6809) */
            *cyc += 5;
            break;
        }
        switch (post & 0x60) {
        case 0x00:          /* ,W */
            ea = c->w;
            break;
        case 0x20:          /* n16,W */
            ea = (uint16_t)(c->w + fetch16(c));
            *cyc += native ? 2 : 3;
            break;
        case 0x40:          /* ,W++ */
            ea = c->w;
            c->w += 2;
            *cyc += native ? 2 : 3;
            break;
        default:            /* ,--W */
            c->w -= 2;
            ea = c->w;
            *cyc += native ? 2 : 3;
            break;
        }
        return ea;
    }
    if (post & 0x10) {
        /* indirect */
        *cyc += 3;
        ea = rd16(c, ea);
    }
    return ea;
}

/* The operand address of columns 1-3 ($9x/$Ax/$Bx, $Dx/$Ex/$Fx): direct,
 * indexed, extended
 */
static inline uint16_t operand_ea(m6809_t *c, uint8_t op, int *cyc)
{
    switch (op & 0x30) {
    case 0x10: return direct(c);
    case 0x20: return indexed(c, cyc);
    default:   return fetch16(c);
    }
}

/* ---- Interrupts ---- */

static void push_all(m6809_t *c)
{
    push16(c, &c->s, c->pc);
    push16(c, &c->s, c->u);
    push16(c, &c->s, c->y);
    push16(c, &c->s, c->x);
    push8(c, &c->s, c->dp);
    if (c->h6309 && NATIVE) {
        push8(c, &c->s, c->f);
        push8(c, &c->s, c->e);
    }
    push8(c, &c->s, c->b);
    push8(c, &c->s, c->a);
    push8(c, &c->s, c->cc);
}

/* Stack everything (E set) and go to the vector */
static int interrupt_all(m6809_t *c, uint16_t vector, uint8_t mask)
{
    c->cc |= M6809_E;
    push_all(c);
    c->cc |= mask;
    c->pc = rd16(c, vector);
    return CYC(19, 21);
}

static int take_interrupt(m6809_t *c)
{
    if (c->nmi_pending) {
        c->nmi_pending = false;
        if (c->nmi_armed) {
            if (c->wait == M6809_CWAI) {
                c->wait = M6809_RUNNING;
                c->cc |= M6809_I | M6809_F;
                c->pc = rd16(c, VEC_NMI);
                return 7;
            }
            c->wait = M6809_RUNNING;
            return interrupt_all(c, VEC_NMI, M6809_I | M6809_F);
        }
    }
    if (c->firq && !(c->cc & M6809_F)) {
        if (c->wait == M6809_CWAI) {
            c->wait = M6809_RUNNING;
            c->cc |= M6809_I | M6809_F;
            c->pc = rd16(c, VEC_FIRQ);
            return 7;
        }
        c->wait = M6809_RUNNING;
        if (c->h6309 && (c->md & M6309_FM)) {
            return interrupt_all(c, VEC_FIRQ, M6809_I | M6809_F);
        }
        c->cc &= (uint8_t)~M6809_E;
        push16(c, &c->s, c->pc);
        push8(c, &c->s, c->cc);
        c->cc |= M6809_I | M6809_F;
        c->pc = rd16(c, VEC_FIRQ);
        return 10;
    }
    if (c->irq && !(c->cc & M6809_I)) {
        if (c->wait == M6809_CWAI) {
            c->wait = M6809_RUNNING;
            c->cc |= M6809_I;
            c->pc = rd16(c, VEC_IRQ);
            return 7;
        }
        c->wait = M6809_RUNNING;
        return interrupt_all(c, VEC_IRQ, M6809_I);
    }
    return 0;
}

/* The 6309's trap: illegal opcode or division by zero */
static int trap(m6809_t *c, uint8_t why)
{
    c->md |= why;
    return interrupt_all(c, VEC_TRAP, M6809_I | M6809_F);
}

/* ---- Register numbers (TFR, EXG, the 6309's register operations) ---- */

static uint16_t reg_read(m6809_t *c, int r, bool for_exg)
{
    switch (r & 0xF) {
    case 0x0: return c->d;
    case 0x1: return c->x;
    case 0x2: return c->y;
    case 0x3: return c->u;
    case 0x4: return c->s;
    case 0x5: return c->pc;
    case 0x6: return c->h6309 ? c->w : 0xFFFF;
    case 0x7: return c->h6309 ? c->v : 0xFFFF;
    case 0x8: return c->h6309 ? (uint16_t)(c->a << 8 | c->a) : (uint16_t)(0xFF00 | c->a);
    case 0x9: return c->h6309 ? (uint16_t)(c->b << 8 | c->b) : (uint16_t)(0xFF00 | c->b);
    case 0xA: return (c->h6309 || !for_exg) ? (uint16_t)(c->cc << 8 | c->cc) : (uint16_t)(0xFF00 | c->cc);
    case 0xB: return (c->h6309 || !for_exg) ? (uint16_t)(c->dp << 8 | c->dp) : (uint16_t)(0xFF00 | c->dp);
    case 0xC: case 0xD: return c->h6309 ? 0 : 0xFFFF;
    case 0xE: return c->h6309 ? (uint16_t)(c->e << 8 | c->e) : 0xFFFF;
    default:  return c->h6309 ? (uint16_t)(c->f << 8 | c->f) : 0xFFFF;
    }
}

static void reg_write(m6809_t *c, int r, uint16_t v)
{
    switch (r & 0xF) {
    case 0x0: c->d = v; break;
    case 0x1: c->x = v; break;
    case 0x2: c->y = v; break;
    case 0x3: c->u = v; break;
    case 0x4: c->s = v; c->nmi_armed = true; break;
    case 0x5: c->pc = v; break;
    case 0x6: if (c->h6309) c->w = v; break;
    case 0x7: if (c->h6309) c->v = v; break;
    case 0x8: c->a = (uint8_t)(c->h6309 ? v >> 8 : v); break;
    case 0x9: c->b = (uint8_t)v; break;
    case 0xA: c->cc = (uint8_t)v; break;
    case 0xB: c->dp = (uint8_t)(c->h6309 ? v >> 8 : v); break;
    case 0xE: if (c->h6309) c->e = (uint8_t)(v >> 8); break;
    case 0xF: if (c->h6309) c->f = (uint8_t)v; break;
    default: break;
    }
}

static inline bool reg_is16(int r)
{
    return !(r & 0x8);
}

static uint8_t *reg8(m6809_t *c, int r, uint8_t *scratch)
{
    switch (r & 0xF) {
    case 0x8: return &c->a;
    case 0x9: return &c->b;
    case 0xA: return &c->cc;
    case 0xB: return &c->dp;
    case 0xE: return &c->e;
    case 0xF: return &c->f;
    default:  *scratch = 0; return scratch;
    }
}

/* ---- Branches ---- */

static inline bool branch_cond(const m6809_t *c, uint8_t op)
{
    uint8_t cc = c->cc;
    bool n = cc & M6809_N, z = cc & M6809_Z, v = cc & M6809_V, cy = cc & M6809_C;
    bool r;
    switch ((op >> 1) & 7) {
    case 0: r = true; break;                    /* BRA / BRN */
    case 1: r = !(cy || z); break;              /* BHI / BLS */
    case 2: r = !cy; break;                     /* BCC / BCS */
    case 3: r = !z; break;                      /* BNE / BEQ */
    case 4: r = !v; break;                      /* BVC / BVS */
    case 5: r = !n; break;                      /* BPL / BMI */
    case 6: r = n == v; break;                  /* BGE / BLT */
    default: r = !z && n == v; break;           /* BGT / BLE */
    }
    return (op & 1) ? !r : r;
}

/* ---- Push and pull ---- */

static int push_regs(m6809_t *c, uint16_t *sp, uint16_t other, uint8_t mask)
{
    int n = 0;
    if (mask & 0x80) { push16(c, sp, c->pc); n += 2; }
    if (mask & 0x40) { push16(c, sp, other); n += 2; }
    if (mask & 0x20) { push16(c, sp, c->y); n += 2; }
    if (mask & 0x10) { push16(c, sp, c->x); n += 2; }
    if (mask & 0x08) { push8(c, sp, c->dp); n++; }
    if (mask & 0x04) { push8(c, sp, c->b); n++; }
    if (mask & 0x02) { push8(c, sp, c->a); n++; }
    if (mask & 0x01) { push8(c, sp, c->cc); n++; }
    return n;
}

/* other: the other stack pointer (U for PULS, S for PULU) */
static int pull_regs(m6809_t *c, uint16_t *sp, uint16_t *other, uint8_t mask)
{
    int n = 0;
    if (mask & 0x01) { c->cc = pull8(c, sp); n++; }
    if (mask & 0x02) { c->a = pull8(c, sp); n++; }
    if (mask & 0x04) { c->b = pull8(c, sp); n++; }
    if (mask & 0x08) { c->dp = pull8(c, sp); n++; }
    if (mask & 0x10) { c->x = pull16(c, sp); n += 2; }
    if (mask & 0x20) { c->y = pull16(c, sp); n += 2; }
    if (mask & 0x40) { *other = pull16(c, sp); n += 2; }
    if (mask & 0x80) { c->pc = pull16(c, sp); n += 2; }
    return n;
}

/* ---- The 6309's division ---- */

static int divd(m6809_t *c, uint8_t divisor)
{
    if (divisor == 0) {
        return trap(c, M6309_DZ);
    }
    uint16_t dividend = c->d;
    bool nsign = dividend & 0x8000, vsign = divisor & 0x80;
    int cyc = CYC(25, 24);
    if (nsign) {
        dividend = (uint16_t)-dividend;
        c->d = dividend;
        cyc++;
    }
    uint8_t div = vsign ? (uint8_t)-divisor : divisor;
    if (vsign) {
        cyc++;
    }
    uint16_t quotient = dividend / div;
    uint8_t remainder = (uint8_t)(dividend % div);
    c->cc &= (uint8_t)~(M6809_N | M6809_Z | M6809_V | M6809_C);
    if (quotient >> 8) {
        /* range overflow */
        c->cc |= M6809_V;
        SET_IF(M6809_N, nsign);
        return cyc - 13;
    }
    if (nsign) {
        remainder = (uint8_t)-remainder;
    }
    if (nsign != vsign) {
        uint16_t neg = (uint16_t)-quotient;
        if ((neg & 0x80) && !(quotient & 0x80)) {
            quotient = neg;
        }
    }
    c->a = remainder;
    c->b = (uint8_t)quotient;
    SET_IF(M6809_C, c->b & 1);
    if ((((quotient >> 15) ^ (c->b >> 7)) & 1) == 0) {
        SET_IF(M6809_Z, c->b == 0);
        SET_IF(M6809_N, c->b & 0x80);
    } else {
        c->cc |= M6809_N | M6809_V;
        cyc--;
    }
    return cyc;
}

static int divq(m6809_t *c, uint16_t divisor)
{
    if (divisor == 0) {
        return trap(c, M6309_DZ);
    }
    uint32_t dividend = (uint32_t)c->d << 16 | c->w;
    bool nsign = dividend & 0x80000000u, vsign = divisor & 0x8000;
    int cyc = CYC(36, 35);
    if (nsign) {
        dividend = (uint32_t)-dividend;
        c->d = (uint16_t)(dividend >> 16);
        c->w = (uint16_t)dividend;
        cyc++;
    }
    uint16_t div = vsign ? (uint16_t)-divisor : divisor;
    if (vsign) {
        cyc++;
    }
    uint32_t quotient = dividend / div;
    uint16_t remainder = (uint16_t)(dividend % div);
    c->cc &= (uint8_t)~(M6809_N | M6809_Z | M6809_V | M6809_C);
    if (quotient >> 16) {
        c->cc |= M6809_V;
        SET_IF(M6809_N, nsign);
        return cyc - 13;
    }
    if (nsign) {
        remainder = (uint16_t)-remainder;
    }
    if (nsign != vsign) {
        uint32_t neg = (uint32_t)-quotient;
        if ((neg & 0x8000) && !(quotient & 0x8000)) {
            quotient = neg;
        }
    }
    c->d = remainder;
    c->w = (uint16_t)quotient;
    SET_IF(M6809_C, c->w & 1);
    if ((((quotient >> 31) ^ (c->w >> 15)) & 1) == 0) {
        SET_IF(M6809_Z, c->w == 0);
        SET_IF(M6809_N, c->w & 0x8000);
    } else {
        c->cc |= M6809_N | M6809_V;
        cyc--;
    }
    return cyc;
}

/* ---- The 6309's register-register operations ($1030-$1037) ---- */

static int register_op(m6809_t *c, uint8_t op)
{
    uint8_t post = fetch(c);
    int src = post >> 4, dst = post & 0xF;
    /* Mixed sizes: the 8-bit one is promoted (A/B: D, E/F: W, CC/DP/0: 0) */
    bool wide = reg_is16(src) || reg_is16(dst);
    if (wide) {
        uint16_t a, b;
        switch (dst) {
        case 0x8: case 0x9: a = c->d; break;
        case 0xE: case 0xF: a = c->w; break;
        case 0xA: case 0xB: case 0xC: case 0xD: a = 0; break;
        default: a = reg_read(c, dst, false); break;
        }
        switch (src) {
        case 0x8: case 0x9: b = c->d; break;
        case 0xE: case 0xF: b = c->w; break;
        case 0xA: case 0xB: case 0xC: case 0xD: b = 0; break;
        default: b = reg_read(c, src, false); break;
        }
        uint16_t r;
        bool store = true;
        switch (op & 7) {
        case 0: r = add16(c, a, b, 0); break;
        case 1: r = add16(c, a, b, CARRY); break;
        case 2: r = sub16(c, a, b, 0); break;
        case 3: r = sub16(c, a, b, CARRY); break;
        case 4: r = ld16(c, a & b); break;
        case 5: r = ld16(c, a | b); break;
        case 6: r = ld16(c, a ^ b); break;
        default: sub16(c, a, b, 0); r = a; store = false; break;
        }
        if (store) {
            switch (dst) {
            case 0x8: case 0x9: c->d = r; break;
            case 0xE: case 0xF: c->w = r; break;
            case 0xA: case 0xB: case 0xC: case 0xD: break;
            default: reg_write(c, dst, r); break;
            }
        }
    } else {
        uint8_t scratch_a, scratch_b;
        uint8_t *pa = reg8(c, dst, &scratch_a);
        uint8_t b = *reg8(c, src, &scratch_b);
        uint8_t a = *pa, r;
        switch (op & 7) {
        case 0: r = add8(c, a, b, 0); break;
        case 1: r = add8(c, a, b, CARRY); break;
        case 2: r = sub8(c, a, b, 0); break;
        case 3: r = sub8(c, a, b, CARRY); break;
        case 4: r = ld8(c, a & b); break;
        case 5: r = ld8(c, a | b); break;
        case 6: r = ld8(c, a ^ b); break;
        default: sub8(c, a, b, 0); r = a; break;
        }
        if ((op & 7) != 7) {
            *pa = r;
        }
    }
    return CYC(4, 4) + ((op & 7) == 7 ? 0 : 0);
}

/* ---- TFM ---- */

static int tfm(m6809_t *c, uint8_t op)
{
    uint8_t post = fetch(c);
    int src = post >> 4, dst = post & 0xF;
    if (src > 4 || dst > 4) {
        return trap(c, M6309_IL);
    }
    uint16_t *regs[5] = { &c->d, &c->x, &c->y, &c->u, &c->s };
    uint16_t *ps = regs[src], *pd = regs[dst];
    int cyc = 6;
    while (c->w) {
        /* Interruptible between bytes: back to the TFM, which goes on */
        if (c->cycles + (uint64_t)cyc >= c->until ||
            c->nmi_pending || (c->firq && !(c->cc & M6809_F)) || (c->irq && !(c->cc & M6809_I))) {
            c->pc -= 3;
            return cyc;
        }
        wr8(c, *pd, rd8(c, *ps));
        switch (op & 3) {
        case 0: (*ps)++; (*pd)++; break;    /* r0+,r1+ */
        case 1: (*ps)--; (*pd)--; break;    /* r0-,r1- */
        case 2: (*ps)++; break;             /* r0+,r1 */
        default: (*pd)++; break;            /* r0,r1+ */
        }
        c->w--;
        cyc += 3;
    }
    return cyc;
}

/* ---- The 6309's bit operations on direct memory ($1130-$1137) ---- */

static int bit_op(m6809_t *c, uint8_t op)
{
    uint8_t post = fetch(c);
    uint16_t ea = direct(c);
    uint8_t mem = rd8(c, ea);
    uint8_t scratch = 0;
    uint8_t *reg = (post & 0xC0) == 0x00 ? &c->cc : (post & 0xC0) == 0x40 ? &c->a :
                   (post & 0xC0) == 0x80 ? &c->b : &scratch;
    int sbit = (post >> 3) & 7, dbit = post & 7;
    bool src = (mem >> sbit) & 1, dst = (*reg >> dbit) & 1, r;
    switch (op & 7) {
    case 0: r = dst && src; break;          /* BAND */
    case 1: r = dst && !src; break;         /* BIAND */
    case 2: r = dst || src; break;          /* BOR */
    case 3: r = dst || !src; break;         /* BIOR */
    case 4: r = dst != src; break;          /* BEOR */
    case 5: r = dst == src; break;          /* BIEOR */
    case 6: r = src; break;                 /* LDBT */
    default: {                              /* STBT: memory bit dbit from register bit sbit */
        bool b = (*reg >> sbit) & 1;
        mem = b ? (uint8_t)(mem | 1 << dbit) : (uint8_t)(mem & ~(1 << dbit));
        wr8(c, ea, mem);
        return CYC(8, 7);
    }
    }
    *reg = r ? (uint8_t)(*reg | 1 << dbit) : (uint8_t)(*reg & ~(1 << dbit));
    return CYC(7, 6);
}

/* ---- Page 0 ---- */

static int illegal(m6809_t *c)
{
    if (c->h6309) {
        return trap(c, M6309_IL);
    }
    return 2;       /* the 6809's unassigned opcodes: as NOPs */
}

/* Rows $8x-$Fx: the accumulator and 16-bit loads/stores/arithmetic */
static int column_op(m6809_t *c, uint8_t op)
{
    int cyc = 0;
    int mode = (op >> 4) & 3;               /* 0 immediate, 1 direct, 2 indexed, 3 extended */
    bool regb = op & 0x40;
    uint8_t *acc = regb ? &c->b : &c->a;
    uint16_t ea = 0;
    uint8_t row = op & 0x0F;

    /* Base cycles: 8-bit ops 2/4/4+/5 (native 2/3/4+/4) */
    static const uint8_t base8[4] = { 2, 4, 4, 5 }, base8n[4] = { 2, 3, 4, 4 };
    static const uint8_t base16[4] = { 4, 6, 6, 7 }, base16n[4] = { 3, 4, 5, 5 };
    static const uint8_t ldst16[4] = { 3, 5, 5, 6 }, ldst16n[4] = { 3, 4, 5, 5 };

    switch (row) {
    case 0x3:                               /* SUBD / ADDD */
    case 0xC:                               /* CMPX / LDD */
    case 0xE:                               /* LDX / LDU */
    case 0xF:                               /* STX / STU */
    case 0xD:                               /* BSR JSR / LDQ# STD */
        break;
    default:
        /* 8-bit */
        if (mode == 0) {
            if (row == 0x7) {               /* STA # (undefined): the 6309 traps */
                if (c->h6309) {
                    return illegal(c);
                }
                c->pc++;
                return 2;
            }
            uint8_t v = fetch(c);
            cyc = 2;
            goto alu8_imm;
        alu8_imm:;
            uint8_t a = *acc;
            switch (row) {
            case 0x0: *acc = sub8(c, a, v, 0); break;
            case 0x1: sub8(c, a, v, 0); break;
            case 0x2: *acc = sub8(c, a, v, CARRY); break;
            case 0x4: *acc = ld8(c, a & v); break;
            case 0x5: ld8(c, a & v); break;
            case 0x6: *acc = ld8(c, v); break;
            case 0x8: *acc = ld8(c, a ^ v); break;
            case 0x9: *acc = add8(c, a, v, CARRY); break;
            case 0xA: *acc = ld8(c, a | v); break;
            case 0xB: *acc = add8(c, a, v, 0); break;
            }
            return cyc;
        }
        ea = operand_ea(c, op, &cyc);
        cyc += CYC(base8[mode], base8n[mode]);
        if (row == 0x7) {                   /* STA / STB */
            wr8(c, ea, ld8(c, *acc));
            return cyc;
        }
        {
            uint8_t v = rd8(c, ea);
            uint8_t a = *acc;
            switch (row) {
            case 0x0: *acc = sub8(c, a, v, 0); break;
            case 0x1: sub8(c, a, v, 0); break;
            case 0x2: *acc = sub8(c, a, v, CARRY); break;
            case 0x4: *acc = ld8(c, a & v); break;
            case 0x5: ld8(c, a & v); break;
            case 0x6: *acc = ld8(c, v); break;
            case 0x8: *acc = ld8(c, a ^ v); break;
            case 0x9: *acc = add8(c, a, v, CARRY); break;
            case 0xA: *acc = ld8(c, a | v); break;
            case 0xB: *acc = add8(c, a, v, 0); break;
            }
        }
        return cyc;
    }

    /* 16-bit rows */
    uint16_t v = 0;
    if (row == 0xD && !regb) {
        /* BSR / JSR */
        if (mode == 0) {
            int8_t off = (int8_t)fetch(c);
            push16(c, &c->s, c->pc);
            c->pc = (uint16_t)(c->pc + off);
            return CYC(7, 6);
        }
        ea = operand_ea(c, op, &cyc);
        push16(c, &c->s, c->pc);
        c->pc = ea;
        static const uint8_t jsr[4] = { 0, 7, 7, 8 }, jsrn[4] = { 0, 6, 6, 7 };
        return cyc + CYC(jsr[mode], jsrn[mode]);
    }
    if (row == 0xF || (row == 0xD && regb)) {
        /* STX STU STD (and the 6309's LDQ #) */
        if (mode == 0) {
            if (row == 0xD && c->h6309) {
                /* LDQ # (5 bytes) */
                c->d = fetch16(c);
                c->w = fetch16(c);
                CLR(M6809_V);
                c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_Z)) | ((c->a & 0x80) ? M6809_N : 0) |
                                  ((c->d | c->w) ? 0 : M6809_Z));
                return 5;
            }
            if (c->h6309) {
                return illegal(c);
            }
            c->pc += 2;                     /* STX # (undefined) */
            return 3;
        }
        ea = operand_ea(c, op, &cyc);
        uint16_t r = row == 0xD ? c->d : regb ? c->u : c->x;
        wr16(c, ea, ld16(c, r));
        return cyc + CYC(ldst16[mode], ldst16n[mode]);
    }
    if (mode == 0) {
        v = fetch16(c);
    } else {
        ea = operand_ea(c, op, &cyc);
        v = rd16(c, ea);
    }
    switch (row) {
    case 0x3:
        if (regb) {
            c->d = add16(c, c->d, v, 0);    /* ADDD */
        } else {
            c->d = sub16(c, c->d, v, 0);    /* SUBD */
        }
        return cyc + CYC(base16[mode], base16n[mode]);
    case 0xC:
        if (regb) {
            c->d = ld16(c, v);              /* LDD */
            return cyc + CYC(ldst16[mode], ldst16n[mode]);
        }
        sub16(c, c->x, v, 0);               /* CMPX */
        return cyc + CYC(base16[mode], base16n[mode]);
    default:                                /* 0xE */
        if (regb) {
            c->u = ld16(c, v);              /* LDU */
        } else {
            c->x = ld16(c, v);              /* LDX */
        }
        return cyc + CYC(ldst16[mode], ldst16n[mode]);
    }
}

/* Rows $0x, $6x, $7x: memory read-modify-write, JMP, and the 6309's
 * OIM/AIM/EIM/TIM at $x1/$x2/$x5/$xB
 */
static int memory_rmw(m6809_t *c, uint8_t op)
{
    int cyc = 0;
    uint8_t lo = op & 0x0F;
    int mode = op >= 0x70 ? 3 : op >= 0x60 ? 2 : 1;
    if (c->h6309 && (lo == 0x1 || lo == 0x2 || lo == 0x5 || lo == 0xB)) {
        uint8_t im = fetch(c);
        uint16_t ea = mode == 1 ? direct(c) : mode == 2 ? indexed(c, &cyc) : fetch16(c);
        uint8_t v = rd8(c, ea);
        switch (lo) {
        case 0x1: v |= im; break;
        case 0x2: v &= im; break;
        case 0x5: v ^= im; break;
        default: ld8(c, v & im); return cyc + (mode == 1 ? 6 : mode == 2 ? 7 : 7);
        }
        wr8(c, ea, ld8(c, v));
        static const uint8_t oim[4] = { 0, 6, 7, 7 };
        return cyc + oim[mode];
    }
    uint16_t ea = mode == 1 ? direct(c) : mode == 2 ? indexed(c, &cyc) : fetch16(c);
    if (lo == 0xE) {                        /* JMP */
        c->pc = ea;
        static const uint8_t jmp[4] = { 0, 3, 3, 4 }, jmpn[4] = { 0, 2, 3, 3 };
        return cyc + CYC(jmp[mode], jmpn[mode]);
    }
    int rop = rmw_op[lo];
    uint8_t v = rd8(c, ea);
    uint8_t r = rmw8(c, rop, v);
    static const uint8_t base[4] = { 0, 6, 6, 7 }, basen[4] = { 0, 5, 6, 6 };
    static const uint8_t tst[4] = { 0, 6, 6, 7 }, tstn[4] = { 0, 4, 5, 5 };
    if (rop == RMW_TST) {
        return cyc + CYC(tst[mode], tstn[mode]);
    }
    wr8(c, ea, r);
    return cyc + CYC(base[mode], basen[mode]);
}

static int page2(m6809_t *c);
static int page3(m6809_t *c);

static int exec_page0(m6809_t *c, uint8_t op)
{
    int cyc = 0;
    switch (op) {
    case 0x10: return page2(c);
    case 0x11: return page3(c);
    case 0x12: return CYC(2, 1);                                    /* NOP */
    case 0x13: c->wait = M6809_SYNC; return CYC(4, 3);              /* SYNC */
    case 0x14:
        if (c->h6309) {                                             /* SEXW */
            c->d = (c->w & 0x8000) ? 0xFFFF : 0;
            c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_Z)) | ((c->w & 0x8000) ? M6809_N : 0) |
                              ((c->d | c->w) ? 0 : M6809_Z));
            return 4;
        }
        return illegal(c);
    case 0x16: {                                                    /* LBRA */
        uint16_t off = fetch16(c);
        c->pc += off;
        return CYC(5, 4);
    }
    case 0x17: {                                                    /* LBSR */
        uint16_t off = fetch16(c);
        push16(c, &c->s, c->pc);
        c->pc += off;
        return CYC(9, 7);
    }
    case 0x19: {                                                    /* DAA */
        uint8_t a = c->a, cf = 0;
        uint8_t msn = a & 0xF0, lsn = a & 0x0F;
        if (lsn > 0x09 || (c->cc & M6809_H)) cf |= 0x06;
        if (msn > 0x80 && lsn > 0x09) cf |= 0x60;
        if (msn > 0x90 || (c->cc & M6809_C)) cf |= 0x60;
        unsigned r = a + cf;
        c->cc &= (uint8_t)~(M6809_V | M6809_N | M6809_Z);
        SET_IF(M6809_C, r & 0x100);
        c->a = (uint8_t)r;
        nz8(c, c->a);
        return CYC(2, 1);
    }
    case 0x1A: c->cc |= fetch(c); return CYC(3, 2);                 /* ORCC */
    case 0x1C: c->cc &= fetch(c); return 3;                         /* ANDCC */
    case 0x1D:                                                      /* SEX */
        c->a = (c->b & 0x80) ? 0xFF : 0;
        nz16(c, c->d);
        return CYC(2, 1);
    case 0x1E: {                                                    /* EXG */
        uint8_t post = fetch(c);
        int r1 = post >> 4, r2 = post & 0xF;
        uint16_t v1 = reg_read(c, r1, true), v2 = reg_read(c, r2, true);
        if (!c->h6309 && reg_is16(r1) != reg_is16(r2)) {
            /* the 6809: 8-bit to 16-bit gets $FF in the high byte */
        }
        reg_write(c, r1, v2);
        reg_write(c, r2, v1);
        return CYC(8, 5);
    }
    case 0x1F: {                                                    /* TFR */
        uint8_t post = fetch(c);
        reg_write(c, post & 0xF, reg_read(c, post >> 4, false));
        return CYC(6, 4);
    }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        int8_t off = (int8_t)fetch(c);
        if (branch_cond(c, op)) {
            c->pc = (uint16_t)(c->pc + off);
        }
        return 3;
    }
    case 0x30: c->x = indexed(c, &cyc); SET_IF(M6809_Z, !c->x); if (c->x) CLR(M6809_Z); return cyc + 4;  /* LEAX */
    case 0x31: c->y = indexed(c, &cyc); SET_IF(M6809_Z, !c->y); if (c->y) CLR(M6809_Z); return cyc + 4;  /* LEAY */
    case 0x32: c->s = indexed(c, &cyc); c->nmi_armed = true; return cyc + 4;                             /* LEAS */
    case 0x33: c->u = indexed(c, &cyc); return cyc + 4;                                                  /* LEAU */
    case 0x34: { uint8_t m = fetch(c); return CYC(5, 4) + push_regs(c, &c->s, c->u, m); }             /* PSHS */
    case 0x35: { uint8_t m = fetch(c); return CYC(5, 4) + pull_regs(c, &c->s, &c->u, m); }            /* PULS */
    case 0x36: { uint8_t m = fetch(c); return CYC(5, 4) + push_regs(c, &c->u, c->s, m); }             /* PSHU */
    case 0x37: { uint8_t m = fetch(c); return CYC(5, 4) + pull_regs(c, &c->u, &c->s, m); }            /* PULU */
    case 0x39: c->pc = pull16(c, &c->s); return CYC(5, 4);                                             /* RTS */
    case 0x3A: c->x += c->b; return CYC(3, 1);                                                        /* ABX */
    case 0x3B:                                                                                         /* RTI */
        c->cc = pull8(c, &c->s);
        if (c->cc & M6809_E) {
            c->a = pull8(c, &c->s);
            c->b = pull8(c, &c->s);
            if (c->h6309 && NATIVE) {
                c->e = pull8(c, &c->s);
                c->f = pull8(c, &c->s);
            }
            c->dp = pull8(c, &c->s);
            c->x = pull16(c, &c->s);
            c->y = pull16(c, &c->s);
            c->u = pull16(c, &c->s);
            c->pc = pull16(c, &c->s);
            return CYC(15, 17);
        }
        c->pc = pull16(c, &c->s);
        return 6;
    case 0x3C:                                                                                         /* CWAI */
        c->cc &= fetch(c);
        c->cc |= M6809_E;
        push_all(c);
        c->wait = M6809_CWAI;
        return CYC(20, 22);
    case 0x3D: {                                                                                       /* MUL */
        c->d = (uint16_t)(c->a * c->b);
        c->cc &= (uint8_t)~(M6809_Z | M6809_C);
        SET_IF(M6809_Z, !c->d);
        SET_IF(M6809_C, c->b & 0x80);
        return CYC(11, 10);
    }
    case 0x3E:
        if (c->h6309) {
            return illegal(c);
        }
        return interrupt_all(c, VEC_RESET, M6809_I | M6809_F);    /* the 6809's undocumented RESET */
    case 0x3F: return interrupt_all(c, VEC_SWI, M6809_I | M6809_F);                                  /* SWI */
    default:
        break;
    }
    if (op < 0x10 || (op >= 0x60 && op < 0x80)) {
        return memory_rmw(c, op);
    }
    if (op >= 0x40 && op < 0x60) {
        /* Inherent A ($4x) and B ($5x) */
        int rop = rmw_op[op & 0x0F];
        if (rop < 0 || (c->h6309 && ((op & 0x0F) == 0x1 || (op & 0x0F) == 0x2 || (op & 0x0F) == 0x5 ||
                                     (op & 0x0F) == 0xB))) {
            return illegal(c);
        }
        uint8_t *r = (op & 0x10) ? &c->b : &c->a;
        *r = rmw8(c, rop, *r);
        return CYC(2, 1);
    }
    if (op >= 0x80) {
        return column_op(c, op);
    }
    return illegal(c);      /* $15 $18 $1B $38 */
}

/* ---- Page 2 ($10) ---- */

static int page2(m6809_t *c)
{
    int cyc = 0;
    uint8_t op = fetch(c);
    if (op >= 0x21 && op <= 0x2F) {
        /* long branches */
        uint16_t off = fetch16(c);
        if (branch_cond(c, op)) {
            c->pc += off;
            return CYC(6, 5);
        }
        return CYC(5, 5);
    }
    if (op == 0x3F) {
        return interrupt_all(c, VEC_SWI2, 0) + CYC(1, 1);           /* SWI2 */
    }
    int mode = (op >> 4) & 3;
    uint8_t row = op & 0x0F;
    if (op >= 0x80) {
        static const uint8_t cmp16[4] = { 5, 7, 7, 8 }, cmp16n[4] = { 4, 5, 6, 6 };
        static const uint8_t ld16t[4] = { 4, 6, 6, 7 }, ld16n[4] = { 4, 5, 6, 6 };
        bool regb = op & 0x40;
        if (!regb) {
            switch (row) {
            case 0x3: case 0xC: {                                   /* CMPD, CMPY */
                uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
                sub16(c, row == 0x3 ? c->d : c->y, v, 0);
                return cyc + CYC(cmp16[mode], cmp16n[mode]);
            }
            case 0xE: {                                             /* LDY */
                uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
                c->y = ld16(c, v);
                return cyc + CYC(ld16t[mode], ld16n[mode]);
            }
            case 0xF:                                               /* STY */
                if (mode == 0) {
                    break;
                }
                wr16(c, operand_ea(c, op, &cyc), ld16(c, c->y));
                return cyc + CYC(ld16t[mode], ld16n[mode]);
            default:
                break;
            }
        } else {
            switch (row) {
            case 0xE: {                                             /* LDS */
                uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
                c->s = ld16(c, v);
                c->nmi_armed = true;
                return cyc + CYC(ld16t[mode], ld16n[mode]);
            }
            case 0xF:                                               /* STS */
                if (mode == 0) {
                    break;
                }
                wr16(c, operand_ea(c, op, &cyc), ld16(c, c->s));
                return cyc + CYC(ld16t[mode], ld16n[mode]);
            default:
                break;
            }
        }
        if (c->h6309) {
            /* The 6309's W and D operations: $1080-$10BF on W/D, $10DC-$10FD LDQ/STQ */
            static const uint8_t w16[4] = { 5, 7, 7, 8 }, w16n[4] = { 4, 5, 6, 6 };
            if (!regb) {
                switch (row) {
                case 0x0: case 0x1: case 0x2: case 0x4: case 0x5: case 0x6: case 0x8:
                case 0x9: case 0xA: case 0xB: {
                    uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
                    switch (row) {
                    case 0x0: c->w = sub16(c, c->w, v, 0); break;           /* SUBW */
                    case 0x1: sub16(c, c->w, v, 0); break;                  /* CMPW */
                    case 0x2: c->d = sub16(c, c->d, v, CARRY); break;       /* SBCD */
                    case 0x4: c->d = ld16(c, c->d & v); break;              /* ANDD */
                    case 0x5: ld16(c, c->d & v); break;                     /* BITD */
                    case 0x6: c->w = ld16(c, v); return cyc + CYC(ld16t[mode], ld16n[mode]);  /* LDW */
                    case 0x8: c->d = ld16(c, c->d ^ v); break;              /* EORD */
                    case 0x9: c->d = add16(c, c->d, v, CARRY); break;       /* ADCD */
                    case 0xA: c->d = ld16(c, c->d | v); break;              /* ORD */
                    default: c->w = add16(c, c->w, v, 0); break;            /* ADDW */
                    }
                    return cyc + CYC(w16[mode], w16n[mode]);
                }
                case 0x7:                                                    /* STW */
                    if (mode == 0) {
                        break;
                    }
                    wr16(c, operand_ea(c, op, &cyc), ld16(c, c->w));
                    return cyc + CYC(ld16t[mode], ld16n[mode]);
                default:
                    break;
                }
            } else if (mode && (row == 0xC || row == 0xD)) {
                uint16_t ea = operand_ea(c, op, &cyc);
                static const uint8_t q[4] = { 0, 8, 8, 9 }, qn[4] = { 0, 7, 8, 8 };
                if (row == 0xC) {                                           /* LDQ */
                    c->d = rd16(c, ea);
                    c->w = rd16(c, (uint16_t)(ea + 2));
                } else {                                                    /* STQ */
                    wr16(c, ea, c->d);
                    wr16(c, (uint16_t)(ea + 2), c->w);
                }
                CLR(M6809_V);
                c->cc = (uint8_t)((c->cc & ~(M6809_N | M6809_Z)) | ((c->a & 0x80) ? M6809_N : 0) |
                                  ((c->d | c->w) ? 0 : M6809_Z));
                return cyc + CYC(q[mode], qn[mode]);
            }
        }
    } else if (c->h6309) {
        switch (op) {
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
            return register_op(c, op);
        case 0x38: push16(c, &c->s, c->w); return CYC(6, 6);           /* PSHSW */
        case 0x39: c->w = pull16(c, &c->s); return CYC(6, 6);          /* PULSW */
        case 0x3A: push16(c, &c->u, c->w); return CYC(6, 6);           /* PSHUW */
        case 0x3B: c->w = pull16(c, &c->u); return CYC(6, 6);          /* PULUW */
        default:
            break;
        }
        if (op >= 0x40 && op < 0x60) {
            /* D ($104x) and W ($105x) inherent operations */
            int rop = rmw_op[op & 0x0F];
            bool isw = op & 0x10;
            static const bool d_ok[16] = { 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1 };
            static const bool w_ok[16] = { 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };
            if (rop >= 0 && (isw ? w_ok : d_ok)[op & 0x0F] && rop != RMW_XNC) {
                uint16_t *r = isw ? &c->w : &c->d;
                *r = rmw16(c, rop, *r);
                return CYC(3, 2);
            }
        }
    }
    if (c->h6309) {
        return illegal(c);
    }
    /* The 6809: an unassigned page 2 opcode runs as the page 0 one */
    return exec_page0(c, op) + 1;
}

/* ---- Page 3 ($11) ---- */

static int page3(m6809_t *c)
{
    int cyc = 0;
    uint8_t op = fetch(c);
    if (op == 0x3F) {
        return interrupt_all(c, VEC_SWI3, 0) + CYC(1, 1);           /* SWI3 */
    }
    int mode = (op >> 4) & 3;
    uint8_t row = op & 0x0F;
    if (op >= 0x80 && !(op & 0x40) && (row == 0x3 || row == 0xC)) {
        /* CMPU, CMPS */
        static const uint8_t cmp16[4] = { 5, 7, 7, 8 }, cmp16n[4] = { 4, 5, 6, 6 };
        uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
        sub16(c, row == 0x3 ? c->u : c->s, v, 0);
        return cyc + CYC(cmp16[mode], cmp16n[mode]);
    }
    if (c->h6309) {
        if (op >= 0x30 && op <= 0x37) {
            return bit_op(c, op);
        }
        if (op >= 0x38 && op <= 0x3B) {
            return tfm(c, op);
        }
        if (op == 0x3C) {                                           /* BITMD # */
            uint8_t im = fetch(c);
            uint8_t bits = c->md & im & 0xC0;
            c->cc = (uint8_t)((c->cc & ~M6809_Z) | (bits ? 0 : M6809_Z));
            c->md &= (uint8_t)~bits;
            return 4;
        }
        if (op == 0x3D) {                                           /* LDMD # */
            c->md = (uint8_t)((c->md & 0xC0) | (fetch(c) & 0x03));
            return 5;
        }
        if (op >= 0x40 && op < 0x60) {
            /* E ($114x) and F ($115x) inherent: COM DEC INC TST CLR */
            int rop = rmw_op[op & 0x0F];
            uint8_t lo = op & 0x0F;
            if (lo == 0x3 || lo == 0xA || lo == 0xC || lo == 0xD || lo == 0xF) {
                uint8_t *r = (op & 0x10) ? &c->f : &c->e;
                *r = rmw8(c, rop, *r);
                return CYC(3, 2);
            }
        }
        if (op >= 0x80) {
            bool regf = op & 0x40;
            uint8_t *acc = regf ? &c->f : &c->e;
            static const uint8_t e8[4] = { 3, 5, 5, 6 }, e8n[4] = { 3, 4, 5, 5 };
            switch (row) {
            case 0x0: case 0x1: case 0x6: case 0xB: {               /* SUBE CMPE LDE ADDE (F) */
                uint8_t v = mode ? rd8(c, operand_ea(c, op, &cyc)) : fetch(c);
                switch (row) {
                case 0x0: *acc = sub8(c, *acc, v, 0); break;
                case 0x1: sub8(c, *acc, v, 0); break;
                case 0x6: *acc = ld8(c, v); break;
                default: *acc = add8(c, *acc, v, 0); break;
                }
                return cyc + CYC(e8[mode], e8n[mode]);
            }
            case 0x7:                                                /* STE STF */
                if (mode == 0) {
                    break;
                }
                wr8(c, operand_ea(c, op, &cyc), ld8(c, *acc));
                return cyc + CYC(e8[mode], e8n[mode]);
            case 0xD:                                                /* DIVD */
                if (regf) {
                    break;
                }
                {
                    uint8_t v = mode ? rd8(c, operand_ea(c, op, &cyc)) : fetch(c);
                    return cyc + divd(c, v) + (mode ? 2 : 0);
                }
            case 0xE:                                                /* DIVQ */
                if (regf) {
                    break;
                }
                {
                    uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
                    return cyc + divq(c, v) + (mode ? 2 : 0);
                }
            case 0xF:                                                /* MULD */
                if (regf) {
                    break;
                }
                {
                    uint16_t v = mode ? rd16(c, operand_ea(c, op, &cyc)) : fetch16(c);
                    uint32_t q = (uint32_t)((int32_t)(int16_t)c->d * (int16_t)v);
                    c->d = (uint16_t)(q >> 16);
                    c->w = (uint16_t)q;
                    c->cc &= (uint8_t)~(M6809_N | M6809_Z | M6809_V | M6809_C);
                    SET_IF(M6809_N, q & 0x80000000u);
                    SET_IF(M6809_Z, !q);
                    static const uint8_t m[4] = { 28, 30, 30, 31 }, mn[4] = { 28, 29, 30, 30 };
                    return cyc + CYC(m[mode], mn[mode]);
                }
            default:
                break;
            }
        }
        return illegal(c);
    }
    return exec_page0(c, op) + 1;
}

/* ---- Running ---- */

void m6809_init(m6809_t *c, const m6809_bus_t *bus, bool h6309)
{
    memset(c, 0, sizeof(*c));
    c->bus = *bus;
    c->h6309 = h6309;
}

void m6809_reset(m6809_t *c)
{
    c->dp = 0;
    c->md = 0;
    c->cc |= M6809_I | M6809_F;
    c->wait = M6809_RUNNING;
    c->nmi_pending = false;
    c->nmi_armed = false;
    c->pc = rd16(c, VEC_RESET);
    c->cycles += 7;
}

int M6809_HOT m6809_step(m6809_t *c)
{
    int cyc;
    if (c->halt) {
        /* Held by HALT: the rest of the run */
        cyc = c->until > c->cycles ? (int)(c->until - c->cycles) : 1;
        c->cycles += cyc;
        return cyc;
    }
    bool nmi = c->nmi_pending;
    bool firq = c->firq && !(c->cc & M6809_F);
    bool irq = c->irq && !(c->cc & M6809_I);
    if (__builtin_expect(c->wait != M6809_RUNNING, 0)) {
        if (c->wait == M6809_SYNC && (c->nmi_pending || c->firq || c->irq)) {
            /* SYNC ends on any interrupt line, masked ones too (then go on) */
            c->wait = M6809_RUNNING;
        } else if (!(nmi || firq || irq)) {
            cyc = c->until > c->cycles ? (int)(c->until - c->cycles) : 1;
            c->cycles += cyc;
            return cyc;
        }
    }
    if (__builtin_expect(nmi || firq || irq, 0)) {
        cyc = take_interrupt(c);
        if (cyc) {
            c->cycles += cyc;
            return cyc;
        }
    }
    c->ppc = c->pc;
    cyc = exec_page0(c, fetch(c));
    c->cycles += cyc;
    return cyc;
}

#ifdef M6809_HOST_HOOK
void m6809_host_hook(m6809_t *c);       /* host builds: tracing */
#endif

void M6809_HOT m6809_run(m6809_t *c)
{
    while (c->cycles < c->until) {
#ifdef M6809_HOST_HOOK
        m6809_host_hook(c);
#endif
#if EMUDBG_ENABLED
        const uint8_t *map = c->dbg_map;
        if (__builtin_expect(map != NULL, 0) && c->wait == M6809_RUNNING && !c->halt &&
            emudbg_map16(map, c->pc)) {
            emudbg_hit(c->dbg, c->pc);
        }
#endif
        m6809_step(c);
    }
}
