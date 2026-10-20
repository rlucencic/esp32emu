/*
 * x86 core: the Intel 8088, 8086 and 80286 (../esp32pc: the IBM PC/XT and
 * PC/AT).
 *
 * Instruction-stepped, one core for the three, the model chosen at
 * x86_init(). They differ where the chips do:
 *  - 8088 and 8086: 0F is POP CS; 60-6F alias 70-7F, C0/C1 alias C2/C3,
 *    C8/C9 alias CA/CB; shift counts aren't masked; PUSH SP pushes the
 *    decremented SP; FLAGS bits 12-15 read as 1; undefined forms do what the
 *    chips do (as the SingleStepTests suites record it); a divide error
 *    returns after the DIV; a word at offset FFFF wraps within its segment;
 *    a string instruction an interrupt cut short resumes at its last prefix.
 *  - 8088 against 8086: the bus (on the 8088 every word takes two transfers,
 *    on the 8086 only a word at an odd address does) and the prefetch queue
 *    (4 against 6 bytes): what the clocks see.
 *  - 80286: the 186/286 instructions (PUSHA, POPA, BOUND, PUSH and IMUL
 *    immediate, INS, OUTS, ENTER, LEAVE, shifts by an immediate), #UD
 *    (interrupt 6) for undefined opcodes, faults that return to the faulting
 *    instruction, a word at offset FFFF faults (interrupt 13, 12 in SS),
 *    FLAGS bits 12-15 clear in real mode, and protected mode: descriptor
 *    tables, segment limits and access rights, the four privilege levels,
 *    call, interrupt, trap and task gates, task switches through the TSS,
 *    exceptions with error codes, the double fault and the shutdown, IOPL,
 *    LLDT, LTR, LAR, LSL, VERR, VERW, ARPL, and LOADALL (in real mode too:
 *    "unreal" segments). The segments' limits are checked in either mode
 *    (real mode's are 0-FFFF unless LOADALL set others).
 *
 * Memory goes through a table of 4 KB pages over the 286's 16 MB (the 8088
 * and 8086 use the first 256): each entry is a host address (4 KB aligned)
 * with the bits below. Accesses the entry doesn't allow go to the bus
 * callbacks. Linear addresses are masked with addr_mask: 1 MB on the 8088
 * and 8086 (FFFF:0010 is 0000:0000), 16 MB on the 286 with bit 20 off
 * while the A20 gate is closed (x86_set_a20()). Each segment register keeps
 * a direct window: from its offset 0 as far as its pages are contiguous
 * host memory that allows the access; accesses inside it skip the table
 * (x86_flush() when the table changes).
 *
 * Time is counted in CPU clocks: each instruction's documented clocks (the
 * 8086's tables for the 8088 and 8086, with their effective address times
 * and the extra transfers above; the 286's), and on the 8088 and 8086 the
 * waits when the prefetch queue runs dry (a model of the queue's fill, not
 * its bus cycles). x86_run() runs until the clocks given are used up.
 *
 * Speed: one run loop per model with the whole instruction set inlined
 * into it (the 286's: in internal RAM on the P4, X86_HOT), the code read
 * through a host pointer held in registers, no per-access bounds but the
 * segments' windows. The SingleStepTests suites (test/) check it.
 *
 * The BIOS trap: FE 38 nn at a linear address within [trap_lo, trap_hi)
 * calls bus.trap(n) with IP past it; anywhere else FE 38 is the chip's.
 *
 * The coprocessor (x87.c, x86_set_fpu()): the 8087 beside the 8088 and
 * 8086, the 287 beside the 286. Its registers hold 80-bit values: loads,
 * stores, moves, the integer and BCD conversions, comparisons, FRNDINT,
 * FSCALE, FXTRACT, FPREM, FSAVE and FRSTOR keep every bit; FADD, FSUB,
 * FMUL, FDIV and FSQRT are exact to the last bit (128-bit intermediates,
 * rounded by the precision and rounding controls, with PE); the
 * transcendental functions go through the C library's doubles. Masked exceptions respond as the chips do;
 * unmasked ones set ES and B and raise bus.fpu_error (the PC/AT's IRQ 13,
 * the XT's NMI). Its instructions take their documented clocks on the CPU's
 * time (the chips ran beside the CPU; FWAIT waited for them).
 *
 * To start: x86_init(), then x86_reset().
 */
#pragma once

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

/* The 286's run loop (~43 KB) and x86_run(): in internal RAM on the P4,
 * which has room for them; the S3's internal RAM hasn't, there they run
 * from PSRAM through the instruction cache
 */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
#include "esp_attr.h"
#define X86_HOT     IRAM_ATTR
#else
#define X86_HOT
#endif

#define X86_PAGE_SHIFT  12
#define X86_PAGES       (1u << (24 - X86_PAGE_SHIFT))      /* 4096 */

