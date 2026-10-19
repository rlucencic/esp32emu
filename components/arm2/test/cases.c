/*
 * ARM2 core: instruction cases, mostly what makes the ARM2 an ARM2 (R15
 * with the PSR, the modes and their registers, exceptions, user-mode
 * transfers, aborts) and the barrel shifter's corners. The ordinary data
 * processing, multiplies and transfers are also checked against another
 * ARM emulator at random (diff.py).
 *
 *   build/cases [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm2.h"

/* ---- A machine: 1 MB of RAM at 0, the page table mapping it ---- */

#define MEM_SIZE    (1024 * 1024)

static uint8_t *s_mem;
static uintptr_t s_page[ARM2_PAGES];
static arm2_t s_cpu;
static bool s_verbose;
static int s_failed, s_run;
static uint32_t s_abort_at = ~0u;           /* bus accesses here abort */
static uint32_t s_io_last_write, s_io_last_addr;
static bool s_end_on_write;                 /* an I/O write ends the run */

static uint32_t bus_read(void *ctx, uint32_t addr, bool byte, bool *abort)
{
    (void)ctx;
    (void)byte;
    if (addr >> 12 == s_abort_at >> 12) {
        *abort = true;
        return 0xDEADDEAD;
    }
    return 0x12345678;                      /* the "I/O" beyond RAM */
}

static void bus_write(void *ctx, uint32_t addr, uint32_t v, bool byte, bool *abort)
{
    (void)ctx;
    (void)byte;
    if (addr >> 12 == s_abort_at >> 12) {
        *abort = true;
        return;
    }
    s_io_last_addr = addr;
    s_io_last_write = v;
    if (s_end_on_write) {
        arm2_end_run(&s_cpu);
    }
}

static void setup(void)
{
    if (!s_mem) {
        s_mem = aligned_alloc(4096, MEM_SIZE);
    }
    memset(s_mem, 0, MEM_SIZE);
    memset(s_page, 0, sizeof(s_page));
    for (uint32_t p = 0; p < MEM_SIZE / ARM2_PAGE_SIZE; p++) {
        s_page[p] = (uintptr_t)(s_mem + p * ARM2_PAGE_SIZE) | ARM2_USER_R | ARM2_USER_W | ARM2_PRIV_R | ARM2_PRIV_W;
    }
    /* 0x80000-0x80FFF: privileged only; 0x81000: user read only */
    s_page[0x80] &= ~(uintptr_t)(ARM2_USER_R | ARM2_USER_W);
    s_page[0x81] &= ~(uintptr_t)ARM2_USER_W;
    s_abort_at = ~0u;
    arm2_bus_t bus = { .read = bus_read, .write = bus_write };
    arm2_init(&s_cpu, &bus, s_page);
    arm2_reset(&s_cpu);
}

static void poke32(uint32_t addr, uint32_t v)
{
    memcpy(s_mem + addr, &v, 4);
}

static uint32_t peek32(uint32_t addr)
{
    uint32_t v;
    memcpy(&v, s_mem + addr, 4);
    return v;
}

/* Code at addr; the CPU starts there in `psr`'s mode */
static void load(uint32_t addr, const uint32_t *code, int n, uint32_t psr)
{
    for (int i = 0; i < n; i++) {
        poke32(addr + 4 * i, code[i]);
    }
    arm2_set_psr(&s_cpu, psr);
    s_cpu.pc = addr;
}

static int step(int n)
{
    int ticks = 0;
    for (int i = 0; i < n; i++) {
        s_cpu.left = 0;
        ticks += arm2_run(&s_cpu, 1);
    }
    return ticks;
}

static void check(const char *name, uint32_t got, uint32_t want)
{
    s_run++;
    if (got != want) {
        s_failed++;
        printf("FAIL %s: got &%08X, want &%08X\n", name, (unsigned)got, (unsigned)want);
    } else if (s_verbose) {
        printf("ok   %s\n", name);
    }
}

/* ---- Encoding ---- */

#define AL  0xE0000000u
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, PC };
enum { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };
enum { LSL, LSR, ASR, ROR };

static uint32_t imm(uint32_t v)
{
    for (unsigned rot = 0; rot < 32; rot += 2) {
        uint32_t x = rot ? (v << rot) | (v >> (32 - rot)) : v;
        if (x < 256) {
            return (1u << 25) | ((rot / 2) << 8) | x;
        }
    }
    fprintf(stderr, "immediate %08X can't be encoded\n", (unsigned)v);
    exit(2);
}

static uint32_t shi(unsigned rm, unsigned type, unsigned amount)
{
    return ((amount & 31) << 7) | (type << 5) | rm;
}

