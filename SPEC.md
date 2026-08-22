# Emerald IDE — spec

A desktop IDE for Emerald in the shape of CoqIDE / Proof General / DrRacket:
a source buffer with an *accepted prefix*, a goal panel, and an obligation
ledger, driven by `emeraldc` and (later) `emerald-lsp`.

Written before any code existed; §6 has since been resolved — the IDE is
implemented as a **pure Tauri app** (Rust backend, webview frontend), with no
native C toolkit anywhere in the tree. Read [`../emerald/README.md`](../emerald/README.md),
[`../emerald/docs/proofs.md`](../emerald/docs/proofs.md), and
[`../emerald-lsp/DESIGN.md`](../emerald-lsp/DESIGN.md) first — this document
assumes all three and does not repeat them.

The headline, mirroring the LSP notes: **most of the work is in the compiler,
not the IDE.** The editor shell is a thin webview frontend over a small Rust
core. The
thing that makes it an IDE *for a proof assistant* — prefix checking, a
readable type environment at a point, a machine-readable account of which
propositions hold — does not exist yet and all of it lives in
`../emerald/src/check.c`.

---

## 0. The honest framing

Emerald has no tactic language, no goal stack, no `Qed`, and no proof term
separate from the program. So "CoqIDE for Emerald" cannot mean *proof script
stepping*, and any spec that pretends otherwise is designing for a language
that does not exist.

What it can mean is what `docs/proofs.md` already claims: `emeraldc --check` is
a proof checker for a small logic, and the ray-tracer scorecard
(`examples/ray_tracer/typed/README.md`) is a real, hand-maintained list of
propositions with a *proved / partial / out-of-reach* verdict on each. The IDE's
job is to make that loop interactive and to keep that scorecard live instead of
in a README.

The dictionary:

| CoqIDE concept | Emerald equivalent that exists today |
|---|---|
| sentence | a top-level statement (`def`, `type`, `import`, top-level assignment) |
| accepted prefix / `Qed` | statements `1..k` of the linked program pass `check.c` |
| goal panel | the type environment at the locus, plus flow-narrowed types |
| the current goal | the residual type at a `never` binding that does not yet typecheck |
| tactic failure | a `Diag` with `expected` / `actual` (`../emerald/include/diag.h:36`) |
| `Admitted` | a proposition on the ledger marked out-of-reach, with the reason |
| `Print Assumptions` | the ledger's summary: what this module's guarantees actually rest on |

Two rows have no equivalent and should not be faked: there is no proof term to
inspect, and there is no separate proof obligation queue — an obligation is
discharged by the program typechecking, not by a separate script.

### What the IDE is *for*

Not "an editor for `.rald` files." VS Code plus `emerald-lsp` will be a better
editor. This exists for three things a general editor will never do:

1. **Prefix checking with a visible locus** — the ability to say "check up to
   here" and read the environment at that point, which is the only way to do
   type-driven development on a batch compiler.
2. **The obligation ledger** — a live, per-module scorecard of propositions and
   their status, which is the deliverable of the whole research programme
   (`../emerald/docs/research-directions.md`).
3. **A typechecked REPL** — an expression evaluated in the scope of the accepted
   prefix, with its inferred type shown. DrRacket's interaction window, except
   the interesting part is the type, not the value.

If a feature does not serve one of those three, it belongs in `emerald-lsp` and
the IDE should get it for free by being an LSP client.

---

## 1. The proof session model

### 1a. Sentence granularity

The unit of advance is a **top-level statement**. Not a line, not a `def` body,
not an expression. Rationale: it is the granularity at which the checker's
environment is meaningful (a partially-checked function body has no coherent
environment to show), it matches `Stmt` in `../emerald/include/ast.h`, and it
is trivially derivable from the AST once statements have end positions
(DESIGN.md §1b).

Consequence: advancing "into" a long function is not possible. That is
acceptable for now. If it ever matters, the escape hatch is statement-level
granularity *inside* a body, which needs nothing new from the checker beyond an
environment snapshot per statement — but do not build it speculatively.

