# Z80 core for the Z80 machines (see include/z80.h); times by menuconfig "Z80 core"
idf_component_register(SRCS "z80.c" "z80_dbg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES emudbg)

# Runs in the emulation loop. Jump tables for the opcode switches (the IDF
# turns them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
