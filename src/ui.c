/* ui.c — the raygui front end.
 *
 * Immediate-mode layout, one frame per window refresh:
 *
 *   ┌ toolbar (file ops, session buttons) ─────────────────┬────────┐
 *   │ source buffer with gutter (locus, prefix shading)   │ goal   │
 *   │                                                     │ panel  │
 *   ├ diagnostics │ ledger │ repl (tabs) ─────────────────┼────────┤
 *   └ status bar ─────────────────────────────────────────┴────────┘
 *
 * The buffer is drawn by hand (per-glyph, monospace) so that the accepted
 * prefix can be shaded and the locus marked; everything chrome-ish uses
 * raygui controls.
 */
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#include "raylib.h"
#include "ide.h"

#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- palette ----------------------------------------------------- */

static const Color COL_BG       = { 0x20, 0x21, 0x26, 0xFF };
static const Color COL_GUTTER   = { 0x26, 0x27, 0x2C, 0xFF };
static const Color COL_LINE     = { 0x2A, 0x2B, 0x32, 0x55 };
static const Color COL_FG       = { 0xD8, 0xDA, 0xDE, 0xFF };
static const Color COL_DIM      = { 0x8A, 0x8D, 0x95, 0xFF };
static const Color COL_GREEN    = { 0x2E, 0x7D, 0x4F, 0x3C };
static const Color COL_AMBER    = { 0xC8, 0x8A, 0x2A, 0x3C };
static const Color COL_RED      = { 0xC4, 0x4A, 0x4A, 0x3C };
static const Color COL_SEL      = { 0x3E, 0x68, 0xD0, 0x90 };
static const Color COL_KEYWORD  = { 0x56, 0x9C, 0xD6, 0xFF };
static const Color COL_STRING   = { 0x98, 0xC3, 0x79, 0xFF };
static const Color COL_COMMENT  = { 0x7A, 0x88, 0x99, 0xFF };
static const Color COL_NUMBER   = { 0xD1, 0x9A, 0x66, 0xFF };
static const Color COL_ACCENT   = { 0x4F, 0xC3, 0xF7, 0xFF };
static const Color COL_OK       = { 0x4E, 0xC9, 0x7A, 0xFF };
static const Color COL_FAIL     = { 0xE0, 0x5B, 0x5B, 0xFF };
static const Color COL_WARN     = { 0xD9, 0xA0, 0x4F, 0xFF };

/* ---- layout ------------------------------------------------------ */

#define FONT_SIZE     16.0f
#define UI_FONT_SIZE  14.0f

static Font g_font;   /* monospace editor font (loaded in main) */
#define LINE_H        (FONT_SIZE + 7.0f)
#define GUTTER_W      64.0f
#define TOOLBAR_H     42
#define STATUS_H      26
#define BOTTOM_H      190
#define GOAL_W        380
#define UNDO_CAP      100

typedef struct {
    Doc    doc;
    int    active_tab;      /* 0 diagnostics, 1 ledger, 2 repl */
    bool   show_goal;
    float  scroll_px;
    bool   dragging;

    Buffer *undo_items;  int undo_count, undo_cap;
    Buffer *redo_items;  int redo_count, redo_cap;
    int    last_edit_frame;

    int    dialog;          /* 0 none, 1 open, 2 save-as */
    char   dialog_text[1024];
    int    dialog_btn;
    bool   secret;

    bool   repl_editing;
    Vector2 goal_scroll, bottom_scroll;

    int    frame;   /* per-frame counter (raylib 6.0 has no GetFrameCount) */
    bool   quit;
    char   shot_path[1024]; /* EMERALD_IDE_SHOT: screenshot after frame 30, then exit */
} App;

static Rectangle buf_area(const App *a) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    float gw = a->show_goal ? GOAL_W : 0.0f;
    return (Rectangle){ 0, TOOLBAR_H, w - gw - 8, h - TOOLBAR_H - BOTTOM_H - STATUS_H };
}

static Rectangle goal_area(const App *a) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    if (!a->show_goal) return (Rectangle){ 0, 0, 0, 0 };
    return (Rectangle){ w - GOAL_W, TOOLBAR_H, GOAL_W, h - TOOLBAR_H - BOTTOM_H - STATUS_H };
}

static Rectangle bottom_area(void) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    return (Rectangle){ 0, h - BOTTOM_H - STATUS_H, w, BOTTOM_H };
}

static Rectangle status_area(void) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    return (Rectangle){ 0, h - STATUS_H, w, STATUS_H };
}

/* ---- small helpers ------------------------------------------------ */

/* Load the vendored monospace font (JetBrains Mono, OFL). Falls back to
 * raylib's default font if the file is missing. */
static Font ide_font_load(void) {
    int cps[512];
    int n = 0;
    for (int c = 0x20; c <= 0x7E; c++) cps[n++] = c;          /* ASCII */
    for (int c = 0xA0; c <= 0xFF; c++) cps[n++] = c;          /* Latin-1 */
    const int extra[] = {
        0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
        0x2190, 0x2191, 0x2192, 0x2193, 0x25B6, 0x25C0, 0x25CF,
        0x2713, 0x2717, 0x00B7,
    };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0] && n < 512; i++)
        cps[n++] = extra[i];
    Font f = LoadFontEx("vendor/fonts/JetBrainsMono-Regular.ttf", 16, cps, n);
    if (f.glyphCount == 0) return GetFontDefault();
    return f;
}

static bool color_eq(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static float glyph_advance(Font f, int cp, float size) {
    int idx = GetGlyphIndex(f, cp);
    if (idx < 0 || idx >= f.glyphCount) return size;
    int adv = f.glyphs[idx].advanceX;
    if (adv <= 0) adv = f.baseSize;
    return size * (float)adv / (float)f.baseSize;
}

static float text_advance(const char *s, int byte_len, Font f, float size) {
    float w = 0;
    for (const char *p = s; p < s + byte_len && *p; ) {
        int len;
        int cp = utf8_codepoint(p, &len);
        if (cp == '\t') w += size * 4;
        else w += glyph_advance(f, cp, size);
        p += len;
    }
    return w;
}

static bool is_keyword(const char *s, int len) {
    static const char *const kw[] = {
        "def", "if", "elif", "else", "while", "for", "in", "return", "and",
        "or", "not", "True", "False", "None", "break", "continue", "pass",
        "type", "const", "match", "pure", "partial", "import", "from", "as",
    };
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++)
        if ((int)strlen(kw[i]) == len && memcmp(kw[i], s, (size_t)len) == 0)
            return true;
    return false;
}