### 1b. Session state

The entire session state is:

```
  locus        : int      /* index of the last accepted top-level statement */
  generation   : uint64   /* bumped on every buffer edit and every command */
  status       : IDLE | CHECKING | FAILED
  diags        : DiagList /* from the last completed check */
  env          : EnvDump  /* from the last completed --env-at, if requested */
```

That is the whole state machine. There is no incremental proof engine, no
undo stack of checker states, no document manager in the Coq STM sense.

### 1c. Re-check the prefix from scratch, every time

**This is the central design decision and it should be revisited only with
profiling data.** Every advance, retract, or goto-cursor re-runs
`emeraldc --check --upto <pos>` on the full buffer from statement 1.

Coq needs an incremental state machine because a single tactic can run for
seconds. Emerald does not: the compiler is ~7k lines of C11, the files are
small, and `--check` stops before codegen. A full re-check is expected to be
single-digit milliseconds for anything short of the ray tracer, which is well
inside a keystroke budget.

What this buys:

- no cache invalidation logic, which is where interactive provers rot
- no divergence between what the IDE thinks is checked and what the compiler
  would say — the two are the same computation
- the §1c leak problem in DESIGN.md never appears, for the same reason the
  subprocess transport dodges it: the process exits
- every session step is a shell command you can paste into a terminal

Measure before optimising. The first thing to try if it ever gets slow is
caching the *parse* of unmodified imported modules, not caching checker state.

### 1d. Retraction on edit — do not lock the buffer

CoqIDE makes the accepted region read-only. It is the single most disliked
thing about CoqIDE and there is no reason to copy it.

Instead: on any edit at line `L`, set `locus = ` the index of the last
top-level statement that *starts strictly before* `L`, and drop any diags at or
after that point. The user never gets blocked; the green region simply recedes
to where their edit invalidated it. Because re-checking is cheap (§1c), the
recovery is invisible.

Edge case worth specifying now: an edit inside a *string literal or comment*
still retracts. Do not try to be clever about which edits matter — a
lexer-level "did the token stream before position L change" check is the only
sound version of that optimisation and it is not worth it.

### 1e. Commands

| Command | Effect |
|---|---|
| advance | `locus += 1`, re-check to the new locus |
| retract | `locus -= 1`, re-check (or just truncate diags — no compiler call needed) |
| goto cursor | `locus = ` statement index containing the cursor, re-check |
| check all | `locus = ` last statement, re-check — the DrRacket "Run" button |
| interrupt | kill the in-flight subprocess, restore `status = IDLE`, keep the old locus |

Retract is the only command that can be answered without the compiler, and it
should be, so that holding the retract key is instant.

---

## 2. What the compiler must expose

Everything below is new work in `../emerald`, and it is the part that blocks the
IDE. It is *additive* to the `--lsp-*` modes proposed in DESIGN.md §3, and
should follow the same conventions: a driver flag, JSON on stdout, stderr for
genuine crashes, a golden test directory.

### 2a. `--check --upto LINE:COL`

Check only the statements whose start position precedes `LINE:COL`, then stop
and report. Semantics to nail down:

- The prefix is taken **after module linking**, over the entry module's own
  statements only — imported modules are always checked in full, because a
  partially-linked import graph is meaningless. So `--upto` truncates the entry
  module's statement list, not the linked program's.
- A truncated prefix that references a name defined later is an ordinary
  unresolved-name error. That is correct and useful: it is exactly the
  information "you cannot advance past here yet."
- Exit code follows the existing `--check` convention. The IDE reads the JSON,
  not the exit code, but keep them consistent.

This one flag is what makes the whole interaction model possible. Everything
else in this section is refinement.

### 2b. `--env-at LINE:COL --json`

Dump the checker's environment at a position. This is the goal panel. Shape:

