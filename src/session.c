/* session.c — the proof-session state machine (SPEC.md §1).
 *
 * Every advance, retract, goto, or edit-triggered recovery re-runs
 * `emeraldc --check --json` over the accepted prefix from scratch (SPEC.md
 * §1c). The prefix is materialised as a temp file next to the source so
 * relative imports resolve identically; diagnostics map back to the buffer
 * by line number.
 *
 * When no emeraldc is found the session falls back to a tiny built-in
 * linter (unterminated blocks/strings) so the IDE remains usable (SPEC.md
 * §14: the editor must survive the compiler being missing).
 */
#include "ide.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>   /* _NSGetExecutablePath: bundle detection */
#endif

#define DIAG_MAX 256
#define CHECK_TIMEOUT_MS 10000

/* ------------------------------------------------------------------ */
/* tiny helpers                                                       */
/* ------------------------------------------------------------------ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Compiler timeout in ms. EMERALD_IDE_CHECK_TIMEOUT_MS overrides it — a
 * test hook so the golden tests can exercise the timeout path without
 * waiting the full 10 s. */
static int check_timeout_ms(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = CHECK_TIMEOUT_MS;
        const char *e = getenv("EMERALD_IDE_CHECK_TIMEOUT_MS");
        if (e && *e) {
            int v = atoi(e);
            if (v >= 50) cached = v;   /* floor: never a 0-ms deadline */
        }
    }
    return cached;
}

/* split "/a/b/c.rald" into dir and base (static buffers, not reentrant) */
static const char *path_dir(const char *path) {
    static char buf[1024];
    const char *slash = strrchr(path, '/');
    if (!slash) return ".";
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, path, n);
    buf[n] = '\0';
    return buf;
}

static const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void free_stmts(Doc *d) {
    free(d->stmts);
    d->stmts = NULL;
    d->nstmts = 0;
}

static void free_diags(Doc *d) {
    free(d->sess.diags);
    d->sess.diags = NULL;
    d->sess.ndiags = 0;
}

static void free_obligs(Doc *d) {
    free(d->obligs);
    d->obligs = NULL;
    d->nobligs = 0;
}

/* ------------------------------------------------------------------ */
/* statement splitter                                                 */
/* ------------------------------------------------------------------ */

/* Emerald statement starter keywords (lexer.c keywords list). */
static const char *const stmt_keywords[] = {
    "def", "if", "while", "for", "return", "break", "continue", "pass",
    "type", "const", "match", "import", "from",
};
#define N_STMT_KEYWORDS (int)(sizeof stmt_keywords / sizeof stmt_keywords[0])

static int keyword_kind(const char *word, int len) {
    for (int i = 0; i < N_STMT_KEYWORDS; i++) {
        if ((int)strlen(stmt_keywords[i]) == len &&
            memcmp(stmt_keywords[i], word, (size_t)len) == 0) {
            switch (i) {
            case 0: return STMT_DEF;
            case 1: return STMT_IF;
            case 2: return STMT_WHILE;
            case 3: return STMT_FOR;
            case 4: return STMT_RETURN;
            case 5: return STMT_BREAK;
            case 6: return STMT_CONTINUE;
            case 7: return STMT_PASS;
            case 8: return STMT_TYPE;
            case 9: return STMT_CONST;
            case 10: return STMT_MATCH;
            case 11: return STMT_IMPORT;
            case 12: return STMT_IMPORT; /* from */
            }
        }
    }
    return -1;
}

typedef struct {
    int depth;      /* paren + bracket + brace nesting across lines */
    char in_string; /* 0 or the quote char */
} ScanState;

typedef struct {
    int first_col;   /* byte col of first token; -1 if blank/comment only */
    int first_kind;  /* STMT_* or -1 if not a starter keyword */
    int last_sig;    /* last significant char in the line, 0 if none */
    bool depth_zero; /* depth == 0 and not in string at end of line */
} ScanLine;

/* Scan one line, updating the running state. Strings and comments are
 * skipped; `{ ( [`/`} ) ]` adjust depth. */
static void scan_line(const char *s, int len, ScanState *st, ScanLine *out) {
    memset(out, 0, sizeof *out);
    out->first_col = -1;
    out->first_kind = -1;
    int i = 0;
    bool have_first = false;

    /* leading whitespace */
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;

    for (; i < len; i++) {
        char c = s[i];
        if (st->in_string) {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == st->in_string) st->in_string = 0;
            continue;
        }
        if (c == '#') break; /* comment to end of line */
        if (c == '"' || c == '\'') { st->in_string = c; continue; }
        if (c == '{' || c == '(' || c == '[') { st->depth++; out->last_sig = c; continue; }
        if (c == '}' || c == ')' || c == ']') {
            if (st->depth > 0) st->depth--;
            out->last_sig = c;
            continue;
        }
        if (!have_first) {
            have_first = true;
            out->first_col = i;
            if (isalpha((unsigned char)c) || c == '_') {
                int j = i;
                while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
                out->first_kind = keyword_kind(s + i, j - i);
                if (out->first_kind < 0 && s[i] == '{') out->first_kind = STMT_BLOCK;
            } else if (c == '{') {
                out->first_kind = STMT_BLOCK;
            }
        }
        if (!isspace((unsigned char)c)) out->last_sig = c;
    }
    out->depth_zero = (st->depth == 0 && st->in_string == 0);
}

/* Does a depth-0 line ending with this char continue onto the next line? */
static bool is_continuation(int c) {
    switch (c) {
    case ',': case '(': case '[': case '.': case '+': case '-': case '*':
    case '/': case '%': case '<': case '>': case '=': case '&': case '|':
    case ':': case '^': case '?': case '!': case '\\':
        return true;
    default:
        return false;
    }
}

static int classify_expr_stmt(const char *line, int len) {
    /* ASSIGN if a top-level '=' or ':' appears before the first '{' */
    ScanState st = { 0, 0 };
    ScanLine sl;
    scan_line(line, len, &st, &sl);
    /* re-scan tracking delimiters, looking for '=' / ':' at depth 0 */
    st = (ScanState){ 0, 0 };
    int depth = 0;
    for (int i = 0; i < len; i++) {
        char c = line[i];
        if (st.in_string) {
            if (c == '\\') { i++; continue; }
            if (c == st.in_string) st.in_string = 0;
            continue;
        }
        if (c == '#') break;
        if (c == '"' || c == '\'') { st.in_string = c; continue; }
        if (c == '{' || c == '(' || c == '[') { depth++; continue; }
        if (c == '}' || c == ')' || c == ']') { if (depth > 0) depth--; continue; }
        if (depth == 0 && (c == '=' || c == ':')) return STMT_ASSIGN;
    }
    return STMT_EXPR;
}

