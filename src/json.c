/* json.c — a deliberately minimal JSON parser.
 *
 * Just enough to consume `emeraldc --check --json` output: an array of
 * objects with string/number fields. Fully escaped strings (including
 * \uXXXX) are handled; numbers are parsed but only used as ints here.
 */
#include "ide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    union {
        int   b;
        double num;
        char *str;
        struct { JVal **items; int count, cap; } arr;
        struct { char **keys; JVal **vals; int count, cap; } obj;
    } u;
};

typedef struct {
    const char *p;
    int line;
} JParser;

static void jerr(JParser *j, const char *msg) {
    fprintf(stderr, "emerald-ide: malformed compiler JSON at line %d: %s\n",
            j->line, msg);
}

static void j_skip_ws(JParser *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') {
        if (*j->p == '\n') j->line++;
        j->p++;
    }
}

static JVal *j_new(JType t) {
    JVal *v = calloc(1, sizeof *v);
    if (!v) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    v->type = t;
    return v;
}

static JVal *j_parse_value(JParser *j);

static void j_append_utf8(char **s, size_t *n, size_t *cap, unsigned cp) {
    if (cp < 0x80) {
        if (*n + 1 > *cap) { *cap = *cap ? *cap * 2 : 16; *s = realloc(*s, *cap); }
        (*s)[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*n + 2 > *cap) { *cap = *cap ? *cap * 2 : 16; *s = realloc(*s, *cap); }
        (*s)[(*n)++] = (char)(0xC0 | (cp >> 6));
        (*s)[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*n + 3 > *cap) { *cap = *cap ? *cap * 2 : 16; *s = realloc(*s, *cap); }
        (*s)[(*n)++] = (char)(0xE0 | (cp >> 12));
        (*s)[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*s)[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*n + 4 > *cap) { *cap = *cap ? *cap * 2 : 16; *s = realloc(*s, *cap); }
        (*s)[(*n)++] = (char)(0xF0 | (cp >> 18));
        (*s)[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*s)[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*s)[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static JVal *j_parse_string(JParser *j) {
    j->p++; /* opening quote */
    size_t n = 0, cap = 16;
    char *s = malloc(cap);
    if (!s) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    while (*j->p && *j->p != '"') {
        if (*j->p == '\\') {
            j->p++;
            switch (*j->p) {
            case '"':  j_append_utf8(&s, &n, &cap, '"');  j->p++; break;
            case '\\': j_append_utf8(&s, &n, &cap, '\\'); j->p++; break;
            case '/':  j_append_utf8(&s, &n, &cap, '/');  j->p++; break;
            case 'b':  j_append_utf8(&s, &n, &cap, '\b'); j->p++; break;
            case 'f':  j_append_utf8(&s, &n, &cap, '\f'); j->p++; break;
            case 'n':  j_append_utf8(&s, &n, &cap, '\n'); j->p++; break;
            case 'r':  j_append_utf8(&s, &n, &cap, '\r'); j->p++; break;
            case 't':  j_append_utf8(&s, &n, &cap, '\t'); j->p++; break;
            case 'u': {
                j->p++;
                unsigned cp = 0;
                for (int k = 0; k < 4; k++) {
                    char c = *j->p;
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                    if (*j->p) j->p++;
                }
                /* surrogate pairs: leave them as-is for our purposes */
                j_append_utf8(&s, &n, &cap, cp);
                break;
            }
            default:
                j_append_utf8(&s, &n, &cap, (unsigned char)*j->p);
                if (*j->p) j->p++;
            }
        } else {
            if (*j->p == '\n') j->line++;
            j_append_utf8(&s, &n, &cap, (unsigned char)*j->p);
            j->p++;
        }
    }
    if (*j->p == '"') j->p++;
    if (n + 1 > cap) { cap = cap * 2; s = realloc(s, cap); }
    s[n] = '\0';
    JVal *v = j_new(J_STR);
    v->u.str = s;
    return v;
}

static JVal *j_parse_array(JParser *j) {
    j->p++; /* '[' */
    JVal *v = j_new(J_ARR);
    j_skip_ws(j);
    if (*j->p == ']') { j->p++; return v; }
    for (;;) {
        j_skip_ws(j);
        if (v->u.arr.count == v->u.arr.cap) {
            v->u.arr.cap = v->u.arr.cap ? v->u.arr.cap * 2 : 4;
            v->u.arr.items = realloc(v->u.arr.items,
                                     sizeof(JVal *) * (size_t)v->u.arr.cap);
        }
        v->u.arr.items[v->u.arr.count++] = j_parse_value(j);
        j_skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        jerr(j, "expected ',' or ']'");
        break;
    }
    return v;
}

static JVal *j_parse_object(JParser *j) {
    j->p++; /* '{' */
    JVal *v = j_new(J_OBJ);
    j_skip_ws(j);
    if (*j->p == '}') { j->p++; return v; }
    for (;;) {
        j_skip_ws(j);
        JVal *k = j_parse_string(j);
        j_skip_ws(j);
        if (*j->p == ':') j->p++;
        JVal *val = j_parse_value(j);
        if (v->u.obj.count == v->u.obj.cap) {
            v->u.obj.cap = v->u.obj.cap ? v->u.obj.cap * 2 : 4;
            v->u.obj.keys = realloc(v->u.obj.keys,
                                    sizeof(char *) * (size_t)v->u.obj.cap);
            v->u.obj.vals = realloc(v->u.obj.vals,
                                    sizeof(JVal *) * (size_t)v->u.obj.cap);
        }
        v->u.obj.keys[v->u.obj.count] = k->u.str;
        v->u.obj.vals[v->u.obj.count] = val;
        v->u.obj.count++;
        free(k);
        j_skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        jerr(j, "expected ',' or '}'");
        break;
    }
    return v;
}

static JVal *j_parse_value(JParser *j) {
    j_skip_ws(j);
    if (*j->p == '"') return j_parse_string(j);
    if (*j->p == '[') return j_parse_array(j);
    if (*j->p == '{') return j_parse_object(j);
    if (strncmp(j->p, "null", 4) == 0) { j->p += 4; return j_new(J_NULL); }
    if (strncmp(j->p, "true", 4) == 0) { j->p += 4; JVal *v = j_new(J_BOOL); v->u.b = 1; return v; }
    if (strncmp(j->p, "false", 5) == 0) { j->p += 5; return j_new(J_BOOL); }
    /* number */
    char *end;
    double d = strtod(j->p, &end);
    if (end == j->p) { jerr(j, "unexpected character"); return j_new(J_NULL); }
    j->p = end;
    JVal *v = j_new(J_NUM);
    v->u.num = d;
    return v;
}

static void j_free(JVal *v) {
    if (!v) return;
    switch (v->type) {
    case J_STR: free(v->u.str); break;
    case J_ARR:
        for (int i = 0; i < v->u.arr.count; i++) j_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case J_OBJ:
        for (int i = 0; i < v->u.obj.count; i++) {
            free(v->u.obj.keys[i]);
            j_free(v->u.obj.vals[i]);
        }
        free(v->u.obj.keys);
        free(v->u.obj.vals);
        break;
    default: break;
    }
    free(v);
}

static const JVal *j_obj_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->u.obj.count; i++)
        if (strcmp(obj->u.obj.keys[i], key) == 0) return obj->u.obj.vals[i];
    return NULL;
}

static void j_copy_str(const JVal *v, char *dst, size_t cap) {
    dst[0] = '\0';
    if (!v || v->type != J_STR) return;
    snprintf(dst, cap, "%s", v->u.str);
}

static int j_copy_int(const JVal *v, int def) {
    if (!v || v->type != J_NUM) return def;
    return (int)v->u.num;
}

/* ------------------------------------------------------------------ */

int json_parse_diags(const char *text, Diag *out, int max) {
    JParser j = { text, 1 };
    JVal *root = j_parse_value(&j);
    if (root->type != J_ARR) {
        j_free(root);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < root->u.arr.count && n < max; i++) {
        const JVal *o = root->u.arr.items[i];
        if (o->type != J_OBJ) continue;
        Diag *d = &out[n];
        memset(d, 0, sizeof *d);
        j_copy_str(j_obj_get(o, "kind"), d->kind, sizeof d->kind);
        j_copy_str(j_obj_get(o, "severity"), d->severity, sizeof d->severity);
        j_copy_str(j_obj_get(o, "code"), d->code, sizeof d->code);
        j_copy_str(j_obj_get(o, "file"), d->file, sizeof d->file);
        j_copy_str(j_obj_get(o, "message"), d->message, sizeof d->message);
        j_copy_str(j_obj_get(o, "expected"), d->expected, sizeof d->expected);
        j_copy_str(j_obj_get(o, "actual"), d->actual, sizeof d->actual);
        j_copy_str(j_obj_get(o, "source_line"), d->source_line, sizeof d->source_line);
        d->line = j_copy_int(j_obj_get(o, "line"), 0);
        d->col = j_copy_int(j_obj_get(o, "column"), 0);
        if (d->line == 0) d->line = j_copy_int(j_obj_get(o, "col"), 0);
        n++;
    }
    j_free(root);
    return n;
}
