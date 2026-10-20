#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Disk images in one folder on the SD card, for the machine's drives. The
 * inserted disks are remembered in NVS and go back in at the next start.
 *
 * Insert, eject and create only while the emulation is paused or not
 * started yet (emu_paused() does that).
 */

#define DISKS_MAX_DRIVES    5           /* the CoCo's: two floppies, hard disk, tape, cartridge */
#define DISKS_NAME_LEN      96

typedef struct {
    uint32_t bytes;
    const char *label;          /* "800 KB" */
} disks_size_t;

typedef struct {
    const char *folder;                 /* relative to the card's root, e.g. "floppies" */
    const char *const *extensions;      /* ".st", ".msa", ..., NULL */
    const char *description;            /* "Disk images (.ST, .MSA)" */
    const char *title;                  /* setup screen: "Floppy disks" */
    const char *button;                 /* its button: "Floppy" */
    const char *nvs_namespace;          /* drives are keys disk_a, disk_b, ... */
    int drives;
    const char *const *drive_names;     /* "A:", "B:" */
    const char *new_prefix;             /* blank disks: EMPTY0001.ST ... */
    const char *new_extension;
    const disks_size_t *sizes;          /* blank disk sizes offered, first = default */
    int size_count;
    uint64_t max_upload;                /* web uploads, bytes */
    /* The folder's subfolders are listed too, as "NAME/", for drives the
     * machine makes of a folder's files (the PC's hard disks): insert()
     * gets the folder's path with the slash. The web page can't download,
     * replace or delete them.
     */
    bool folders;

    /* The machine's drives */
    bool (*insert)(int drive, const char *path);
    void (*eject)(int drive);
    const char *(*path)(int drive);     /* NULL when empty */
    bool (*write_protected)(int drive);
    bool (*create)(const char *path, uint32_t bytes);
    /* An image is deleted or replaced: drop files made from it (optional) */
    void (*removed)(const char *path);
    /* Optional, for drives of different kinds (floppies and a cartridge
     * slot): which images a drive takes (the setup screen lists only
     * those), and whether it gets "New" blank disks. NULL: all of them.
     */
    bool (*accepts)(int drive, const char *name);
    bool (*takes_blank)(int drive);
    /* Optional, for blank images of different kinds (the Spectrum's TR-DOS
     * and +3 disks, tapes): the size made for a drive when none is chosen
     * (drive -1: any; NULL: sizes[0]), and the extension of a blank image
     * of that size (NULL: new_extension)
     */
    uint32_t (*default_size)(int drive);
    const char *(*extension_for)(uint32_t bytes);
} disks_config_t;

/* Make the folder if needed; false without an SD card */
bool disks_init(const disks_config_t *config);
const disks_config_t *disks_config(void);
bool disks_ready(void);

/* Put the remembered disks back in */
void disks_restore(void);

/* Image file names in the folder, sorted; free with disks_free_list() */
int disks_list(char ***names);
void disks_free_list(char **names, int count);

bool disks_insert(int drive, const char *name);
void disks_eject(int drive);

/* The machine ejected the disk itself (from the emulation task): forget
 * it (saved a moment later by disks_poll())
 */
void disks_ejected(int drive);
void disks_poll(void);

/* File name of the disk in the drive, NULL when empty */
const char *disks_name(int drive);

/* Path of the folder (no trailing slash) */
const char *disks_folder(void);

/* `name` is a plain file name with one of the extensions: no folders */
bool disks_valid_name(const char *name);

/* `name` can go into a drive: a valid name, or with `folders` a subfolder's
 * "NAME/" that's there
 */
bool disks_can_insert(const char *name);

/* `name` is in a drive */
bool disks_in_use(const char *name);

/* `name` was deleted or replaced */
void disks_removed(const char *name);

/* The blank disk size made for a drive (-1: any) when none is chosen */
uint32_t disks_default_size(int drive);

/* Blank disk <prefix>nnnn<ext> (next free number) of `bytes`, put into the
 * drive (drive < 0: just made); its name in `name`
 */
bool disks_create(int drive, uint32_t bytes, char *name, size_t len);