static void push_stmt(Statement **arr, int *n, int *cap, Statement s) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = realloc(*arr, sizeof(Statement) * (size_t)*cap);
        if (!*arr) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    }
    (*arr)[(*n)++] = s;
}

void sess_split_statements(Doc *d) {
    free_stmts(d);
    Buffer *b = &d->buf;
    Statement *stmts = NULL;
    int n = 0, cap = 0;
    ScanState st = { 0, 0 };
    int cur = -1;      /* open statement index */
    bool cont = false; /* previous line ended with a continuation */

    for (int i = 0; i < b->count; i++) {
        /* the statement-start decision uses the nesting state at the START
         * of the line: a line like `def f() {` must open a statement even
         * though scanning it leaves us inside a block. */
        int depth_at = st.depth;
        char str_at = st.in_string;
        ScanLine sl;
        scan_line(b->lines[i], b->lens[i], &st, &sl);

        bool blank = (sl.first_col < 0);
        bool starts = false;
        if (!blank && depth_at == 0 && str_at == 0) {
            if (cur < 0) starts = true;
            else if (sl.first_kind >= 0) starts = true;
            else if (!cont) starts = true;
        }

        if (starts) {
            if (cur >= 0) stmts[cur].end_line = i - 1;
            Statement s;
            memset(&s, 0, sizeof s);
            s.kind = sl.first_kind >= 0 ? sl.first_kind
                     : classify_expr_stmt(b->lines[i], b->lens[i]);
            s.start_line = i;
            s.start_col = sl.first_col;
            s.end_line = i;
            s.end_col = b->lens[i];
            push_stmt(&stmts, &n, &cap, s);
            cur = n - 1;
            cont = false;
        } else if (cur >= 0) {
            /* continuation (or blank/comment line inside a statement) */
            stmts[cur].end_line = i;
            stmts[cur].end_col = b->lens[i];
            cont = sl.depth_zero && is_continuation(sl.last_sig);
        } else {
            cont = false;
        }
    }
    if (cur >= 0) {
        stmts[cur].unterminated = (st.depth > 0 || st.in_string != 0);
    }
    d->stmts = stmts;
    d->nstmts = n;
}

/* Return the 0-based statement index containing `line`, or nstmts. */
int sess_stmt_at_line(const Doc *d, int line) {
    int lo = 0, hi = d->nstmts;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->stmts[mid].start_line <= line) lo = mid + 1;
        else hi = mid;
    }
    return lo > 0 ? lo - 1 : 0;
}

static char *stmt_text(const Doc *d, int i) {
    const Buffer *b = &d->buf;
    int l1 = d->stmts[i].start_line, l2 = d->stmts[i].end_line;
    size_t total = 1;
    for (int l = l1; l <= l2; l++) total += (size_t)b->lens[l] + 1;
    char *out = malloc(total);
    if (!out) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    char *p = out;
    for (int l = l1; l <= l2; l++) {
        memcpy(p, b->lines[l], (size_t)b->lens[l]);
        p += b->lens[l];
        if (l < l2) *p++ = '\n';
    }
    *p = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* subprocess runner                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int  exit_code;    /* -1 = exec failed */
    bool timed_out;
    char out[65536];
    char err[65536];
} RunResult;

/* Run argv with a timeout, capturing stdout/stderr. Never blocks the caller
 * longer than timeout_ms. */
static void run_cmd(const char *const argv[], int timeout_ms, RunResult *res) {
    memset(res, 0, sizeof *res);
    res->exit_code = -1;

    int outpipe[2], errpipe[2];
    if (pipe(outpipe) != 0 || pipe(errpipe) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        return;
    }
    if (pid == 0) {
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(outpipe[1]);
    close(errpipe[1]);

    struct pollfd fds[2] = {
        { outpipe[0], POLLIN, 0 },
        { errpipe[0], POLLIN, 0 },
    };
    size_t on = 0, en = 0;
    bool out_open = true, err_open = true;
    double deadline = now_ms() + (double)timeout_ms;

    while ((out_open || err_open) && now_ms() < deadline) {
        int remaining = (int)(deadline - now_ms());
        if (remaining < 1) remaining = 1;
        int nfds = 0;
        struct pollfd pf[2];
        for (int k = 0; k < 2; k++) {
            bool open = k == 0 ? out_open : err_open;
            if (open) pf[nfds++] = fds[k];
        }
        if (nfds == 0) break;
        int r = poll(pf, (nfds_t)nfds, remaining);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < 2; k++) {
            int fd = k == 0 ? outpipe[0] : errpipe[0];
            bool *openp = k == 0 ? &out_open : &err_open;
            size_t *np = k == 0 ? &on : &en;
            char *buf = k == 0 ? res->out : res->err;
            size_t cap = sizeof res->out;
            /* is fd ready? */
            bool ready = false;
            for (int q = 0; q < nfds; q++) {
                if (pf[q].fd == fd) ready = true;
            }
            if (!ready || !*openp) continue;
            char chunk[4096];
            ssize_t got = read(fd, chunk, sizeof chunk);
            if (got > 0) {
                size_t room = cap - 1 - *np;
                if (room > 0) {
                    size_t take = (size_t)got < room ? (size_t)got : room;
                    memcpy(buf + *np, chunk, take);
                    *np += take;
                    buf[*np] = '\0';
                }
            } else if (got == 0) {
                *openp = false;
            } else if (errno != EINTR && errno != EAGAIN) {
                *openp = false;
            }
        }
    }

    bool timed = (out_open || err_open);
    if (timed) {
        kill(pid, SIGKILL);
        res->timed_out = true;
    }
    close(outpipe[0]);
    close(errpipe[0]);

    int wstatus = 0;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(wstatus)) res->exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) res->exit_code = 128 + WTERMSIG(wstatus);
}