/* Page table entry bits (the host address is 4 KB aligned) */
enum {
    X86_NO_READ = 0x01,             /* reads (and code fetches) go to bus.read */
    X86_NO_WRITE = 0x02,            /* writes go to bus.write (ROM: it ignores them) */
};

typedef enum { X86_8088, X86_8086, X86_80286 } x86_model_t;

/* Registers as encoded in instructions */
enum { X86_AX, X86_CX, X86_DX, X86_BX, X86_SP, X86_BP, X86_SI, X86_DI };
enum { X86_ES, X86_CS, X86_SS, X86_DS };

/* FLAGS */
enum {
    X86_CF = 0x0001, X86_PF = 0x0004, X86_AF = 0x0010, X86_ZF = 0x0040,
    X86_SF = 0x0080, X86_TF = 0x0100, X86_IF = 0x0200, X86_DF = 0x0400,
    X86_OF = 0x0800, X86_IOPL = 0x3000, X86_NT = 0x4000,
};

/* The 286's MSW */
enum { X86_MSW_PE = 0x0001, X86_MSW_MP = 0x0002, X86_MSW_EM = 0x0004, X86_MSW_TS = 0x0008 };

typedef struct {
    uint16_t sel;                   /* the segment register as the program sees it */
    uint32_t base;                  /* linear base (real mode: sel * 16) */
    /* Offsets a byte may be read from / written to: [rlo, rhi], [wlo, whi]
     * (real mode 0-FFFF; a segment that can't be written has wlo > whi)
     */
    uint16_t rlo, rhi, wlo, whi;
    uint8_t access;                 /* the 286 descriptor's access byte */
    /* Its direct window (x86.c keeps it): offsets below rd_lim (reads) and
     * wr_lim (writes) are plain host memory at host + offset; worked out at
     * the first access after the segment changed (stale: limits 0 till then)
     */
    uintptr_t host;
    uint32_t rd_lim, wr_lim;
    bool stale;
} x86_seg_t;

/* A segment window worked out (x86.c) */
typedef struct {
    uint32_t base, gen;
    uintptr_t host;
    uint32_t rd_lim, wr_lim;
} x86_window_t;

typedef struct x86 x86_t;

/* The coprocessor's registers: 80-bit values */
typedef struct {
    uint64_t m;                     /* the mantissa (bit 63 the integer bit) */
    uint16_t se;                    /* the sign (bit 15) and the exponent (bias 16383) */
} x87_reg_t;

typedef struct {
    x87_reg_t st[8];                /* the physical registers: ST(i) is st[(TOP + i) & 7] */
    uint16_t cw, sw, tw;            /* control, status (TOP in bits 11-13), tags (2 bits each) */
    uint16_t ip_off, ip_seg, opcode;    /* the last instruction's (FSTENV, FSAVE) */
    uint16_t dp_off, dp_seg;            /* and its operand's */
    bool i8087;                     /* the 8087 (its interrupt mask, FENI, FDISI), else the 287 */
    bool pm;                        /* FSETPM (the 287): the protected-mode environment */
    bool error;                     /* an unmasked exception: the chip's ERROR (INT) line */
} x87_t;

typedef struct {
    /* Pages whose entry doesn't allow the access (linear addresses, after
     * addr_mask). They may add wait states (cpu->clk += n) and call
     * x86_end_run().
     */
    uint8_t (*read)(void *ctx, uint32_t addr);
    void (*write)(void *ctx, uint32_t addr, uint8_t v);
    /* I/O. A word: the 8088 makes two byte transfers (port, then port + 1);
     * the 8086 and 286 one, through inw/outw (NULL: two byte calls too)
     */
    uint8_t (*in)(void *ctx, uint16_t port);
    void (*out)(void *ctx, uint16_t port, uint8_t v);
    uint16_t (*inw)(void *ctx, uint16_t port);
    void (*outw)(void *ctx, uint16_t port, uint16_t v);
    /* Interrupt acknowledge (INTR was taken): the vector */
    uint8_t (*inta)(void *ctx);
    /* The BIOS trap (FE 38 nn inside [trap_lo, trap_hi)): IP is past it;
     * it may change registers and memory, and call x86_interrupt() once to
     * run an interrupt handler that returns past the trap (NULL: none)
     */
    void (*trap)(void *ctx, x86_t *cpu, uint8_t n);
    /* A shutdown (a fault while taking a fault, twice: the 286's triple
     * fault): the AT resets the CPU (NULL: it stops, `stuck`)
     */
    void (*shutdown)(void *ctx, x86_t *cpu);
    /* The coprocessor's error line (an unmasked exception, cleared by
     * FCLEX, FINIT or FLDENV/FRSTOR without one) changed (NULL: unwired)
     */
    void (*fpu_error)(void *ctx, bool on);
    void *ctx;
} x86_bus_t;

