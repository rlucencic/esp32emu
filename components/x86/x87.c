/*
 * The coprocessor: the 8087 and the 80287 (see x86.h). x86.c decodes an ESC
 * instruction, moves its memory operand's bytes (x87_operand() says how
 * many each way) and calls x87_exec().
 *
 * Values are 80-bit (x87_reg_t). What only moves bits works on them as they
 * are. FADD, FSUB, FMUL, FDIV and FSQRT work on the 64-bit mantissas with
 * 128-bit intermediates (the bits below sticky) and pack128() rounds them
 * to the precision control's 24, 53 or 64 bits by the rounding control:
 * exact, as the chips (checked against Fraction arithmetic on 120,000
 * operands). The transcendental functions split a value into a double.
 */
#include <math.h>
#include <string.h>
#include "x87.h"

enum {
    IE = 0x01, DE = 0x02, ZE = 0x04, OE = 0x08, UE = 0x10, PE = 0x20,
    ES = 0x80, C0 = 0x100, C1 = 0x200, C2 = 0x400, C3 = 0x4000, BUSY = 0x8000,
    CC = C0 | C1 | C2 | C3,
};
enum { RC_NEAREST, RC_DOWN, RC_UP, RC_CHOP };
enum { TAG_VALID, TAG_ZERO, TAG_SPECIAL, TAG_EMPTY };

#define BIAS        16383
#define TOP_OF(f)   (((f)->sw >> 11) & 7)
#define HALF        0x8000000000000000ull

typedef x87_reg_t f80;

static const f80 F_ZERO = { 0, 0 };
static const f80 F_ONE = { HALF, BIAS };
static const f80 F_INDEFINITE = { 0xC000000000000000ull, 0xFFFF };   /* the negative quiet NaN */

static int exp_of(f80 a)
{
    return a.se & 0x7FFF;
}

static bool sign_of(f80 a)
{
    return a.se >> 15;
}

static bool is_nan(f80 a)
{
    return exp_of(a) == 0x7FFF && (a.m << 1) != 0;
}

static bool is_inf(f80 a)
{
    return exp_of(a) == 0x7FFF && (a.m << 1) == 0;
}

static bool is_zero(f80 a)
{
    return exp_of(a) != 0x7FFF && a.m == 0;
}

static bool is_denormal(f80 a)
{
    return exp_of(a) == 0 && a.m != 0;
}

static f80 with_sign(f80 a, bool neg)
{
    a.se = (uint16_t)((a.se & 0x7FFF) | (neg ? 0x8000 : 0));
    return a;
}

static f80 zero(bool neg)
{
    return with_sign(F_ZERO, neg);
}

static f80 inf(bool neg)
{
    f80 r = { HALF, 0x7FFF };
    return with_sign(r, neg);
}

/* A finite value's exponent (unbiased) and mantissa with bit 63 set: false for zero */
static bool normal(f80 a, int *e, uint64_t *m)
{
    if (a.m == 0) {
        return false;
    }
    int x = exp_of(a) ? exp_of(a) : 1;
    int lz = __builtin_clzll(a.m);
    *m = a.m << lz;
    *e = x - lz - BIAS;
    return true;
}

static int tag_of(f80 a)
{
    if (is_zero(a)) {
        return TAG_ZERO;
    }
    if (exp_of(a) == 0 || exp_of(a) == 0x7FFF || !(a.m & HALF)) {
        return TAG_SPECIAL;
    }
    return TAG_VALID;
}

/* ---- Exceptions ---- */

/* The flags; true when the operation stops (an unmasked invalid operation,
 * denormal or zero divide: the destination stays as it was)
 */
static bool raise(x87_t *f, uint16_t bits)
{
    f->sw |= bits;
    uint16_t unmasked = bits & ~f->cw & 0x3F;
    if (unmasked) {
        f->sw |= ES | BUSY;
        f->error = !(f->i8087 && (f->cw & 0x80));     /* (the 8087's interrupt mask) */
    }
    return (unmasked & (IE | DE | ZE)) != 0;
}

/* The flags against the masks again (FLDCW, FLDENV): ES, B, the error line */
static void recheck(x87_t *f)
{
    bool pending = (f->sw & ~f->cw & 0x3F) != 0;
    f->sw = (uint16_t)(pending ? f->sw | ES | BUSY : f->sw & ~(ES | BUSY));
    f->error = pending && !(f->i8087 && (f->cw & 0x80));
}

/* A quiet NaN from the operands (the one with the larger mantissa), IE for
 * a signaling one: false when the operation stops
 */
static bool nan_result(x87_t *f, f80 a, f80 b, f80 *r)
{
    bool an = is_nan(a), bn = is_nan(b);
    *r = an && bn ? (a.m >= b.m ? a : b) : an ? a : b;
    r->m |= 0xC000000000000000ull;
    if ((an && !(a.m & 0x4000000000000000ull)) || (bn && !(b.m & 0x4000000000000000ull))) {
        return !raise(f, IE);
    }
    return true;
}

/* (the same where the result goes in anyway) */
static f80 nan_of(x87_t *f, f80 a, f80 b)
{
    f80 r;
    nan_result(f, a, b, &r);
    return r;
}

/* ---- Rounding and packing ---- */

/* m >> shift rounded by rc (shift >= 1, as far as wanted) */
static uint64_t round_shift(uint64_t m, int shift, bool neg, int rc, bool *inexact)
{
    uint64_t kept, rest;
    if (shift < 64) {
        kept = m >> shift;
        rest = m << (64 - shift);
    } else if (shift == 64) {
        kept = 0;
        rest = m;
    } else {
        kept = 0;
        rest = m ? 1 : 0;           /* (well below a half) */
    }
    *inexact = rest != 0;
    bool up;
    switch (rc) {
    case RC_NEAREST: up = rest > HALF || (rest == HALF && (kept & 1)); break;
    case RC_DOWN: up = rest && neg; break;
    case RC_UP: up = rest && !neg; break;
    default: up = false; break;
    }
    return kept + up;
}

/* 128 bits, for the exact arithmetic */
typedef struct {
    uint64_t hi, lo;
} u128;

static u128 shl128(u128 v, int n)          /* n < 64 */
{
    if (n == 0) {
        return v;
    }
    u128 r = { v.hi << n | v.lo >> (64 - n), v.lo << n };
    return r;
}

