# ARM2 core (see include/arm2.h)
idf_component_register(SRCS "arm2.c" "arm2_dis.c" "arm2_dbg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES emudbg
                       LDFRAGMENTS "linker.lf")

# Runs in the emulation loop: jump tables for the instruction switches (the
# IDF turns them off for code that may run with the flash cache disabled;
# the emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