struct x86 {
    union {
        uint16_t w[8];              /* AX CX DX BX SP BP SI DI */
        uint8_t b[16];              /* AL AH CL CH ...: see x86_reg8() */
    } r;
    x86_seg_t seg[4];               /* ES CS SS DS */
    uint16_t ip;
    uint16_t flags;                 /* TF IF DF IOPL NT always; the arithmetic flags while
                                       lf_op is 0 (else worked out from lf_*: x86_get_flags()) */
    uint8_t lf_op;                  /* the last arithmetic operation (x86.c), 0: none */
    uint32_t lf_res, lf_a, lf_b;    /* its result and operands */

    x86_model_t model;

    /* 286 */
    uint16_t msw;
    struct { uint32_t base; uint16_t limit; } gdtr, idtr;
    x86_seg_t ldtr, tr;

    /* Memory */
    const uintptr_t *page;          /* X86_PAGES entries */
    uint32_t addr_mask;
    x86_bus_t bus;
    uint32_t trap_lo, trap_hi;      /* where FE 38 nn traps (linear) */

    /* Interrupts (`lines`: any of them, one test between instructions) */
    union {
        struct {
            bool intr;              /* the INTR line (a level) */
            bool nmi;               /* an NMI edge is pending */
            bool halted;            /* HLT: until an interrupt */
            bool inhibit;           /* no interrupt before the next instruction (STI, MOV SS) */
        };
        uint32_t lines;
    };

    /* Time */
    int32_t clk;                    /* clocks of this run up to the current instruction (bus
                                       callbacks may add wait states) */
    int32_t budget;                 /* the run's clocks (0: x86_end_run()) */
    uint64_t cycles;                /* clocks since power-on, up to this run */
    uint32_t insns;                 /* instructions run (statistics) */

    /* A shutdown with no bus.shutdown to reset it: the CPU stops (halted
     * with interrupts ignored)
     */
    bool stuck;
    uint8_t cpl;                    /* the current privilege level (protected mode; 0 in real mode) */

    /* The core's own */
    uint16_t ip0, sp0;              /* the instruction's IP (its first prefix) and SP */
    uint16_t last_prefix;           /* IP of its last prefix (the 8086's resumed strings) */
    int16_t bank;                   /* the prefetch queue's fill, in clocks of fetching (8088, 8086) */
    uint16_t busy;                  /* clocks the instruction's data transfers took the bus */
    uint8_t fetched;                /* bytes the instruction took from the queue */
    uint8_t max_len;                /* the longest instruction (the 286's 10; 255: no limit) */
    bool flush;                     /* it jumped: the queue starts empty */
    bool ip_set;                    /* IP was set during the instruction (a jump, an interrupt,
                                       the BIOS trap): it isn't the one past its bytes */
    bool resync;                    /* the code pointer starts again from CS:IP (IP set, the
                                       window gone; on the 286 the 10-byte limit is on) */
    bool single;                    /* x86_step(): string instructions run to the end */
    bool in_run;                    /* inside x86_run()/x86_step(): faults can leave the instruction */
    uint8_t faulting;               /* faults while delivering one (double fault, shutdown) */
    uint8_t wait[2];                /* extra clocks of a word transfer at an even, odd address */
    uint8_t mem_wait;               /* wait states of each memory transfer (x86_set_wait()) */
    uint8_t xfer;                   /* bus clocks of a byte transfer (8088, 8086: 4) */
    /* The code window: IPs win_lo to win_lo + win_n - 1 (one page, the
     * segment's end) are at host address win_base + IP. Empty (win_n 0)
     * when CS, the page table or the A20 gate change.
     */
    uint32_t win_lo, win_n;
    uintptr_t win_base, win_end;
    uintptr_t fetch[3];             /* (x86.c's fetch_slow(): the code pointer it leaves) */
    /* The 8088/8086's queue against code that changes the bytes ahead
     * (programs tell the two apart so): writes near the instruction noted
     * (q_log), and the old bytes of those the queue had already fetched
     * (q_old) run instead, until a jump empties it
     */
    uint32_t q_lin;                 /* the instruction's linear address */
    uint8_t q_log_n, q_old_n;
    struct { uint32_t lin; uint8_t old; } q_log[8], q_old[6];
    /* Segment windows worked out before, by base (x86.c's seg_window()):
     * valid while gen is window_gen (x86_flush() moves it on)
     */
    x86_window_t windows[64];
    uint32_t window_gen;
    jmp_buf fault_jmp;              /* the 286's faults leave the instruction */
    int fault_vector;
    uint16_t fault_error;           /* its error code (protected mode), if it has one */
    bool fault_has_error;
    bool ext;                       /* delivering an event from outside the program (EXT) */