/* ------------------------------------------------------------------ */
/* emeraldc discovery and invocation                                  */
/* ------------------------------------------------------------------ */

static bool file_exists(const char *path) {
    return path && path[0] && access(path, X_OK) == 0;
}

/* When the IDE runs from a macOS .app bundle, the absolute path of the
 * bundle's Contents/Resources directory (".../Emerald IDE.app/Contents/
 * Resources"); "" otherwise. Resources — the font, the window icon, the
 * bundled compiler and its stdlib — are resolved relative to this so the
 * app works when launched from Finder, whose working directory is "/". */
const char *bundle_resources(void) {
#if defined(__APPLE__)
    static char res[PATH_MAX];
    static bool done = false;
    if (!done) {
        done = true;
        char exe[PATH_MAX];
        uint32_t size = sizeof exe;
        if (_NSGetExecutablePath(exe, &size) == 0) {
            /* ".../Emerald IDE.app/Contents/MacOS/emerald-ide" */
            char *p = strstr(exe, "/Contents/MacOS/");
            if (p) {
                *p = '\0';
                snprintf(res, sizeof res, "%s/Contents/Resources", exe);
            }
        }
    }
    return res;
#else
    return "";
#endif
}

void doc_resolve_compiler(Doc *d) {
    d->compiler[0] = '\0';
    d->using_builtin = true;
    const char *env = getenv("EMERALDC");
    if (env && file_exists(env)) {
        snprintf(d->compiler, sizeof d->compiler, "%s", env);
        d->using_builtin = false;
        return;
    }
    /* compiler bundled inside the .app (Resources/../MacOS/emeraldc); the
     * stdlib sits in Resources/stdlib, so point $EMERALD_STDLIB at it for
     * the child compiler process (it would otherwise look for a relative
     * "stdlib" under the Finder-launched cwd and fail). */
    const char *res = bundle_resources();
    if (res[0]) {
        char cand[PATH_MAX];
        snprintf(cand, sizeof cand, "%s/../MacOS/emeraldc", res);
        if (file_exists(cand)) {
            snprintf(d->compiler, sizeof d->compiler, "%s", cand);
            d->using_builtin = false;
            char std[PATH_MAX];
            snprintf(std, sizeof std, "%s/stdlib", res);
            setenv("EMERALD_STDLIB", std, 0);
            return;
        }
    }
    /* PATH lookup */
    const char *path = getenv("PATH");
    if (path) {
        char *copy = strdup(path);
        char *save = NULL;
        for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
            char cand[1024];
            snprintf(cand, sizeof cand, "%s/emeraldc", dir);
            if (file_exists(cand)) {
                snprintf(d->compiler, sizeof d->compiler, "%s", cand);
                d->using_builtin = false;
                free(copy);
                return;
            }
        }
        free(copy);
    }
    /* sibling checkout of the emerald repo (this project's default layout) */
    char sibling[1024];
    snprintf(sibling, sizeof sibling, "../emerald/bin/emeraldc");
    if (file_exists(sibling)) {
        snprintf(d->compiler, sizeof d->compiler, "%s", sibling);
        d->using_builtin = false;
    }
}

/* Where the truncated prefix is written for a check. */
static void temp_path_for(const Doc *d, char *out, size_t cap) {
    if (d->file_path[0]) {
        snprintf(out, cap, "%s/.%s.eide.rald", path_dir(d->file_path),
                 path_base(d->file_path));
    } else {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        snprintf(out, cap, "%s/eide-unsaved.rald", tmp);
    }
}

/* ------------------------------------------------------------------ */
/* the session                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* check worker                                                        */
/* ------------------------------------------------------------------ */
/*
 * Checks run on a worker thread so the UI never blocks. sess_check()
 * snapshots everything the check needs (the prefix text, the statement
 * table, the compiler path, the temp path) into a CheckReq and hands it
 * to the worker; the worker runs emeraldc and publishes a CheckResult.
 * A generation counter makes stale results harmless: prefix submits and
 * every sess_clear() bump the Doc's expected generation (REPL submits peek
 * instead, so a REPL result can't evict an in-flight prefix check), and
 * sess_poll() only applies results whose generation still matches. The
 * worker never touches the Doc — it works entirely on the snapshot.
 */

enum { CHECK_PREFIX = 0, CHECK_REPL = 1 };

typedef struct {
    unsigned gen;
    int      mode;            /* CHECK_PREFIX | CHECK_REPL */
    int      nstmts;          /* statement count at submit time */
    int      locus;           /* accepted-prefix statement count at submit */
    char     compiler[1024];
    char     tmppath[2048];
    char    *text;            /* snapshot of lines [0..end_line] joined with \n */
    char     repl_expr[1024]; /* CHECK_REPL */
    Statement *stmts;         /* snapshot of the statement table */
} CheckReq;

typedef struct {
    unsigned gen;
    int      mode;
    int      status;          /* SESS_* */
    int      first_failing;
    Diag    *diags;
    int      ndiags;
    char     last_error[512];
    double   last_check_ms;
    char     repl_output[4096]; /* CHECK_REPL */
    bool     fell_back;       /* compiler vanished mid-run; builtin linter used */
} CheckResult;

static pthread_mutex_t g_check_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_check_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t       g_worker;
static bool            g_worker_started = false;
static bool            g_no_worker = false; /* worker could not be created */
static bool            g_shutdown = false;
static bool            g_busy = false;      /* worker is running a request */
static unsigned        g_gen = 0;           /* global generation counter */
static CheckReq        g_pending;
static bool            g_has_pending = false;
static CheckResult     g_result;            /* latest completed prefix check */
static bool            g_has_result = false;
static CheckResult     g_repl_result;       /* latest completed REPL check */
static bool            g_has_repl_result = false;

static void free_req(CheckReq *r) {
    free(r->text);
    free(r->stmts);
    memset(r, 0, sizeof *r);
}

static void free_res(CheckResult *r) {
    free(r->diags);
    memset(r, 0, sizeof *r);
}

/* Bump the generation the Doc expects results for; returns the new value.
 * Any result published for an older generation is discarded by sess_poll. */
static unsigned sess_bump_gen(Doc *d) {
    pthread_mutex_lock(&g_check_lock);
    d->sess.gen = ++g_gen;
    pthread_mutex_unlock(&g_check_lock);
    return d->sess.gen;
}