static uint32_t shr(unsigned rm, unsigned type, unsigned rs)
{
    return (rs << 8) | (type << 5) | 0x10 | rm;
}

static uint32_t dp(unsigned opc, bool s, unsigned rd, unsigned rn, uint32_t op2)
{
    return AL | (opc << 21) | (s ? 1u << 20 : 0) | (rn << 16) | (rd << 12) | op2;
}

static uint32_t ldr(bool load, bool byte, unsigned rd, unsigned rn, int off, bool pre, bool wb)
{
    return AL | (1u << 26) | (pre ? 1u << 24 : 0) | (off >= 0 ? 1u << 23 : 0) | (byte ? 1u << 22 : 0) |
           (wb ? 1u << 21 : 0) | (load ? 1u << 20 : 0) | (rn << 16) | (rd << 12) | (uint32_t)abs(off);
}

static uint32_t ldm(bool load, unsigned rn, uint32_t list, bool pre, bool up, bool wb, bool s)
{
    return AL | (4u << 25) | (pre ? 1u << 24 : 0) | (up ? 1u << 23 : 0) | (s ? 1u << 22 : 0) |
           (wb ? 1u << 21 : 0) | (load ? 1u << 20 : 0) | (rn << 16) | list;
}

#define SWI(n)      (AL | 0x0F000000u | (n))
#define B(off)      (AL | 0x0A000000u | (((uint32_t)(off) >> 2) & 0xFFFFFF))
#define BL(off)     (AL | 0x0B000000u | (((uint32_t)(off) >> 2) & 0xFFFFFF))
#define MUL(rd, rm, rs)         (AL | ((rd) << 16) | ((rs) << 8) | 0x90 | (rm))
#define MLA(rd, rm, rs, rn)     (AL | (1u << 21) | ((rd) << 16) | ((rn) << 12) | ((rs) << 8) | 0x90 | (rm))
#define COND(c, op)             (((op) & 0x0FFFFFFFu) | ((uint32_t)(c) << 28))

#define SVC         (ARM2_SVC | ARM2_I | ARM2_F)
#define USR         ARM2_USR

/* ---- Cases ---- */

static void r15_operands(void)
{
    /* As Rn the PC + 8 alone; as Rm with the PSR; + 12 shifted by a register */
    setup();
    uint32_t code[] = {
        dp(ADD, false, R0, PC, imm(0)),
        dp(MOV, false, R1, 0, shi(PC, LSL, 0)),
        dp(MOV, false, R2, 0, shr(PC, LSL, R9)),
        dp(ADD, false, R3, PC, shr(R9, LSL, R9)),
    };
    load(0x8000, code, 4, SVC | ARM2_N | ARM2_C);
    s_cpu.r[9] = 0;
    step(4);
    check("ADD R0,PC,#0", s_cpu.r[0], 0x8008);
    check("MOV R1,PC", s_cpu.r[1], 0x800C | SVC | ARM2_N | ARM2_C);
    check("MOV R2,PC,LSL R9", s_cpu.r[2], 0x8014 | SVC | ARM2_N | ARM2_C);
    check("ADD R3,PC,R9,LSL R9", s_cpu.r[3], 0x8018);
}

static void r15_stores(void)
{
    setup();
    uint32_t code[] = {
        ldr(false, false, PC, R0, 0, true, false),       /* STR PC,[R0] */
        ldm(false, R1, 1u << PC, false, true, false, false),    /* STMIA R1,{PC} */
    };
    load(0x8000, code, 2, SVC | ARM2_Z);
    s_cpu.r[0] = 0x100;
    s_cpu.r[1] = 0x200;
    step(2);
    check("STR PC stores PC+12 and PSR", peek32(0x100), 0x800C | SVC | ARM2_Z);
    check("STM {PC} stores PC+12 and PSR", peek32(0x200), 0x8010 | SVC | ARM2_Z);
}

