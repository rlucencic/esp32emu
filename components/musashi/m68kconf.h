/*
 * Musashi configuration shared by the 68000 machines (Atari STE, Macintosh).
 *
 * Replaces Musashi's stock m68kconf.h. See musashi/m68kconf.h upstream for
 * the full description of every switch.
 *
 * The machine provides the m68k_host_*() hooks declared below.
 *
 * Musashi is Copyright Karl Stenerud, MIT licence (see musashi/readme.txt).
 */
#ifndef M68KCONF__HEADER
#define M68KCONF__HEADER

#define OPT_OFF             0
#define OPT_ON              1
#define OPT_SPECIFY_HANDLER 2

#define M68K_COMPILE_FOR_MAME       OPT_OFF

/* ---- CPU model, selected at compile time: a plain 68000, or a 68020 or
 * a 68030 for the machines that ask for one (CONFIG_M68K_68020 or
 * CONFIG_M68K_68030 in the project's sdkconfig, -DM68K_68020 or
 * -DM68K_68030 in host builds: the Macintosh LC and II, LC II and LC III).
 * Or chosen at run time, 68000 to 68020 (CONFIG_M68K_RUNTIME,
 * -DM68K_RUNTIME: the Amiga's A500 and A1200 in one firmware): the type
 * checks read m68k_set_cpu_type()'s choice instead of folding.
 */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_M68K_68020) && !defined(M68K_68020)
#define M68K_68020
#endif
#if defined(CONFIG_M68K_68030) && !defined(M68K_68030)
#define M68K_68030
#endif
#if defined(CONFIG_M68K_RUNTIME) && !defined(M68K_RUNTIME)
#define M68K_RUNTIME
#endif
#if defined(CONFIG_M68K_NO_BUS_ERRORS) && !defined(M68K_NO_BUS_ERRORS)
#define M68K_NO_BUS_ERRORS
#endif
#if defined(CONFIG_M68K_PROGRAM_READS) && !defined(M68K_PROGRAM_READS)
#define M68K_PROGRAM_READS
#endif
#if defined(CONFIG_M68K_PREFETCH) && !defined(M68K_PREFETCH)
#define M68K_PREFETCH
#endif
#if defined(CONFIG_M68K_HOST_LOOP) && !defined(M68K_HOST_LOOP)
#define M68K_HOST_LOOP
#endif

#if defined(M68K_68020) || defined(M68K_68030) || defined(M68K_RUNTIME)
/* Musashi's CPU_TYPE_IS_010_LESS() and friends are only right with the
 * models below it on too (with just the 020, the 68000's address errors
 * stayed in); with the type fixed they fold to constants all the same
 */
#define M68K_EMULATE_010            OPT_ON
#define M68K_EMULATE_EC020          OPT_ON
#define M68K_EMULATE_020            OPT_ON
#else
#define M68K_EMULATE_010            OPT_OFF
#define M68K_EMULATE_EC020          OPT_OFF
#define M68K_EMULATE_020            OPT_OFF
#endif
#ifdef M68K_68030
#define M68K_EMULATE_030            OPT_ON
#define M68K_EMULATE_PMMU           OPT_ON
#define M68K_FIXED_CPU_TYPE         CPU_TYPE_030
#elif defined(M68K_68020)
#define M68K_EMULATE_030            OPT_OFF
#define M68K_EMULATE_PMMU           OPT_OFF
#define M68K_FIXED_CPU_TYPE         CPU_TYPE_020
#elif defined(M68K_RUNTIME)
#define M68K_EMULATE_030            OPT_OFF
#define M68K_EMULATE_PMMU           OPT_OFF
#else
#define M68K_EMULATE_030            OPT_OFF
#define M68K_EMULATE_PMMU           OPT_OFF
#define M68K_FIXED_CPU_TYPE         CPU_TYPE_000
#endif
#define M68K_EMULATE_040            OPT_OFF

/* ---- Bus ---- */
/* Instruction words and PC-relative operands through their own callbacks
 * (m68k_read_immediate_*(), m68k_read_pcrelative_*()) for a machine that
 * tells them apart (CONFIG_M68K_PROGRAM_READS, -DM68K_PROGRAM_READS: the
 * Amiga's 68EC020 instruction cache)
 */