static const char *status_str(const Doc *d) {
    switch (d->sess.status) {
    case SESS_OK: return "OK";
    case SESS_FAILED: return "FAILED";
    case SESS_ERROR: return "ERROR";
    default: return "IDLE";
    }
}

static Color status_color(const Doc *d) {
    switch (d->sess.status) {
    case SESS_OK: return COL_OK;
    case SESS_FAILED: return COL_FAIL;
    case SESS_ERROR: return COL_WARN;
    default: return COL_DIM;
    }
}

static int visible_first_line(const App *a, float lh) {
    int first = (int)(a->scroll_px / lh);
    if (first < 0) first = 0;
    if (first >= a->doc.buf.count) first = a->doc.buf.count - 1;
    if (first < 0) first = 0;
    return first;
}

/* ---- buffer rendering --------------------------------------------- */

/* per-line string state (strings may span lines) */
static void compute_string_states(const App *a, char *out) {
    char q = 0;
    for (int i = 0; i < a->doc.buf.count; i++) {
        const char *s = a->doc.buf.lines[i];
        for (const char *p = s; *p; p++) {
            if (q) {
                if (*p == '\\') { if (p[1]) p++; continue; }
                if (*p == q) q = 0;
            } else if (*p == '"' || *p == '\'') {
                q = *p;
            }
        }
        out[i] = q;
    }
}

static Color shade_for(const Doc *d, int line, int s) {
    const Session *ss = &d->sess;
    (void)line;
    switch (ss->status) {
    case SESS_OK:
    case SESS_IDLE:
    case SESS_ERROR:
        return s < ss->locus ? COL_GREEN : (Color){ 0, 0, 0, 0 };
    default: { /* FAILED */
        int ff = ss->first_failing;
        if (s < ff) return COL_GREEN;
        if (s == ff) return COL_RED;
        if (s < ss->locus) return COL_AMBER;
        return (Color){ 0, 0, 0, 0 };
    }
    }
}

static int locus_marker_line(const App *a) {
    const Doc *d = &a->doc;
    if (d->nstmts == 0) return -1;
    if (d->sess.locus < d->nstmts) return d->stmts[d->sess.locus].start_line;
    return d->stmts[d->nstmts - 1].end_line;
}

static bool line_has_oblig(const App *a, int line) {
    for (int i = 0; i < a->doc.nobligs; i++)
        if (a->doc.obligs[i].line - 1 == line) return true;
    return false;
}

static bool line_has_diag(const App *a, int line) {
    for (int i = 0; i < a->doc.sess.ndiags; i++)
        if (a->doc.sess.diags[i].from_entry && a->doc.sess.diags[i].line - 1 == line)
            return true;
    return false;
}

static void flush_run(char *run, int *n, Color col, Font f, float fs,
                      float *x, float y) {
    if (*n == 0) return;
    run[*n] = '\0';
    DrawTextEx(f, run, (Vector2){ *x, y }, fs, 0, col);
    *x += MeasureTextEx(f, run, fs, 0).x;
    *n = 0;
}

/* draw one line with minimal syntax coloring; advances *strstate */
static void draw_line_text(const char *line, float y, float x0, Font f, float fs,
                           char *strstate) {
    char run[512];
    int rn = 0;
    Color cur = COL_FG;
    float x = x0;
    bool in_comment = false;
    char q = *strstate;

    for (const char *p = line; *p; ) {
        int len;
        int cp = utf8_codepoint(p, &len);
        Color col = COL_FG;

        if (q) {
            col = COL_STRING;
            if (cp == '\\' && p[len]) {
                /* escaped char: consume it too, stay in the string */
                int len2;
                utf8_codepoint(p + len, &len2);
                if (!color_eq(col, cur)) { flush_run(run, &rn, cur, f, fs, &x, y); cur = col; }
                if (rn + len + len2 >= (int)sizeof run - 1)
                    flush_run(run, &rn, cur, f, fs, &x, y);
                memcpy(run + rn, p, (size_t)(len + len2));
                rn += len + len2;
                p += len + len2;
                continue;
            }
            if (*p == q) q = 0;
        } else if (in_comment) {
            col = COL_COMMENT;
        } else if (*p == '#') {
            in_comment = true;
            col = COL_COMMENT;
        } else if (*p == '"' || *p == '\'') {
            q = (char)*p;
            col = COL_STRING;
        } else if (cp == '\t') {
            flush_run(run, &rn, cur, f, fs, &x, y);
            x += fs * 4;
            p += len;
            continue;
        } else if (isalnum((unsigned char)*p) || *p == '_') {
            int j = 0;
            while (p[j] && (isalnum((unsigned char)p[j]) || p[j] == '_')) j++;
            if (isdigit((unsigned char)*p)) col = COL_NUMBER;
            else if (is_keyword(p, j)) col = COL_KEYWORD;
            if (!color_eq(col, cur)) { flush_run(run, &rn, cur, f, fs, &x, y); cur = col; }
            for (int k = 0; k < j; k++) {
                if (rn >= (int)sizeof run - 2) flush_run(run, &rn, cur, f, fs, &x, y);
                run[rn++] = p[k];
            }
            p += j;
            continue;
        }

        if (!color_eq(col, cur)) { flush_run(run, &rn, cur, f, fs, &x, y); cur = col; }
        if (rn >= (int)sizeof run - 2) flush_run(run, &rn, cur, f, fs, &x, y);
        memcpy(run + rn, p, (size_t)len);
        rn += len;
        p += len;
    }
    flush_run(run, &rn, cur, f, fs, &x, y);
    *strstate = q;
}

static void draw_selection_bg(App *a, int i, float y, float x0, Font f, float fs,
                              int l1, int c1, int l2, int c2) {
    const Buffer *b = &a->doc.buf;
    if (i < l1 || i > l2) return;
    int from = (i == l1) ? c1 : 0;
    int to = (i == l2) ? c2 : b->lens[i];
    if (to <= from) return;
    float xa = x0 + text_advance(b->lines[i], from, f, fs);
    float xb = x0 + text_advance(b->lines[i], to, f, fs);
    DrawRectangle(xa, y, xb - xa, LINE_H, COL_SEL);
}

