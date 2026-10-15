#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "emu/json.h"

void emu_json_printf(emu_json_t *j, const char *fmt, ...)
{
    if (j->len >= j->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, ap);
    va_end(ap);
    j->len += n > 0 ? n : 0;
    if (j->len >= j->size) {
        j->len = j->size - 1;
    }
}

void emu_json_begin(emu_json_t *j, char *buf, size_t size)
{
    j->buf = buf;
    j->size = size;
    j->len = 0;
    j->depth = 0;
    j->first[0] = true;
    buf[0] = 0;
    emu_json_printf(j, "{");
}

/* Comma if needed, then the key */
static void item(emu_json_t *j, const char *key)
{
    if (!j->first[j->depth]) {
        emu_json_printf(j, ",");
    }
    j->first[j->depth] = false;
    if (key != NULL) {
        emu_json_printf(j, "\"%s\":", key);
    }
}

static void string(emu_json_t *j, const char *s)
{
    if (s == NULL) {
        emu_json_printf(j, "null");
        return;
    }
    emu_json_printf(j, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            emu_json_printf(j, "\\%c", *s);
        } else if ((unsigned char)*s >= 0x20) {
            emu_json_printf(j, "%c", *s);
        }
    }
    emu_json_printf(j, "\"");
}

void emu_json_str(emu_json_t *j, const char *key, const char *s)
{
    item(j, key);
    string(j, s);
}

void emu_json_num(emu_json_t *j, const char *key, double v)
{
    item(j, key);
    if (isnan(v)) {
        emu_json_printf(j, "null");
    } else {
        emu_json_printf(j, "%.1f", v);
    }
}

void emu_json_int(emu_json_t *j, const char *key, long long v)
{
    item(j, key);
    emu_json_printf(j, "%lld", v);
}

void emu_json_bool(emu_json_t *j, const char *key, bool v)
{
    item(j, key);
    emu_json_printf(j, v ? "true" : "false");
}

static void open_scope(emu_json_t *j, const char *key, const char *bracket)
{
    item(j, key);
    emu_json_printf(j, "%s", bracket);
    if (j->depth < EMU_JSON_DEPTH - 1) {
        j->depth++;
    }
    j->first[j->depth] = true;
}

void emu_json_object(emu_json_t *j, const char *key)
{
    open_scope(j, key, "{");
}

void emu_json_array(emu_json_t *j, const char *key)
{
    open_scope(j, key, "[");
}

void emu_json_end_object(emu_json_t *j)
{
    if (j->depth > 0) {
        j->depth--;
    }
    emu_json_printf(j, "}");
}

void emu_json_end_array(emu_json_t *j)
{
    if (j->depth > 0) {
        j->depth--;
    }
    emu_json_printf(j, "]");
}

void emu_json_row(emu_json_t *j, const char *key, const char *html)
{
    emu_json_array(j, NULL);
    emu_json_str(j, NULL, key);
    emu_json_str(j, NULL, html);
    emu_json_end_array(j);
}

void emu_json_row_bar(emu_json_t *j, const char *key, const char *html, int percent)
{
    emu_json_array(j, NULL);
    emu_json_str(j, NULL, key);
    emu_json_str(j, NULL, html);
    emu_json_int(j, NULL, percent);
    emu_json_end_array(j);
}

void emu_json_drive(emu_json_t *j, const char *label, const char *name, bool read_only,
                    bool light, unsigned long reads, unsigned long writes, const char *detail)
{
    emu_json_object(j, NULL);
    emu_json_str(j, "label", label);
    emu_json_str(j, "name", name);
    emu_json_bool(j, "read_only", read_only);
    emu_json_bool(j, "light", light);
    emu_json_int(j, "reads", reads);
    emu_json_int(j, "writes", writes);
    emu_json_str(j, "detail", detail);
    emu_json_end_object(j);
}