static void mode_changes(void)
{
    setup();
    /* TEQP PC,#0 from SVC: user mode, interrupts on, R13/R14 banked */
    uint32_t code[] = {
        dp(TEQ, true, PC, PC, imm(0)),
        dp(MOV, false, R13, 0, imm(0x1200)),
        SWI(0x11),
    };
    load(0x8000, code, 3, SVC);
    s_cpu.r[13] = 0xAAAA;
    s_cpu.r[14] = 0xBBBB;
    step(2);
    check("TEQP PC,#0: user mode", s_cpu.psr, USR);
    check("user R13", s_cpu.r[13], 0x1200);
    check("SVC R13 banked", arm2_get_reg(&s_cpu, ARM2_SVC, 13), 0xAAAA);
    poke32(0x08, dp(MOV, true, PC, 0, shi(R14, LSL, 0)));   /* the SWI handler: MOVS PC,R14 */
    step(1);
    check("SWI: SVC mode, I set", s_cpu.psr, ARM2_SVC | ARM2_I);
    check("SWI: vector", s_cpu.pc, 0x08);
    check("SWI: R14_svc = next with the old PSR", s_cpu.r[14], 0x800C | USR);
    check("SWI: SVC's R13 back", s_cpu.r[13], 0xAAAA);
    step(1);
    check("MOVS PC,R14: user mode again", s_cpu.psr, USR);
    check("MOVS PC,R14: PC", s_cpu.pc, 0x800C);
    check("MOVS PC,R14: user R13", s_cpu.r[13], 0x1200);

    /* In user mode MOVS PC changes the flags only, TEQP too */
    setup();
    uint32_t code2[] = {
        dp(MOV, true, PC, 0, shi(R0, LSL, 0)),
    };
    load(0x8000, code2, 1, USR);
    s_cpu.r[0] = 0x9000 | ARM2_N | ARM2_I | ARM2_SVC;
    step(1);
    check("user MOVS PC: flags only", s_cpu.psr, USR | ARM2_N);
    check("user MOVS PC: PC", s_cpu.pc, 0x9000);
}

static void ldm_stm_psr_and_user(void)
{
    setup();
    /* STMDB R13!,{R13,R14}^ in SVC: the user R13, R14 */
    uint32_t code[] = {
        ldm(false, R0, (1u << R13) | (1u << R14), true, false, false, true),
        ldm(true, R1, (1u << R2) | (1u << PC), false, true, true, true),     /* LDMIA R1!,{R2,PC}^ */
    };
    load(0x8000, code, 2, USR);
    s_cpu.r[13] = 0x1313;
    s_cpu.r[14] = 0x1414;
    arm2_set_psr(&s_cpu, SVC);
    s_cpu.r[13] = 0x2000;
    s_cpu.r[14] = 0x3000;
    s_cpu.r[0] = 0x400;
    s_cpu.r[1] = 0x500;
    poke32(0x500, 0x77);
    poke32(0x504, 0xA000 | USR | ARM2_C);
    step(1);
    check("STM ^ user R13", peek32(0x3F8), 0x1313);
    check("STM ^ user R14", peek32(0x3FC), 0x1414);
    step(1);
    check("LDM ^ with PC: user mode", s_cpu.psr, USR | ARM2_C);
    check("LDM ^ with PC: PC", s_cpu.pc, 0xA000);
    check("LDM ^ with PC: R2", s_cpu.r[2], 0x77);
    check("LDM ^ with PC: writeback (SVC's R1)", s_cpu.r[1], 0x508);
    check("LDM ^: back in user R13", s_cpu.r[13], 0x1313);

    /* LDM ^ without the PC: into the user registers */
    setup();
    uint32_t code2[] = { ldm(true, R0, (1u << R13) | (1u << R14), false, true, false, true) };
    load(0x8000, code2, 1, SVC);
    s_cpu.r[13] = 0x55;
    s_cpu.r[0] = 0x600;
    poke32(0x600, 0xAAAA0013);
    poke32(0x604, 0xAAAA0014);
    step(1);
    check("LDM ^ no PC: SVC R13 kept", s_cpu.r[13], 0x55);
    check("LDM ^ no PC: user R13", arm2_get_reg(&s_cpu, ARM2_USR, 13), 0xAAAA0013);
    check("LDM ^ no PC: user R14", arm2_get_reg(&s_cpu, ARM2_USR, 14), 0xAAAA0014);
}

