/*
 * The SingleStepTests suites for the 8088 and 8086 (github.com/
 * SingleStepTests, MIT; MOO files, fetched by `make data`):
 *
 *   build/ssts [-m 8088|8086|286] [-u] [-i] [-w] [-v] [-n N] [-t] file.MOO.gz ...
 *
 * Each test sets the registers and memory, runs one instruction with
 * x86_step() and compares registers, memory and flags (flags the
 * suite's metadata calls undefined are left out unless -u). Writes go
 * through the bus so that a write the test doesn't expect shows (-w: the
 * pages writable instead, the core's direct writes tested; stray writes not
 * seen then). -v prints
 * each failure, -n the first N failures of a file, -t compares the clocks
 * with the test's cycles (statistics only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "x86.h"

#define RAM_SIZE    (1u << 24)      /* the 286's 16 MB (the 8088's 1 MB is its start) */

static uint8_t *ram;
static uintptr_t page[X86_PAGES];
static x86_t cpu;

static uint32_t written[256];
static uint8_t written_v[256];
static int written_n;

static uint8_t bus_read(void *ctx, uint32_t a)
{
    return ram[a & (RAM_SIZE - 1)];
}

static void bus_write(void *ctx, uint32_t a, uint8_t v)
{
    a &= RAM_SIZE - 1;
    ram[a] = v;
    if (written_n < 256) {
        written[written_n] = a;
        written_v[written_n++] = v;
    }
}

static uint8_t io_in(void *ctx, uint16_t port)
{
    return 0xFF;
}

static void io_out(void *ctx, uint16_t port, uint8_t v)
{
}

static uint8_t inta(void *ctx)
{
    return 0;
}

/* ---- The metadata's undefined flags ("F6.4 65323" lines from masks.py) ---- */

static uint16_t masks[256][9];      /* [opcode][reg + 1], [opcode][0] for the whole opcode */

static void load_masks(const char *path)
{
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 9; j++) {
            masks[i][j] = 0xFFFF;
        }
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "no %s: all flags compared\n", path);
        return;
    }
    char key[16];
    unsigned mask;
    while (fscanf(f, "%15s %u", key, &mask) == 2) {
        unsigned op = (unsigned)strtoul(key, NULL, 16);
        int reg = key[2] == '.' ? key[3] - '0' + 1 : 0;
        if (op < 256) {
            masks[op][reg] = (uint16_t)mask;
        }
    }
    fclose(f);
}

/* ---- MOO files ---- */

