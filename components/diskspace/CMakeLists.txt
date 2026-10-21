# Free space on disk images, as their own filesystems count it (see include/diskspace.h)
idf_component_register(SRCS "fat.c" "apple.c" "cpm.c" "trdos.c" "cbm.c" "atari.c" "coco.c" "acorn.c"
                       INCLUDE_DIRS "include")
