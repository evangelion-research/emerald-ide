/* buffer.c — a simple line-array text buffer.
 *
 * Files this size (Emerald sources are small) do not need a gap buffer or a
 * rope; a line array keeps every position query trivial and the cursor,
 * selection and statement-splitting code simple. Line and column are
 * 0-based, columns are byte offsets into the line (UTF-8 handled at the
 * rendering boundary via the utf8_* helpers).
 */
#include "ide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static char *xstrndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void line_set(Buffer *b, int i, const char *s, int len) {
    free(b->lines[i]);
    b->lines[i] = xstrndup(s, (size_t)len);
    b->lens[i] = len;
}

void buf_init(Buffer *b) {
    memset(b, 0, sizeof *b);
    b->cap = 16;
    b->lines = malloc(sizeof(char *) * (size_t)b->cap);
    b->lens = malloc(sizeof(int) * (size_t)b->cap);
    if (!b->lines || !b->lens) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    b->lines[0] = xstrndup("", 0);
    b->lens[0] = 0;
    b->count = 1;
}

void buf_free(Buffer *b) {
    if (!b->lines) return;
    for (int i = 0; i < b->count; i++) free(b->lines[i]);
    free(b->lines);
    free(b->lens);
    memset(b, 0, sizeof *b);
}

void buf_copy(const Buffer *src, Buffer *dst) {
    dst->cur_line = src->cur_line;
    dst->cur_col = src->cur_col;
    dst->sel_line = src->sel_line;
    dst->sel_col = src->sel_col;
    dst->dirty = src->dirty;
    dst->count = src->count;
    dst->cap = src->count > 0 ? src->count : 1;
    dst->lines = malloc(sizeof(char *) * (size_t)dst->cap);
    dst->lens = malloc(sizeof(int) * (size_t)dst->cap);
    if (!dst->lines || !dst->lens) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    for (int i = 0; i < src->count; i++) {
        dst->lines[i] = xstrndup(src->lines[i], (size_t)src->lens[i]);
        dst->lens[i] = src->lens[i];
    }
}

static void buf_grow(Buffer *b, int need) {
    if (need <= b->cap) return;
    int ncap = b->cap ? b->cap : 16;
    while (ncap < need) ncap *= 2;
    b->lines = realloc(b->lines, sizeof(char *) * (size_t)ncap);
    b->lens = realloc(b->lens, sizeof(int) * (size_t)ncap);
    if (!b->lines || !b->lens) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    b->cap = ncap;
}

void buf_set_text(Buffer *b, const char *text) {
    for (int i = 0; i < b->count; i++) free(b->lines[i]);
    b->count = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        /* strip a trailing \r (CRLF files) */
        if (n > 0 && p[n - 1] == '\r') n--;
        buf_grow(b, b->count + 1);
        b->lines[b->count] = xstrndup(p, n);
        b->lens[b->count] = (int)n;
        b->count++;
        if (!nl) break;
        p = nl + 1;
    }
    if (b->count == 0) {
        buf_grow(b, 1);
        b->lines[0] = xstrndup("", 0);
        b->lens[0] = 0;
        b->count = 1;
    }
    buf_ensure_cursor(b);
}

char *buf_get_text(const Buffer *b) {
    size_t total = 1;
    for (int i = 0; i < b->count; i++) total += (size_t)b->lens[i] + 1;
    char *out = malloc(total);
    if (!out) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    char *p = out;
    for (int i = 0; i < b->count; i++) {
        memcpy(p, b->lines[i], (size_t)b->lens[i]);
        p += b->lens[i];
        *p++ = '\n';
    }
    *p = '\0';
    return out;
}

void buf_ensure_cursor(Buffer *b) {
    if (b->cur_line >= b->count) b->cur_line = b->count - 1;
    if (b->cur_line < 0) b->cur_line = 0;
    if (b->cur_col > b->lens[b->cur_line]) b->cur_col = b->lens[b->cur_line];
    if (b->cur_col < 0) b->cur_col = 0;
    if (b->sel_line >= 0) {
        if (b->sel_line >= b->count) b->sel_line = b->count - 1;
        if (b->sel_col > b->lens[b->sel_line]) b->sel_col = b->lens[b->sel_line];
    }
}