static void draw_buffer(App *a) {
    Rectangle area = buf_area(a);
    Font f = g_font;
    float fs = FONT_SIZE;

    float content_h = (float)a->doc.buf.count * LINE_H;
    float max_scroll = content_h - area.height;
    if (max_scroll < 0) max_scroll = 0;
    if (a->scroll_px > max_scroll) a->scroll_px = max_scroll;
    if (a->scroll_px < 0) a->scroll_px = 0;

    int first = visible_first_line(a, LINE_H);
    float yoff = (float)first * LINE_H - a->scroll_px;
    int visible = (int)(area.height / LINE_H) + 2;

    DrawRectangleRec(area, COL_BG);

    char *strstate = calloc((size_t)a->doc.buf.count, 1);
    compute_string_states(a, strstate);

    int l1, c1, l2, c2;
    bool has_sel = buf_sel_range(&a->doc.buf, &l1, &c1, &l2, &c2);
    int marker = locus_marker_line(a);
    bool caret_on = ((int)(GetTime() * 2.0) % 2) == 0;

    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);

    for (int i = first; i < a->doc.buf.count && i < first + visible; i++) {
        float y = area.y + yoff + (float)(i - first) * LINE_H;
        int s = sess_stmt_at_line(&a->doc, i);

        /* background: cursor line first, then the prefix shade on top so
         * the green/amber/red region stays visible on the cursor line */
        if (i == a->doc.buf.cur_line)
            DrawRectangle(area.x, y, area.width, LINE_H + 1, COL_LINE);
        Color shade = shade_for(&a->doc, i, s);
        if (shade.a) DrawRectangle(area.x, y, area.width, LINE_H + 1, shade);

        /* selection */
        if (has_sel)
            draw_selection_bg(a, i, y, area.x + GUTTER_W, f, fs, l1, c1, l2, c2);

        /* gutter */
        DrawRectangle(area.x, y, GUTTER_W, LINE_H + 1, COL_GUTTER);
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        float nw = text_advance(num, (int)strlen(num), f, fs);
        DrawTextEx(f, num, (Vector2){ area.x + GUTTER_W - nw - 8, y }, fs, 0, COL_DIM);

        /* markers */
        if (line_has_diag(a, i))
            DrawCircle(area.x + 10, y + LINE_H / 2, 3.0f, COL_FAIL);
        if (line_has_oblig(a, i))
            DrawCircle(area.x + 24, y + LINE_H / 2, 3.0f, COL_ACCENT);
        if (i == marker) {
            DrawTriangle((Vector2){ area.x + GUTTER_W - 2, y + 3 },
                         (Vector2){ area.x + GUTTER_W - 12, y + LINE_H / 2 },
                         (Vector2){ area.x + GUTTER_W - 2, y + LINE_H - 3 }, COL_ACCENT);
        }
        DrawLine(area.x + GUTTER_W, y, area.x + GUTTER_W, y + LINE_H,
                 (Color){ 0x3A, 0x3B, 0x42, 0xFF });

        /* text */
        char st = strstate[i > 0 ? i - 1 : 0];
        draw_line_text(a->doc.buf.lines[i], y, area.x + GUTTER_W + 8, f, fs, &st);

        /* diag underline */
        for (int k = 0; k < a->doc.sess.ndiags; k++) {
            const Diag *dg = &a->doc.sess.diags[k];
            if (!dg->from_entry || dg->line - 1 != i) continue;
            int from = dg->col > 0 ? dg->col - 1 : 0;
            int span = (int)strlen(dg->source_line);
            float xa = area.x + GUTTER_W + 8 +
                       text_advance(a->doc.buf.lines[i], from, f, fs);
            float xb = xa + text_advance(dg->source_line, span, f, fs);
            if (xb - xa < fs) xb = xa + fs;
            DrawRectangle(xa, y + LINE_H - 3, xb - xa, 2, COL_FAIL);
        }

        /* caret */
        if (i == a->doc.buf.cur_line && caret_on && !a->repl_editing && !a->dialog) {
            float cx = area.x + GUTTER_W + 8 +
                       text_advance(a->doc.buf.lines[i], a->doc.buf.cur_col, f, fs);
            DrawRectangle(cx, y + 2, 2, LINE_H - 4, COL_FG);
        }
    }
    EndScissorMode();
    free(strstate);

    /* scrollbar */
    if (max_scroll > 0) {
        float bar_h = area.height * (area.height / content_h);
        float bar_y = area.y + (a->scroll_px / max_scroll) * (area.height - bar_h);
        DrawRectangle(area.x + area.width - 6, bar_y, 4, bar_h,
                      (Color){ 0x55, 0x56, 0x5E, 0xAA });
    }
}

/* ---- panels ------------------------------------------------------- */

static void draw_toolbar(App *a) {
    Rectangle area = { 0, 0, (float)GetScreenWidth(), TOOLBAR_H };
    GuiPanel(area, NULL);

    float x = 8;
    if (GuiButton((Rectangle){ x, 7, 52, 26 }, "New")) {
        doc_new(&a->doc);
        a->scroll_px = 0;
    }
    x += 58;
    if (GuiButton((Rectangle){ x, 7, 56, 26 }, "Open")) {
        a->dialog = 1;
        a->dialog_text[0] = '\0';
    }
    x += 62;
    if (GuiButton((Rectangle){ x, 7, 56, 26 }, "Save")) {
        if (a->doc.file_path[0]) {
            char err[256];
            if (!doc_save(&a->doc, a->doc.file_path, err, sizeof err))
                snprintf(a->doc.repl_output, sizeof a->doc.repl_output,
                         "save failed: %s", err);
        } else {
            a->dialog = 2;
            a->dialog_text[0] = '\0';
        }
    }
    x += 62;

    /* session buttons */
    GuiLabel((Rectangle){ x, 10, 60, 20 }, "prefix:");
    x += 58;
    if (GuiButton((Rectangle){ x, 7, 40, 26 }, "◀")) sess_retract(&a->doc);
    x += 44;
    if (GuiButton((Rectangle){ x, 7, 40, 26 }, "▶")) sess_advance(&a->doc);
    x += 44;
    if (GuiButton((Rectangle){ x, 7, 56, 26 }, "Goto")) sess_goto_cursor(&a->doc);
    x += 60;
    if (GuiButton((Rectangle){ x, 7, 66, 26 }, "Check all")) sess_check_all(&a->doc);
    x += 72;
    if (GuiButton((Rectangle){ x, 7, 60, 26 }, "Stop")) sess_clear(&a->doc);
    x += 66;

    /* file name */
    char title[512];
    snprintf(title, sizeof title, "%s%s", a->doc.base_name,
             a->doc.buf.dirty ? " *" : "");
    GuiLabel((Rectangle){ x, 10, 280, 20 }, title);

    /* session status chip on the right */
    const Doc *d = &a->doc;
    char chip[128];
    snprintf(chip, sizeof chip, "%s  %d/%d", status_str(d), d->sess.locus, d->nstmts);
    float cw = GuiGetTextWidth(chip);
    Color c = status_color(d);
    Rectangle chipr = { (float)GetScreenWidth() - cw - 40, 8, cw + 28, 26 };
    DrawRectangleRec(chipr, (Color){ (unsigned char)(c.r / 5), (unsigned char)(c.g / 5),
                                     (unsigned char)(c.b / 5), 120 });
    DrawRectangleLinesEx(chipr, 1, c);
    GuiLabel((Rectangle){ chipr.x + 8, 11, cw, 20 }, chip);
}