static uint8_t *load_gz(const char *path, size_t *len)
{
    gzFile f = gzopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t cap = 1 << 24, n = 0;
    uint8_t *buf = malloc(cap);
    int got;
    while ((got = gzread(f, buf + n, (unsigned)(cap - n))) > 0) {
        n += (size_t)got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    gzclose(f);
    *len = n;
    return buf;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* The registers in the file's order */
enum { R_AX, R_BX, R_CX, R_DX, R_CS, R_SS, R_DS, R_ES, R_SP, R_BP, R_SI, R_DI, R_IP, R_FLAGS, R_COUNT };
static const char *const reg_names[R_COUNT] = {
    "ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"
};

typedef struct {
    uint16_t regs[R_COUNT];
    uint16_t have;                  /* registers present */
    const uint8_t *ram;             /* entries: 4-byte address, value */
    uint32_t ram_n;
} state_t;

typedef struct {
    char name[128];
    uint8_t bytes[16];
    int nbytes;
    state_t init, fina;
    uint32_t cycles;
    uint32_t idx;
} test_t;

static void parse_state(const uint8_t *p, uint32_t len, state_t *s)
{
    memset(s, 0, sizeof(*s));
    const uint8_t *end = p + len;
    while (p + 8 <= end) {
        uint32_t clen = le32(p + 4);
        const uint8_t *d = p + 8;
        if (!memcmp(p, "REGS", 4)) {
            s->have = le16(d);
            const uint8_t *v = d + 2;
            for (int r = 0; r < R_COUNT; r++) {
                if (s->have & (1 << r)) {
                    s->regs[r] = le16(v);
                    v += 2;
                }
            }
        } else if (!memcmp(p, "RAM ", 4)) {
            s->ram_n = le32(d);
            s->ram = d + 4;
        }
        p = d + clen;
    }
}

static void parse_test(const uint8_t *p, uint32_t len, test_t *t)
{
    memset(t, 0, sizeof(*t));
    t->idx = le32(p);
    const uint8_t *end = p + len;
    p += 4;
    while (p + 8 <= end) {
        uint32_t clen = le32(p + 4);
        const uint8_t *d = p + 8;
        if (!memcmp(p, "NAME", 4)) {
            uint32_t n = le32(d);
            if (n > sizeof(t->name) - 1) {
                n = sizeof(t->name) - 1;
            }
            memcpy(t->name, d + 4, n);
        } else if (!memcmp(p, "BYTS", 4)) {
            t->nbytes = (int)le32(d);
            memcpy(t->bytes, d + 4, t->nbytes > 16 ? 16 : t->nbytes);
        } else if (!memcmp(p, "INIT", 4)) {
            parse_state(d, clen, &t->init);
        } else if (!memcmp(p, "FINA", 4)) {
            parse_state(d, clen, &t->fina);
        } else if (!memcmp(p, "CYCL", 4)) {
            t->cycles = le32(d);
        }
        p = d + clen;
    }
}

/* ---- Running ---- */

static bool undefined_too, verbose, timing, writable;
static bool halt_after;             /* the 286 suite: IP is past a HLT after the instruction */
static bool skip_div_flags;         /* -i: the 286's flags pushed by DIV/IDIV/AAM's interrupt 0 */
static int show_first;
static long total, failed, time_n;
static double time_err;

static void set_regs(const uint16_t *r)
{
    cpu.r.w[X86_AX] = r[R_AX];
    cpu.r.w[X86_BX] = r[R_BX];
    cpu.r.w[X86_CX] = r[R_CX];
    cpu.r.w[X86_DX] = r[R_DX];
    cpu.r.w[X86_SP] = r[R_SP];
    cpu.r.w[X86_BP] = r[R_BP];
    cpu.r.w[X86_SI] = r[R_SI];
    cpu.r.w[X86_DI] = r[R_DI];
    x86_load_seg(&cpu, X86_CS, r[R_CS]);
    x86_load_seg(&cpu, X86_SS, r[R_SS]);
    x86_load_seg(&cpu, X86_DS, r[R_DS]);
    x86_load_seg(&cpu, X86_ES, r[R_ES]);
    cpu.ip = r[R_IP];
    x86_set_flags(&cpu, r[R_FLAGS]);
}

static void get_regs(uint16_t *r)
{
    r[R_AX] = cpu.r.w[X86_AX];
    r[R_BX] = cpu.r.w[X86_BX];
    r[R_CX] = cpu.r.w[X86_CX];
    r[R_DX] = cpu.r.w[X86_DX];
    r[R_SP] = cpu.r.w[X86_SP];
    r[R_BP] = cpu.r.w[X86_BP];
    r[R_SI] = cpu.r.w[X86_SI];
    r[R_DI] = cpu.r.w[X86_DI];
    r[R_CS] = cpu.seg[X86_CS].sel;
    r[R_SS] = cpu.seg[X86_SS].sel;
    r[R_DS] = cpu.seg[X86_DS].sel;
    r[R_ES] = cpu.seg[X86_ES].sel;
    r[R_IP] = cpu.ip;
    r[R_FLAGS] = x86_get_flags(&cpu);
}

/* The expected value at addr: the final state's, else the initial one's,
 * else 0 (memory starts clear); -1 if neither lists it
 */
static int expected_at(const test_t *t, uint32_t a, bool *listed)
{
    for (uint32_t i = 0; i < t->fina.ram_n; i++) {
        if (le32(t->fina.ram + 5 * i) == a) {
            *listed = true;
            return t->fina.ram[5 * i + 4];
        }
    }
    for (uint32_t i = 0; i < t->init.ram_n; i++) {
        if (le32(t->init.ram + 5 * i) == a) {
            *listed = true;
            return t->init.ram[5 * i + 4];
        }
    }
    *listed = false;
    return 0;
}

static bool run_test(const test_t *t, uint16_t flags_mask, char *why, size_t len)
{
    x86_reset(&cpu);
    for (uint32_t i = 0; i < t->init.ram_n; i++) {
        ram[le32(t->init.ram + 5 * i) & (RAM_SIZE - 1)] = t->init.ram[5 * i + 4];
    }
    set_regs(t->init.regs);
    written_n = 0;
    int32_t clocks = x86_step(&cpu);

    bool ok = true;
    size_t n = 0;
    why[0] = 0;
    int k = 0;
    while (k < t->nbytes && ((t->bytes[k] & 0xE7) == 0x26 || (t->bytes[k] & 0xFC) == 0xF0)) {
        k++;                        /* the prefixes */
    }
    uint16_t got[R_COUNT];
    get_regs(got);
    if (halt_after && !(k < t->nbytes && t->bytes[k] == 0xF4)) {
        got[R_IP]++;                /* past the HLT after the instruction (HLT itself: once) */
    }
    for (int r = 0; r < R_COUNT; r++) {
        uint16_t want = (t->fina.have & (1 << r)) ? t->fina.regs[r] : t->init.regs[r];
        uint16_t mask = r == R_FLAGS && !undefined_too ? flags_mask : 0xFFFF;
        if ((got[r] ^ want) & mask) {
            ok = false;
            n += (size_t)snprintf(why + n, n < len ? len - n : 0, " %s=%04X(want %04X)", reg_names[r], got[r], want);
        }
    }
    /* The 286's divide error: where the flags were pushed (undefined then) */
    uint32_t flags_at = ~0u;
    if (skip_div_flags) {
        int op = t->bytes[k], reg = k + 1 < t->nbytes ? (t->bytes[k + 1] >> 3) & 7 : 0;
        if (op == 0xD4 || ((op == 0xF6 || op == 0xF7) && reg >= 6)) {
            flags_at = ((uint32_t)t->init.regs[R_SS] << 4) + (uint16_t)(t->init.regs[R_SP] - 2);
        }
    }
    for (uint32_t i = 0; i < t->fina.ram_n; i++) {
        uint32_t a = le32(t->fina.ram + 5 * i);
        if (a == flags_at || a == flags_at + 1) {
            continue;
        }
        if (ram[a & (RAM_SIZE - 1)] != t->fina.ram[5 * i + 4]) {
            ok = false;
            n += (size_t)snprintf(why + n, n < len ? len - n : 0, " [%05X]=%02X(want %02X)", a,
                                  ram[a & (RAM_SIZE - 1)], t->fina.ram[5 * i + 4]);
        }
    }
    for (int i = 0; i < written_n; i++) {
        bool listed;
        int want = expected_at(t, written[i], &listed);
        if (!listed && written_v[i] != want) {
            ok = false;
            n += (size_t)snprintf(why + n, n < len ? len - n : 0, " stray write [%05X]=%02X", written[i], written_v[i]);
        }
    }
    if (timing && t->cycles) {
        time_err += abs((int)clocks - (int)t->cycles);
        time_n++;
    }

    /* Clear what the test touched */
    for (uint32_t i = 0; i < t->init.ram_n; i++) {
        ram[le32(t->init.ram + 5 * i) & (RAM_SIZE - 1)] = 0;
    }
    for (uint32_t i = 0; i < t->fina.ram_n; i++) {
        ram[le32(t->fina.ram + 5 * i) & (RAM_SIZE - 1)] = 0;
    }
    for (int i = 0; i < written_n; i++) {
        ram[written[i]] = 0;
    }
    return ok;
}

static void run_file(const char *path)
{
    size_t len;
    uint8_t *buf = load_gz(path, &len);
    if (!buf || len < 8 || memcmp(buf, "MOO ", 4)) {
        fprintf(stderr, "%s: not a MOO file\n", path);
        free(buf);
        return;
    }
    /* The opcode and reg from the file name: "F6.4.MOO.gz" */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    unsigned op = (unsigned)strtoul(base, NULL, 16);
    int reg = base[2] == '.' && base[3] >= '0' && base[3] <= '7' ? base[3] - '0' + 1 : 0;
    uint16_t mask = masks[op & 0xFF][reg] != 0xFFFF ? masks[op & 0xFF][reg] : masks[op & 0xFF][0];

    long file_total = 0, file_failed = 0;
    const uint8_t *p = buf + 8 + le32(buf + 4);
    const uint8_t *end = buf + len;
    test_t t;
    char why[512];
    while (p + 8 <= end) {
        uint32_t clen = le32(p + 4);
        if (!memcmp(p, "TEST", 4)) {
            parse_test(p + 8, clen, &t);
            file_total++;
            if (!run_test(&t, mask, why, sizeof(why))) {
                file_failed++;
                if (verbose || file_failed <= show_first) {
                    printf("  %s #%u %s: (ax %04X flags %04X)%s\n", base, t.idx, t.name,
                           t.init.regs[R_AX], t.init.regs[R_FLAGS], why);
                }
            }
        }
        p += 8 + clen;
    }
    total += file_total;
    failed += file_failed;
    if (file_failed) {
        printf("%-14s %6ld/%ld failed\n", base, file_failed, file_total);
    }
    free(buf);
}

int main(int argc, char **argv)
{
    x86_model_t model = X86_8088;
    const char *mask_file = "build/masks_8088.txt";
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            i++;
            model = !strcmp(argv[i], "8086") ? X86_8086 : !strcmp(argv[i], "286") ? X86_80286 : X86_8088;
            mask_file = model == X86_8086 ? "build/masks_8086.txt" :
                        model == X86_80286 ? "build/masks_80286.txt" : "build/masks_8088.txt";
            halt_after = model == X86_80286;
        } else if (!strcmp(argv[i], "-i")) {
            skip_div_flags = true;
        } else if (!strcmp(argv[i], "-u")) {
            undefined_too = true;
        } else if (!strcmp(argv[i], "-v")) {
            verbose = true;
        } else if (!strcmp(argv[i], "-t")) {
            timing = true;
        } else if (!strcmp(argv[i], "-w")) {
            writable = true;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            show_first = atoi(argv[++i]);
        }
    }
    load_masks(mask_file);
    ram = aligned_alloc(4096, RAM_SIZE);
    memset(ram, 0, RAM_SIZE);
    for (uint32_t p = 0; p < RAM_SIZE >> 12; p++) {
        page[p] = (uintptr_t)(ram + (p << 12)) | (writable ? 0 : X86_NO_WRITE);
    }
    static const x86_bus_t bus = {
        .read = bus_read, .write = bus_write, .in = io_in, .out = io_out, .inta = inta,
    };
    x86_init(&cpu, model, &bus, page);
    for (; i < argc; i++) {
        run_file(argv[i]);
    }
    printf("%ld tests, %ld failed", total, failed);
    if (time_n) {
        printf(", clocks off by %.2f on average", time_err / time_n);
    }
    printf("\n");
    return failed != 0;
}