/* v >> n, the bits shifted out ORed into bit 0 (so rounding sees them) */
static u128 shr_sticky(u128 v, int n)
{
    u128 r;
    bool lost;
    if (n == 0) {
        return v;
    } else if (n < 64) {
        r.hi = v.hi >> n;
        r.lo = v.lo >> n | v.hi << (64 - n);
        lost = (v.lo << (64 - n)) != 0;
    } else if (n == 64) {
        r.hi = 0;
        r.lo = v.hi;
        lost = v.lo != 0;
    } else if (n < 128) {
        r.hi = 0;
        r.lo = v.hi >> (n - 64);
        lost = (v.hi << (128 - n)) != 0 || v.lo != 0;
    } else {
        r.hi = r.lo = 0;
        lost = v.hi || v.lo;
    }
    r.lo |= lost;
    return r;
}

static int clz128(u128 v)
{
    return v.hi ? __builtin_clzll(v.hi) : 64 + __builtin_clzll(v.lo);
}

static u128 mul64(uint64_t a, uint64_t b)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    u128 r = { p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), mid << 32 | (uint32_t)p00 };
    return r;
}

/* v (bit 127 the leading one) times 2^(e - 127): an 80-bit value rounded to
 * bits of precision (24, 53, 64) by the rounding control, overflowing to
 * infinity (or the largest number), underflowing to a denormal; PE, OE, UE
 */
static f80 pack128(x87_t *f, bool neg, u128 v, int e, int bits)
{
    int rc = (f->cw >> 10) & 3;
    int be = e + BIAS;
    int shift = 64 - bits + (be <= 0 ? 1 - be : 0);     /* (a denormal keeps fewer bits) */
    uint64_t kept, rest;
    bool sticky;
    if (shift == 0) {
        kept = v.hi;
        rest = v.lo;
        sticky = false;
    } else if (shift < 64) {
        kept = v.hi >> shift;
        rest = v.hi << (64 - shift) | v.lo >> shift;
        sticky = (v.lo << (64 - shift)) != 0;
    } else if (shift == 64) {
        kept = 0;
        rest = v.hi;
        sticky = v.lo != 0;
    } else {
        kept = 0;
        rest = 0;
        sticky = v.hi || v.lo;      /* (well below a half) */
    }
    bool inexact = rest || sticky, up;
    switch (rc) {
    case RC_NEAREST: up = rest > HALF || (rest == HALF && (sticky || (kept & 1))); break;
    case RC_DOWN: up = inexact && neg; break;
    case RC_UP: up = inexact && !neg; break;
    default: up = false; break;
    }
    kept += up;
    uint64_t m;
    if (be > 0) {
        if (bits < 64 ? (kept >> bits) != 0 : (up && kept == 0)) {
            kept = bits < 64 ? kept >> 1 : HALF;        /* rounded up to the next power of two */
            be++;
        }
        m = bits < 64 ? kept << (64 - bits) : kept;
    } else {
        m = bits < 64 ? kept << (64 - bits) : kept;
        be = (m & HALF) ? 1 : 0;    /* (rounded up into the normals) */
        if (inexact) {
            raise(f, UE);
        }
    }
    if (be >= 0x7FFF) {
        raise(f, OE | PE);
        bool to_inf = rc == RC_NEAREST || (rc == RC_UP && !neg) || (rc == RC_DOWN && neg);
        if (to_inf) {
            return inf(neg);
        }
        f80 max = { bits < 64 ? ~0ull << (64 - bits) : ~0ull, 0x7FFE };
        return with_sign(max, neg);
    }
    if (inexact) {
        raise(f, PE);
    }
    f80 r = { m, (uint16_t)be };
    return with_sign(r, neg);
}

/* sign, mantissa (bit 63 set), exponent: rounded and packed as above */
static f80 pack(x87_t *f, bool neg, uint64_t m, int e, int bits)
{
    u128 v = { m, 0 };
    return pack128(f, neg, v, e, bits);
}

static int precision(const x87_t *f)
{
    switch ((f->cw >> 8) & 3) {
    case 0: return 24;
    case 2: return 53;
    default: return 64;
    }
}

/* An integer times 2^e2, exact */
static f80 from_int(x87_t *f, bool neg, uint64_t v, int e2)
{
    if (v == 0) {
        return zero(neg);
    }
    int lz = __builtin_clzll(v);
    return pack(f, neg, v << lz, e2 + 63 - lz, 64);
}

/* ---- Splitting into doubles ---- */

typedef struct {
    bool neg;
    int e;
    double m;                       /* [1, 2], or 0 */
} split_t;

static split_t split(f80 a)
{
    split_t s = { sign_of(a), 0, 0.0 };
    uint64_t m;
    if (normal(a, &s.e, &m)) {
        s.m = (double)m * 0x1p-63;
    }
    return s;
}

/* m (>= 0, finite) times 2^e, packed at the precision control's precision */
static f80 join(x87_t *f, bool neg, double m, int e)
{
    if (m == 0.0) {
        return zero(neg);
    }
    int k;
    double fr = frexp(m, &k);       /* [0.5, 1) */
    uint64_t mant = (uint64_t)ldexp(fr, 64);
    int bits = precision(f);
    return pack(f, neg, mant, e + k - 1, bits < 53 ? bits : 64);
}

static double to_double(f80 a)
{
    if (is_nan(a)) {
        return NAN;
    }
    if (is_inf(a)) {
        return sign_of(a) ? -INFINITY : INFINITY;
    }
    split_t s = split(a);
    double v = ldexp(s.m, s.e);
    return s.neg ? -v : v;
}

double x87_to_double(x87_reg_t r)
{
    return to_double(r);
}

static f80 from_double(x87_t *f, double v)
{
    if (isnan(v)) {
        return F_INDEFINITE;
    }
    if (isinf(v)) {
        return inf(v < 0);
    }
    return join(f, signbit(v), fabs(v), 0);
}

/* ---- The stack ---- */

static int phys(const x87_t *f, int i)
{
    return (TOP_OF(f) + i) & 7;
}

static int tag(const x87_t *f, int p)
{
    return (f->tw >> (2 * p)) & 3;
}

static void set_tag(x87_t *f, int p, int t)
{
    f->tw = (uint16_t)((f->tw & ~(3 << (2 * p))) | t << (2 * p));
}

static bool empty(const x87_t *f, int i)
{
    return tag(f, phys(f, i)) == TAG_EMPTY;
}

static void set_top(x87_t *f, int top)
{
    f->sw = (uint16_t)((f->sw & ~0x3800) | (top & 7) << 11);
}

static void put(x87_t *f, int i, f80 v)
{
    int p = phys(f, i);
    f->st[p] = v;
    set_tag(f, p, tag_of(v));
}

/* ST(i) into v: false when the operation stops (empty: a stack underflow,
 * the indefinite when masked)
 */
static bool get(x87_t *f, int i, f80 *v)
{
    if (empty(f, i)) {
        *v = F_INDEFINITE;
        return !raise(f, IE);
    }
    *v = f->st[phys(f, i)];
    return true;
}