static void draw_status_bar(App *a) {
    const Doc *d = &a->doc;
    const Buffer *b = &d->buf;
    char text[600];
    const Diag *first = NULL;
    for (int i = 0; i < d->sess.ndiags; i++)
        if (d->sess.diags[i].from_entry) { first = &d->sess.diags[i]; break; }
    const char *comp = d->compiler[0] ? d->compiler : "builtin linter";
    snprintf(text, sizeof text,
             "line %d, col %d    locus %d/%d    %s    %d diag%s    %.0f ms    %s",
             b->cur_line + 1, b->cur_col + 1, d->sess.locus, d->nstmts,
             status_str(d),
             d->sess.ndiags, d->sess.ndiags == 1 ? "" : "s",
             d->sess.last_check_ms,
             d->using_builtin ? "builtin linter (no emeraldc)" : comp);
    if (first && d->sess.status == SESS_FAILED) {
        size_t n = strlen(text);
        snprintf(text + n, sizeof text - n, "    %s", first->message);
    } else if (d->sess.last_error[0]) {
        size_t n = strlen(text);
        snprintf(text + n, sizeof text - n, "    ! %s", d->sess.last_error);
    }
    GuiStatusBar(status_area(), text);
}

/* right-hand goal panel */
static void draw_goal(App *a) {
    if (!a->show_goal) return;
    Rectangle area = goal_area(a);

    char header[128];
    snprintf(header, sizeof header, "Goal — %s", status_str(&a->doc));
    GuiPanel(area, header);

    int lines = 1;
    for (const char *p = a->doc.env_text; *p; p++)
        if (*p == '\n') lines++;

    float row_h = UI_FONT_SIZE + 4;
    Rectangle content = { 0, 0, area.width - 20, (float)lines * row_h + 8 };
    Rectangle view;
    GuiScrollPanel((Rectangle){ area.x, area.y + 26, area.width, area.height - 30 },
                   NULL, content, &a->goal_scroll, &view);

    Font f = g_font;
    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    float y = view.y - a->goal_scroll.y + 6;
    const char *p = a->doc.env_text;
    char line[512];
    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof line - 1) line[n++] = *p++;
        line[n] = '\0';
        if (*p == '\n') p++;
        Color col = COL_FG;
        if (strncmp(line, "\xE2\x80\x94", 3) == 0) col = COL_ACCENT;      /* — */
        else if (strstr(line, "\xE2\x9C\x93") != NULL) col = COL_OK;      /* ✓ */
        DrawTextEx(f, line, (Vector2){ view.x - a->goal_scroll.x + 8, y },
                   UI_FONT_SIZE, 0, col);
        y += row_h;
    }
    EndScissorMode();
}

/* diagnostics tab */
static void draw_diag_list(App *a, Rectangle area) {
    const Doc *d = &a->doc;
    char summary[256];
    int errs = 0, warns = 0;
    for (int i = 0; i < d->sess.ndiags; i++) {
        if (strcmp(d->sess.diags[i].severity, "error") == 0) errs++;
        else warns++;
    }
    snprintf(summary, sizeof summary, "%d error%s, %d warning%s",
             errs, errs == 1 ? "" : "s", warns, warns == 1 ? "" : "s");
    GuiLabel((Rectangle){ area.x, area.y, 300, 18 }, summary);

    float row_h = UI_FONT_SIZE + 4;
    Rectangle content = { 0, 0, area.width - 20, (float)d->sess.ndiags * row_h + 4 };
    Rectangle view;
    GuiScrollPanel((Rectangle){ area.x, area.y + 22, area.width, area.height - 24 },
                   NULL, content, &a->bottom_scroll, &view);

    Font f = g_font;
    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    Vector2 mp = GetMousePosition();
    for (int i = 0; i < d->sess.ndiags; i++) {
        const Diag *dg = &d->sess.diags[i];
        float y = view.y - a->bottom_scroll.y + 2 + (float)i * row_h;
        Rectangle row = { view.x - a->bottom_scroll.x + 2, y, view.width - 6, row_h };
        if (CheckCollisionPointRec(mp, row)) {
            DrawRectangleRec(row, (Color){ 0x33, 0x36, 0x3E, 0xFF });
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && dg->from_entry) {
                a->doc.buf.cur_line = dg->line - 1;
                a->doc.buf.cur_col = dg->col > 0 ? dg->col - 1 : 0;
                buf_sel_clear(&a->doc.buf);
                sess_goto_cursor(&a->doc);
            }
        }
        char text[900];
        if (dg->from_entry)
            snprintf(text, sizeof text, "%d  [%s] %s", dg->line, dg->code, dg->message);
        else
            snprintf(text, sizeof text, "%s:%d  [%s] %s",
                     dg->file, dg->line, dg->code, dg->message);
        if (dg->expected[0]) {
            size_t n = strlen(text);
            snprintf(text + n, sizeof text - n, "   expected: %s", dg->expected);
        }
        if (dg->actual[0]) {
            size_t n = strlen(text);
            snprintf(text + n, sizeof text - n, "   actual: %s", dg->actual);
        }
        DrawTextEx(f, text, (Vector2){ row.x + 4, row.y }, UI_FONT_SIZE, 0,
                   dg->from_entry && dg->severity[0] == 'e' ? COL_FAIL : COL_DIM);
    }
    EndScissorMode();
}