```json
{
  "file": "/abs/path/foo.rald",
  "line": 42, "col": 5,
  "in_function": { "name": "area", "signature": "(Shape) -> int" },
  "bindings": [
    { "name": "s", "type": "Circle | Square", "narrowed": "Square",
      "def_line": 40, "def_col": 14 }
  ],
  "narrowings": [
    { "expr": "s.kind == \"circle\"", "line": 41, "outcome": "false",
      "eliminated": "Circle" }
  ],
  "residual": "Square"
}
```

Three fields carry the weight:

- `narrowed` — the flow-narrowed type at this point, distinct from the declared
  type. This is the thing no other tool shows and the thing that makes
  exhaustiveness reasoning legible.
- `narrowings[]` — *why* it narrowed: which comparison, on which line, with
  which outcome, eliminating which member. This is the closest Emerald has to a
  proof term and it is the most valuable single output in this spec.
- `residual` — for a position at an `impossible: never = s` binding, the type
  that remains. Empty means the case analysis is exhaustive and the proposition
  is proved. Non-empty *is the unsolved goal*, and should be rendered as one.

Implementation note: this needs the position index (DESIGN.md §4a) and the
expression→type side table (§4b), and additionally an environment snapshot,
which the checker currently discards as it walks. Recording narrowing *reasons*
is genuinely new work in `check.c` and is not free — it is the one item in this
spec that is not a straightforward extension of something already there.

### 2c. `--obligations --json`

Enumerate the propositions in a module and their status. This depends on a
source-level convention for declaring a proposition, which does not exist yet —
see §8 and the open questions in §15. The compiler side, once the convention
exists, is a walk over the AST plus the checker's verdict per site.

### 2d. Everything else comes from DESIGN.md

The IDE needs `--lsp-index` (§3), end positions (§1b), parser error recovery
(§1a), and the loader overlay hook (§2a) exactly as specified there, for
exactly the same reasons. Do not duplicate that work; the IDE is a second
consumer of the same JSON.

One addition the IDE needs that a language server does not: **a statement
outline with byte ranges for the entry module**, so the IDE can map cursor
position → statement index without parsing Emerald itself. Fold it into
`--lsp-symbols` rather than adding a mode.

---

## 3. Process architecture

Three processes, two channels:

```
  emerald-ide (Tauri: webview frontend + Rust core)
      │
      ├─ LSP / JSON-RPC over stdio ──► emerald-lsp (Python) ──► emeraldc --lsp-*
      │     hover, completion, goto-def, references, semantic tokens
      │
      └─ direct subprocess ─────────► emeraldc --check --upto / --env-at / --obligations
            the proof session
```

### 3a. Why the proof channel bypasses the LSP

None of §2 is LSP-shaped. There is no standard `proof/advance`, and tunnelling
it through custom LSP requests means: inventing a protocol extension, teaching
the Python layer to proxy commands it has no opinion about, and adding a hop
that can only add latency and failure modes. The IDE owns both ends; talk
directly.

The practical payoff is bootstrapping order: **the proof session works before
`emerald-lsp` exists.** The IDE is useful the day `--check --upto` lands, and
gains hover and goto-definition later, as pure additions.

### 3b. Overlay discipline

Every compiler invocation from the IDE must pass unsaved buffers as an overlay
map, keyed on canonical paths, per DESIGN.md §2a. The IDE has *more* dirty
buffers than a language server typically does (it is the thing the user is
typing in), so this is not optional and not a later refinement. The single most
confusing possible bug in this program is analysis that reflects the disk while
the screen shows something else.

Each open file is its own entry point, same policy as DESIGN.md §2b. Do not
guess a workspace `main`.

### 3c. Subprocess, not a library

Start with `std::process::Command` + pipes, one process per query, same
reasoning as DESIGN.md §3: leaks don't matter, crashes are contained, every
query is a pasteable command, everything is golden-testable.

If profiling ever demands it, the escape hatch is `libemerald.a` linked
directly into the Rust binary — the JSON contract is unchanged, so it swaps one
function. This is *more* tempting here than in the LSP and should still be
resisted until measured, because in-process linking re-imports every problem in
DESIGN.md §1c and §1d:
the arena allocator, `exit(1)` on OOM, and stderr as a reporting channel would
all become the IDE's problem on day one — plus an FFI layer to write and
maintain.

