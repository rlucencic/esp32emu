# Motorola 6809 / Hitachi 6309 core (see include/m6809.h)
idf_component_register(SRCS "m6809.c" "m6809_dis.c" "m6809_dbg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES emudbg
                       LDFRAGMENTS "linker.lf")

# Runs in the emulation loop: jump tables for the opcode switches (the IDF
# turns them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