/* ledger tab */
static void draw_ledger(App *a, Rectangle area) {
    const Doc *d = &a->doc;
    int proved = 0, reach = 0, unchecked = 0;
    for (int i = 0; i < d->nobligs; i++) {
        if (strcmp(d->obligs[i].verdict, "proved") == 0) proved++;
        else if (strcmp(d->obligs[i].verdict, "out of reach") == 0) reach++;
        else unchecked++;
    }
    char summary[256];
    snprintf(summary, sizeof summary,
             "obligations: %d proved \xC2\xB7 %d out of reach \xC2\xB7 %d unchecked",
             proved, reach, unchecked);
    GuiLabel((Rectangle){ area.x, area.y, 560, 18 }, summary);

    float row_h = UI_FONT_SIZE + 4;
    Rectangle content = { 0, 0, area.width - 20, (float)d->nobligs * row_h + 4 };
    Rectangle view;
    GuiScrollPanel((Rectangle){ area.x, area.y + 22, area.width, area.height - 24 },
                   NULL, content, &a->bottom_scroll, &view);

    Font f = g_font;
    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    Vector2 mp = GetMousePosition();
    for (int i = 0; i < d->nobligs; i++) {
        const Obligation *o = &d->obligs[i];
        float y = view.y - a->bottom_scroll.y + 2 + (float)i * row_h;
        Rectangle row = { view.x - a->bottom_scroll.x + 2, y, view.width - 6, row_h };
        if (CheckCollisionPointRec(mp, row)) {
            DrawRectangleRec(row, (Color){ 0x33, 0x36, 0x3E, 0xFF });
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                a->doc.buf.cur_line = o->line - 1;
                a->doc.buf.cur_col = 0;
                buf_sel_clear(&a->doc.buf);
                sess_goto_cursor(&a->doc);
            }
        }
        char text[400];
        snprintf(text, sizeof text, "\xE2\x97\x8F  %d  %s — %s", o->line, o->name, o->kind);
        DrawTextEx(f, text, (Vector2){ row.x + 4, row.y }, UI_FONT_SIZE, 0, COL_FG);
        float tw = text_advance(text, (int)strlen(text), f, UI_FONT_SIZE);
        Color vc = strcmp(o->verdict, "proved") == 0 ? COL_OK
                  : strcmp(o->verdict, "out of reach") == 0 ? COL_FAIL : COL_DIM;
        DrawTextEx(f, o->verdict, (Vector2){ row.x + 4 + tw + 12, row.y },
                   UI_FONT_SIZE, 0, vc);
    }
    EndScissorMode();
}

/* repl tab */
static void draw_repl(App *a, Rectangle area) {
    Rectangle input = { area.x, area.y, area.width - 130, 26 };
    int r = GuiTextBox(input, a->doc.repl_input, sizeof a->doc.repl_input,
                       a->repl_editing);
    if (r == 1) {
        if (a->repl_editing && IsKeyPressed(KEY_ENTER)) sess_run_repl(&a->doc);
        a->repl_editing = !a->repl_editing;
    }
    if (GuiButton((Rectangle){ area.x + area.width - 122, area.y, 122, 26 },
                  "Evaluate")) {
        sess_run_repl(&a->doc);
    }

    int lines = 1;
    for (const char *p = a->doc.repl_output; *p; p++)
        if (*p == '\n') lines++;
    float row_h = UI_FONT_SIZE + 4;
    Rectangle content = { 0, 0, area.width - 20, (float)lines * row_h + 4 };
    Rectangle view;
    GuiScrollPanel((Rectangle){ area.x, area.y + 32, area.width, area.height - 36 },
                   NULL, content, &a->bottom_scroll, &view);
    Font f = g_font;
    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    float y = view.y - a->bottom_scroll.y + 2;
    const char *p = a->doc.repl_output;
    char line[512];
    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof line - 1) line[n++] = *p++;
        line[n] = '\0';
        if (*p == '\n') p++;
        Color col = COL_FG;
        if (strncmp(line, "\xE2\x9C\x93", 3) == 0) col = COL_OK;       /* ✓ */
        else if (strncmp(line, "\xE2\x9C\x97", 3) == 0) col = COL_FAIL; /* ✗ */
        DrawTextEx(f, line, (Vector2){ view.x - a->bottom_scroll.x + 4, y },
                   UI_FONT_SIZE, 0, col);
        y += row_h;
    }
    EndScissorMode();
}

static void draw_bottom(App *a) {
    Rectangle area = bottom_area();
    GuiPanel(area, NULL);
    GuiToggleGroup((Rectangle){ area.x + 8, area.y + 6, 320, 24 },
                   "Diagnostics;Ledger;REPL", &a->active_tab);
    Rectangle content = { area.x + 8, area.y + 36, area.width - 16, area.height - 42 };
    if (a->active_tab == 0) draw_diag_list(a, content);
    else if (a->active_tab == 1) draw_ledger(a, content);
    else draw_repl(a, content);
}

static void draw_dialog(App *a) {
    if (!a->dialog) return;
    int w = GetScreenWidth(), h = GetScreenHeight();
    Rectangle box = { (float)w / 2 - 320, (float)h / 2 - 90, 640, 180 };
    const char *title = a->dialog == 1 ? "Open file" : "Save as";
    int btn = 0;
    int res = GuiTextInputBox(box, title, "path:", a->dialog_text,
                              sizeof a->dialog_text, "OK;Cancel", &btn, NULL);
    if (res == 1) {
        if (btn == 1 && a->dialog_text[0]) {
            char err[256];
            if (a->dialog == 1) {
                if (!doc_open(&a->doc, a->dialog_text, err, sizeof err))
                    snprintf(a->doc.repl_output, sizeof a->doc.repl_output,
                             "open failed: %s", err);
                else a->scroll_px = 0;
            } else {
                if (!doc_save(&a->doc, a->dialog_text, err, sizeof err))
                    snprintf(a->doc.repl_output, sizeof a->doc.repl_output,
                             "save failed: %s", err);
            }
        }
        a->dialog = 0;
    }
}

/* ---- input -------------------------------------------------------- */

