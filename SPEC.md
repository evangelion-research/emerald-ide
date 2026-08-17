# Emerald IDE — spec

A native macOS IDE for Emerald in the shape of CoqIDE / Proof General / DrRacket:
a source buffer with an *accepted prefix*, a goal panel, and an obligation
ledger, driven by `emeraldc` and (later) `emerald-lsp`.

Written before any code exists, and deliberately parked until the core language
evolves. Read [`../emerald/README.md`](../emerald/README.md),
[`../emerald/docs/proofs.md`](../emerald/docs/proofs.md), and
[`../emerald-lsp/DESIGN.md`](../emerald-lsp/DESIGN.md) first — this document
assumes all three and does not repeat them.

The headline, mirroring the LSP notes: **most of the work is in the compiler,
not the IDE.** The editor shell is a few thousand lines of ordinary C. The
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
  emerald-ide (C)
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

Start with `posix_spawn` + pipes, one process per query, same reasoning as
DESIGN.md §3: leaks don't matter, crashes are contained, every query is a
pasteable command, everything is golden-testable.

If profiling ever demands it, the escape hatch is `libemerald.a` linked
directly into the IDE — the JSON contract is unchanged, so it swaps one
function. This is *more* tempting here than in the LSP (the IDE is already C,
so there is no FFI to write) and should still be resisted until measured,
because in-process linking re-imports every problem in DESIGN.md §1c and §1d:
the arena allocator, `exit(1)` on OOM, and stderr as a reporting channel would
all become the IDE's problem on day one.

---

## 4. Code layout: make the GUI toolkit a reversible decision

The choice of GUI toolkit is the decision most likely to sink this project, so
structure the code so that changing it costs a week rather than a rewrite.

```
  core/     pure C11, zero UI dependencies
    buffer.c      gap buffer + line index
    doc.c         document set, dirty tracking, canonical paths
    session.c     the §1b state machine
    proc.c        subprocess spawn, pipe pumping, cancellation
    json.c        vendored yyjson or cJSON
    proto.c       compiler JSON → structs; LSP JSON-RPC client
    ledger.c      obligation ledger model
    pos.c         byte ↔ UTF-8 ↔ UTF-16 column conversion (one place, always)

  ui/       one backend, implementing a small surface
    ui.h          ~15 functions the core calls
    ui_gtk.c      (or ui_appkit.m, or ui_sdl.c)

  headless/ a driver binary that runs a scripted session and prints results
  tests/    golden tests, directory-per-case, mirroring ../emerald/tests
```

The rule: **`core/` never includes a UI header, and `ui/` never spawns a
process.** If that holds, the headless driver can exercise every interesting
behaviour without a window, which is where the test coverage will live.