#ifdef M68K_PROGRAM_READS
#define M68K_SEPARATE_READS         OPT_ON
#else
#define M68K_SEPARATE_READS         OPT_OFF
#endif
#define M68K_SIMULATE_PD_WRITES     OPT_OFF
#define M68K_EMULATE_FC             OPT_OFF
#define M68K_SET_FC_CALLBACK(A)     your_set_fc_handler_function(A)

/* ---- Exceptions ---- */
/* Vectored or autovectored interrupts: the machine answers through
 * m68k_set_int_ack_callback().
 */
#define M68K_EMULATE_INT_ACK        OPT_ON
#define M68K_INT_ACK_CALLBACK(A)    your_int_ack_handler_function(A)

/* RESET instruction resets the peripherals: m68k_set_reset_instr_callback() */
#define M68K_EMULATE_RESET          OPT_ON
#define M68K_RESET_CALLBACK()       your_reset_handler_function()

/* Trace mode is used by debuggers and some copy protections (e.g. Copylock) */
#define M68K_EMULATE_TRACE          OPT_ON

/* Odd-address word/long accesses raise address errors ("3 bombs") */
#define M68K_EMULATE_ADDRESS_ERROR  OPT_ON

/* Bus errors raised from memory callbacks via m68k_bus_error() ("2 bombs");
 * off for a machine without them (CONFIG_M68K_NO_BUS_ERRORS, the Amiga):
 * the 68010 and up then don't copy their registers before each instruction
 */
#ifdef M68K_NO_BUS_ERRORS
#define M68K_BUS_ERR_ENABLE         OPT_OFF
#else
#define M68K_BUS_ERR_ENABLE         OPT_ON
#endif

#define M68K_EMULATE_BKPT_ACK       OPT_OFF
#define M68K_BKPT_ACK_CALLBACK()    your_bkpt_ack_handler_function()
/* The next instruction word fetched ahead (CONFIG_M68K_PREFETCH,
 * -DM68K_PREFETCH): an instruction writing over the next one runs the old
 * word, as on a 68000 (the Amiga's Copylock-style checks)
 */
#ifdef M68K_PREFETCH
#define M68K_EMULATE_PREFETCH       OPT_ON
#else
#define M68K_EMULATE_PREFETCH       OPT_OFF
#endif

/* ---- Hooks (all off: they cost time on every instruction) ---- */
#define M68K_CMPILD_HAS_CALLBACK    OPT_OFF
#define M68K_CMPILD_CALLBACK(v,r)   your_cmpild_handler_function(v,r)
#define M68K_RTE_HAS_CALLBACK       OPT_OFF
#define M68K_RTE_CALLBACK()         your_rte_handler_function()
#define M68K_TAS_HAS_CALLBACK       OPT_OFF
#define M68K_TAS_CALLBACK()         your_tas_handler_function()
/* Illegal opcodes are offered to the machine first: native calls from its
 * own 68000 code (the ST's GEMDOS cartridge, the Mac's disk driver). Return
 * nonzero when handled: execution goes on after the opcode (or wherever the
 * hook set the PC).
 */
#define M68K_ILLG_HAS_CALLBACK      OPT_SPECIFY_HANDLER
#define M68K_ILLG_CALLBACK(opcode)  m68k_host_illegal(opcode)
int m68k_host_illegal(int opcode);

/* A double bus/address fault halts the CPU, as on a real 68000 */
#define M68K_HALT_CALLBACK()        m68k_host_halted()
void m68k_host_halted(void);

/* Every exception, once its stack frame is pushed */
#define M68K_VECTOR_CALLBACK(v)     m68k_host_exception(v)
void m68k_host_exception(unsigned int vector);
#define M68K_MONITOR_PC             OPT_OFF
#define M68K_SET_PC_CALLBACK(A)     your_pc_changed_handler_function(A)

/* Before every instruction: the debugger (emudbg.h) looks at m68k_dbg_map,
 * a bit per 256-byte page (NULL: no debugger), and stops in pages with a
 * breakpoint (m68k_dbg.c). Host debug builds trace too, with
 * -DM68K_INSTRUCTION_HOOK=2 and their m68k_host_instruction().
 */