static void push_undo(App *a) {
    if (a->last_edit_frame == a->frame) return;
    a->last_edit_frame = a->frame;
    if (a->undo_count == a->undo_cap) {
        a->undo_cap = a->undo_cap ? a->undo_cap * 2 : 16;
        a->undo_items = realloc(a->undo_items, sizeof(Buffer) * (size_t)a->undo_cap);
        if (!a->undo_items) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    }
    Buffer snap;
    buf_copy(&a->doc.buf, &snap);
    a->undo_items[a->undo_count++] = snap;
    if (a->undo_count > UNDO_CAP) {
        buf_free(&a->undo_items[0]);
        memmove(a->undo_items, a->undo_items + 1,
                sizeof(Buffer) * (size_t)(a->undo_count - 1));
        a->undo_count--;
    }
    for (int i = 0; i < a->redo_count; i++) buf_free(&a->redo_items[i]);
    a->redo_count = 0;
}

static void restore_snapshot(App *a, Buffer *stack, int *count, int *cap,
                             Buffer **items) {
    if (*count == 0) return;
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *items = realloc(*items, sizeof(Buffer) * (size_t)*cap);
        if (!*items) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    }
    Buffer cur;
    buf_copy(&a->doc.buf, &cur);
    (*items)[(*count)++] = cur;

    Buffer *top = &stack[*count - 1];
    buf_free(&a->doc.buf);
    a->doc.buf = *top;
    (*count)--;
    a->doc.buf.dirty = true;
    sess_on_edit(&a->doc, a->doc.buf.cur_line);
}

static void undo(App *a) { restore_snapshot(a, a->undo_items, &a->undo_count,
                                            &a->undo_cap, &a->redo_items); }
static void redo(App *a) { restore_snapshot(a, a->redo_items, &a->redo_count,
                                            &a->redo_cap, &a->undo_items); }

static void end_edit(App *a, int line) {
    a->doc.buf.dirty = true;
    sess_on_edit(&a->doc, line);
}

/* insert s (may contain \n) at the cursor, replacing any selection */
static void type_text(App *a, const char *s, size_t n) {
    Buffer *b = &a->doc.buf;
    int l1, c1, l2, c2;
    int edit_line = b->cur_line;
    push_undo(a);
    if (buf_sel_range(b, &l1, &c1, &l2, &c2)) {
        buf_sel_delete(b);
        edit_line = l1;
    }
    buf_insert_text(b, b->cur_line, b->cur_col, s, n);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') { b->cur_line++; b->cur_col = 0; }
        else b->cur_col++;
    }
    end_edit(a, edit_line);
}

static void backspace(App *a) {
    Buffer *b = &a->doc.buf;
    int l1, c1, l2, c2;
    int edit_line = b->cur_line;
    push_undo(a);
    if (buf_sel_range(b, &l1, &c1, &l2, &c2)) {
        buf_sel_delete(b);
        end_edit(a, l1);
        return;
    }
    if (b->cur_col > 0) {
        const char *prev = utf8_prev(b->lines[b->cur_line],
                                     b->lines[b->cur_line] + b->cur_col);
        int len = b->cur_col - (int)(prev - b->lines[b->cur_line]);
        buf_delete_range(b, b->cur_line, (int)(prev - b->lines[b->cur_line]),
                         b->cur_line, b->cur_col);
        b->cur_col -= len;
    } else if (b->cur_line > 0) {
        int plen = b->lens[b->cur_line - 1];
        buf_delete_range(b, b->cur_line - 1, plen, b->cur_line, 0);
        b->cur_line--;
        b->cur_col = plen;
        edit_line = b->cur_line;
    }
    end_edit(a, edit_line);
}

static void delete_fwd(App *a) {
    Buffer *b = &a->doc.buf;
    int l1, c1, l2, c2;
    int edit_line = b->cur_line;
    push_undo(a);
    if (buf_sel_range(b, &l1, &c1, &l2, &c2)) {
        buf_sel_delete(b);
        end_edit(a, l1);
        return;
    }
    if (b->cur_col < b->lens[b->cur_line]) {
        const char *nx = utf8_next(b->lines[b->cur_line],
                                   b->lines[b->cur_line] + b->cur_col);
        int len = (int)(nx - (b->lines[b->cur_line] + b->cur_col));
        buf_delete_range(b, b->cur_line, b->cur_col, b->cur_line, b->cur_col + len);
    } else if (b->cur_line + 1 < b->count) {
        buf_delete_range(b, b->cur_line, b->cur_col, b->cur_line + 1, 0);
    }
    end_edit(a, edit_line);
}

static void insert_newline(App *a) {
    Buffer *b = &a->doc.buf;
    char indent[128];
    size_t ni = 0;
    const char *ls = b->lines[b->cur_line];
    while ((ls[ni] == ' ' || ls[ni] == '\t') && ni < sizeof indent - 1) {
        indent[ni] = ls[ni];
        ni++;
    }
    indent[ni] = '\0';
    size_t total = 1 + ni;
    char *s = malloc(total);
    if (!s) { fputs("emerald-ide: out of memory\n", stderr); exit(1); }
    s[0] = '\n';
    memcpy(s + 1, indent, ni);
    type_text(a, s, total);
    free(s);
}

static void keep_cursor_visible(App *a) {
    float lh = LINE_H;
    int first = visible_first_line(a, lh);
    Rectangle area = buf_area(a);
    int vis = (int)(area.height / lh);
    if (a->doc.buf.cur_line < first)
        a->scroll_px = (float)a->doc.buf.cur_line * lh;
    else if (a->doc.buf.cur_line >= first + vis)
        a->scroll_px = (float)(a->doc.buf.cur_line - vis + 1) * lh;
}

static void move_cursor(App *a, int dl, int dc, bool extend) {
    Buffer *b = &a->doc.buf;
    if (extend && b->sel_line < 0) {
        b->sel_line = b->cur_line;
        b->sel_col = b->cur_col;
    }
    if (!extend) buf_sel_clear(b);
    if (dl != 0) {
        int col_disp = utf8_byte_to_col(b->lines[b->cur_line], b->cur_col);
        b->cur_line += dl;
        if (b->cur_line < 0) b->cur_line = 0;
        if (b->cur_line >= b->count) b->cur_line = b->count - 1;
        b->cur_col = utf8_col_to_byte(b->lines[b->cur_line], col_disp);
    }
    if (dc > 0) {
        if (b->cur_col < b->lens[b->cur_line])
            b->cur_col = (int)(utf8_next(b->lines[b->cur_line],
                                         b->lines[b->cur_line] + b->cur_col) -
                               b->lines[b->cur_line]);
    } else if (dc < 0) {
        if (b->cur_col > 0)
            b->cur_col = (int)(utf8_prev(b->lines[b->cur_line],
                                         b->lines[b->cur_line] + b->cur_col) -
                               b->lines[b->cur_line]);
    }
    buf_ensure_cursor(b);
    keep_cursor_visible(a);
}