---

## 4. Code layout: keep the session core behind a command boundary

The toolkit decision is made (§6: Tauri), and the layout keeps it reversible
the same way the original `core/`/`ui/` split meant to: all session logic
lives where no DOM or webview API can reach it.

```
  src/            webview frontend (TypeScript + Vite)
    editor.ts       layered <textarea> code editor (gutter, regions, undo)
    session.ts      the §1b state machine (locus, generation, debounced checks)
    ipc.ts          typed wrappers around the Tauri commands
    panels.ts       goal / symbols / ledger / diagnostics / repl rail
    theme.ts        theme registry + picker

  src-tauri/      backend (Rust)
    src/lib.rs      commands: analyze, check, read/write file; compiler
                    discovery; subprocess runner with timeout
    src/emerald.rs  tree-sitter analysis of the buffer (statements,
                    symbols, tokens) for highlighting and the outline
    grammar/        vendored tree-sitter grammar for Emerald
    tauri.conf.json window + bundler config (.app/.dmg)

  index.html      webview shell: titlebar, editor layers, rail, statusbar
```

The rule: **`src/session.ts` never spawns a process and never touches the
DOM; `src-tauri/src/lib.rs` never imports webview state.** Everything crosses
one typed command boundary (`ipc.ts` ↔ `#[tauri::command]`), so the frontend
could be replaced wholesale without touching the proof channel, and the
backend's behaviour is exercisable without a window via `cargo test`.

Build tooling follows Node/Rust conventions rather than `../emerald`'s
Taskfile: npm + Vite for the frontend (`npm run dev`, `npm run build`),
cargo for the backend, and `npm run tauri dev` / `npm run tauri build` to run
and bundle the app.

---

## 5. Text buffer and document model

As built: a transparent `<textarea>` holds the text and owns input/selection/
IME; rendering layers underneath draw highlighting, prefix shading, and
diagnostic underlines. Consequences:

- **No gap buffer to write or maintain.** The browser's textarea is the
  buffer; `session.ts` treats it as a string plus a byte offset. At `.rald`
  sizes this is free performance nobody has to think about again.
- **Statement ranges come from tree-sitter** (`src-tauri/src/emerald.rs`), not
  a hand-rolled line index: the backend returns statement/symbol spans, the
  frontend maps caret ↔ statement through them.
- **UTF-16 ↔ byte conversion lives in exactly one place** (`editor.ts`'s
  caret mapping), because the DOM speaks UTF-16 code units while emeraldc
  speaks 1-based lines and byte columns. Three coordinate systems, one
  converter, tested with emoji and combining characters from the first commit.
- **Undo:** a flat list of inverse edits in `editor.ts` with coalescing by
  time and adjacency. Do not attempt a persistent/branching undo tree.
- **File watching:** the IDE must notice on-disk changes to files it has open
  and to imported modules it does not — FSEvents via a Rust crate, or a 1s
  `stat` poll over the loaded module set, which is honestly adequate and
  ~30 lines. Not implemented yet; start with the poll.

---

## 6. GUI toolkit

The original spec ranked GTK4 + GtkSourceView, AppKit + Scintilla, SDL3 +
CoreText, and a cimgui prototype, all C or Objective-C.

**Decision: none of the above — Tauri 2 (Rust core + system webview).** The
interaction model turned out to be layers of styled DOM over a textarea,
which a webview gives you for free: selection, IME, clipboard, scrolling,
accessibility, and undo are platform code you don't write. The Rust side
supplies tree-sitter for highlighting and statement ranges, plus the
subprocess runner for the proof channel. No C toolkit is compiled, vendored,
or maintained; the whole UI is portable across desktop platforms that Tauri
supports.

---

## 7. Screen layout

