# x86 core: 8088, 8086, 80286 (see include/x86.h)
idf_component_register(SRCS "x86.c" "x87.c" "x86_dis.c" "x86_dbg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES emudbg
                       LDFRAGMENTS "linker.lf")

# Runs in the emulation loop: jump tables for the opcode switch (the IDF
# turns them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