static void home(App *a, bool extend) {
    Buffer *b = &a->doc.buf;
    if (extend && b->sel_line < 0) { b->sel_line = b->cur_line; b->sel_col = b->cur_col; }
    if (!extend) buf_sel_clear(b);
    b->cur_col = 0;
    keep_cursor_visible(a);
}

static void end_line(App *a, bool extend) {
    Buffer *b = &a->doc.buf;
    if (extend && b->sel_line < 0) { b->sel_line = b->cur_line; b->sel_col = b->cur_col; }
    if (!extend) buf_sel_clear(b);
    b->cur_col = b->lens[b->cur_line];
    keep_cursor_visible(a);
}

static void copy_selection(App *a) {
    char *sel = buf_sel_text(&a->doc.buf);
    if (sel) {
        SetClipboardText(sel);
        free(sel);
    }
}

static void cut_selection(App *a) {
    copy_selection(a);
    Buffer *b = &a->doc.buf;
    int l1, c1, l2, c2;
    if (buf_sel_range(b, &l1, &c1, &l2, &c2)) {
        push_undo(a);
        buf_sel_delete(b);
        end_edit(a, l1);
    }
}

static void paste_clipboard(App *a) {
    const char *txt = GetClipboardText();
    if (txt && *txt) type_text(a, txt, strlen(txt));
}

static void select_all(App *a) {
    Buffer *b = &a->doc.buf;
    b->sel_line = 0;
    b->sel_col = 0;
    b->cur_line = b->count - 1;
    b->cur_col = b->lens[b->count - 1];
}

static void handle_mouse(App *a) {
    Rectangle area = buf_area(a);
    Vector2 mp = GetMousePosition();
    Font f = g_font;
    float fs = FONT_SIZE;
    float cw = glyph_advance(f, 'M', fs);

    if (CheckCollisionPointRec(mp, area)) {
        float y_in = mp.y - area.y + a->scroll_px;
        int line = (int)(y_in / LINE_H);
        if (line < 0) line = 0;
        if (line >= a->doc.buf.count) line = a->doc.buf.count - 1;
        float x_in = mp.x - (area.x + GUTTER_W + 8);
        int col = (int)(x_in / cw);
        if (col < 0) col = 0;
        col = utf8_col_to_byte(a->doc.buf.lines[line], col);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            a->repl_editing = false;
            buf_sel_clear(&a->doc.buf);
            a->doc.buf.cur_line = line;
            a->doc.buf.cur_col = col;
            a->dragging = true;
        } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && a->dragging) {
            if (a->doc.buf.sel_line < 0) {
                a->doc.buf.sel_line = a->doc.buf.cur_line;
                a->doc.buf.sel_col = a->doc.buf.cur_col;
            }
            a->doc.buf.cur_line = line;
            a->doc.buf.cur_col = col;
        }
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            a->scroll_px -= wheel * LINE_H * 2.5f;
            float content_h = (float)a->doc.buf.count * LINE_H;
            float max_scroll = content_h - area.height;
            if (max_scroll < 0) max_scroll = 0;
            if (a->scroll_px < 0) a->scroll_px = 0;
            if (a->scroll_px > max_scroll) a->scroll_px = max_scroll;
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) a->dragging = false;
}