#include <stdint.h>
#include "emudbg.h"
extern const uint8_t *volatile m68k_dbg_map;
void m68k_dbg_hit(unsigned int pc);
static inline void m68k_dbg_instruction(unsigned int pc)
{
#if EMUDBG_ENABLED
    const uint8_t *map = m68k_dbg_map;
    if (__builtin_expect(map != 0, 0) && emudbg_map24(map, pc)) {
        m68k_dbg_hit(pc);
    }
#else
    (void)pc;
#endif
}
void m68k_host_instruction(unsigned int pc);
#if defined(M68K_INSTRUCTION_HOOK)
#define M68K_HOST_HOOK              1
#define M68K_INSTRUCTION_CALLBACK(pc) do { m68k_host_instruction(pc); m68k_dbg_instruction(pc); } while (0)
#elif EMUDBG_ENABLED
#define M68K_INSTRUCTION_HOOK       OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) m68k_dbg_instruction(pc)
#else
#define M68K_INSTRUCTION_HOOK       OPT_OFF
#define M68K_INSTRUCTION_CALLBACK(pc)
#endif

/* m68k_execute()'s quick loop (no trace bit, no debugger as a slice
 * starts) checks nothing before an instruction but the hosts' own hook;
 * a debugger's map makes the slice run the loop that checks it
 */
#if EMUDBG_ENABLED
#define M68K_DEBUGGER_WANTED()      (m68k_dbg_map != 0)
#else
#define M68K_DEBUGGER_WANTED()      0
#endif
#ifdef M68K_HOST_HOOK
#define M68K_QUICK_CALLBACK(pc)     m68k_host_instruction(pc)
#else
#define M68K_QUICK_CALLBACK(pc)
#endif

/* The machine's own loop for runs of instructions (CONFIG_M68K_HOST_LOOP,
 * -DM68K_HOST_LOOP: the Amiga's 68EC020, its instruction fetch inline in
 * the loop). The quick loop calls m68k_host_run() as it starts: 0, not
 * this slice (Musashi's loop runs it). Otherwise the machine has run
 * instructions as the quick loop runs them (its hook M68K_QUICK_CALLBACK()
 * with the PC, REG_PPC, the opcode at the PC through its own fetch with
 * the PC moved on, REG_IR, the handler, the cycles) until the cycles ran
 * out, m68ki_tracing was set, or a PC it leaves to Musashi (odd: the
 * address error), and is called again after Musashi ran that one. The
 * state it needs is in m68k_loop.
 */
#ifdef M68K_HOST_LOOP
typedef struct {
    unsigned int *pc, *ppc, *ir;            /* REG_PC, REG_PPC, REG_IR */
    void (***handlers)(void);               /* the opcode jump table in use */
    const unsigned char **cycles;           /* the cycle table in use */
    int *remaining;                         /* the slice's cycles left */
    unsigned int *tracing;                  /* m68ki_tracing */
} m68k_loop_t;
extern const m68k_loop_t m68k_loop;
int m68k_host_run(void);
#endif

#define M68K_LOG_ENABLE             OPT_OFF
#define M68K_LOG_1010_1111          OPT_OFF
#define M68K_LOG_FILEHANDLE         some_file_handle

/* ---- Build options ---- */
#define M68K_DASM_ENABLE            OPT_ON

/* Decode and cycle tables are generated at build time (const, in flash)
 * instead of 320 KB of RAM at runtime.
 */
#define M68K_DYNAMIC_INSTR_TABLES   OPT_OFF
#define M68K_CYCLE_COUNTING         OPT_ON

#define M68K_USE_64_BIT             OPT_ON

/* Hot core functions run from IRAM */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define M68K_FAST_FUNC(x)           IRAM_ATTR x
#endif

/* On RISC-V (the P4) the CPU's state and the cycle count go into small data,
 * within 2 KB of the global pointer: the linker then reaches them from gp,
 * without the lui every handler spent on m68ki_cpu's address (its hot
 * fields are its first ~450 bytes; the 68030 MMU's page cache is apart).
 * The linker relaxes a reference only if the whole object, plus a margin
 * for alignment, lies in the window: not plain .sdata, which starts at the
 * window's very edge.
 */
#if defined(ESP_PLATFORM) && defined(__riscv)
#define M68K_STATE_ATTR             __attribute__((section(".sdata.m68k")))
#endif

#endif /* M68KCONF__HEADER */
