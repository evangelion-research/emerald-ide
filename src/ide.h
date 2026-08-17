/* ide.h — Emerald IDE core types and API.
 *
 * The core (buffer, statements, session, compiler runner) is pure C11 with
 * zero GUI dependencies, mirroring the "core/ never includes a UI header"
 * rule from SPEC.md §4. The raygui front end lives entirely in ui.c.
 */
#ifndef EMERALD_IDE_H
#define EMERALD_IDE_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* text buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char  **lines;      /* each line: no '\n' and no '\r' */
    int    *lens;       /* byte length per line */
    int     count;      /* number of lines */
    int     cap;
    int     cur_line;   /* 0-based cursor line */
    int     cur_col;    /* cursor byte offset within line */
    int     sel_line;   /* selection anchor line, -1 = none */
    int     sel_col;    /* selection anchor byte offset */
    bool    dirty;
} Buffer;

void  buf_init(Buffer *b);
void  buf_free(Buffer *b);
void  buf_copy(const Buffer *src, Buffer *dst);   /* deep copy */
void  buf_set_text(Buffer *b, const char *text);  /* replaces content */
char *buf_get_text(const Buffer *b);              /* malloc'd, caller frees */
bool  buf_load_file(Buffer *b, const char *path, char *err, size_t errcap);
bool  buf_save_file(const Buffer *b, const char *path, char *err, size_t errcap);
void  buf_ensure_cursor(Buffer *b);

void  buf_insert_char(Buffer *b, int line, int col, unsigned char c);
void  buf_insert_text(Buffer *b, int line, int col, const char *s, size_t n);
void  buf_delete_range(Buffer *b, int l1, int c1, int l2, int c2); /* [..) */

/* selection helpers */
void  buf_sel_clear(Buffer *b);
bool  buf_sel_range(const Buffer *b, int *l1, int *c1, int *l2, int *c2);
char *buf_sel_text(const Buffer *b);              /* malloc'd, caller frees */
void  buf_sel_delete(Buffer *b);

/* utf-8 helpers: columns are display units, byte offsets are buffer units */
int  utf8_codepoint(const char *s, int *len);
const char *utf8_prev(const char *s, const char *p);
const char *utf8_next(const char *s, const char *p);
int  utf8_col_to_byte(const char *line, int col);
int  utf8_byte_to_col(const char *line, int byte);

/* ------------------------------------------------------------------ */
/* statements — top-level units of advance                            */
/* ------------------------------------------------------------------ */

enum {
    STMT_DEF, STMT_TYPE, STMT_IMPORT, STMT_CONST, STMT_IF, STMT_WHILE,
    STMT_FOR, STMT_MATCH, STMT_RETURN, STMT_BREAK, STMT_CONTINUE, STMT_PASS,
    STMT_BLOCK, STMT_ASSIGN, STMT_EXPR,
};

typedef struct {
    int  kind;
    int  start_line;   /* first content line, 0-based */
    int  start_col;    /* byte offset of first token on start_line */
    int  end_line;     /* last line, 0-based */
    int  end_col;      /* byte offset one past last char on end_line */
    bool unterminated; /* block/string left open at end of buffer */
} Statement;

/* ------------------------------------------------------------------ */
/* diagnostics — from emeraldc --check --json                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char kind[16];
    char severity[16];
    char code[32];
    char file[512];
    int  line;         /* 1-based */
    int  col;          /* 1-based */
    char message[512];
    char expected[256];
    char actual[256];
    char source_line[512];
    bool from_entry;   /* diag refers to the checked file itself */
} Diag;

int json_parse_diags(const char *text, Diag *out, int max);

/* ------------------------------------------------------------------ */
/* session — the proof-session state machine (SPEC.md §1b)            */
/* ------------------------------------------------------------------ */

enum { SESS_IDLE, SESS_OK, SESS_FAILED, SESS_ERROR };

typedef struct {
    int   locus;          /* statements [0, locus) covered by the last check */
    int   first_failing;  /* index of first failing stmt; nstmts if none */
    int   status;         /* SESS_* */
    Diag *diags;
    int   ndiags;
    char  last_error[512];
    double last_check_ms;
} Session;

/* obligation ledger */
typedef struct {
    int   stmt;           /* statement index */
    int   line;           /* 1-based line of the proposition site */
    char  kind[24];       /* "never binding" | "negation fn" | "generic" ... */
    char  name[160];      /* enclosing function / site name */
    char  verdict[16];    /* proved | out of reach | unchecked */
} Obligation;

typedef struct {
    Buffer     buf;
    char       file_path[1024];  /* "" = unsaved */
    char       base_name[256];

    Statement *stmts;
    int        nstmts;

    Session    sess;

    char       env_text[16384];  /* goal panel, source-derived */
    Obligation *obligs;
    int        nobligs;

    char       compiler[1024];   /* resolved emeraldc path, "" if none */
    bool       using_builtin;    /* fell back to the built-in linter */

    /* repl */
    char       repl_input[1024];
    char       repl_output[4096];
} Doc;

void doc_init(Doc *d, const char *path);
void doc_free(Doc *d);
void doc_new(Doc *d);                              /* blank unsaved doc */
bool doc_open(Doc *d, const char *path, char *err, size_t errcap);
bool doc_save(Doc *d, const char *path, char *err, size_t errcap);
void doc_resolve_compiler(Doc *d);
/* Absolute path of the macOS .app bundle's Contents/Resources, "" when not
 * running from a bundle (dev builds, headless driver). */
const char *bundle_resources(void);

/* session operations */
void sess_split_statements(Doc *d);
void sess_clear(Doc *d);
void sess_check(Doc *d);           /* re-check prefix [0, locus) */
void sess_advance(Doc *d);
void sess_retract(Doc *d);
void sess_goto_cursor(Doc *d);
void sess_check_all(Doc *d);
void sess_on_edit(Doc *d, int edit_line);   /* retract + re-check */
void sess_derive(Doc *d);          /* rebuild env panel text from current state */
void sess_ledger(Doc *d);          /* rebuild the obligation ledger */
int  sess_stmt_at_line(const Doc *d, int line);
void sess_run_repl(Doc *d);        /* evaluate repl_input in accepted scope */

#endif /* EMERALD_IDE_H */