static void handle_keys(App *a) {
    bool cmd = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (a->dialog) return;
    if (a->repl_editing) return; /* the text box owns the keys */

    if (cmd) {
        if (IsKeyPressed(KEY_Q)) { a->quit = true; return; }
        if (IsKeyPressed(KEY_S) && shift) {
            a->dialog = 2;
            a->dialog_text[0] = '\0';
            return;
        }
        if (IsKeyPressed(KEY_S)) {
            if (a->doc.file_path[0]) {
                char err[256];
                if (!doc_save(&a->doc, a->doc.file_path, err, sizeof err))
                    snprintf(a->doc.repl_output, sizeof a->doc.repl_output,
                             "save failed: %s", err);
            } else {
                a->dialog = 2;
                a->dialog_text[0] = '\0';
            }
            return;
        }
        if (IsKeyPressed(KEY_O)) { a->dialog = 1; a->dialog_text[0] = '\0'; return; }
        if (IsKeyPressed(KEY_N)) { doc_new(&a->doc); a->scroll_px = 0; return; }
        if (IsKeyPressed(KEY_Z) && shift) { redo(a); return; }
        if (IsKeyPressed(KEY_Z)) { undo(a); return; }
        if (IsKeyPressed(KEY_Y)) { redo(a); return; }
        if (IsKeyPressed(KEY_C)) { copy_selection(a); return; }
        if (IsKeyPressed(KEY_X)) { cut_selection(a); return; }
        if (IsKeyPressed(KEY_V)) { paste_clipboard(a); return; }
        if (IsKeyPressed(KEY_A)) { select_all(a); return; }
        if (IsKeyPressed(KEY_DOWN)) { sess_advance(&a->doc); return; }
        if (IsKeyPressed(KEY_UP)) { sess_retract(&a->doc); return; }
        if (IsKeyPressed(KEY_RIGHT)) { sess_goto_cursor(&a->doc); return; }
        if (IsKeyPressed(KEY_ENTER)) { sess_check_all(&a->doc); return; }
        if (IsKeyPressed(KEY_PERIOD)) { sess_clear(&a->doc); return; }
        if (IsKeyPressed(KEY_ONE)) { a->active_tab = 0; return; }
        if (IsKeyPressed(KEY_TWO)) { a->active_tab = 1; return; }
        if (IsKeyPressed(KEY_THREE)) { a->active_tab = 2; return; }
        if (IsKeyPressed(KEY_G)) { a->show_goal = !a->show_goal; return; }
        if (IsKeyPressed(KEY_HOME)) {
            Buffer *b = &a->doc.buf;
            b->cur_line = 0; b->cur_col = 0; buf_ensure_cursor(b); return;
        }
        if (IsKeyPressed(KEY_END)) {
            Buffer *b = &a->doc.buf;
            b->cur_line = b->count - 1;
            b->cur_col = b->lens[b->count - 1];
            return;
        }
        if (IsKeyPressed(KEY_LEFT)) { move_cursor(a, 0, -1, false); return; }
        return;
    }

    /* plain editing */
    Buffer *b = &a->doc.buf;
    if (alt && IsKeyPressed(KEY_LEFT)) { move_cursor(a, 0, -1, shift); return; }
    if (alt && IsKeyPressed(KEY_RIGHT)) { move_cursor(a, 0, 1, shift); return; }

    if (IsKeyPressed(KEY_LEFT)) { move_cursor(a, 0, -1, shift); return; }
    if (IsKeyPressed(KEY_RIGHT)) { move_cursor(a, 0, 1, shift); return; }
    if (IsKeyPressed(KEY_UP)) { move_cursor(a, -1, 0, shift); return; }
    if (IsKeyPressed(KEY_DOWN)) { move_cursor(a, 1, 0, shift); return; }
    if (IsKeyPressed(KEY_HOME)) { home(a, shift); return; }
    if (IsKeyPressed(KEY_END)) { end_line(a, shift); return; }
    if (IsKeyPressed(KEY_PAGE_UP)) { move_cursor(a, -12, 0, shift); return; }
    if (IsKeyPressed(KEY_PAGE_DOWN)) { move_cursor(a, 12, 0, shift); return; }

    if (IsKeyPressed(KEY_BACKSPACE)) { backspace(a); return; }
    if (IsKeyPressed(KEY_DELETE)) { delete_fwd(a); return; }
    if (IsKeyPressed(KEY_ENTER)) { insert_newline(a); return; }
    if (IsKeyPressed(KEY_TAB)) { type_text(a, "    ", 4); return; }
    if (IsKeyPressed(KEY_ESCAPE)) { buf_sel_clear(b); return; }

    /* printable characters */
    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (cp == '\t' || cp == '\n' || cp == '\r') continue;
        char utf[8];
        int n = 0;
        if (cp < 0x80) utf[n++] = (char)cp;
        else if (cp < 0x800) {
            utf[n++] = (char)(0xC0 | (cp >> 6));
            utf[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf[n++] = (char)(0xE0 | (cp >> 12));
            utf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            utf[n++] = (char)(0xF0 | (cp >> 18));
            utf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            utf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf[n++] = (char)(0x80 | (cp & 0x3F));
        }
        type_text(a, utf, (size_t)n);
    }
}

static void handle_dropped(App *a) {
    if (IsFileDropped()) {
        FilePathList files = LoadDroppedFiles();
        if (files.count > 0) {
            char err[256];
            if (!doc_open(&a->doc, files.paths[0], err, sizeof err))
                snprintf(a->doc.repl_output, sizeof a->doc.repl_output,
                         "open failed: %s", err);
            else a->scroll_px = 0;
        }
        UnloadDroppedFiles(files);
    }
}

/* ---- main --------------------------------------------------------- */

static void style_setup(void) {
    GuiSetStyle(DEFAULT, TEXT_SIZE, 14);
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt((Color){ 0x1A, 0x1B, 0x20, 0xFF }));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt((Color){ 0x24, 0x25, 0x2B, 0xFF }));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt((Color){ 0x2E, 0x30, 0x38, 0xFF }));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt((Color){ 0x35, 0x37, 0x40, 0xFF }));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt((Color){ 0xD0, 0xD2, 0xD8, 0xFF }));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt((Color){ 0xE6, 0xE8, 0xEE, 0xFF }));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt((Color){ 0xFF, 0xFF, 0xFF, 0xFF }));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt((Color){ 0x3C, 0x3E, 0x46, 0xFF }));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt((Color){ 0x4A, 0x4C, 0x56, 0xFF }));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt((Color){ 0x4F, 0xC3, 0xF7, 0xFF }));
}

static void draw(App *a) {
    BeginDrawing();
    ClearBackground(COL_BG);
    draw_toolbar(a);
    draw_buffer(a);
    draw_goal(a);
    draw_bottom(a);
    draw_status_bar(a);
    draw_dialog(a);
    EndDrawing();
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    App app;
    memset(&app, 0, sizeof app);
    app.show_goal = true;

    doc_init(&app.doc, "");
    char err[256];
    if (argc > 1) {
        if (!doc_open(&app.doc, argv[1], err, sizeof err))
            snprintf(app.doc.repl_output, sizeof app.doc.repl_output,
                     "open failed: %s", err);
    } else {
        /* a sensible demo file when run with no argument */
        const char *demo = "../emerald/examples/ray_tracer/typed/main.rald";
        FILE *f = fopen(demo, "r");
        if (f) {
            fclose(f);
            if (!doc_open(&app.doc, demo, err, sizeof err)) {
                snprintf(app.doc.repl_output, sizeof app.doc.repl_output,
                         "open failed: %s", err);
            }
        }
    }

    const char *shot = getenv("EMERALD_IDE_SHOT");
    if (shot && *shot) snprintf(app.shot_path, sizeof app.shot_path, "%s", shot);
    const char *tab = getenv("EMERALD_IDE_TAB");
    if (tab && *tab) app.active_tab = atoi(tab);

    InitWindow(1280, 800, "Emerald IDE");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    g_font = ide_font_load();
    GuiSetFont(g_font);
    style_setup();

    while (!WindowShouldClose() && !app.quit) {
        app.frame++;
        handle_dropped(&app);
        handle_mouse(&app);
        handle_keys(&app);
        sess_derive(&app.doc);
        sess_ledger(&app.doc);
        draw(&app);
        if (app.shot_path[0] && app.frame == 30) {
            TakeScreenshot(app.shot_path);
            app.quit = true;
        }
    }

    doc_free(&app.doc);
    for (int i = 0; i < app.undo_count; i++) buf_free(&app.undo_items[i]);
    for (int i = 0; i < app.redo_count; i++) buf_free(&app.redo_items[i]);
    free(app.undo_items);
    free(app.redo_items);
    UnloadFont(g_font);
    CloseWindow();
    return 0;
}
