/*
 * The SingleStepTests 65816 suite (github.com/SingleStepTests/65816): v1/XX.e.json
 * and XX.n.json, 10,000 tests per opcode and mode (MVN/MVP: whole moves).
 * Each test runs twice on a flat 16 MB:
 *   - fast: page tables into the memory; the final registers, RAM and the
 *     cycle count (the length of the test's "cycles" list) must match;
 *   - bus: no pages, every access through the callbacks, which add one
 *     wait cycle each; the accesses must be the test's cycles with VDA or
 *     VPA, in order, with their values, and the count must grow by them.
 * MVN and MVP step until the PC leaves the instruction. WAI and STP list a
 * fourth cycle with nothing on the bus (the stopped CPU): not counted.
 *
 *   ssts [-v] <file.json | folder> ...   (-v: every failure, not just the first)
 */
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w65816.h"

#define MAX_RAM     8192
#define MAX_CYCLES  16384

typedef struct {
    long pc, s, p, a, x, y, dbr, d, pbr, e;
    int nram;
    long ram[MAX_RAM][2];
} state_t;

typedef struct {
    long addr, value;               /* -1: null */
    char flags[16];
} cycle_t;

typedef struct {
    char name[64];
    state_t init, fin;
    int ncycles;
    cycle_t cycles[MAX_CYCLES];
} test_t;

/* ---- A reader for just this JSON ---- */

static const char *s_buf, *s_p, *s_end, *s_file;

static void fail(const char *what)
{
    fprintf(stderr, "%s: %s at byte %ld\n", s_file, what, (long)(s_p - s_buf));
    exit(2);
}

static void ws(void)
{
    while (s_p < s_end && isspace((unsigned char)*s_p)) {
        s_p++;
    }
}

static int lit(char ch)
{
    ws();
    if (s_p < s_end && *s_p == ch) {
        s_p++;
        return 1;
    }
    return 0;
}

static void need(char ch)
{
    if (!lit(ch)) {
        char m[32];
        snprintf(m, sizeof(m), "expected '%c'", ch);
        fail(m);
    }
}

/* An integer, or null (-1) */
static long number(void)
{
    ws();
    if (s_end - s_p >= 4 && memcmp(s_p, "null", 4) == 0) {
        s_p += 4;
        return -1;
    }
    char *e;
    long v = strtol(s_p, &e, 10);
    if (e == s_p) {
        fail("expected a number");
    }
    s_p = e;
    return v;
}

static void string(char *buf, int size)
{
    need('"');
    int n = 0;
    while (s_p < s_end && *s_p != '"') {
        if (*s_p == '\\') {
            s_p++;
        }
        if (n < size - 1) {
            buf[n++] = *s_p;
        }
        s_p++;
    }
    buf[n] = 0;
    need('"');
}

static void skip_value(void)
{
    ws();
    if (*s_p == '"') {
        char tmp[256];
        string(tmp, sizeof(tmp));
    } else if (*s_p == '[' || *s_p == '{') {
        char close = *s_p == '[' ? ']' : '}';
        s_p++;
        if (lit(close)) {
            return;
        }
        do {
            if (close == '}') {
                char key[64];
                string(key, sizeof(key));
                need(':');
            }
            skip_value();
        } while (lit(','));
        need(close);
    } else {
        number();
    }
}

static void read_state(state_t *st)
{
    need('{');
    st->nram = 0;
    do {
        char key[16];
        string(key, sizeof(key));
        need(':');
        if (strcmp(key, "ram") == 0) {
            need('[');
            if (!lit(']')) {
                do {
                    need('[');
                    long a = number();
                    need(',');
                    long v = number();
                    need(']');
                    if (st->nram == MAX_RAM) {
                        fail("too much RAM");
                    }
                    st->ram[st->nram][0] = a;
                    st->ram[st->nram][1] = v;
                    st->nram++;
                } while (lit(','));
                need(']');
            }
            continue;
        }
        static const char *const names[] = { "pc", "s", "p", "a", "x", "y", "dbr", "d", "pbr", "e" };
        long *fields[] = { &st->pc, &st->s, &st->p, &st->a, &st->x, &st->y, &st->dbr, &st->d, &st->pbr, &st->e };
        int i = 0;
        while (i < 10 && strcmp(key, names[i]) != 0) {
            i++;
        }
        if (i < 10) {
            *fields[i] = number();
        } else {
            skip_value();
        }
    } while (lit(','));
    need('}');
}

static void read_cycles(test_t *t)
{
    t->ncycles = 0;
    need('[');
    if (lit(']')) {
        return;
    }
    do {
        if (t->ncycles == MAX_CYCLES) {
            fail("too many cycles");
        }
        cycle_t *cy = &t->cycles[t->ncycles++];
        need('[');
        cy->addr = number();
        need(',');
        cy->value = number();
        need(',');
        string(cy->flags, sizeof(cy->flags));
        need(']');
    } while (lit(','));
    need(']');
}