static void pop(x87_t *f)
{
    set_tag(f, phys(f, 0), TAG_EMPTY);
    set_top(f, TOP_OF(f) + 1);
}

/* Room for a push: false when the operation stops (full: a stack
 * overflow, the indefinite pushed when masked)
 */
static bool push(x87_t *f, f80 v)
{
    int p = (TOP_OF(f) - 1) & 7;
    if (tag(f, p) != TAG_EMPTY) {
        if (raise(f, IE)) {
            return false;
        }
        v = F_INDEFINITE;
    }
    set_top(f, TOP_OF(f) - 1);
    put(f, 0, v);
    return true;
}

/* ---- Arithmetic ---- */

enum { OP_ADD, OP_MUL, OP_COM, OP_COMP, OP_SUB, OP_SUBR, OP_DIV, OP_DIVR };

static bool denormal_operand(x87_t *f, f80 a, f80 b)
{
    return (is_denormal(a) || is_denormal(b)) && raise(f, DE);
}

static bool add(x87_t *f, f80 a, f80 b, bool negate_b, f80 *r)
{
    if (negate_b) {
        b = with_sign(b, !sign_of(b));
    }
    if (is_nan(a) || is_nan(b)) {
        return nan_result(f, a, b, r);
    }
    if (is_inf(a) || is_inf(b)) {
        if (is_inf(a) && is_inf(b) && sign_of(a) != sign_of(b)) {
            *r = F_INDEFINITE;
            return !raise(f, IE);
        }
        *r = is_inf(a) ? a : b;
        return true;
    }
    if (denormal_operand(f, a, b)) {
        return false;
    }
    int rc = (f->cw >> 10) & 3;
    if (is_zero(a) && is_zero(b)) {
        *r = zero(sign_of(a) == sign_of(b) ? sign_of(a) : rc == RC_DOWN);
        return true;
    }
    if (is_zero(a) || is_zero(b)) {
        /* the other, at the precision control's precision */
        f80 x = is_zero(b) ? a : b;
        int e;
        uint64_t m;
        normal(x, &e, &m);
        *r = pack(f, sign_of(x), m, e, precision(f));
        return true;
    }
    /* the larger magnitude first, the other shifted to it */
    int ea, eb;
    uint64_t ma, mb;
    normal(a, &ea, &ma);
    normal(b, &eb, &mb);
    bool na = sign_of(a), nb = sign_of(b);
    if (eb > ea || (eb == ea && mb > ma)) {
        int te = ea;
        uint64_t tm = ma;
        bool tn = na;
        ea = eb, ma = mb, na = nb;
        eb = te, mb = tm, nb = tn;
    }
    u128 x = { ma, 0 }, y = { mb, 0 }, v;
    y = shr_sticky(y, ea - eb > 200 ? 200 : ea - eb);
    int e = ea;
    if (na == nb) {
        v.lo = x.lo + y.lo;
        v.hi = x.hi + y.hi + (v.lo < x.lo);
        if (v.hi < x.hi) {          /* a carry out: one place right */
            v = shr_sticky(v, 1);
            v.hi |= HALF;
            e++;
        }
    } else {
        v.lo = x.lo - y.lo;
        v.hi = x.hi - y.hi - (x.lo < y.lo);
        if (!v.hi && !v.lo) {
            *r = zero(rc == RC_DOWN);
            return true;
        }
        int lz = clz128(v);
        while (lz > 0) {
            int n = lz < 63 ? lz : 63;
            v = shl128(v, n);
            lz -= n;
            e -= n;
        }
    }
    *r = pack128(f, na, v, e, precision(f));
    return true;
}

static bool mul(x87_t *f, f80 a, f80 b, f80 *r)
{
    bool neg = sign_of(a) != sign_of(b);
    if (is_nan(a) || is_nan(b)) {
        return nan_result(f, a, b, r);
    }
    if (is_inf(a) || is_inf(b)) {
        if (is_zero(a) || is_zero(b)) {
            *r = F_INDEFINITE;
            return !raise(f, IE);
        }
        *r = inf(neg);
        return true;
    }
    if (denormal_operand(f, a, b)) {
        return false;
    }
    if (is_zero(a) || is_zero(b)) {
        *r = zero(neg);
        return true;
    }
    int ea, eb;
    uint64_t ma, mb;
    normal(a, &ea, &ma);
    normal(b, &eb, &mb);
    u128 p = mul64(ma, mb);         /* [2^126, 2^128) */
    int e = ea + eb + 1;
    if (!(p.hi & HALF)) {
        p = shl128(p, 1);
        e--;
    }
    *r = pack128(f, neg, p, e, precision(f));
    return true;
}

static bool divide(x87_t *f, f80 a, f80 b, f80 *r)
{
    bool neg = sign_of(a) != sign_of(b);
    if (is_nan(a) || is_nan(b)) {
        return nan_result(f, a, b, r);
    }
    if ((is_inf(a) && is_inf(b)) || (is_zero(a) && is_zero(b))) {
        *r = F_INDEFINITE;
        return !raise(f, IE);
    }
    if (is_inf(a)) {
        *r = inf(neg);
        return true;
    }
    if (is_inf(b)) {
        *r = zero(neg);
        return true;
    }
    if (denormal_operand(f, a, b)) {
        return false;
    }
    if (is_zero(b)) {
        *r = inf(neg);
        return !raise(f, ZE);
    }
    if (is_zero(a)) {
        *r = zero(neg);
        return true;
    }
    int ea, eb;
    uint64_t ma, mb;
    normal(a, &ea, &ma);
    normal(b, &eb, &mb);
    /* long division, 67 quotient bits from [1, 2), the remainder sticky */
    uint64_t rem = ma;
    bool carry = false;
    int e = ea - eb;
    if (ma < mb) {
        carry = rem >> 63;
        rem <<= 1;
        e--;
    }
    u128 q = { 0, 0 };
    for (int i = 0; i < 67; i++) {
        bool bit = carry || rem >= mb;
        if (bit) {
            rem -= mb;
        }
        q = shl128(q, 1);
        q.lo |= bit;
        carry = rem >> 63;
        rem <<= 1;
    }
    q = shl128(q, 61);
    q.lo |= rem != 0 || carry;
    *r = pack128(f, neg, q, e, precision(f));
    return true;
}