    /* The debugger (emudbg.h): before an instruction whose linear address
     * has its 256-byte page's bit set in dbg_map, x86_run() calls
     * emudbg_hit() with dbg (NULL: never). Looked at when a run starts: a
     * run with it set takes a slower loop.
     */
    const uint8_t *volatile dbg_map;
    void *dbg;

    /* The coprocessor (x86_set_fpu()) */
    bool has_fpu;
    x87_t fpu;
};

void x86_init(x86_t *cpu, x86_model_t model, const x86_bus_t *bus, const uintptr_t *page);

/* RESET: 8088/8086 CS:IP FFFF:0000; 286 F000:FFF0 with the CS base at
 * FF0000 (the top of 16 MB) until CS is loaded; flags clear, real mode
 */
void x86_reset(x86_t *cpu);

/* Whole instructions until `clocks` more are used up (the last may
 * overrun), the CPU halts with no interrupt to take (the rest of the
 * clocks pass idle), or a bus callback calls x86_end_run(). The clocks
 * that passed.
 */
int32_t x86_run(x86_t *cpu, int32_t clocks);

/* One instruction, a string instruction with its whole repeat (and the
 * interrupt it may cause): its clocks. For the tests.
 */
int32_t x86_step(x86_t *cpu);

/* Clocks since power-on, inside a run too (for the bus callbacks: as the
 * current instruction began, give or take its wait states)
 */
static inline uint64_t x86_now(const x86_t *cpu)
{
    return cpu->cycles + (uint64_t)(int64_t)cpu->clk;
}

/* From a bus callback: the run ends after this instruction */
static inline void x86_end_run(x86_t *cpu)
{
    cpu->budget = 0;
}

static inline void x86_set_intr(x86_t *cpu, bool level)
{
    cpu->intr = level;
}

static inline void x86_nmi(x86_t *cpu)
{
    cpu->nmi = true;
}

/* Wait states on each memory transfer (the IBM AT's 1); a word at an odd
 * address is two transfers
 */
void x86_set_wait(x86_t *cpu, int waits);

/* A coprocessor beside the CPU (after x86_init(); the 8087 or the 287 by
 * the model), or none: ESC instructions then do nothing but their bus cycle
 */
void x86_set_fpu(x86_t *cpu, bool present);

/* The 286's A20 gate (the AT's 8042): open, addresses reach bit 20 */
void x86_set_a20(x86_t *cpu, bool open);

/* The page table changed (the core's views of memory are worked out again) */
void x86_flush(x86_t *cpu);

/* FLAGS as PUSHF would push them */
uint16_t x86_get_flags(const x86_t *cpu);
/* As POPF in real mode (all of TF, IF, DF and the arithmetic flags) */
void x86_set_flags(x86_t *cpu, uint16_t flags);

/* An 8-bit register (0-7: AL CL DL BL AH CH DH BH) */
static inline uint8_t *x86_reg8(x86_t *cpu, int r)
{
    return &cpu->r.b[((r & 3) << 1) | (r >> 2)];
}

/* Load a segment register: in real mode base = sel * 16; in protected
 * mode from its descriptor, checked as MOV (CS as a far JMP to a code
 * segment would load it). A bus callback that sets CS and IP (cpu->ip)
 * does it from the BIOS trap, as the instruction's last act.
 */
void x86_load_seg(x86_t *cpu, int seg, uint16_t sel);

/* Interrupt n now (as INT n: flags, CS and IP pushed), from a bus
 * callback or between runs
 */
void x86_interrupt(x86_t *cpu, uint8_t n);

/* Memory through the page table and the bus, as the CPU sees it (linear
 * addresses; bus callbacks run): for the BIOS
 */
uint8_t x86_read8(x86_t *cpu, uint32_t addr);
uint16_t x86_read16(x86_t *cpu, uint32_t addr);
void x86_write8(x86_t *cpu, uint32_t addr, uint8_t v);
void x86_write16(x86_t *cpu, uint32_t addr, uint16_t v);

/* Memory without side effects (what the table doesn't map reads 0xFF) */
uint8_t x86_peek8(const x86_t *cpu, uint32_t addr);

#ifdef X86_HOST_HOOK
/* Host builds: called before each instruction (tracing) */
void x86_host_hook(x86_t *cpu);
#endif

/* The instruction at seg:off as text ("MOV AX,[BX+SI+12]": upper case, hex
 * numbers as DOS DEBUG writes them), read through x86_peek8(); its length
 */
int x86_disassemble(const x86_t *cpu, uint16_t seg, uint16_t off, char *buf, int len);
/* The same from bytes (up to 16 of them) */
int x86_disassemble_bytes(x86_model_t model, const uint8_t *bytes, uint16_t off, char *buf, int len);