/* The next test, or 0 at the end of the file */
static int read_test(test_t *t)
{
    if (!lit('{')) {
        return 0;
    }
    t->name[0] = 0;
    do {
        char key[16];
        string(key, sizeof(key));
        need(':');
        if (strcmp(key, "name") == 0) {
            string(t->name, sizeof(t->name));
        } else if (strcmp(key, "initial") == 0) {
            read_state(&t->init);
        } else if (strcmp(key, "final") == 0) {
            read_state(&t->fin);
        } else if (strcmp(key, "cycles") == 0) {
            read_cycles(t);
        } else {
            skip_value();
        }
    } while (lit(','));
    need('}');
    lit(',');
    return 1;
}

/* ---- Running ---- */

static uint8_t *s_mem;                  /* 16 MB */
static uint8_t *s_pages[65536];         /* into s_mem */
static uint8_t *s_none[65536];          /* none: all through the bus */

typedef struct {
    uint32_t addr;
    uint8_t v;
    char write;
} access_t;

static access_t s_acc[MAX_CYCLES];
static int s_nacc;

static uint8_t bus_read(void *ctx, uint32_t a)
{
    w65816_t *cpu = ctx;
    if (a > 0xFFFFFF) {
        fprintf(stderr, "read of $%X\n", a);
        exit(2);
    }
    if (s_nacc < MAX_CYCLES) {
        s_acc[s_nacc++] = (access_t){ a, s_mem[a], 'r' };
    }
    cpu->penalty++;
    return s_mem[a];
}

static void bus_write(void *ctx, uint32_t a, uint8_t v)
{
    w65816_t *cpu = ctx;
    if (a > 0xFFFFFF) {
        fprintf(stderr, "write of $%X\n", a);
        exit(2);
    }
    if (s_nacc < MAX_CYCLES) {
        s_acc[s_nacc++] = (access_t){ a, v, 'w' };
    }
    cpu->penalty++;
    s_mem[a] = v;
}

static char s_why[512];

static void why(const char *fmt, ...)
{
    if (s_why[0]) {
        return;                         /* the first difference */
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_why, sizeof(s_why), fmt, ap);
    va_end(ap);
}

/* The instruction's cycles: the list less the stopped CPU's (WAI, STP) */
static int expected_cycles(const test_t *t)
{
    int n = t->ncycles;
    while (n > 0 && t->cycles[n - 1].addr < 0 && t->cycles[n - 1].value < 0 &&
           strcmp(t->cycles[n - 1].flags, "--------") == 0) {
        n--;
    }
    return n;
}

static void set_ram(const state_t *st, int clear)
{
    for (int i = 0; i < st->nram; i++) {
        s_mem[st->ram[i][0] & 0xFFFFFF] = clear ? 0 : (uint8_t)st->ram[i][1];
    }
}

/* Run the test (bus: through the callbacks); 1 if it passed */
static int run(w65816_t *cpu, const test_t *t, int bus)
{
    s_why[0] = 0;
    set_ram(&t->init, 0);
    const state_t *in = &t->init, *out = &t->fin;
    cpu->pc = in->pc;
    cpu->s = in->s;
    cpu->p = in->p;
    cpu->a = in->a;
    cpu->x = in->x;
    cpu->y = in->y;
    cpu->dbr = in->dbr;
    cpu->d = in->d;
    cpu->pbr = in->pbr;
    cpu->e = in->e;
    cpu->stopped = cpu->waiting = cpu->irq = cpu->nmi = false;
    cpu->cycles = 0;
    cpu->rd = cpu->wr = bus ? s_none : s_pages;
    s_nacc = 0;

    uint8_t op = s_mem[(in->pbr << 16) | in->pc];
    long n = 0;
    int steps = 0;
    do {
        n += w65816_step(cpu);
    } while ((op == 0x44 || op == 0x54) && cpu->pc == in->pc && cpu->pbr == in->pbr && ++steps < 70000);
    if ((long)cpu->cycles != n) {
        why("w65816_step() returned %ld cycles, cpu->cycles is %llu", n, (unsigned long long)cpu->cycles);
    }

    static const char *const names[] = { "pc", "s", "p", "a", "x", "y", "dbr", "d", "pbr", "e" };
    long got[] = { cpu->pc, cpu->s, cpu->p, cpu->a, cpu->x, cpu->y, cpu->dbr, cpu->d, cpu->pbr, cpu->e };
    long want[] = { out->pc, out->s, out->p, out->a, out->x, out->y, out->dbr, out->d, out->pbr, out->e };
    for (int i = 0; i < 10; i++) {
        if (got[i] != want[i]) {
            why("%s is $%lX, expected $%lX", names[i], got[i], want[i]);
        }
    }
    for (int i = 0; i < out->nram; i++) {
        uint32_t a = out->ram[i][0];
        if (s_mem[a] != out->ram[i][1]) {
            why("RAM $%06X is $%02X, expected $%02lX", a, s_mem[a], out->ram[i][1]);
        }
    }
    int cycles = expected_cycles(t);
    if (bus) {
        int k = 0;
        for (int i = 0; i < t->ncycles; i++) {
            const cycle_t *cy = &t->cycles[i];
            if (cy->flags[0] != 'd' && cy->flags[1] != 'p') {
                continue;               /* VDA = VPA = 0: internal */
            }
            if (k >= s_nacc) {
                why("bus: cycle %d ([%ld, %ld, %s]) missing", i, cy->addr, cy->value, cy->flags);
                break;
            }
            const access_t *ac = &s_acc[k++];
            if (ac->addr != cy->addr || ac->write != cy->flags[3] || (cy->value >= 0 && ac->v != cy->value)) {
                why("bus: cycle %d is %c $%06X $%02X, expected [%ld, %ld, %s]", i, ac->write, ac->addr, ac->v,
                    cy->addr, cy->value, cy->flags);
                break;
            }
        }
        if (k < s_nacc) {
            why("bus: %d accesses more than expected, the first %c $%06X", s_nacc - k, s_acc[k].write,
                s_acc[k].addr);
        }
        cycles += s_nacc;               /* a wait cycle each */
    }
    if (n != cycles) {
        why("%ld cycles, expected %d", n, cycles);
    }
    set_ram(&t->init, 1);
    set_ram(&t->fin, 1);
    return s_why[0] == 0;
}

