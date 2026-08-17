/* headless/main.c — scripted session driver (SPEC.md §4, §14).
 *
 * Reads a session script, one command per line, and prints the session
 * state after each step so the interaction model can be verified without
 * a window — the seed of the spec's golden-test suite.
 *
 * Commands:
 *   open <path>          open a file (check-all on open)
 *   checkall | advance | retract | goto
 *   cursor <line> <col>  move the cursor (1-based line/col)
 *   edit <line> <col> <text>   insert text at 1-based line:col
 *   repl <expr>          evaluate an expression in the accepted scope
 *   # ...                comment
 */
#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* make REPL output deterministic: collapse "(123 ms)" to "(N ms)" */
static void sanitize_repl(char *s) {
    for (char *p = s; (p = strchr(p, '(')) != NULL; p++) {
        char *q = p + 1;
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) q++;
            if (strncmp(q, " ms)", 4) == 0) {
                p[1] = 'N';
                memmove(p + 2, q, strlen(q) + 1);
            }
        }
    }
}

static void print_state(const Doc *d) {
    printf("state locus=%d status=%d first_failing=%d nstmts=%d ndiags=%d\n",
           d->sess.locus, d->sess.status, d->sess.first_failing,
           d->nstmts, d->sess.ndiags);
    for (int i = 0; i < d->sess.ndiags; i++) {
        const Diag *g = &d->sess.diags[i];
        printf("  diag line=%d col=%d code=%s msg=%s\n",
               g->line, g->col, g->code, g->message);
    }
    printf("  obligations=%d\n", d->nobligs);
    for (int i = 0; i < d->nobligs; i++)
        printf("    oblig stmt=%d line=%d kind=%s name=%s verdict=%s\n",
               d->obligs[i].stmt, d->obligs[i].line,
               d->obligs[i].kind, d->obligs[i].name, d->obligs[i].verdict);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: headless <script>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }

    Doc d;
    doc_init(&d, "");

    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        char err[256];
        if (strncmp(line, "open ", 5) == 0) {
            if (!doc_open(&d, line + 5, err, sizeof err))
                printf("error %s\n", err);
        } else if (strcmp(line, "checkall") == 0) {
            sess_check_all(&d);
        } else if (strcmp(line, "advance") == 0) {
            sess_advance(&d);
        } else if (strcmp(line, "retract") == 0) {
            sess_retract(&d);
        } else if (strcmp(line, "goto") == 0) {
            sess_goto_cursor(&d);
        } else if (strncmp(line, "cursor ", 7) == 0) {
            int l = 1, c = 1;
            if (sscanf(line + 7, "%d %d", &l, &c) == 2) {
                d.buf.cur_line = l - 1;
                d.buf.cur_col = c - 1;
                buf_ensure_cursor(&d.buf);
            }
        } else if (strncmp(line, "edit ", 5) == 0) {
            int l = 1, c = 1;
            const char *text = strchr(line + 5, ' ');
            if (text && sscanf(line + 5, "%d %d", &l, &c) == 2) {
                text++;
                while (*text == ' ') text++;
                buf_insert_text(&d.buf, l - 1, c - 1, text, strlen(text));
                sess_on_edit(&d, l - 1);
            }
        } else if (strncmp(line, "repl ", 5) == 0) {
            snprintf(d.repl_input, sizeof d.repl_input, "%s", line + 5);
            sess_run_repl(&d);
            char out[4096];
            snprintf(out, sizeof out, "%s", d.repl_output);
            sanitize_repl(out);
            printf("  repl: %s\n", out);
            continue;
        } else {
            printf("unknown command: %s\n", line);
            continue;
        }
        sess_derive(&d);
        sess_ledger(&d);
        print_state(&d);
    }

    doc_free(&d);
    fclose(f);
    return 0;
}
