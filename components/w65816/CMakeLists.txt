# WDC 65C816 core (see include/w65816.h)
idf_component_register(SRCS "w65816.c" "w65816_dis.c" "w65816_dbg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES emudbg
                       LDFRAGMENTS "linker.lf")

# Runs in the emulation loop: small (-Os: the run loop must fit the
# instruction cache, see w65816.c), with jump tables for the opcode switch
# (the IDF turns them off for code that may run with the flash cache
# disabled; the emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -fjump-tables -ftree-switch-conversion)