```
 ┌───────────────────────────────┬──────────────────────────────┐
 │  source buffer                │  goal / environment          │
 │                               │                              │
 │  ▓▓ accepted prefix (green)   │  in area(s: Shape) -> int    │
 │  ░░ in flight (amber)         │  s : Circle | Square         │
 │  ▁▁ failing sentence (red)    │      narrowed to Square      │
 │                               │      because line 41 was     │
 │  ► locus marker in the gutter │      false, eliminating      │
 │  ● obligation marks in gutter │      Circle                  │
 │                               │  residual: (none) ✓ proved   │
 ├───────────────────────────────┼──────────────────────────────┤
 │  diagnostics │ ledger │ repl  │                              │
 └───────────────────────────────┴──────────────────────────────┘
```

Notes:

- The accepted-prefix shading must be **calm**. CoqIDE's saturated green is
  exhausting over an hour. A 3–4% tint of the foreground colour is enough.
- The locus is a gutter marker, not a caret. It must be distinguishable from
  the text cursor at a glance.
- The goal panel renders `narrowings[]` as prose, as sketched above. That
  rendering is the product; the JSON is just transport.
- Panels are tabs, not simultaneous. Three information-dense panes plus a buffer
  is more than fits on a laptop screen.

---

## 8. The obligation ledger

The distinguishing feature, and the one with the most unresolved design.

The model comes from `examples/ray_tracer/typed/README.md`: a list of
propositions, each with a verdict of **proved**, **partial**, or **out of
reach**, and for the latter two, the reason and the missing language feature.
That table is currently maintained by hand. The IDE should maintain it.

### 8a. What a proposition is, at the source level

This is unresolved and depends on where the language goes. Three candidates:

1. **A comment pragma** — `#[prop] unit vectors stay unit-length`. Zero language
   change, works today, and the compiler can attach it to the following
   statement. But comments are discarded by the parser entirely (DESIGN.md §6
   item 11 notes the same problem for formatting), so this needs comment
   retention first.
2. **A naming convention** — every `impossible: never = x` binding is a
   proposition, named by the enclosing function. Free, but expresses only one
   proof form out of the four in `docs/proofs.md`.
3. **A real declaration form** — a `prop` keyword. Correct, most work, and
   premature until the type system knows what it wants to say.

**Recommendation: (2) now, (1) when comments are retained, (3) only if the
language grows a proof fragment that needs it.** Start by having the ledger
detect the forms `docs/proofs.md` already describes — `never` bindings, negation
functions returning `never`, generic signatures used parametrically, literal
unions used for enumeration — and label them structurally. A ledger that is
derived rather than declared cannot drift out of date, which is most of its
value.

### 8b. Out-of-reach propositions

The seven unprovable ones in the scorecard are the point of the research
programme and cannot be derived from the source, because nothing in the source
states them. They need a declaration form, so they need (1) or (3) above. Until
then, the ledger can read them from a checked-in sidecar file
(`obligations.toml` or similar) and show them alongside the derived ones,
flagged as unverified assertions. That is honest and it is useful immediately.

### 8c. Ledger UI

Per-module list, grouped by verdict, each row linking to its site. A summary
line — "6 proved, 1 partial, 7 asserted" — that changes as you edit is the
single most motivating thing this IDE can display, and it is the closest analog
to `Print Assumptions` that Emerald can support.

---

## 9. The interaction pane (REPL)

