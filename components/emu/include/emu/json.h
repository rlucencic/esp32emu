#pragma once

#include <stdbool.h>
#include <stddef.h>

/* A JSON text built into a fixed buffer (output past its end is dropped) */
#define EMU_JSON_DEPTH  8

typedef struct {
    char *buf;
    size_t len, size;
    int depth;
    bool first[EMU_JSON_DEPTH]; /* no item yet in the object/array at this depth */
} emu_json_t;

/* Start a document in buf ({ is written: end it with emu_json_end_object) */
void emu_json_begin(emu_json_t *j, char *buf, size_t size);

void emu_json_printf(emu_json_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* "key":"escaped string" (or null), with a comma before it if needed; a
 * NULL key writes just the value (in arrays)
 */
void emu_json_str(emu_json_t *j, const char *key, const char *s);

/* "key":number with one decimal (null for NAN) */
void emu_json_num(emu_json_t *j, const char *key, double v);

/* "key":integer, "key":true/false */
void emu_json_int(emu_json_t *j, const char *key, long long v);
void emu_json_bool(emu_json_t *j, const char *key, bool v);

/* A row for the web page's machine card: ["key","html value"] or with a
 * bar [..., percent]. Only inside the machine's status() callback.
 */
void emu_json_row(emu_json_t *j, const char *key, const char *html);
void emu_json_row_bar(emu_json_t *j, const char *key, const char *html, int percent);

/* A drive for the web page: busy is worked out from the counters moving;
 * light: the machine's drive light is on
 */
void emu_json_drive(emu_json_t *j, const char *label, const char *name, bool read_only,
                    bool light, unsigned long reads, unsigned long writes, const char *detail);
/* ... with the disk's free space and size in bytes, as its own filesystem
 * counts them (size 0: not known; the page shows a bar)
 */
void emu_json_drive_space(emu_json_t *j, const char *label, const char *name, bool read_only, bool light,
                          unsigned long reads, unsigned long writes, const char *detail, uint64_t free_bytes,
                          uint64_t size);

/* Begin "key":{ / "key":[ ... and end them */
void emu_json_object(emu_json_t *j, const char *key);
void emu_json_array(emu_json_t *j, const char *key);
void emu_json_end_object(emu_json_t *j);
void emu_json_end_array(emu_json_t *j);