bool buf_load_file(Buffer *b, const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errcap, "cannot open '%s'", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)sz + 1);
    if (!data || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        snprintf(err, errcap, "cannot read '%s'", path);
        return false;
    }
    data[sz] = '\0';
    fclose(f);
    buf_set_text(b, data);
    free(data);
    return true;
}

bool buf_save_file(const Buffer *b, const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errcap, "cannot write '%s'", path);
        return false;
    }
    for (int i = 0; i < b->count; i++) {
        if (fwrite(b->lines[i], 1, (size_t)b->lens[i], f) != (size_t)b->lens[i] ||
            fputc('\n', f) == EOF) {
            fclose(f);
            snprintf(err, errcap, "write error on '%s'", path);
            return false;
        }
    }
    if (fclose(f) != 0) {
        snprintf(err, errcap, "close error on '%s'", path);
        return false;
    }
    return true;
}

/* --- editing ------------------------------------------------------- */

void buf_insert_text(Buffer *b, int line, int col, const char *s, size_t n) {
    if (line < 0 || line >= b->count) return;
    if (col < 0) col = 0;
    if (col > b->lens[line]) col = b->lens[line];
    char *nl = memchr(s, '\n', n);
    if (!nl) {
        /* simple in-line insert */
        int len = b->lens[line];
        char *newl = malloc((size_t)len + n + 1);
        if (!newl) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
        memcpy(newl, b->lines[line], (size_t)col);
        memcpy(newl + col, s, n);
        memcpy(newl + col + n, b->lines[line] + col, (size_t)(len - col));
        newl[len + (int)n] = '\0';
        free(b->lines[line]);
        b->lines[line] = newl;
        b->lens[line] = len + (int)n;
        return;
    }
    /* multi-line insert: split at the first newline, recurse */
    size_t first = (size_t)(nl - s);
    buf_insert_text(b, line, col, s, first);
    buf_grow(b, b->count + 1);
    memmove(&b->lines[line + 2], &b->lines[line + 1],
            sizeof(char *) * (size_t)(b->count - line - 1));
    memmove(&b->lens[line + 2], &b->lens[line + 1],
            sizeof(int) * (size_t)(b->count - line - 1));
    b->count++;
    int rest = b->lens[line] - col;
    b->lines[line + 1] = xstrndup(b->lines[line] + col, (size_t)rest);
    b->lens[line + 1] = rest;
    line_set(b, line, b->lines[line], col);
    buf_insert_text(b, line + 1, 0, nl + 1, n - first - 1);
}

void buf_insert_char(Buffer *b, int line, int col, unsigned char c) {
    char s[2] = { (char)c, '\0' };
    buf_insert_text(b, line, col, s, 1);
}

void buf_delete_range(Buffer *b, int l1, int c1, int l2, int c2) {
    if (l1 < 0 || l1 >= b->count || l2 < 0 || l2 >= b->count) return;
    if (l1 > l2 || (l1 == l2 && c1 > c2)) return;
    if (l1 == l2) {
        int len = b->lens[l1];
        if (c1 < 0) c1 = 0;
        if (c2 > len) c2 = len;
        if (c1 == c2) return;
        char *newl = malloc((size_t)len - (size_t)(c2 - c1) + 1);
        if (!newl) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
        memcpy(newl, b->lines[l1], (size_t)c1);
        memcpy(newl + c1, b->lines[l1] + c2, (size_t)(len - c2));
        newl[len - (c2 - c1)] = '\0';
        free(b->lines[l1]);
        b->lines[l1] = newl;
        b->lens[l1] = len - (c2 - c1);
        return;
    }
    /* delete from (l1,c1) to (l2,c2): join the two lines */
    int len1 = b->lens[l1], len2 = b->lens[l2];
    if (c1 > len1) c1 = len1;
    if (c2 > len2) c2 = len2;
    char *joined = malloc((size_t)c1 + (size_t)(len2 - c2) + 1);
    if (!joined) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    memcpy(joined, b->lines[l1], (size_t)c1);
    memcpy(joined + c1, b->lines[l2] + c2, (size_t)(len2 - c2));
    joined[c1 + (len2 - c2)] = '\0';
    free(b->lines[l1]);
    b->lines[l1] = joined;
    b->lens[l1] = c1 + (len2 - c2);
    int drop = l2 - l1;
    for (int i = l1 + 1; i + drop < b->count; i++) {
        free(b->lines[i]);
        b->lines[i] = b->lines[i + drop];
        b->lens[i] = b->lens[i + drop];
    }
    b->count -= drop;
}