/* read the generation the Doc expects results for, without bumping it.
 * REPL checks use this: applying a REPL result must not invalidate an
 * in-flight prefix check, but a later edit or advance (which bumps the
 * generation) must still discard a stale REPL result. */
static unsigned sess_peek_gen(Doc *d) {
    pthread_mutex_lock(&g_check_lock);
    unsigned g = d->sess.gen;
    pthread_mutex_unlock(&g_check_lock);
    return g;
}

/* map a 1-based diag line in the checked prefix to an entry statement */
static int stmt_for_line_snap(const Statement *stmts, int nstmts, int line) {
    int l = line - 1;
    for (int i = 0; i < nstmts; i++)
        if (l >= stmts[i].start_line && l <= stmts[i].end_line)
            return i;
    int lo = 0, hi = nstmts;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (stmts[mid].start_line <= l) lo = mid + 1;
        else hi = mid;
    }
    return lo > 0 ? lo - 1 : 0;
}

static void mark_import_failure_snap(const Statement *stmts, int nstmts,
                                     CheckResult *res) {
    int last_import = -1;
    for (int i = 0; i < nstmts; i++)
        if (stmts[i].kind == STMT_IMPORT) last_import = i;
    res->first_failing = last_import >= 0 ? last_import : 0;
}

/* the built-in linter (snapshot variant): no compiler, only structural
 * checks. `count` is the accepted-prefix statement count — statements
 * beyond the locus are not in scope and must not fail the check. */
static void mock_check_snap(const Statement *stmts, int count, CheckResult *res) {
    res->status = SESS_OK;
    res->first_failing = count;
    res->last_error[0] = '\0';
    res->diags = NULL;
    res->ndiags = 0;

    for (int i = 0; i < count; i++) {
        if (stmts[i].unterminated) {
            res->status = SESS_FAILED;
            res->first_failing = i;
            Diag *dg = calloc(1, sizeof(Diag));
            if (!dg) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
            snprintf(dg->kind, sizeof dg->kind, "syntax");
            snprintf(dg->severity, sizeof dg->severity, "error");
            snprintf(dg->code, sizeof dg->code, "E_UNTERMINATED");
            dg->line = stmts[i].end_line + 1;
            dg->col = stmts[i].end_col + 1;
            snprintf(dg->message, sizeof dg->message,
                     "unterminated block or string (builtin linter; emeraldc not found)");
            dg->from_entry = true;
            res->diags = dg;
            res->ndiags = 1;
            break;
        }
    }
}

/* run emeraldc --check --json over the snapshot prefix; fills res. */
static void run_prefix_check(const CheckReq *req, CheckResult *res) {
    res->status = SESS_ERROR;   /* until a definitive outcome is known */
    res->first_failing = req->nstmts;

    FILE *f = fopen(req->tmppath, "wb");
    if (!f) {
        snprintf(res->last_error, sizeof res->last_error,
                 "cannot write check file '%s': %s", req->tmppath, strerror(errno));
        return;
    }
    fputs(req->text, f);
    fclose(f);

    const char *argv[] = { req->compiler, "--check", "--json", req->tmppath, NULL };
    RunResult rr;
    double t0 = now_ms();
    run_cmd(argv, check_timeout_ms(), &rr);
    res->last_check_ms = now_ms() - t0;
    unlink(req->tmppath);

    if (rr.timed_out) {
        res->status = SESS_ERROR;
        snprintf(res->last_error, sizeof res->last_error,
                 "emeraldc timed out after %d ms", check_timeout_ms());
        return;
    }
    if (rr.exit_code == 127) {
        /* exec failed — compiler disappeared between resolve and run */
        res->fell_back = true;
        mock_check_snap(req->stmts, req->locus, res);
        return;
    }
    if (rr.exit_code < 0) {
        res->status = SESS_ERROR;
        snprintf(res->last_error, sizeof res->last_error,
                 "could not run '%s'", req->compiler);
        return;
    }

    Diag *diags = calloc((size_t)DIAG_MAX, sizeof(Diag));
    if (!diags) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    int nd = json_parse_diags(rr.out, diags, DIAG_MAX);

    char expect[600];
    snprintf(expect, sizeof expect, "%s", req->tmppath);
    for (int i = 0; i < nd; i++)
        diags[i].from_entry = (strcmp(diags[i].file, expect) == 0);

    if (rr.exit_code == 0 && nd == 0) {
        /* clean accept: no diagnostics reported */
        res->status = SESS_OK;
        res->first_failing = req->nstmts;
        free(diags);
        res->diags = NULL;
        res->ndiags = 0;
        return;
    }
    if (nd == 0) {
        /* the compiler failed and produced nothing we can map — surface an
         * error instead of silently accepting the prefix */
        res->status = SESS_ERROR;
        snprintf(res->last_error, sizeof res->last_error,
                 "emeraldc exited %d with no JSON diagnostics", rr.exit_code);
        free(diags);
        res->diags = NULL;
        res->ndiags = 0;
        return;
    }

    /* failed: find the first entry-level error */
    int first_line = 1 << 30;
    bool entry_err = false;
    for (int i = 0; i < nd; i++) {
        if (diags[i].from_entry && diags[i].line > 0 && diags[i].line < first_line) {
            first_line = diags[i].line;
            entry_err = true;
        }
    }
    res->diags = diags;
    res->ndiags = nd;
    res->status = SESS_FAILED;
    res->first_failing = entry_err ? stmt_for_line_snap(req->stmts, req->nstmts, first_line)
                                   : (req->nstmts > 0 ? req->nstmts : 0);
    if (!entry_err) mark_import_failure_snap(req->stmts, req->nstmts, res);
}

