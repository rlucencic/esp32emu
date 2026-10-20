# emu8950 (Mitsutaka Okazaki, MIT): the Y8950, YM3526 and YM3812 (OPL, OPL2)
idf_component_register(SRCS "emu8950.c" "emuadpcm.c" "emu8950_alloc.c"
                       INCLUDE_DIRS "include"
                       LDFRAGMENTS "linker.lf"
                       PRIV_REQUIRES heap)

# Its allocations PSRAM first (emu8950_alloc.c), the source unchanged
set_source_files_properties(emu8950.c emuadpcm.c PROPERTIES
                            COMPILE_DEFINITIONS "calloc=emu8950_calloc;malloc=emu8950_malloc")

# Runs on the speaker's task, every sample
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function)