static void block_writeback(void)
{
    setup();
    /* STMIA R0!,{R0,R1}: R0 first in the list stores the old base;
     * STMIA R2!,{R1,R2}: the new one
     */
    uint32_t code[] = {
        ldm(false, R0, (1u << R0) | (1u << R1), false, true, true, false),
        ldm(false, R2, (1u << R1) | (1u << R2), false, true, true, false),
        ldm(true, R3, (1u << R3) | (1u << R4), false, true, true, false),    /* LDMIA R3!,{R3,R4} */
    };
    load(0x8000, code, 3, SVC);
    s_cpu.r[0] = 0x100;
    s_cpu.r[1] = 0x11;
    s_cpu.r[2] = 0x200;
    s_cpu.r[3] = 0x300;
    poke32(0x300, 0x3333);
    poke32(0x304, 0x4444);
    step(3);
    check("STM base first: old base", peek32(0x100), 0x100);
    check("STM base first: writeback", s_cpu.r[0], 0x108);
    check("STM base second: new base", peek32(0x204), 0x208);
    check("LDM base in list: loaded wins", s_cpu.r[3], 0x3333);
    check("LDM R4", s_cpu.r[4], 0x4444);

    /* The four addressing modes */
    setup();
    uint32_t code2[] = {
        ldm(false, R0, 0x0006, false, true, false, false),  /* IA */
        ldm(false, R3, 0x0006, true, true, false, false),   /* IB */
        ldm(false, R4, 0x0006, false, false, false, false), /* DA */
        ldm(false, R5, 0x0006, true, false, false, false),  /* DB */
    };
    load(0x8000, code2, 4, SVC);
    s_cpu.r[1] = 0x1111;
    s_cpu.r[2] = 0x2222;
    s_cpu.r[0] = 0x100;
    s_cpu.r[3] = 0x200;
    s_cpu.r[4] = 0x300;
    s_cpu.r[5] = 0x400;
    step(4);
    check("STMIA", peek32(0x100) == 0x1111 && peek32(0x104) == 0x2222, 1);
    check("STMIB", peek32(0x204) == 0x1111 && peek32(0x208) == 0x2222, 1);
    check("STMDA", peek32(0x2FC) == 0x1111 && peek32(0x300) == 0x2222, 1);
    check("STMDB", peek32(0x3F8) == 0x1111 && peek32(0x3FC) == 0x2222, 1);

    /* An unaligned base: the aligned words, not rotated (only LDR rotates);
     * blocks across a page boundary (a word at a time) as within one
     */
    setup();
    uint32_t code3[] = {
        ldm(true, R0, (1u << R1) | (1u << R2), false, true, false, false),                /* LDMIA R0,{R1,R2} */
        ldm(false, R3, (1u << R1) | (1u << R2) | (1u << R4), false, true, true, false),   /* STMIA R3!,{R1,R2,R4} */
        ldm(true, R5, (1u << R6) | (1u << R7) | (1u << R8), false, true, false, false),   /* LDMIA R5,{R6-R8} */
        ldm(false, R9, (1u << R1) | (1u << R2) | (1u << R4), false, true, true, false),   /* STMIA R9!,{R1,R2,R4} */
        ldm(true, R10, (1u << R6) | (1u << R7) | (1u << R8), false, true, false, false),  /* LDMIA R10,{R6-R8} */
    };
    load(0x8000, code3, 5, SVC);
    s_cpu.r[0] = 0x102;
    poke32(0x100, 0x11111111);
    poke32(0x104, 0x22222222);
    s_cpu.r[3] = 0xFFC;
    s_cpu.r[4] = 0x4444;
    s_cpu.r[5] = 0xFFC;
    s_cpu.r[9] = 0x400;
    s_cpu.r[10] = 0x400;
    step(1);
    check("LDM unaligned: not rotated", s_cpu.r[1], 0x11111111);
    check("LDM unaligned: the next word", s_cpu.r[2], 0x22222222);
    int across_stm = step(1);
    check("STM across a page", peek32(0xFFC) == 0x11111111 && peek32(0x1000) == 0x22222222 &&
          peek32(0x1004) == 0x4444, 1);
    check("STM across a page: writeback", s_cpu.r[3], 0x1008);
    int across_ldm = step(1);
    check("LDM across a page", s_cpu.r[6] == 0x11111111 && s_cpu.r[7] == 0x22222222 && s_cpu.r[8] == 0x4444, 1);
    check("STM ticks across a page as within one", across_stm, step(1));
    check("LDM ticks across a page as within one", across_ldm, step(1));
    check("STM ticks (1N + 2S + 1I, and the fetch)", across_stm, 6);
}

static void branches(void)
{
    setup();
    uint32_t code[] = { BL(0x100 - 8) };
    load(0x8000, code, 1, SVC | ARM2_V);
    int t = step(1);
    check("BL target", s_cpu.pc, 0x8100);
    check("BL R14 = next with the PSR", s_cpu.r[14], 0x8004 | SVC | ARM2_V);
    check("B ticks (2S + 1N)", t, 4);
    setup();
    uint32_t back[] = { B(-8) };
    load(0x8000, back, 1, SVC);
    step(1);
    check("B . loops", s_cpu.pc, 0x8000);
    /* The PC wraps within 26 bits */
    setup();
    uint32_t wrap[] = { B(-0x10000) };
    load(0x8000, wrap, 1, SVC);
    step(1);
    check("B wraps", s_cpu.pc, (0x8008 - 0x10000) & ARM2_PC);
}