/* REPL: type-check `print(expr)` appended to the snapshot prefix. */
static void run_repl_check(const CheckReq *req, CheckResult *res) {
    FILE *f = fopen(req->tmppath, "wb");
    if (!f) {
        snprintf(res->repl_output, sizeof res->repl_output,
                 "cannot write check file: %s", strerror(errno));
        return;
    }
    fputs(req->text, f);
    fprintf(f, "print(%s)\n", req->repl_expr);
    fclose(f);

    const char *argv[] = { req->compiler, "--check", "--json", req->tmppath, NULL };
    RunResult rr;
    double t0 = now_ms();
    run_cmd(argv, check_timeout_ms(), &rr);
    double ms = now_ms() - t0;
    unlink(req->tmppath);

    if (rr.timed_out) {
        snprintf(res->repl_output, sizeof res->repl_output,
                 "emeraldc timed out after %d ms", check_timeout_ms());
        return;
    }
    if (rr.exit_code == 0) {
        snprintf(res->repl_output, sizeof res->repl_output,
                 "\xE2\x9C\x93 typechecks in the accepted scope   (%d ms)\n"
                 "(inferred type shown once emeraldc --env-at lands)",
                 (int)ms);
        return;
    }
    Diag diags[8];
    int nd = json_parse_diags(rr.out, diags, 8);
    if (nd == 0) {
        snprintf(res->repl_output, sizeof res->repl_output,
                 "check failed (exit %d) with no JSON diagnostics", rr.exit_code);
        return;
    }
    const Diag *g = &diags[nd - 1];
    size_t n = 0;
    n += (size_t)snprintf(res->repl_output + n, sizeof res->repl_output - n,
                          "\xE2\x9C\x97 line %d: %s\n", g->line, g->message);
    if (g->expected[0])
        n += (size_t)snprintf(res->repl_output + n, sizeof res->repl_output - n,
                              "  expected: %s\n", g->expected);
    if (g->actual[0])
        n += (size_t)snprintf(res->repl_output + n, sizeof res->repl_output - n,
                              "  actual:   %s\n", g->actual);
    n += (size_t)snprintf(res->repl_output + n, sizeof res->repl_output - n,
                          "  (%d ms)", (int)ms);
}

static void *check_worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_check_lock);
        while (!g_has_pending && !g_shutdown)
            pthread_cond_wait(&g_check_cond, &g_check_lock);
        if (g_shutdown) { pthread_mutex_unlock(&g_check_lock); break; }
        CheckReq req = g_pending;
        g_has_pending = false;
        g_busy = true;
        pthread_mutex_unlock(&g_check_lock);

        CheckResult res;
        memset(&res, 0, sizeof res);
        res.gen = req.gen;
        res.mode = req.mode;
        if (req.mode == CHECK_REPL) run_repl_check(&req, &res);
        else run_prefix_check(&req, &res);

        pthread_mutex_lock(&g_check_lock);
        if (res.mode == CHECK_REPL) {
            /* REPL results live in their own slot: a completed REPL check
             * must not evict a valid prefix result (or vice versa). */
            if (g_has_repl_result) free_res(&g_repl_result);
            g_repl_result = res;
            g_has_repl_result = true;
        } else {
            if (g_has_result) free_res(&g_result);   /* superseded */
            g_result = res;
            g_has_result = true;
        }
        g_busy = false;
        pthread_cond_broadcast(&g_check_cond);
        pthread_mutex_unlock(&g_check_lock);

        free_req(&req);
    }
    return NULL;
}

static void apply_result(Doc *d, CheckResult *res);   /* defined below */

/* snapshot the current prefix and hand it to the worker. */
static void submit_check(Doc *d, int mode, int end_line, const char *repl_expr) {
    CheckReq req;
    memset(&req, 0, sizeof req);
    req.mode = mode;
    req.gen = (mode == CHECK_REPL) ? sess_peek_gen(d) : sess_bump_gen(d);
    req.nstmts = d->nstmts;
    req.locus = d->sess.locus;
    snprintf(req.compiler, sizeof req.compiler, "%s", d->compiler);
    temp_path_for(d, req.tmppath, sizeof req.tmppath);

    size_t total = 1;
    for (int i = 0; i <= end_line && i < d->buf.count; i++)
        total += (size_t)d->buf.lens[i] + 1;
    req.text = malloc(total);
    if (!req.text) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    char *p = req.text;
    for (int i = 0; i <= end_line && i < d->buf.count; i++) {
        memcpy(p, d->buf.lines[i], (size_t)d->buf.lens[i]);
        p += d->buf.lens[i];
        *p++ = '\n';
    }
    *p = '\0';

    req.stmts = malloc(sizeof(Statement) * (size_t)d->nstmts);
    if (!req.stmts) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    memcpy(req.stmts, d->stmts, sizeof(Statement) * (size_t)d->nstmts);
    if (repl_expr)
        snprintf(req.repl_expr, sizeof req.repl_expr, "%s", repl_expr);

    pthread_mutex_lock(&g_check_lock);
    if (!g_worker_started && !g_shutdown) {
        if (pthread_create(&g_worker, NULL, check_worker_main, NULL) == 0)
            g_worker_started = true;
        else
            g_no_worker = true;
    }
    if (g_no_worker || g_shutdown) {
        /* no worker available (pthread_create failed, or shutting down):
         * run the check inline so the caller still gets a result. */
        pthread_mutex_unlock(&g_check_lock);
        CheckResult res;
        memset(&res, 0, sizeof res);
        res.gen = req.gen;
        res.mode = req.mode;
        if (req.mode == CHECK_REPL) run_repl_check(&req, &res);
        else run_prefix_check(&req, &res);
        if (res.gen == d->sess.gen) apply_result(d, &res);
        free_res(&res);
        free_req(&req);
        return;
    }
    if (g_has_pending) free_req(&g_pending);   /* supersede queued request */
    g_pending = req;
    g_has_pending = true;
    pthread_cond_signal(&g_check_cond);
    pthread_mutex_unlock(&g_check_lock);
}

/* apply a completed result to the Doc (takes ownership of diags). REPL
 * results only refresh repl_output; they never touch the session state the
 * prefix check owns. */
static void apply_result(Doc *d, CheckResult *res) {
    if (res->mode == CHECK_REPL) {
        if (res->repl_output[0])
            snprintf(d->repl_output, sizeof d->repl_output, "%s", res->repl_output);
        return;
    }
    free_diags(d);
    d->sess.status = res->status;
    d->sess.first_failing = res->first_failing;
    d->sess.last_check_ms = res->last_check_ms;
    if (res->last_error[0])
        snprintf(d->sess.last_error, sizeof d->sess.last_error, "%s", res->last_error);
    else
        d->sess.last_error[0] = '\0';
    d->sess.diags = res->diags;
    d->sess.ndiags = res->ndiags;
    res->diags = NULL;
    res->ndiags = 0;
    if (res->fell_back) d->using_builtin = true;
}