/* C3 C2 C0 for a against b: greater 000, less 001, equal 100, unordered 111 */
static uint16_t compare(x87_t *f, f80 a, f80 b)
{
    if (is_nan(a) || is_nan(b)) {
        raise(f, IE);
        return C3 | C2 | C0;
    }
    if (is_inf(a) || is_inf(b)) {
        if (!(f->cw & 0x1000)) {
            /* projective closure (the 8087's and 287's default): one
             * unsigned infinity, which only equals itself
             */
            if (is_inf(a) && is_inf(b)) {
                return C3;
            }
            raise(f, IE);
            return C3 | C2 | C0;
        }
    }
    if (denormal_operand(f, a, b)) {
        return C3 | C2 | C0;
    }
    if (is_zero(a) && is_zero(b)) {
        return C3;
    }
    bool na = sign_of(a) && !is_zero(a), nb = sign_of(b) && !is_zero(b);
    if (na != nb) {
        return na ? C0 : 0;
    }
    /* the same sign: the magnitudes (zero below everything) */
    int ea = 0, eb = 0;
    uint64_t ma = 0, mb = 0;
    bool za = is_inf(a) ? false : !normal(a, &ea, &ma);
    bool zb = is_inf(b) ? false : !normal(b, &eb, &mb);
    int c;                          /* |a| against |b| */
    if (is_inf(a) || is_inf(b)) {
        c = is_inf(a) && is_inf(b) ? 0 : is_inf(a) ? 1 : -1;
    } else if (za || zb) {
        c = za && zb ? 0 : za ? -1 : 1;
    } else {
        c = ea != eb ? (ea > eb ? 1 : -1) : ma != mb ? (ma > mb ? 1 : -1) : 0;
    }
    if (na) {
        c = -c;
    }
    return c > 0 ? 0 : c < 0 ? C0 : C3;
}

static void set_cc(x87_t *f, uint16_t cc)
{
    f->sw = (uint16_t)((f->sw & ~CC) | cc);
}

/* ---- Integers ---- */

/* To an integer of `bits` (16, 32, 64) by the rounding control: false
 * (the integer indefinite, IE) if out of range
 */
static bool to_int(x87_t *f, f80 a, int bits, int64_t *out)
{
    const int64_t indefinite = bits == 64 ? INT64_MIN : -(1ll << (bits - 1));
    if (is_nan(a) || is_inf(a)) {
        *out = indefinite;
        return !raise(f, IE);
    }
    bool neg = sign_of(a);
    int e;
    uint64_t m, v;
    if (!normal(a, &e, &m)) {
        *out = 0;
        return true;
    }
    if (is_denormal(a) && raise(f, DE)) {
        return false;
    }
    bool inexact = false;
    if (e >= 64) {
        *out = indefinite;
        return !raise(f, IE);
    }
    v = e == 63 ? m : round_shift(m, 63 - e, neg, (f->cw >> 10) & 3, &inexact);
    uint64_t limit = (1ull << (bits - 1)) - (neg ? 0 : 1);
    if (v > limit) {
        *out = indefinite;
        return !raise(f, IE);
    }
    if (inexact) {
        raise(f, PE);
    }
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

static f80 from_int64(x87_t *f, int64_t v)
{
    bool neg = v < 0;
    return from_int(f, neg, neg ? 0 - (uint64_t)v : (uint64_t)v, 0);
}

static f80 round_int(x87_t *f, f80 a)
{
    int e;
    uint64_t m;
    if (is_nan(a) || is_inf(a) || !normal(a, &e, &m) || e >= 63) {
        return a;
    }
    bool inexact;
    uint64_t v = round_shift(m, 63 - e, sign_of(a), (f->cw >> 10) & 3, &inexact);
    if (inexact) {
        raise(f, PE);
    }
    return from_int(f, sign_of(a), v, 0);
}

/* ---- Memory formats ---- */

static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) {
        v = v << 8 | p[i];
    }
    return v;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* A single (frac 23, exp 8) or double (52, 11) real into 80 bits, exact */
static f80 from_real(x87_t *f, uint64_t bits, int frac, int expw)
{
    int bias = (1 << (expw - 1)) - 1, emax = (1 << expw) - 1;
    bool neg = (bits >> (frac + expw)) & 1;
    int e = (int)((bits >> frac) & (uint64_t)emax);
    uint64_t m = bits & ((1ull << frac) - 1);
    if (e == emax) {
        if (m == 0) {
            return inf(neg);
        }
        f80 r = { HALF | m << (63 - frac), 0x7FFF };
        if (!(r.m & 0x4000000000000000ull)) {
            raise(f, IE);           /* a signaling NaN: quiet */
            r.m |= 0x4000000000000000ull;
        }
        return with_sign(r, neg);
    }
    if (e == 0) {
        if (m == 0) {
            return zero(neg);
        }
        raise(f, DE);
        return from_int(f, neg, m, 1 - bias - frac);
    }
    return from_int(f, neg, m | 1ull << frac, e - bias - frac);
}

/* 80 bits to a single or double real, rounded by the rounding control */
static uint64_t to_real(x87_t *f, f80 a, int frac, int expw)
{
    int bias = (1 << (expw - 1)) - 1, emax = (1 << expw) - 1;
    uint64_t sign = (uint64_t)sign_of(a) << (frac + expw);
    if (is_nan(a)) {
        if (!(a.m & 0x4000000000000000ull)) {
            raise(f, IE);
        }
        return sign | (uint64_t)emax << frac | 1ull << (frac - 1) | ((a.m << 1) >> (64 - frac));
    }
    if (is_inf(a)) {
        return sign | (uint64_t)emax << frac;
    }
    int e;
    uint64_t m;
    if (!normal(a, &e, &m)) {
        return sign;
    }
    if (is_denormal(a)) {
        raise(f, DE);
    }
    bool neg = sign_of(a);
    int rc = (f->cw >> 10) & 3;
    int be = e + bias;
    bool inexact;
    uint64_t v;
    if (be <= 0) {
        /* a denormal: fewer bits */
        v = round_shift(m, 63 - frac + 1 - be, neg, rc, &inexact);
        if (inexact) {
            raise(f, UE | PE);
        }
        return sign | v;            /* (a carry into bit `frac` makes it the smallest normal) */
    }
    v = round_shift(m, 63 - frac, neg, rc, &inexact);
    if (v >> (frac + 1)) {
        v >>= 1;
        be++;
    }
    if (be >= emax) {
        raise(f, OE | PE);
        bool to_inf = rc == RC_NEAREST || (rc == RC_UP && !neg) || (rc == RC_DOWN && neg);
        return sign | (to_inf ? (uint64_t)emax << frac : ((uint64_t)(emax - 1) << frac | ((1ull << frac) - 1)));
    }
    if (inexact) {
        raise(f, PE);
    }
    return sign | (uint64_t)be << frac | (v & ((1ull << frac) - 1));
}

static f80 from_bcd(x87_t *f, const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 8; i >= 0; i--) {
        v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 15);
    }
    return from_int(f, p[9] & 0x80, v, 0);
}