static void unaligned_and_bytes(void)
{
    setup();
    uint32_t code[] = {
        ldr(true, false, R0, R9, 1, true, false),
        ldr(true, false, R1, R9, 2, true, false),
        ldr(true, true, R2, R9, 3, true, false),
        ldr(false, true, R3, R9, 0x101, true, false),
        ldr(false, false, R3, R9, 0x203, true, false),
    };
    load(0x8000, code, 5, SVC);
    s_cpu.r[9] = 0x100;
    s_cpu.r[3] = 0xCAFEBABE;
    poke32(0x100, 0x44332211);
    step(5);
    check("LDR unaligned +1 rotates", s_cpu.r[0], 0x11443322);
    check("LDR unaligned +2 rotates", s_cpu.r[1], 0x22114433);
    check("LDRB", s_cpu.r[2], 0x44);
    check("STRB", peek32(0x200), 0xBE00);
    check("STR unaligned writes the aligned word", peek32(0x300), 0xCAFEBABE);
}

static void post_index_and_writeback(void)
{
    setup();
    uint32_t code[] = {
        ldr(true, false, R0, R1, 4, false, false),     /* LDR R0,[R1],#4 */
        ldr(true, false, R2, R3, -4, true, true),      /* LDR R2,[R3,#-4]! */
        ldr(true, false, R4, R4, 0, true, true),       /* LDR R4,[R4]! : the load wins */
    };
    load(0x8000, code, 3, SVC);
    s_cpu.r[1] = 0x100;
    s_cpu.r[3] = 0x204;
    s_cpu.r[4] = 0x300;
    poke32(0x100, 1);
    poke32(0x200, 2);
    poke32(0x300, 3);
    step(3);
    check("post-index load", s_cpu.r[0], 1);
    check("post-index writeback", s_cpu.r[1], 0x104);
    check("pre-index writeback load", s_cpu.r[2], 2);
    check("pre-index writeback", s_cpu.r[3], 0x200);
    check("load into the base wins", s_cpu.r[4], 3);
}

static void privileges_and_aborts(void)
{
    /* LDRT in SVC mode from a privileged page: the bus, which aborts */
    setup();
    s_abort_at = 0x80000;
    uint32_t code[] = { ldr(true, false, R0, R1, 0, false, true) };    /* LDRT R0,[R1] */
    load(0x8000, code, 1, SVC);
    s_cpu.r[1] = 0x80000;
    s_cpu.r[0] = 0x5555;
    step(1);
    check("LDRT abort: vector", s_cpu.pc, ARM2_VEC_DATA_ABORT);
    check("LDRT abort: R14 = PC + 8", s_cpu.r[14] & ARM2_PC, 0x8008);
    check("LDRT abort: R0 kept", s_cpu.r[0], 0x5555);

    /* Plain LDR in SVC mode from that page works */
    setup();
    poke32(0x80000, 0xFEED);
    uint32_t code2[] = { ldr(true, false, R0, R1, 0, true, false) };
    load(0x8000, code2, 1, SVC);
    s_cpu.r[1] = 0x80000;
    step(1);
    check("SVC reads a privileged page", s_cpu.r[0], 0xFEED);

    /* User mode writing a read-only page: the bus (which takes it here) */
    setup();
    uint32_t code3[] = { ldr(false, false, R0, R1, 0, true, false) };
    load(0x8000, code3, 1, USR);
    s_cpu.r[0] = 0xABCD;
    s_cpu.r[1] = 0x81000;
    step(1);
    check("user write to a read-only page goes to the bus", s_io_last_write, 0xABCD);
    check("... not to memory", peek32(0x81000), 0);

    /* Address exception */
    setup();
    uint32_t code4[] = { ldr(true, false, R0, R1, 0, true, false) };
    load(0x8000, code4, 1, SVC);
    s_cpu.r[1] = 0x04000000;
    step(1);
    check("address exception: vector", s_cpu.pc, ARM2_VEC_ADDRESS);
    check("address exception: R14", s_cpu.r[14] & ARM2_PC, 0x8008);

    /* Prefetch abort */
    setup();
    s_page[0x90] = 0;
    s_abort_at = 0x90000;
    load(0x8000, NULL, 0, SVC);
    s_cpu.pc = 0x90000;
    step(1);
    check("prefetch abort: vector", s_cpu.pc, ARM2_VEC_PREFETCH_ABORT);
    check("prefetch abort: R14 = PC + 4", s_cpu.r[14] & ARM2_PC, 0x90004);
}