void sess_poll(Doc *d) {
    pthread_mutex_lock(&g_check_lock);
    if (g_has_result) {
        if (g_result.gen == d->sess.gen) {
            apply_result(d, &g_result);
            free_res(&g_result);
        } else {
            free_res(&g_result);   /* stale: a newer request superseded it */
        }
        g_has_result = false;
    }
    if (g_has_repl_result) {
        if (g_repl_result.gen == d->sess.gen) {
            apply_result(d, &g_repl_result);
            free_res(&g_repl_result);
        } else {
            free_res(&g_repl_result);
        }
        g_has_repl_result = false;
    }
    pthread_mutex_unlock(&g_check_lock);
}

void sess_wait(Doc *d) {
    pthread_mutex_lock(&g_check_lock);
    while ((g_has_pending || g_busy) && !g_shutdown)
        pthread_cond_wait(&g_check_cond, &g_check_lock);
    pthread_mutex_unlock(&g_check_lock);
    sess_poll(d);
}

void sess_checker_shutdown(void) {
    pthread_mutex_lock(&g_check_lock);
    g_shutdown = true;
    pthread_cond_broadcast(&g_check_cond);
    pthread_mutex_unlock(&g_check_lock);
    if (g_worker_started) {
        pthread_join(g_worker, NULL);
        g_worker_started = false;
    }
    pthread_mutex_lock(&g_check_lock);
    if (g_has_pending) free_req(&g_pending);
    if (g_has_result) free_res(&g_result);
    if (g_has_repl_result) free_res(&g_repl_result);
    g_has_pending = g_has_result = g_has_repl_result = false;
    pthread_mutex_unlock(&g_check_lock);
}

void sess_clear(Doc *d) {
    sess_bump_gen(d);   /* invalidate any in-flight check */
    d->sess.status = SESS_IDLE;
    d->sess.first_failing = d->nstmts;
    d->sess.last_error[0] = '\0';
    free_diags(d);
}

/* the built-in linter (Doc variant, synchronous — no subprocess). */
static void mock_check(Doc *d) {
    CheckResult res;
    memset(&res, 0, sizeof res);
    mock_check_snap(d->stmts, d->sess.locus, &res);
    apply_result(d, &res);
    free_res(&res);
}

void sess_check(Doc *d) {
    d->sess.last_error[0] = '\0';

    if (d->nstmts == 0) {
        sess_bump_gen(d);
        free_diags(d);
        d->sess.status = SESS_OK;
        d->sess.first_failing = 0;
        return;
    }
    if (d->sess.locus <= 0) {
        sess_bump_gen(d);
        free_diags(d);
        d->sess.status = SESS_IDLE;
        d->sess.first_failing = 0;
        return;
    }

    if (d->using_builtin || d->compiler[0] == '\0') {
        sess_bump_gen(d);   /* discard any in-flight async result */
        double t0 = now_ms();
        mock_check(d);
        d->sess.last_check_ms = now_ms() - t0;
        return;
    }

    /* hand the snapshot to the worker; the UI thread returns immediately */
    int end_line = d->stmts[d->sess.locus - 1].end_line;
    submit_check(d, CHECK_PREFIX, end_line, NULL);
}

void sess_advance(Doc *d) {
    if (d->nstmts == 0) return;
    if (d->sess.locus < d->nstmts) d->sess.locus++;
    sess_check(d);
}

void sess_retract(Doc *d) {
    if (d->sess.locus > 0) d->sess.locus--;
    sess_clear(d);
}

void sess_goto_cursor(Doc *d) {
    int s = sess_stmt_at_line(d, d->buf.cur_line);
    d->sess.locus = s + 1;
    sess_check(d);
}

void sess_check_all(Doc *d) {
    d->sess.locus = d->nstmts;
    sess_check(d);
}

/* SPEC.md §1d: an edit at line L retracts the locus to the last statement
 * that starts strictly before L, then recovers invisibly (cheap re-check).
 * The statement table is re-derived first — edits change statement
 * boundaries and open/close blocks, so a stale table would mis-shade and
 * mis-report (the builtin linter relies on the `unterminated` flags). */
void sess_on_edit(Doc *d, int edit_line) {
    sess_split_statements(d);
    int locus = 0;
    for (int i = 0; i < d->nstmts; i++) {
        if (d->stmts[i].start_line < edit_line) locus = i + 1;
        else break;
    }
    d->sess.locus = locus;
    sess_clear(d);
    if (locus > 0) sess_check(d);
}

/* ------------------------------------------------------------------ */
/* goal panel (source-derived) and obligation ledger                  */
/* ------------------------------------------------------------------ */

/* find `pat` in text, skipping strings and comments; NULL if absent */
static const char *find_masked(const char *text, const char *pat) {
    size_t plen = strlen(pat);
    char in_string = 0;
    const char *p = text;
    while (*p) {
        if (in_string) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == in_string) in_string = 0;
            p++;
            continue;
        }
        if (*p == '#') break;
        if (*p == '"' || *p == '\'') { in_string = *p; p++; continue; }
        if (strncmp(p, pat, plen) == 0) return p;
        p++;
    }
    return NULL;
}

/* scan every masked occurrence of `lead`; return position of `needle`
 * if it follows any of them (after whitespace). Returns NULL if absent. */
static const char *find_any_after(const char *text, const char *lead, const char *needle) {
    size_t lead_len = strlen(lead), needle_len = strlen(needle);
    char in_string = 0;
    const char *p = text;
    while (*p) {
        if (in_string) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == in_string) in_string = 0;
            p++;
            continue;
        }
        if (*p == '#') break;
        if (*p == '"' || *p == '\'') { in_string = *p; p++; continue; }
        if (strncmp(p, lead, lead_len) == 0) {
            const char *q = p + lead_len;
            while (*q == ' ' || *q == '\t') q++;
            if (strncmp(q, needle, needle_len) == 0 &&
                !(isalnum((unsigned char)q[needle_len]) || q[needle_len] == '_'))
                return q;
            p += lead_len;
            continue;
        }
        p++;
    }
    return NULL;
}

