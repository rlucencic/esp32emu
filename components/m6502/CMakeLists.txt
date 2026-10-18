# MOS 6502 core (see include/m6502.h)
idf_component_register(SRCS "m6502.c" "m6502_dbg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES emudbg
                       LDFRAGMENTS "linker.lf")

# Runs in the emulation loop: jump tables for the opcode switch (the IDF
# turns them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