Match `../emerald`'s conventions: `Taskfile.yml`, `-std=c11 -Wall -Wextra -O2
-g`, a `bless` task, golden `.expected` files, and a `docs/` that starts at
`docs/README.md`.

---

## 5. Text buffer and document model

- **Gap buffer, not a rope.** These are `.rald` source files, not 100MB logs. A
  gap buffer is ~200 lines and will never hit its limits here. A rope is the
  correct answer to a problem this project does not have.
- **A separate line-start index**, rebuilt on edit. Every position query the
  compiler protocol makes needs it, and it is cheap at these sizes.
- **UTF-16 column math lives in exactly one function**, next to the line index,
  and is used at every LSP boundary and nowhere else. DESIGN.md §5 calls this
  *the trap*; it is the same trap here. The compiler speaks 1-based lines and
  byte columns; LSP speaks 0-based lines and UTF-16 code units; the buffer
  speaks byte offsets. Three coordinate systems, one converter, tested with
  emoji and combining characters from the first commit.
- **Undo:** a flat list of inverse edits with coalescing by time and adjacency.
  Do not attempt a persistent/branching undo tree.
- **File watching:** the IDE must notice on-disk changes to files it has open
  and to imported modules it does not. On macOS this is FSEvents (a C API), or
  a 1s `stat` poll over the loaded module set, which is honestly adequate and
  ~30 lines. Start with the poll.

---

## 6. GUI toolkit

Ranked, with costs stated plainly. All of these are viable; none is obviously
right.

### 1. GTK4 + GtkSourceView — *fastest path to daily use*

Pure C API. GtkSourceView supplies syntax highlighting from an XML language
spec, line numbers, undo, folding, search, and gutter marks. Critically,
`GtkTextTag` gives background-shaded regions directly, which is exactly the
accepted-prefix rendering, and `GtkSourceMark` gives the gutter icons.

- **Pro:** the widget layer is already an IDE; you write the parts that are
  about Emerald.
- **Con:** it does not look or feel native on macOS (menus, scrolling, IME,
  emoji picker), Homebrew GTK4 on macOS is periodically broken, and shipping a
  `.app` means bundling a large dependency tree.

### 2. AppKit + Scintilla — *best long-term feel on macOS*

`ScintillaView` embedded in a native window. Scintilla's API is C-callable;
indicators, markers, and styling were designed for precisely this use.

- **Pro:** native menus, native text input, native scrolling; a real macOS app.
  Scintilla is battle-tested in this exact role.
- **Con:** the shell must be Objective-C. Driving the ObjC runtime from `.c` via
  `objc_msgSend` is possible and gets miserable past ~200 lines — just write
  `ui_appkit.m` and keep `core/` in C. Scintilla is C++ internally, so the
  build grows a C++ toolchain.

### 3. SDL3 + CoreText + a hand-written editor widget — *total control*

CoreText is a C API and does real shaping; SDL3 supplies the window and Metal
surface.

- **Pro:** complete control over locus rendering, gutter, and the goal panel's
  typography. No toolkit fighting you.
- **Con:** you are now implementing selection, IME, clipboard, scrolling,
  accessibility, and undo yourself. Only justified if the editor itself is a
  research artifact rather than a tool.

### 4. cimgui (Dear ImGui) — *prototype only*

Excellent for the side panels — ledger, diagnostics, environment inspector are
all immediate-mode-shaped. Poor for the main buffer.

- **Verdict:** viable as a week-one prototype to validate the protocol and the
  interaction model with a throwaway UI. Do not ship on it.

### Recommendation

**GTK4 if the goal is "usable within a month"; AppKit + Scintilla if the goal
is "comfortable on macOS for years."** Given that this project is explicitly
being parked until the language evolves, and that the value is in §2 (compiler
work) rather than the shell, the sequencing that dominates is: build `core/` and
`headless/` first, prototype the UI in cimgui to feel out the interaction, then
choose between GTK4 and AppKit with actual experience of what the panels need.

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

Emerald compiles to C and links a native binary; there is no interpreter. So a
REPL means: take the accepted prefix, append the typed expression as a
`print(...)` statement, run `emeraldc --emit-c` plus `cc`, execute, capture
stdout.

- Round trip is on the order of 100ms, dominated by `cc`. That is fine for a
  scratch pane and unacceptable for anything on the typing path — so the REPL is
  explicitly *not* a live evaluation feature.
- **Show the inferred type, always, above the value.** In a language whose whole
  claim is about type systems, the type is the interesting output. This is the
  one respect in which this REPL beats DrRacket's.
- Errors come back as ordinary structured diagnostics and render identically to
  buffer diagnostics.
- Requires the accepted prefix to be a valid program; if `status != IDLE` the
  pane is disabled rather than silently using stale scope.

A `--emit-c`-only mode (skip `cc`, skip running) is worth a toggle: for
type-level experimentation, the answer is the type, and paying for a C compile
is pointless.

---

## 10. Concurrency and cancellation

- **The UI thread never blocks on a subprocess.** One worker thread, one request
  queue, results posted back to the UI thread.
- **Cancellation by generation counter.** Every request carries the generation
  it was issued under; a reply whose generation is stale is discarded. ~50
  lines, and it is a complete cancellation story without any protocol support.
  The LSP's `$/cancelRequest` can be wired later for politeness; it changes
  nothing about correctness.
- **Coalesce.** Rapid advance keypresses should collapse to one check at the
  final locus. Same for retract.
- **Kill in-flight work on interrupt** — `SIGKILL` the child and reap it. The
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
| toggle panel | `⌘1/2/3` |

Everything else is standard macOS text editing, which is an argument for §6
option 2.

---

## 13. Build and packaging

- `Taskfile.yml`, matching `../emerald`: `task` builds, `task test` runs the
  golden suites, `task bless` regenerates expectations.
- `core/` and `headless/` must build and test with **no GUI dependency
  installed at all**. This is the property that keeps CI simple and the toolkit
  decision reversible.
- Shipping a `.app` bundle with a bundled `emeraldc` is a later problem. Until
  then, resolve `emeraldc` from `$PATH` with an override in settings.
- No installer, no auto-update, no signing until someone other than the author
  runs it.

---

## 14. Testing

Extend `../emerald`'s golden-test culture rather than inventing anything.

- **Session goldens.** `headless/` reads a script (`open f.rald`, `advance ×3`,
  `edit 12:1 "foo"`, `retract`, …) and prints the session state after each step.
  Diff against a checked-in `.expected`. Directory-per-case, `flags` file,
  `bad_*` prefix for expected failures — copy the `imports` suite's shape,
  since multi-module scenarios matter here too.
- **Buffer property tests.** Random edit sequences against a naive
  string-rebuild reference implementation, asserting the gap buffer and line
  index agree. This catches essentially every buffer bug and is ~50 lines.
- **Position-encoding tests.** Emoji and combining characters, asserting byte ↔
  UTF-16 ↔ line/col round-trips. Same test as DESIGN.md §8; steal it.
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
- **Not cross-platform, yet.** macOS only. The `core/` split means a Linux port
  is a UI backend, but do not carry the burden before there is a second user.
- **No proof script language.** If Emerald ever grows tactics, this spec needs a
  real revision, not an extension.

---

## 16. Open questions — revisit when the language evolves

These are the reasons this project is parked, and each one is a question about
Emerald rather than about the IDE.

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
   Worth prototyping both before committing — the cimgui prototype in §6 exists
   for exactly this question.

---

## Suggested order

```
  ../emerald: end positions → parser error recovery → loader overlay hook
        →  --check --upto  →  statement outline in --lsp-symbols
        →  --env-at (narrowing reasons: the real work)

  emerald-ide: core/ buffer + doc + proc + json
        →  headless/ session driver + golden tests
        →  cimgui prototype: locus, shading, diagnostics, goal panel
        →  decide the toolkit  →  real UI
        →  obligation ledger  →  repl pane
        →  LSP client (hover, goto-def, completion) — last
```

The first three compiler steps are shared with `emerald-lsp` and should be done
once, for both. Everything in the IDE before "decide the toolkit" runs without a
window, and that is deliberate: the interesting part of this program is a state
machine and a protocol, and both are fully testable in the dark.