As built: take the accepted prefix, append the typed expression, and re-run
`emeraldc --check` over the extension (`check`'s `suffix` parameter). Errors
come back as ordinary structured diagnostics; a clean check means the
expression typechecks in scope.

- **Show the inferred type, always.** In a language whose whole claim is about
  type systems, the type is the interesting output. Until `--env-at` exists
  (§2b) the pane reports accept/fail only; the moment the compiler can dump
  the expression's type, this becomes the one respect in which this REPL beats
  DrRacket's.
- Emerald compiles to C and links a native binary; there is no interpreter, so
  *evaluating* an entry means `emeraldc --emit-c` plus `cc`, on the order of
  100ms dominated by `cc`. Fine for a scratch pane, unacceptable on the typing
  path — so the REPL is explicitly *not* live evaluation, and execution is a
  later addition behind the same command boundary.
- Requires the accepted prefix to be a valid program; if `status != IDLE` the
  pane is disabled rather than silently using stale scope.

---

## 10. Concurrency and cancellation

- **The UI thread never blocks on a subprocess.** The proof channel runs as
  async Tauri commands, so the webview keeps painting while `emeraldc` works.
- **Cancellation by generation counter.** Every request carries the generation
  it was issued under (`session.ts`); a reply whose generation is stale is
  discarded. ~50
  lines, and it is a complete cancellation story without any protocol support.
  The LSP's `$/cancelRequest` can be wired later for politeness; it changes
  nothing about correctness.
- **Coalesce.** Rapid advance keypresses collapse to one check at the final
  locus via the debounce in `session.ts`. Same for retract.
- **Kill in-flight work on timeout or interrupt** — kill the child and reap it
  (`run_with_timeout`). The
  compiler holds no state the IDE cares about, so there is nothing to clean up.
- **One in-flight proof query at a time.** Do not build a pipeline; there is
  nothing to overlap.

---

## 11. Diagnostics rendering

Consume `--json` from the first commit; never parse the human-rendered caret
output. `Diag` (`../emerald/include/diag.h:36`) already carries `kind`,
`severity`, `code`, `file`, `line`, `col`, `message`, `expected`, `actual`, and
structured `notes[]`.

Because `expected` and `actual` are separate fields rather than a formatted
string, the IDE can render a **structural type diff** — the two types side by
side, with the differing node highlighted — instead of asking the user to
eyeball two long structural types. For a language whose types are records with
many fields, this is a large and nearly free usability win, and it is the
clearest argument for the structured-diagnostics design paying off.

Two compiler-side gaps block good rendering, both already on the LSP list:
`end_line`/`end_col` for underlining (DESIGN.md §1b) and `related[]` with
locations for "defined here" jumps (§4d).

---

## 12. Keybindings

Follow Proof General's muscle memory, since that is the audience:

| Action | Binding |
|---|---|
| advance | `⌘↓` |
| retract | `⌘↑` |
| goto cursor | `⌘→` |
| check all | `⌘⏎` |
| interrupt | `⌘.` |
| panels: goal / symbols / ledger / diagnostics / repl | `⌘1–⌘5` |
| cycle panel | `⌘[` / `⌘]` |

Everything else is standard macOS text editing, which the webview textarea
provides for free.

---

## 13. Build and packaging

- npm + Vite build the frontend; cargo builds the backend;
  `npm run tauri dev` / `npm run tauri build` run and bundle the app.
  `tauri.conf.json` is the single bundler config (`.app` / `.dmg` targets).
- The session core is testable with **no window at all** (`cargo test` in
  `src-tauri`, plus frontend tests if added). This is the property that keeps
  CI simple.
- The `.app` bundle can carry its own `emeraldc` and the Emerald stdlib in
  `Contents/`; the resolver prefers it when present, then falls back to
  `$EMERALDC`, `$PATH`, or the sibling checkout. Tauri's bundler handles
  icons, signing, and the DMG.
- No installer, no auto-update beyond what the bundler emits until someone
  other than the author runs it.

---

## 14. Testing

Extend `../emerald`'s golden-test culture rather than inventing anything.

- **Session state machine tests.** The locus/generation/retraction logic in
  `session.ts` is pure given an injected fake `check` — table-driven tests
  over edit/advance/retract sequences are the replacement for the old C
  golden scripts.
- **Backend unit tests.** Statement splitting, symbol extraction, and diag
  JSON parsing live in `src-tauri/src/emerald.rs` and `lib.rs` behind pure
  functions; cover them with `cargo test`.
- **Position-encoding tests.** Emoji and combining characters, asserting byte ↔
  UTF-16 ↔ line/col round-trips through the editor's caret mapping. Same test
  as DESIGN.md §8; steal it.
- **Retraction tests.** An edit above the locus must retract to the right
  statement. Off-by-one here is the bug users will hit hourly.
- **Overlay tests.** On-disk text and buffer text disagree; assert the check
  reflects the buffer.
- **Subprocess failure tests.** `emeraldc` missing, `emeraldc` segfaulting,
  `emeraldc` emitting malformed JSON, `emeraldc` hanging. The IDE must survive
  all four with a visible error and a usable buffer — it is an editor first, and
  losing unsaved work because the compiler crashed is unforgivable.

---

## 15. Non-goals

Stated so they do not creep in:

- **Not a general-purpose editor.** No plugin system, no multi-language support,
  no terminal emulator, no git integration. If the user wants those they should
  use VS Code with `emerald-lsp`.
- **Not a debugger.** The GC and runtime are interesting
  (`../emerald/docs/gc.md`) and a heap visualiser is tempting; it is a separate
  project.
- **Not a formatter host.** Formatting needs a pretty-printer and comment
  retention (DESIGN.md §6 item 11) and belongs in the compiler, not here.
- **Not cross-platform, yet.** macOS is the only tested target today. Tauri
  keeps Windows and Linux open (the frontend is plain DOM and the backend is
  std-only Rust apart from tree-sitter), but do not carry the burden before
  there is a second user.
- **No proof script language.** If Emerald ever grows tactics, this spec needs a
  real revision, not an extension.

---

## 16. Open questions — revisit when the language evolves

These are the reasons the deeper features are parked, and each one is a
question about Emerald rather than about the IDE.

1. **Does a proof fragment arrive?** `docs/research-directions.md` lists "a
   proof fragment that actually means something" as a track. If it lands with a
   tactic-like surface, §1 and §8 are both wrong and should be redesigned around
   an actual goal state.
2. **What declares a proposition?** §8a. Blocks the ledger, which is the
   feature most worth building.
3. **Do effects and termination checking arrive?** Both change what the goal
   panel must show — an effect row and a termination verdict per function are
   new panel content, not new plumbing.
4. **Do shape/indexed types arrive?** Tensor shapes in the environment panel are
   the case where rendering types as plain strings via `type_write()`
   (`check.c:393`) stops being adequate and the IDE needs structured types over
   the wire.
5. **Does `emeraldc` become a library?** If the arena allocator (DESIGN.md §1c)
   gets built for the LSP anyway, in-process linking becomes much more
   attractive and §3c should be revisited.
6. **Is prefix checking actually what type-driven development wants here?** It
   is borrowed from a language with a linear proof script. Emerald programs are
   ordinary programs; it is possible the right interaction is "check everything,
   always" (DrRacket) plus a rich environment inspector, with no locus at all.
   Worth prototyping both before committing — the session state machine is
   small enough that a no-locus variant can live behind the same command
   boundary and be compared directly.

---

## Where things stand, and what is next

The IDE shell is built and shipped as a pure Tauri app: editor with locus
shading and tree-sitter highlighting, the proof session over
`emeraldc --check` (prefix truncation done client-side, materialised next to
the source so relative imports resolve), goal panel, obligation ledger,
REPL pane, compiler discovery, `.app` bundling.

```
   done: Tauri shell + session state machine + panels + ledger + repl
         + bundled emeraldc resolution

   ../emerald: --check --upto  (replaces client-side prefix truncation)
         →  statement outline in --lsp-symbols
         →  --env-at (narrowing reasons: the real work — unblocks the
            goal panel's narrowed types and the REPL's inferred types)

   emerald-ide: file watching (stat poll first)
         →  structural type diff in diagnostics rendering
         →  sidecar `obligations.toml` for out-of-reach propositions
         →  LSP client (hover, goto-def, completion) — last
```

The remaining compiler work is shared with `emerald-lsp` and should be done
once, for both. On the IDE side the interesting part was always the state
machine and the protocol; both now live behind the command boundary where they
are testable without a window (`cargo test`, plus table-driven tests against a
fake `check`).