static void interrupts(void)
{
    setup();
    uint32_t code[] = {
        dp(MOV, false, R0, 0, imm(1)),
        dp(MOV, false, R1, 0, imm(2)),
    };
    load(0x8000, code, 2, USR);
    poke32(ARM2_VEC_IRQ, dp(SUB, true, PC, R14, imm(4)));  /* SUBS PC,R14,#4 */
    step(1);
    arm2_set_irq(&s_cpu, true);
    step(1);
    check("IRQ taken: vector", s_cpu.pc, ARM2_VEC_IRQ);
    check("IRQ: mode, I set", s_cpu.psr, ARM2_IRQ | ARM2_I);
    check("IRQ: R14 = next + 4", s_cpu.r[14], 0x8008 | USR);
    arm2_set_irq(&s_cpu, false);
    step(1);
    check("SUBS PC,R14,#4: back", s_cpu.pc, 0x8004);
    check("SUBS PC,R14,#4: user mode", s_cpu.psr, USR);
    step(1);
    check("the interrupted instruction runs", s_cpu.r[1], 2);

    /* FIQ: R8-R14 banked, I and F set */
    setup();
    load(0x8000, code, 2, USR);
    for (int i = 8; i < 15; i++) {
        s_cpu.r[i] = 0x100 + i;
    }
    arm2_set_fiq(&s_cpu, true);
    step(1);
    check("FIQ: mode", s_cpu.psr, ARM2_FIQ | ARM2_I | ARM2_F);
    check("FIQ: R8 banked", s_cpu.r[8], 0);
    check("FIQ: user R8 kept", arm2_get_reg(&s_cpu, ARM2_USR, 8), 0x108);
    check("FIQ: R14", s_cpu.r[14], 0x8004 | USR);

    /* IRQ masked by I */
    setup();
    load(0x8000, code, 2, USR | ARM2_I);
    arm2_set_irq(&s_cpu, true);
    step(1);
    check("IRQ masked", s_cpu.pc, 0x8004);
}

static void undefined(void)
{
    setup();
    uint32_t code[] = {
        0xEE000010,                         /* MCR: no coprocessor */
        0xE7F000F0,                         /* register offset LDR form with bit 4 */
        0xE1000090,                         /* SWP on an ARM2 */
    };
    load(0x8000, code, 3, SVC);
    step(1);
    check("MCR: undefined", s_cpu.pc, ARM2_VEC_UNDEFINED);
    check("MCR: R14 = next", s_cpu.r[14] & ARM2_PC, 0x8004);
    s_cpu.pc = 0x8004;
    step(1);
    check("LDR with bit 4: undefined", s_cpu.pc, ARM2_VEC_UNDEFINED);
    s_cpu.pc = 0x8008;
    step(1);
    check("SWP on an ARM2: undefined", s_cpu.pc, ARM2_VEC_UNDEFINED);

    setup();
    s_cpu.swp = true;
    uint32_t swp[] = { 0xE1010092 };        /* SWP R0,R2,[R1] */
    load(0x8000, swp, 1, SVC);
    s_cpu.r[1] = 0x100;
    s_cpu.r[2] = 0x22;
    poke32(0x100, 0x11);
    step(1);
    check("SWP: loaded", s_cpu.r[0], 0x11);
    check("SWP: stored", peek32(0x100), 0x22);
}