static bool to_bcd(x87_t *f, f80 a, uint8_t *p)
{
    int64_t v;
    if (!to_int(f, a, 64, &v)) {
        return false;
    }
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    if (u > 999999999999999999ull) {
        if (raise(f, IE)) {
            return false;
        }
        memset(p, 0, 10);           /* the indefinite */
        p[7] = 0xC0;
        p[8] = p[9] = 0xFF;
        return true;
    }
    for (int i = 0; i < 9; i++) {
        uint8_t lo = (uint8_t)(u % 10);
        u /= 10;
        p[i] = (uint8_t)(lo | (u % 10) << 4);
        u /= 10;
    }
    p[9] = v < 0 ? 0x80 : 0x00;
    return true;
}

static f80 load80(const uint8_t *p)
{
    f80 r = { le(p, 8), (uint16_t)le(p + 8, 2) };
    return r;
}

static void store80(uint8_t *p, f80 a)
{
    put_le(p, a.m, 8);
    put_le(p + 8, a.se, 2);
}

/* ---- Constants ---- */

static const f80 s_constants[7] = {
    { HALF, BIAS },                                 /* 1 */
    { 0xD49A784BCD1B8AFEull, BIAS + 1 },            /* log2(10) */
    { 0xB8AA3B295C17F0BCull, BIAS },                /* log2(e) */
    { 0xC90FDAA22168C235ull, BIAS + 1 },            /* pi */
    { 0x9A209A84FBCFF799ull, BIAS - 2 },            /* log10(2) */
    { 0xB17217F7D1CF79ACull, BIAS - 1 },            /* ln(2) */
    { 0, 0 },                                       /* 0 */
};

/* ---- Environment ---- */

static uint16_t full_tags(const x87_t *f)
{
    uint16_t tw = 0;
    for (int p = 0; p < 8; p++) {
        int t = tag(f, p) == TAG_EMPTY ? TAG_EMPTY : tag_of(f->st[p]);
        tw |= (uint16_t)(t << (2 * p));
    }
    return tw;
}

static void store_env(const x87_t *f, uint8_t *p)
{
    put_le(p, f->cw, 2);
    put_le(p + 2, f->sw, 2);
    put_le(p + 4, full_tags(f), 2);
    if (f->pm) {
        put_le(p + 6, f->ip_off, 2);
        put_le(p + 8, f->ip_seg, 2);
        put_le(p + 10, f->dp_off, 2);
        put_le(p + 12, f->dp_seg, 2);
    } else {
        uint32_t ip = ((uint32_t)f->ip_seg << 4) + f->ip_off, dp = ((uint32_t)f->dp_seg << 4) + f->dp_off;
        put_le(p + 6, ip & 0xFFFF, 2);
        put_le(p + 8, (ip >> 16 & 15) << 12 | (f->opcode & 0x7FF), 2);
        put_le(p + 10, dp & 0xFFFF, 2);
        put_le(p + 12, (dp >> 16 & 15) << 12, 2);
    }
}

static void load_env(x87_t *f, const uint8_t *p)
{
    f->cw = (uint16_t)le(p, 2);
    f->sw = (uint16_t)le(p + 2, 2);
    f->tw = (uint16_t)le(p + 4, 2);
    if (f->pm) {
        f->ip_off = (uint16_t)le(p + 6, 2);
        f->ip_seg = (uint16_t)le(p + 8, 2);
        f->dp_off = (uint16_t)le(p + 10, 2);
        f->dp_seg = (uint16_t)le(p + 12, 2);
    } else {
        uint32_t w = (uint32_t)le(p + 8, 2), ip = (uint32_t)le(p + 6, 2) | (w >> 12) << 16;
        uint32_t dp = (uint32_t)le(p + 10, 2) | (uint32_t)(le(p + 12, 2) >> 12) << 16;
        f->ip_seg = (uint16_t)(ip >> 4 & 0xF000);
        f->ip_off = (uint16_t)(ip - ((uint32_t)f->ip_seg << 4));
        f->opcode = (uint16_t)(w & 0x7FF);
        f->dp_seg = (uint16_t)(dp >> 4 & 0xF000);
        f->dp_off = (uint16_t)(dp - ((uint32_t)f->dp_seg << 4));
    }
    recheck(f);
}

/* ---- The instructions ---- */

void x87_init(x87_t *f)
{
    f->cw = f->i8087 ? 0x03FF : 0x037F;    /* everything masked, 64 bits, nearest (the 8087's interrupts off) */
    f->sw = 0;
    f->tw = 0xFFFF;
    f->ip_off = f->ip_seg = f->opcode = f->dp_off = f->dp_seg = 0;
    f->error = false;
}

void x87_reset(x87_t *f, bool i8087)
{
    memset(f, 0, sizeof(*f));
    f->i8087 = i8087;
    x87_init(f);
}

void x87_operand(uint8_t op, uint8_t reg, int *read, int *write)
{
    static const int8_t sizes[8][8] = {
        /* + read, - written */
        { 4, 4, 4, 4, 4, 4, 4, 4 },             /* D8: m32 real */
        { 4, 0, -4, -4, 14, 2, -14, -2 },       /* D9: FLD, -, FST, FSTP m32, FLDENV, FLDCW, FSTENV, FSTCW */
        { 4, 4, 4, 4, 4, 4, 4, 4 },             /* DA: m32 integer */
        { 4, 0, -4, -4, 0, 10, 0, -10 },        /* DB: FILD, -, FIST, FISTP m32, -, FLD m80, -, FSTP m80 */
        { 8, 8, 8, 8, 8, 8, 8, 8 },             /* DC: m64 real */
        { 8, 0, -8, -8, 94, 0, -94, -2 },       /* DD: FLD, -, FST, FSTP m64, FRSTOR, -, FSAVE, FSTSW */
        { 2, 2, 2, 2, 2, 2, 2, 2 },             /* DE: m16 integer */
        { 2, 0, -2, -2, 10, 8, -10, -8 },       /* DF: FILD, -, FIST, FISTP m16, FBLD, FILD m64, FBSTP, FISTP m64 */
    };
    int n = sizes[op & 7][reg & 7];
    *read = n > 0 ? n : 0;
    *write = n < 0 ? -n : 0;
}