/* parse `def NAME(params) -> RET` from the statement's first line */
static void env_def_line(const char *line, char *out, size_t cap) {
    const char *p = find_masked(line, "def");
    if (!p) { snprintf(out, cap, "%s", line); return; }
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    const char *name = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (p == name) { snprintf(out, cap, "%s", line); return; }
    size_t n = (size_t)snprintf(out, cap, "%.*s", (int)(p - name), name);

    /* params: the text from '(' to its matching ')' (strings skipped) */
    const char *lp = p;
    while (*lp && *lp != '(' && *lp != '\n') lp++;
    if (*lp == '(') {
        int depth = 0;
        bool in_str = false;
        const char *q = lp;
        for (; *q && *q != '\n'; q++) {
            if (in_str) {
                if (*q == '\\' && q[1]) q++;
                else if (*q == '"' || *q == '\'') in_str = false;
            } else {
                if (*q == '"' || *q == '\'') in_str = true;
                else if (*q == '(') depth++;
                else if (*q == ')') {
                    depth--;
                    if (depth == 0) { q++; break; }
                }
            }
        }
        size_t plen = (size_t)(q - lp);
        if (n + plen < cap) {
            snprintf(out + n, cap - n, "%.*s", (int)plen, lp);
            n += plen;
        }
    }

    /* return type */
    const char *arrow = find_masked(p, "->");
    if (arrow) {
        arrow += 2;
        while (*arrow == ' ' || *arrow == '\t') arrow++;
        const char *end = arrow;
        while (*end && *end != '{' && *end != '\n' && *end != '#' &&
               !(*end == ' ' && (strncmp(end + 1, "pure", 4) == 0 ||
                                 strncmp(end + 1, "partial", 7) == 0)))
            end++;
        if (n + 4 < cap) { snprintf(out + n, cap - n, " -> "); n += 4; }
        if (n + (size_t)(end - arrow) < cap) {
            n += (size_t)snprintf(out + n, cap - n, "%.*s", (int)(end - arrow), arrow);
        }
    }
    if (n < cap) out[n] = '\0';
}

static void env_type_line(const char *line, char *out, size_t cap) {
    const char *p = find_masked(line, "type");
    if (!p) { snprintf(out, cap, "%s", line); return; }
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    const char *name = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    size_t n = (size_t)snprintf(out, cap, "type %.*s", (int)(p - name), name);
    const char *eq = find_masked(p, "=");
    if (eq) {
        eq++;
        while (*eq == ' ' || *eq == '\t') eq++;
        size_t rh = strlen(eq);
        if (rh > 56) rh = 56;
        if (n + 3 + rh < cap) {
            n += (size_t)snprintf(out + n, cap - n, " = %.*s", (int)rh, eq);
        }
    }
}

static void env_assign_line(const char *line, char *out, size_t cap) {
    /* `name [: TYPE] = value` */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    const char *name = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (p == name || *p == '\0') { snprintf(out, cap, "%s", line); return; }
    size_t n = (size_t)snprintf(out, cap, "%.*s", (int)(p - name), name);
    const char *colon = p;
    while (*colon == ' ' || *colon == '\t') colon++;
    if (*colon == ':') {
        colon++;
        while (*colon == ' ' || *colon == '\t') colon++;
        const char *end = colon;
        while (*end && *end != '=' && *end != '\n' && *end != '#') end++;
        if (n + 3 < cap) { snprintf(out + n, cap - n, " : "); n += 3; }
        if (n + (size_t)(end - colon) < cap) {
            n += (size_t)snprintf(out + n, cap - n, "%.*s", (int)(end - colon), colon);
        }
    }
    if (find_any_after(line, ":", "never")) {
        if (n + 2 < cap) { snprintf(out + n, cap - n, "  \xE2\x9C\x93"); n += 2; }
    }
    if (n < cap) out[n] = '\0';
}

/* Build the goal-panel text: residual goal first, then the in-scope env. */
void sess_derive(Doc *d) {
    char *buf = d->env_text;
    size_t cap = sizeof d->env_text;
    size_t n = 0;

    int accepted = (d->sess.status == SESS_FAILED) ? d->sess.first_failing
                                                   : d->sess.locus;

    /* ---- current goal ---- */
    n += (size_t)snprintf(buf + n, cap - n, "— current goal —\n");
    if (d->sess.status == SESS_FAILED) {
        const Diag *g = NULL;
        for (int i = 0; i < d->sess.ndiags; i++)
            if (d->sess.diags[i].from_entry) { g = &d->sess.diags[i]; break; }
        if (g) {
            n += (size_t)snprintf(buf + n, cap - n, "  %s\n", g->message);
            if (g->expected[0])
                n += (size_t)snprintf(buf + n, cap - n, "  expected: %s\n", g->expected);
            if (g->actual[0])
                n += (size_t)snprintf(buf + n, cap - n, "  actual:   %s\n", g->actual);
        } else {
            n += (size_t)snprintf(buf + n, cap - n, "  (failure in an imported module)\n");
        }
    } else if (d->sess.status == SESS_OK) {
        n += (size_t)snprintf(buf + n, cap - n, "  none — prefix accepted ✓\n");
    } else if (d->sess.status == SESS_ERROR) {
        n += (size_t)snprintf(buf + n, cap - n, "  %s\n", d->sess.last_error);
    } else {
        n += (size_t)snprintf(buf + n, cap - n, "  (idle — retracted)\n");
    }

    /* ---- in-scope environment ---- */
    n += (size_t)snprintf(buf + n, cap - n, "\n— in scope (statements 1–%d) —\n",
                          accepted > 0 ? accepted : 0);
    for (int i = 0; i < accepted && i < d->nstmts; i++) {
        char line[512];
        const char *first = d->buf.lines[d->stmts[i].start_line];
        switch (d->stmts[i].kind) {
        case STMT_DEF:
            env_def_line(first, line, sizeof line);
            n += (size_t)snprintf(buf + n, cap - n, "  %s\n", line);
            break;
        case STMT_TYPE:
            env_type_line(first, line, sizeof line);
            n += (size_t)snprintf(buf + n, cap - n, "  %s\n", line);
            break;
        case STMT_ASSIGN:
        case STMT_CONST:
            env_assign_line(first, line, sizeof line);
            n += (size_t)snprintf(buf + n, cap - n, "  %s\n", line);
            break;
        case STMT_IMPORT: {
            size_t ln = (size_t)d->buf.lens[d->stmts[i].start_line];
            if (ln > 70) ln = 70;
            n += (size_t)snprintf(buf + n, cap - n, "  %.*s\n", (int)ln, first);
            break;
        }
        default:
            break;
        }
        if (n > cap - 512) break;
    }
    if (n < cap) buf[n] = '\0';
    else buf[cap - 1] = '\0';
}