/* --- selection ----------------------------------------------------- */

void buf_sel_clear(Buffer *b) { b->sel_line = -1; }

bool buf_sel_range(const Buffer *b, int *l1, int *c1, int *l2, int *c2) {
    if (b->sel_line < 0) return false;
    if (b->sel_line < b->cur_line ||
        (b->sel_line == b->cur_line && b->sel_col < b->cur_col)) {
        *l1 = b->sel_line; *c1 = b->sel_col; *l2 = b->cur_line; *c2 = b->cur_col;
    } else {
        *l1 = b->cur_line; *c1 = b->cur_col; *l2 = b->sel_line; *c2 = b->sel_col;
    }
    return true;
}

char *buf_sel_text(const Buffer *b) {
    int l1, c1, l2, c2;
    if (!buf_sel_range(b, &l1, &c1, &l2, &c2)) return NULL;
    size_t total = 1;
    for (int i = l1; i <= l2; i++) {
        int from = (i == l1) ? c1 : 0;
        int to = (i == l2) ? c2 : b->lens[i];
        total += (size_t)(to - from) + 1;
    }
    char *out = malloc(total);
    if (!out) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    char *p = out;
    for (int i = l1; i <= l2; i++) {
        int from = (i == l1) ? c1 : 0;
        int to = (i == l2) ? c2 : b->lens[i];
        memcpy(p, b->lines[i] + from, (size_t)(to - from));
        p += to - from;
        if (i < l2) *p++ = '\n';
    }
    *p = '\0';
    return out;
}

void buf_sel_delete(Buffer *b) {
    int l1, c1, l2, c2;
    if (!buf_sel_range(b, &l1, &c1, &l2, &c2)) return;
    buf_delete_range(b, l1, c1, l2, c2);
    b->cur_line = l1;
    b->cur_col = c1;
    buf_sel_clear(b);
}

/* --- utf-8 helpers -------------------------------------------------- */

int utf8_codepoint(const char *s, int *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { if (len) *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        if (len) *len = 2;
        return ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        if (len) *len = 3;
        return ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
        (u[3] & 0xC0) == 0x80) {
        if (len) *len = 4;
        return ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
               ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    }
    if (len) *len = 1;
    return u[0];
}

const char *utf8_prev(const char *s, const char *p) {
    (void)s;
    if (p <= s) return s;
    p--;
    while (p > s && ((unsigned char)*p & 0xC0) == 0x80) p--;
    return p;
}

const char *utf8_next(const char *s, const char *p) {
    if (!*p) return p;
    int len;
    utf8_codepoint(p, &len);
    return p + len;
}

int utf8_col_to_byte(const char *line, int col) {
    const char *p = line;
    int n = 0;
    while (*p && n < col) {
        p = utf8_next(line, p);
        n++;
    }
    return (int)(p - line);
}

int utf8_byte_to_col(const char *line, int byte) {
    const char *p = line;
    int n = 0;
    while (*p && (p - line) < byte) {
        p = utf8_next(line, p);
        n++;
    }
    return n;
}