/* The arithmetic group (D8, DC, DA, DE; registers and memory): ST(d) op= v */
static void arith(x87_t *f, int kind, int d, f80 v, bool pop_after)
{
    f80 a, r;
    if (!get(f, d, &a)) {
        return;
    }
    bool ok;
    switch (kind) {
    case OP_ADD: ok = add(f, a, v, false, &r); break;
    case OP_MUL: ok = mul(f, a, v, &r); break;
    case OP_COM:
    case OP_COMP:
        set_cc(f, compare(f, a, v));
        if (kind == OP_COMP || pop_after) {
            pop(f);
        }
        return;
    case OP_SUB: ok = add(f, a, v, true, &r); break;
    case OP_SUBR: ok = add(f, v, a, true, &r); break;
    case OP_DIV: ok = divide(f, a, v, &r); break;
    default: ok = divide(f, v, a, &r); break;
    }
    if (ok) {
        put(f, d, r);
        if (pop_after) {
            pop(f);
        }
    }
}

/* D9 E0-FF: the constants and functions */
static int d9_function(x87_t *f, uint8_t rm_byte)
{
    f80 a, b, r;
    switch (rm_byte) {
    case 0xE0:                      /* FCHS */
        if (get(f, 0, &a)) {
            put(f, 0, with_sign(a, !sign_of(a)));
        }
        return 15;
    case 0xE1:                      /* FABS */
        if (get(f, 0, &a)) {
            put(f, 0, with_sign(a, false));
        }
        return 15;
    case 0xE4:                      /* FTST */
        if (get(f, 0, &a)) {
            set_cc(f, compare(f, a, F_ZERO));
        } else {
            set_cc(f, C3 | C2 | C0);
        }
        return 40;
    case 0xE5: {                    /* FXAM: the class in C3 C2 C0, the sign in C1 */
        uint16_t cc;
        a = f->st[phys(f, 0)];
        if (empty(f, 0)) {
            cc = C3 | C0;
        } else if (is_nan(a)) {
            cc = C0;
        } else if (is_inf(a)) {
            cc = C2 | C0;
        } else if (is_zero(a)) {
            cc = C3;
        } else if (is_denormal(a)) {
            cc = C3 | C2;
        } else if (a.m & HALF) {
            cc = C2;
        } else {
            cc = 0;                 /* unnormal */
        }
        set_cc(f, cc | (sign_of(a) ? C1 : 0));
        return 17;
    }
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE:
        push(f, s_constants[rm_byte - 0xE8]);
        return rm_byte == 0xEE ? 14 : 20;
    case 0xF0:                      /* F2XM1 */
        if (get(f, 0, &a) && !is_nan(a)) {
            put(f, 0, from_double(f, expm1(to_double(a) * M_LN2)));
            raise(f, PE);
        }
        return 500;
    case 0xF1:                      /* FYL2X: ST(1) * log2(ST) */
    case 0xF9: {                    /* FYL2XP1: ST(1) * log2(ST + 1) */
        if (!get(f, 0, &a) || !get(f, 1, &b)) {
            return 900;
        }
        double x = to_double(a), y = to_double(b);
        if (is_nan(a) || is_nan(b)) {
            r = nan_of(f, a, b);
        } else if (rm_byte == 0xF1 ? x < 0 : x < -1) {
            r = F_INDEFINITE;
            if (raise(f, IE)) {
                return 900;
            }
        } else if (rm_byte == 0xF1 && x == 0) {
            r = inf(!sign_of(b));
            if (raise(f, ZE)) {
                return 900;
            }
        } else {
            double l = rm_byte == 0xF1 ? log2(x) : log1p(x) / M_LN2;
            r = from_double(f, y * l);
            raise(f, PE);
        }
        put(f, 1, r);
        pop(f);
        return rm_byte == 0xF1 ? 950 : 850;
    }
    case 0xF2:                      /* FPTAN: tan(ST) as ST(1) / ST, 1 the divisor */
        if (get(f, 0, &a)) {
            if (is_nan(a) || is_inf(a)) {
                if (!raise(f, IE)) {
                    put(f, 0, F_INDEFINITE);
                    push(f, F_INDEFINITE);
                }
                return 450;
            }
            if (exp_of(a) >= BIAS + 63) {
                f->sw |= C2;        /* out of range */
                return 450;
            }
            f->sw &= ~C2;
            put(f, 0, from_double(f, tan(to_double(a))));
            push(f, F_ONE);
            raise(f, PE);
        }
        return 450;
    case 0xF3:                      /* FPATAN: atan(ST(1) / ST) */
        if (get(f, 0, &a) && get(f, 1, &b)) {
            r = is_nan(a) || is_nan(b) ? nan_of(f, a, b) : from_double(f, atan2(to_double(b), to_double(a)));
            put(f, 1, r);
            pop(f);
            raise(f, PE);
        }
        return 650;
    case 0xF4: {                    /* FXTRACT: the exponent, then the significand */
        if (!get(f, 0, &a)) {
            return 50;
        }
        int e;
        uint64_t m;
        if (is_nan(a) || is_inf(a)) {
            f80 x = is_inf(a) ? inf(false) : a;
            put(f, 0, x);
            push(f, a);
        } else if (!normal(a, &e, &m)) {
            if (raise(f, ZE)) {
                return 50;
            }
            put(f, 0, inf(true));
            push(f, a);
        } else {
            f80 s = { m, (uint16_t)(BIAS | (sign_of(a) ? 0x8000 : 0)) };
            put(f, 0, from_int64(f, e));
            push(f, s);
        }
        return 50;
    }
    case 0xF6:                      /* FDECSTP */
        set_top(f, TOP_OF(f) - 1);
        return 10;
    case 0xF7:                      /* FINCSTP */
        set_top(f, TOP_OF(f) + 1);
        return 10;
    case 0xF8: {                    /* FPREM: the partial remainder of ST / ST(1) */
        if (!get(f, 0, &a) || !get(f, 1, &b)) {
            return 125;
        }
        if (is_nan(a) || is_nan(b)) {
            put(f, 0, nan_of(f, a, b));
            return 125;
        }
        if (is_inf(a) || is_zero(b)) {
            if (!raise(f, IE)) {
                put(f, 0, F_INDEFINITE);
            }
            return 125;
        }
        int ea, eb;
        uint64_t ma, mb;
        if (is_inf(b) || !normal(a, &ea, &ma)) {
            set_cc(f, 0);           /* zero, or below an infinity: as it is */
            return 125;
        }
        normal(b, &eb, &mb);
        int d = ea - eb;
        if (d < 0) {
            set_cc(f, 0);
            return 125;
        }
        /* long division, a quotient bit at a time; at most 63 bits for a
         * partial remainder (C2: more to do)
         */
        bool partial = d >= 64;
        int steps = partial ? 32 + d % 32 : d;
        uint64_t rem = ma, q = 0;
        for (int i = 0; i <= steps; i++) {
            bool carry = false;
            if (i) {
                carry = rem >> 63;
                rem <<= 1;
            }
            q <<= 1;
            if (carry || rem >= mb) {
                rem -= mb;
                q |= 1;
            }
        }
        int re = ea - steps;        /* the remainder's units: 2^(re - 63) */
        f80 rr = rem ? from_int(f, sign_of(a), rem, re - 63) : zero(sign_of(a));
        put(f, 0, rr);
        uint16_t cc = partial ? C2 : (uint16_t)(((q >> 2) & 1 ? C0 : 0) | ((q >> 1) & 1 ? C1 : 0) | (q & 1 ? C3 : 0));
        set_cc(f, cc);
        return 125;
    }
    case 0xFA:                      /* FSQRT */
        if (get(f, 0, &a)) {
            if (is_nan(a)) {
                r = nan_of(f, a, a);
            } else if (sign_of(a) && !is_zero(a)) {
                r = F_INDEFINITE;
                if (raise(f, IE)) {
                    return 180;
                }
            } else if (is_zero(a) || is_inf(a)) {
                r = a;
            } else {
                if (is_denormal(a) && raise(f, DE)) {
                    return 180;
                }
                /* the integer square root of the mantissa shifted to
                 * 128 bits (one more for an odd exponent), the remainder
                 * against the root for rounding (no ties)
                 */
                int e;
                uint64_t m;
                normal(a, &e, &m);
                u128 n = (e & 1) ? (u128){ m, 0 } : (u128){ m >> 1, m << 63 };
                int half_e = (e & 1) ? (e - 1) / 2 : e / 2;
                u128 rem = { 0, 0 };
                uint64_t root = 0;
                for (int i = 0; i < 64; i++) {
                    rem = shl128(rem, 2);
                    rem.lo |= n.hi >> 62;
                    n = shl128(n, 2);
                    u128 trial = { root >> 62, root << 2 | 1 };
                    if (rem.hi > trial.hi || (rem.hi == trial.hi && rem.lo >= trial.lo)) {
                        uint64_t lo = rem.lo - trial.lo;
                        rem.hi = rem.hi - trial.hi - (rem.lo < trial.lo);
                        rem.lo = lo;
                        root = root << 1 | 1;
                    } else {
                        root <<= 1;
                    }
                }
                u128 v = { root, 0 };
                if (rem.hi || rem.lo > root) {
                    v.lo = HALF | 1;        /* above the half */
                } else if (rem.lo) {
                    v.lo = 1;
                }
                r = pack128(f, false, v, half_e, precision(f));
            }
            put(f, 0, r);
        }
        return 180;
    case 0xFC:                      /* FRNDINT */
        if (get(f, 0, &a)) {
            put(f, 0, is_nan(a) ? nan_of(f, a, a) : round_int(f, a));
        }
        return 45;
    case 0xFD: {                    /* FSCALE: ST * 2^trunc(ST(1)) */
        if (!get(f, 0, &a) || !get(f, 1, &b)) {
            return 35;
        }
        if (is_nan(a) || is_nan(b)) {
            put(f, 0, nan_of(f, a, b));
            return 35;
        }
        int e;
        uint64_t m;
        if (is_inf(b)) {
            if ((sign_of(b) && is_inf(a)) || (!sign_of(b) && is_zero(a))) {
                if (!raise(f, IE)) {
                    put(f, 0, F_INDEFINITE);
                }
            } else if (!is_zero(a) && !is_inf(a)) {
                put(f, 0, sign_of(b) ? zero(sign_of(a)) : inf(sign_of(a)));
            }
            return 35;
        }
        if (is_inf(a) || !normal(a, &e, &m)) {
            return 35;
        }
        int n = 0, eb;
        uint64_t mb;
        if (normal(b, &eb, &mb) && eb >= 0) {
            n = eb > 20 ? 1 << 20 : (int)(mb >> (63 - eb));
            n = sign_of(b) ? -n : n;
        }
        put(f, 0, pack(f, sign_of(a), m, e + n, 64));
        return 35;
    }
    default:                        /* FNOP (D9 D0) and the 387's */
        return 3;
    }
}