static void print_state(const char *label, const state_t *st)
{
    printf("      %s pc=%02lX:%04lX s=%04lX p=%02lX a=%04lX x=%04lX y=%04lX dbr=%02lX d=%04lX e=%ld\n", label,
           st->pbr, st->pc, st->s, st->p, st->a, st->x, st->y, st->dbr, st->d, st->e);
}

static void report(w65816_t *cpu, const test_t *t, const char *pass)
{
    char dis[32];
    cpu->rd = s_pages;
    set_ram(&t->init, 0);
    cpu->pbr = t->init.pbr;
    cpu->p = t->init.p;
    cpu->e = t->init.e;
    w65816_disassemble(cpu, t->init.pbr << 16 | t->init.pc, dis, sizeof(dis));
    set_ram(&t->init, 1);
    printf("    %s (%s, %s): %s\n", t->name, dis, pass, s_why);
    print_state("initial", &t->init);
    print_state("final  ", &t->fin);
}

static long s_total, s_failed, s_files_failed;
static int s_verbose;

static void run_file(const char *path, test_t *t, w65816_t *cpu)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf == NULL || fread(buf, 1, size, f) != (size_t)size) {
        perror(path);
        exit(2);
    }
    fclose(f);
    buf[size] = 0;
    s_file = path;
    s_buf = s_p = buf;
    s_end = buf + size;
    need('[');
    long tests = 0, fast_ok = 0, bus_ok = 0;
    int shown = 0;
    while (read_test(t)) {
        tests++;
        int ok = run(cpu, t, 0);
        fast_ok += ok;
        if (!ok && (s_verbose || shown++ == 0)) {
            report(cpu, t, "fast");
        }
        ok = run(cpu, t, 1);
        bus_ok += ok;
        if (!ok && (s_verbose || shown++ == 0)) {
            report(cpu, t, "bus");
        }
    }
    free(buf);
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    int bad = fast_ok != tests || bus_ok != tests;
    printf("%-10s %5ld/%ld passed, bus %5ld/%ld%s\n", name, fast_ok, tests, bus_ok, tests, bad ? "  FAILED" : "");
    fflush(stdout);
    s_total += tests;
    s_failed += (tests - fast_ok) + (tests - bus_ok);
    s_files_failed += bad;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
    s_mem = calloc(1, 1 << 24);
    test_t *t = malloc(sizeof(*t));
    if (s_mem == NULL || t == NULL) {
        return 2;
    }
    for (int i = 0; i < 65536; i++) {
        s_pages[i] = s_mem + i * 256;
    }
    w65816_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.bus = (w65816_bus_t){ .read = bus_read, .write = bus_write, .ctx = &cpu };
    cpu.rd = cpu.wr = s_pages;
    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            s_verbose = 1;
            continue;
        }
        DIR *dir = opendir(argv[i]);
        if (dir == NULL) {
            run_file(argv[i], t, &cpu);
            files++;
            continue;
        }
        char **names = NULL;
        int n = 0;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len > 5 && strcmp(de->d_name + len - 5, ".json") == 0) {
                names = realloc(names, (n + 1) * sizeof(*names));
                names[n] = malloc(strlen(argv[i]) + len + 2);
                sprintf(names[n++], "%s/%s", argv[i], de->d_name);
            }
        }
        closedir(dir);
        qsort(names, n, sizeof(*names), by_name);
        for (int k = 0; k < n; k++) {
            run_file(names[k], t, &cpu);
            free(names[k]);
            files++;
        }
        free(names);
    }
    if (files == 0) {
        fprintf(stderr, "usage: %s [-v] <file.json | folder> ...\n", argv[0]);
        return 2;
    }
    printf("%d files, %ld tests (x2: fast and bus), %ld failed, %ld files with failures\n", files, s_total,
           s_failed, s_files_failed);
    return s_failed != 0;
}