static void shifter(void)
{
    struct {
        const char *name;
        uint32_t op2, rm, rs, cin, result, cout;
    } t[] = {
        { "LSL #0 keeps C", shi(R1, LSL, 0), 0x80000001, 0, 1, 0x80000001, 1 },
        { "LSL #1", shi(R1, LSL, 1), 0x80000001, 0, 0, 2, 1 },
        { "LSR #32", shi(R1, LSR, 0), 0x80000001, 0, 0, 0, 1 },
        { "ASR #32", shi(R1, ASR, 0), 0x80000000, 0, 0, 0xFFFFFFFF, 1 },
        { "RRX", shi(R1, ROR, 0), 0x00000003, 0, 1, 0x80000001, 1 },
        { "ROR #4", shi(R1, ROR, 4), 0x0000000F, 0, 0, 0xF0000000, 1 },
        { "LSL R2 (0) keeps C", shr(R1, LSL, R2), 5, 0, 1, 5, 1 },
        { "LSL R2 (32)", shr(R1, LSL, R2), 1, 32, 0, 0, 1 },
        { "LSL R2 (33)", shr(R1, LSL, R2), 1, 33, 1, 0, 0 },
        { "LSR R2 (32)", shr(R1, LSR, R2), 0x80000000, 32, 0, 0, 1 },
        { "LSR R2 (33)", shr(R1, LSR, R2), 0x80000000, 33, 1, 0, 0 },
        { "ASR R2 (40)", shr(R1, ASR, R2), 0x80000000, 40, 0, 0xFFFFFFFF, 1 },
        { "ROR R2 (32)", shr(R1, ROR, R2), 0x80000000, 32, 0, 0x80000000, 1 },
        { "ROR R2 (36)", shr(R1, ROR, R2), 0x10, 36, 0, 1, 0 },
        { "LSL R2 uses the low byte (0x101)", shr(R1, LSL, R2), 1, 0x101, 0, 2, 0 },
        { "#imm rotated sets C from bit 31", imm(0xF0000000), 0, 0, 0, 0xF0000000, 1 },
        { "#imm unrotated keeps C", imm(0x55), 0, 0, 1, 0x55, 1 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        setup();
        uint32_t code[] = { dp(MOV, true, R0, 0, t[i].op2) };
        load(0x8000, code, 1, SVC | (t[i].cin ? ARM2_C : 0));
        s_cpu.r[1] = t[i].rm;
        s_cpu.r[2] = t[i].rs;
        step(1);
        char name[80];
        snprintf(name, sizeof(name), "MOVS %s: result", t[i].name);
        check(name, s_cpu.r[0], t[i].result);
        snprintf(name, sizeof(name), "MOVS %s: C", t[i].name);
        check(name, (s_cpu.psr >> 29) & 1, t[i].cout);
    }
}

static void arithmetic(void)
{
    struct {
        const char *name;
        unsigned opc;
        uint32_t a, b, cin, result, nzcv;
    } t[] = {
        { "ADDS overflow", ADD, 0x7FFFFFFF, 1, 0, 0x80000000, 0x9 },
        { "ADDS carry, zero", ADD, 0xFFFFFFFF, 1, 0, 0, 0x6 },
        { "ADCS with carry", ADC, 0xFFFFFFFF, 0, 1, 0, 0x6 },
        { "ADCS 0+0+1", ADC, 0, 0, 1, 1, 0x0 },
        { "SUBS equal: Z C", SUB, 5, 5, 0, 0, 0x6 },
        { "SUBS borrow", SUB, 4, 5, 0, 0xFFFFFFFF, 0x8 },
        { "SUBS overflow", SUB, 0x80000000, 1, 0, 0x7FFFFFFF, 0x3 },
        { "SBCS no carry: one less", SBC, 5, 5, 0, 0xFFFFFFFF, 0x8 },
        { "SBCS with carry", SBC, 5, 5, 1, 0, 0x6 },
        { "RSBS", RSB, 1, 3, 0, 2, 0x2 },
        { "RSCS", RSC, 1, 3, 0, 1, 0x2 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        setup();
        uint32_t code[] = { dp(t[i].opc, true, R0, R1, shi(R2, LSL, 0)) };
        load(0x8000, code, 1, SVC | (t[i].cin ? ARM2_C : 0));
        s_cpu.r[1] = t[i].a;
        s_cpu.r[2] = t[i].b;
        step(1);
        char name[80];
        snprintf(name, sizeof(name), "%s: result", t[i].name);
        check(name, s_cpu.r[0], t[i].result);
        snprintf(name, sizeof(name), "%s: NZCV", t[i].name);
        check(name, s_cpu.psr >> 28, t[i].nzcv);
    }

    /* CMP with Rd 0 (the usual) and the P form: TEQP writes the PSR */
    setup();
    uint32_t code[] = { dp(CMP, true, 0, R1, imm(1)), dp(TEQ, true, PC, R2, imm(0)) };
    load(0x8000, code, 2, SVC);
    s_cpu.r[1] = 1;
    s_cpu.r[2] = ARM2_N | ARM2_V | ARM2_IRQ;
    step(1);
    check("CMP: Z C", s_cpu.psr >> 28, 0x6);
    step(1);
    check("TEQP R2,#0: the PSR from the result", s_cpu.psr, ARM2_N | ARM2_V | ARM2_IRQ);
}

static void multiplies(void)
{
    setup();
    uint32_t code[] = { MUL(R0, R1, R2) | (1u << 20), MLA(R3, R1, R2, R4) };
    load(0x8000, code, 2, SVC | ARM2_C | ARM2_V);
    s_cpu.r[1] = 0xFFFFFFFF;
    s_cpu.r[2] = 3;
    s_cpu.r[4] = 10;
    int t = step(1);
    check("MULS", s_cpu.r[0], 0xFFFFFFFD);
    check("MULS: N, V kept", s_cpu.psr & (ARM2_N | ARM2_Z | ARM2_V), ARM2_N | ARM2_V);
    check("MUL by 3: 1S + 1I", t, 2);
    step(1);
    check("MLA", s_cpu.r[3], 7);
    setup();
    uint32_t big[] = { MUL(R0, R1, R2) };
    load(0x8000, big, 1, SVC);
    s_cpu.r[2] = 0x80000000;
    check("MUL by &80000000: 1S + 16I", step(1), 17);
}

/* A bus callback ends the run: it stops after that instruction, with the
 * ticks it used (time doesn't jump to the run's planned end)
 */
static void end_run(void)
{
    setup();
    static const uint32_t code[] = {
        0xE3A00601,                         /* MOV R0,#&100000 (I/O) */
        0xE5801000,                         /* STR R1,[R0] */
        0xE3A02001,                         /* MOV R2,#1 */
        0xE3A03001,                         /* MOV R3,#1 */
    };
    load(0x8000, code, 4, SVC);
    uint64_t t0 = s_cpu.ticks;
    s_end_on_write = true;
    s_cpu.left = 0;
    int32_t ran = arm2_run(&s_cpu, 1000);
    s_end_on_write = false;
    check("end run: stops after the STR", s_cpu.pc, 0x8008);
    check("end run: R2 not run yet", s_cpu.r[2], 0);
    check("end run: the ticks it used (MOV 1S, STR 1S + 2N)", (uint32_t)ran, 4);
    check("end run: time went on by those", (uint32_t)(s_cpu.ticks - t0), 4);
    check("end run: and arm2_now() with it", (uint32_t)(arm2_now(&s_cpu) - t0), 4);
}

static void conditions(void)
{
    static const struct { unsigned cond; uint32_t nzcv; bool runs; } t[] = {
        { 0x0, 0x4, true }, { 0x0, 0x0, false }, { 0x1, 0x0, true },
        { 0x2, 0x2, true }, { 0x3, 0x2, false }, { 0x4, 0x8, true },
        { 0x5, 0x8, false }, { 0x6, 0x1, true }, { 0x7, 0x1, false },
        { 0x8, 0x2, true }, { 0x8, 0x6, false }, { 0x9, 0x4, true },
        { 0xA, 0x9, true }, { 0xA, 0x8, false }, { 0xB, 0x8, true },
        { 0xC, 0x0, true }, { 0xC, 0x4, false }, { 0xD, 0x4, true },
        { 0xE, 0xF, true }, { 0xF, 0x0, false },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        setup();
        uint32_t code[] = { COND(t[i].cond, dp(MOV, false, R0, 0, imm(1))) };
        load(0x8000, code, 1, SVC | (t[i].nzcv << 28));
        step(1);
        char name[48];
        snprintf(name, sizeof(name), "condition %X with NZCV %X", t[i].cond, (unsigned)t[i].nzcv);
        check(name, s_cpu.r[0], t[i].runs);
    }
}

static void disassembly(void)
{
    static const struct { uint32_t op; const char *text; } t[] = {
        { 0xE2910010, "ADDS R0,R1,#&10" },
        { 0xE33FF003, "TEQP PC,#3" },
        { 0xE1B0F00E, "MOVS PC,R14" },
        { 0xE8FD800F, "LDMIA R13!,{R0-R3,PC}^" },
        { 0xE8A00006, "STMIA R0!,{R1,R2}" },
        { 0xE4F10001, "LDRBT R0,[R1],#1" },
        { 0xE0203291, "MLA R0,R1,R2,R3" },
        { 0xEF000020, "SWI &20" },
        { 0xE59F0008, "LDR R0,&00008010" },
        { 0x0A000002, "BEQ &00008010" },
        { 0xE1A01102, "MOV R1,R2,LSL #2" },
        { 0xE1A01062, "MOV R1,R2,RRX" },
        { 0xE0811312, "ADD R1,R1,R2,LSL R3" },
        { 0xE7910102, "LDR R0,[R1,R2,LSL #2]" },
        { 0xE5310004, "LDR R0,[R1,#-4]!" },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char buf[64];
        arm2_disassemble_word(t[i].op, 0x8000, buf, sizeof(buf));
        s_run++;
        if (strcmp(buf, t[i].text) != 0) {
            s_failed++;
            printf("FAIL disassembly of &%08X: \"%s\", want \"%s\"\n", (unsigned)t[i].op, buf, t[i].text);
        } else if (s_verbose) {
            printf("ok   %s\n", buf);
        }
    }
}

int main(int argc, char **argv)
{
    s_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    r15_operands();
    r15_stores();
    mode_changes();
    ldm_stm_psr_and_user();
    block_writeback();
    branches();
    unaligned_and_bytes();
    post_index_and_writeback();
    privileges_and_aborts();
    interrupts();
    undefined();
    shifter();
    arithmetic();
    multiplies();
    conditions();
    end_run();
    disassembly();
    printf("%d of %d checks passed\n", s_run - s_failed, s_run);
    return s_failed ? 1 : 0;
}