int x87_exec(x86_t *c, uint8_t op, uint8_t mod, uint8_t reg, uint8_t rm, uint8_t *mem, uint16_t dp_off,
             uint16_t dp_seg)
{
    x87_t *f = &c->fpu;
    const bool i8087 = c->model != X86_80286;
    uint8_t rm_byte = (uint8_t)(0xC0 | reg << 3 | rm);
    int g = op & 7;
    f80 v, a;
    int64_t n;

    /* the control instructions keep the last instruction's pointers */
    bool control = (g == 1 && mod != 3 && reg >= 4) || (g == 3 && mod == 3 && reg == 4) ||
                   (g == 5 && mod != 3 && reg >= 4) || (g == 7 && mod == 3 && reg == 4);
    if (!control) {
        f->ip_off = c->ip0;
        f->ip_seg = c->seg[X86_CS].sel;
        f->opcode = (uint16_t)(g << 8 | (mod << 6 | reg << 3 | rm));
        if (mod != 3) {
            f->dp_off = dp_off;
            f->dp_seg = dp_seg;
        }
    }

    if (mod == 3) {
        switch (g) {
        case 0:                     /* FADD ... FDIVR ST, ST(i) */
            if (get(f, rm, &v)) {
                arith(f, reg, 0, v, false);
            }
            return reg == 1 ? 130 : reg >= 6 ? 198 : reg == 2 || reg == 3 ? 45 : 85;
        case 4:                     /* FADD ... ST(i), ST (SUB/SUBR and DIV/DIVR turned round) */
        case 6:                     /* ... and pop */
            if (g == 6 && reg == 3) {
                if (rm != 1) {
                    return 3;
                }
                if (get(f, 1, &v)) {    /* FCOMPP */
                    arith(f, OP_COMP, 0, v, false);
                    pop(f);
                }
                return 50;
            }
            if (get(f, 0, &v)) {
                int kind = reg >= 4 ? reg ^ 1 : reg;
                if (reg == 2 || reg == 3) {
                    /* (DC D0-DF, DE D0-D7: FCOM, FCOMP aliases) */
                    if (get(f, rm, &v)) {
                        arith(f, reg, 0, v, false);
                        if (g == 6) {
                            pop(f);
                        }
                    }
                } else {
                    arith(f, kind, rm, v, g == 6);
                }
            }
            return reg == 1 ? 130 : reg >= 6 ? 198 : 85;
        case 1:
            switch (reg) {
            case 0:                 /* FLD ST(i) */
                if (get(f, rm, &v)) {
                    push(f, v);
                }
                return 20;
            case 1: {               /* FXCH */
                f80 x, y;
                if (get(f, 0, &x) && get(f, rm, &y)) {
                    put(f, 0, y);
                    put(f, rm, x);
                }
                return 12;
            }
            case 3:                 /* (D9 D8-DF: an FSTP alias) */
                if (get(f, 0, &v)) {
                    put(f, rm, v);
                    pop(f);
                }
                return 20;
            default:
                return d9_function(f, rm_byte);
            }
        case 3:
            switch (rm_byte) {
            case 0xE0:              /* FENI (the 8087's) */
                if (i8087) {
                    f->cw &= (uint16_t)~0x80;
                }
                return 5;
            case 0xE1:              /* FDISI */
                if (i8087) {
                    f->cw |= 0x80;
                }
                return 5;
            case 0xE2:              /* FCLEX */
                f->sw &= (uint16_t)~(0x3F | ES | BUSY);
                f->error = false;
                return 5;
            case 0xE3:              /* FINIT */
                x87_init(f);
                return 5;
            case 0xE4:              /* FSETPM (the 287's) */
                if (!i8087) {
                    f->pm = true;
                }
                return 5;
            default:
                return 3;
            }
        case 5:
            switch (reg) {
            case 0:                 /* FFREE */
                set_tag(f, phys(f, rm), TAG_EMPTY);
                return 11;
            case 1:                 /* (an FXCH alias) */
                return x87_exec(c, 0xD9, 3, 1, rm, mem, 0, 0);
            case 2:                 /* FST ST(i) */
            case 3:                 /* FSTP ST(i) */
                if (get(f, 0, &v)) {
                    put(f, rm, v);
                    if (reg == 3) {
                        pop(f);
                    }
                }
                return 18;
            default:
                return 3;
            }
        case 7:
            if (reg == 4 && rm == 0 && !i8087) {
                c->r.w[X86_AX] = f->sw;     /* FSTSW AX (the 287's) */
                return 15;
            }
            if (reg == 0) {         /* (FFREEP) */
                set_tag(f, phys(f, rm), TAG_EMPTY);
                pop(f);
                return 11;
            }
            if (reg == 1) {
                return x87_exec(c, 0xD9, 3, 1, rm, mem, 0, 0);
            }
            if (reg == 2 || reg == 3) {
                return x87_exec(c, 0xD9, 3, 3, rm, mem, 0, 0);
            }
            return 3;
        default:
            return 3;               /* (DA: the 387's FUCOMPP) */
        }
    }

    /* memory operands */
    switch (g) {
    case 0:                         /* m32 real */
    case 4:                         /* m64 real */
    case 2:                         /* m32 integer */
    case 6:                         /* m16 integer */
        if (g == 0) {
            v = from_real(f, le(mem, 4), 23, 8);
        } else if (g == 4) {
            v = from_real(f, le(mem, 8), 52, 11);
        } else if (g == 2) {
            v = from_int64(f, (int32_t)le(mem, 4));
        } else {
            v = from_int64(f, (int16_t)le(mem, 2));
        }
        arith(f, reg, 0, v, false);
        return (reg == 1 ? 130 : reg >= 6 ? 200 : reg == 2 || reg == 3 ? 55 : 90) + (g & 2 ? 20 : 0);
    case 1:
        switch (reg) {
        case 0:                     /* FLD m32 */
            v = from_real(f, le(mem, 4), 23, 8);
            push(f, v);
            return 45;
        case 2:                     /* FST m32 */
        case 3:                     /* FSTP m32 */
            if (get(f, 0, &v)) {
                put_le(mem, to_real(f, v, 23, 8), 4);
                if (reg == 3) {
                    pop(f);
                }
            }
            return 85;
        case 4:                     /* FLDENV */
            load_env(f, mem);
            return 45;
        case 5:                     /* FLDCW */
            f->cw = (uint16_t)le(mem, 2);
            recheck(f);
            return 10;
        case 6:                     /* FSTENV: then everything masked */
            store_env(f, mem);
            f->cw |= 0x3F;
            return 45;
        case 7:                     /* FSTCW */
            put_le(mem, f->cw, 2);
            return 15;
        default:
            return 3;
        }
    case 3:
        switch (reg) {
        case 0:                     /* FILD m32 */
            push(f, from_int64(f, (int32_t)le(mem, 4)));
            return 55;
        case 2:                     /* FIST m32 */
        case 3:                     /* FISTP m32 */
        do_fist:
            if (get(f, 0, &v)) {
                int bits = g == 3 ? 32 : (reg == 7 ? 64 : 16);
                if (to_int(f, v, bits, &n)) {
                    put_le(mem, (uint64_t)n, bits / 8);
                    if (reg == 3 || reg == 7) {
                        pop(f);
                    }
                }
            }
            return 90;
        case 5:                     /* FLD m80 */
            v = load80(mem);
            push(f, v);
            return 55;
        case 7:                     /* FSTP m80 */
            if (get(f, 0, &v)) {
                store80(mem, v);
                pop(f);
            }
            return 55;
        default:
            return 3;
        }
    case 5:
        switch (reg) {
        case 0:                     /* FLD m64 */
            push(f, from_real(f, le(mem, 8), 52, 11));
            return 45;
        case 2:                     /* FST m64 */
        case 3:                     /* FSTP m64 */
            if (get(f, 0, &v)) {
                put_le(mem, to_real(f, v, 52, 11), 8);
                if (reg == 3) {
                    pop(f);
                }
            }
            return 105;
        case 4:                     /* FRSTOR */
            load_env(f, mem);
            for (int i = 0; i < 8; i++) {
                f->st[phys(f, i)] = load80(mem + 14 + 10 * i);
            }
            return 210;
        case 6:                     /* FSAVE: then FINIT */
            store_env(f, mem);
            for (int i = 0; i < 8; i++) {
                store80(mem + 14 + 10 * i, f->st[phys(f, i)]);
            }
            x87_init(f);
            return 210;
        case 7:                     /* FSTSW */
            put_le(mem, f->sw, 2);
            return 15;
        default:
            return 3;
        }
    case 7:
        switch (reg) {
        case 0:                     /* FILD m16 */
            push(f, from_int64(f, (int16_t)le(mem, 2)));
            return 50;
        case 2:                     /* FIST m16 */
        case 3:                     /* FISTP m16 */
        case 7:                     /* FISTP m64 */
            goto do_fist;
        case 4:                     /* FBLD */
            push(f, from_bcd(f, mem));
            return 300;
        case 5:                     /* FILD m64 */
            push(f, from_int64(f, (int64_t)le(mem, 8)));
            return 60;
        case 6:                     /* FBSTP */
            if (get(f, 0, &a) && to_bcd(f, a, mem)) {
                pop(f);
            }
            return 530;
        default:
            return 3;
        }
    default:
        return 3;
    }
}