/* derive the obligation ledger from the accepted region */
void sess_ledger(Doc *d) {
    free_obligs(d);
    int cap = 0;
    for (int i = 0; i < d->nstmts; i++) {
        char *text = stmt_text(d, i);
        const char *site = NULL;
        char kind[24] = "", name[160] = "";

        site = find_any_after(text, "->", "never");
        if (site) {
            snprintf(kind, sizeof kind, "negation fn");
            const char *d_ = find_masked(text, "def");
            if (d_) {
                d_ += 3;
                while (*d_ == ' ' || *d_ == '\t') d_++;
                const char *nm = d_;
                while (isalnum((unsigned char)*d_) || *d_ == '_') d_++;
                snprintf(name, sizeof name, "%.*s", (int)(d_ - nm), nm);
            }
        } else {
            /* never binding: the identifier before the ':' that precedes never */
            site = find_any_after(text, ":", "never");
            if (site) {
                snprintf(kind, sizeof kind, "never binding");
                const char *colon = site - 1;
                while (colon > text && (*colon == ' ' || *colon == '\t')) colon--;
                const char *nm = colon;
                while (nm > text && (isalnum((unsigned char)nm[-1]) || nm[-1] == '_')) nm--;
                if (nm <= colon && colon > text &&
                    (isalnum((unsigned char)*nm) || *nm == '_'))
                    snprintf(name, sizeof name, "%.*s", (int)(colon - nm + 1), nm);
            }
        }

        if (kind[0] && site) {
            /* line of the site within the statement */
            int within = 0;
            for (const char *q = text; q < site; q++)
                if (*q == '\n') within++;
            if (d->nobligs == cap) {
                cap = cap ? cap * 2 : 8;
                d->obligs = realloc(d->obligs, sizeof(Obligation) * (size_t)cap);
                if (!d->obligs) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
            }
            Obligation *o = &d->obligs[d->nobligs++];
            memset(o, 0, sizeof *o);
            o->stmt = i;
            o->line = d->stmts[i].start_line + within + 1;
            snprintf(o->kind, sizeof o->kind, "%s", kind);
            snprintf(o->name, sizeof o->name, "%s", name[0] ? name : "(unnamed)");
            int accepted = (d->sess.status == SESS_FAILED) ? d->sess.first_failing
                                                           : d->sess.locus;
            if (i < accepted) snprintf(o->verdict, sizeof o->verdict, "proved");
            else if (i == d->sess.first_failing && d->sess.status == SESS_FAILED)
                snprintf(o->verdict, sizeof o->verdict, "out of reach");
            else snprintf(o->verdict, sizeof o->verdict, "unchecked");
        }
        free(text);
    }
}

/* ------------------------------------------------------------------ */
/* document lifecycle and REPL                                        */
/* ------------------------------------------------------------------ */

void doc_init(Doc *d, const char *path) {
    memset(d, 0, sizeof *d);
    buf_init(&d->buf);
    if (path && path[0]) {
        snprintf(d->file_path, sizeof d->file_path, "%s", path);
        snprintf(d->base_name, sizeof d->base_name, "%s", path_base(path));
    } else {
        snprintf(d->base_name, sizeof d->base_name, "untitled.rald");
    }
    doc_resolve_compiler(d);
    sess_split_statements(d);
    sess_clear(d);
}

void doc_new(Doc *d) {
    d->file_path[0] = '\0';
    snprintf(d->base_name, sizeof d->base_name, "untitled.rald");
    buf_set_text(&d->buf, "");
    sess_split_statements(d);
    sess_clear(d);
}

void doc_free(Doc *d) {
    buf_free(&d->buf);
    free_stmts(d);
    free_diags(d);
    free_obligs(d);
}

bool doc_open(Doc *d, const char *path, char *err, size_t errcap) {
    if (!buf_load_file(&d->buf, path, err, errcap)) return false;
    snprintf(d->file_path, sizeof d->file_path, "%s", path);
    snprintf(d->base_name, sizeof d->base_name, "%s", path_base(path));
    d->buf.dirty = false;
    sess_split_statements(d);
    sess_clear(d);
    d->sess.locus = d->nstmts;
    sess_check(d);   /* "check all" on open */
    return true;
}

bool doc_save(Doc *d, const char *path, char *err, size_t errcap) {
    if (!buf_save_file(&d->buf, path, err, errcap)) return false;
    snprintf(d->file_path, sizeof d->file_path, "%s", path);
    snprintf(d->base_name, sizeof d->base_name, "%s", path_base(path));
    d->buf.dirty = false;
    return true;
}

void sess_run_repl(Doc *d) {
    d->repl_output[0] = '\0';
    const char *expr = d->repl_input;
    while (*expr == ' ' || *expr == '\t') expr++;
    if (!*expr) {
        snprintf(d->repl_output, sizeof d->repl_output,
                 "type an expression to evaluate it in the accepted prefix");
        return;
    }
    if (d->sess.status != SESS_OK && d->sess.status != SESS_IDLE) {
        snprintf(d->repl_output, sizeof d->repl_output,
                 "prefix not accepted (%s) — retract until it is",
                 d->sess.status == SESS_FAILED ? "FAILED" : "ERROR");
        return;
    }
    if (d->using_builtin || d->compiler[0] == '\0') {
        snprintf(d->repl_output, sizeof d->repl_output,
                 "cannot type-check: no emeraldc found (builtin linter active)");
        return;
    }
    if (d->nstmts == 0) {
        snprintf(d->repl_output, sizeof d->repl_output,
                 "nothing accepted yet — advance the prefix first");
        return;
    }

    int accepted = (d->sess.status == SESS_FAILED) ? d->sess.first_failing
                                                   : d->sess.locus;
    int end_line = d->stmts[accepted > 0 ? accepted - 1 : 0].end_line;
    snprintf(d->repl_output, sizeof d->repl_output, "checking...");
    submit_check(d, CHECK_REPL, end_line, expr);
}
