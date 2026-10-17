# uPD765A floppy controller with DSK/EDSK images (see include/upd765.h)
idf_component_register(SRCS "upd765.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES heap log)
