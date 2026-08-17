# Emerald IDE

A working, basic IDE for the Emerald language, built with **raylib 6.0** and
**raygui v5.0**, implementing the interaction model in
[`SPEC.md`](SPEC.md): a source buffer with an *accepted prefix*, a goal
panel, an obligation ledger, and a type-checking REPL.

This is the cimgui-style prototype step of the spec's suggested order —
built on raygui instead of Dear ImGui — with the real compiler doing the
checking.

## What it does

- **Edit `.rald` files.** Open (drag-drop, ⌘O, or the Open button), save
  (⌘S), new (⌘N). Line numbers, gutter, syntax coloring (keywords, strings,
  comments, numbers), selection, copy/paste, undo/redo, UTF-8 aware.
- **Prefix checking with a visible locus.** Statements are the unit of
  advance (`def`, `type`, `import`, assignments, …). The accepted prefix is
  shaded green, the failing statement red, the in-flight region amber, and a
  `►` gutter marker shows the locus — CoqIDE-style, but the buffer stays
  editable: editing anywhere retracts the locus to the last statement before
  the edit and re-checks invisibly (SPEC.md §1d).
- **Drives the real compiler.** Every check runs
  `emeraldc --check --json` over the truncated prefix (materialised as a
  temp file next to your source, so relative imports resolve identically)
  and parses the structured diagnostics. `emeraldc` is found via `$EMERALDC`,
  `$PATH`, or the sibling `../emerald/bin/emeraldc`. If it is missing, a tiny
  built-in linter keeps the editor usable instead.
- **Goal panel.** The residual goal (the failing diagnostic's
  expected/actual) plus a source-derived environment of the accepted prefix:
  function signatures, type aliases, and annotated bindings.
- **Obligation ledger.** `impossible: never =` bindings and `-> never`
  functions are detected and given a verdict — *proved*, *out of reach*, or
  *unchecked* — with a live summary line.
- **REPL.** Type an expression; it is type-checked in the scope of the
  accepted prefix via `emeraldc --check`. (The inferred type will appear
  once the compiler grows `--env-at`, SPEC.md §2b.)

## Keybindings

| Action | Binding |
|---|---|
| advance | ⌘↓ |
| retract | ⌘↑ |
| goto cursor | ⌘→ |
| check all | ⌘⏎ |
| interrupt (drop diagnostics) | ⌘. |
| panels: diagnostics / ledger / repl | ⌘1 / ⌘2 / ⌘3 |
| toggle goal panel | ⌘G |
| open / save / save-as / new | ⌘O / ⌘S / ⇧⌘S / ⌘N |
| undo / redo | ⌘Z / ⇧⌘Z |
| cut / copy / paste / select all | ⌘X / ⌘C / ⌘V / ⌘A |
| quit | ⌘Q |

The toolbar also exposes prefix controls for mouse users (◀ ▶ Goto Check all).

## Build and run

Requires macOS with Xcode command-line tools (raylib 6.0, raygui, and the
JetBrains Mono font are vendored under `vendor/`; no Homebrew needed — the
Makefile builds raylib once into `vendor/raylib/src/libraylib.a`).

```sh
make run          # builds bin/emerald-ide, opens the ray-tracer demo
./bin/emerald-ide path/to/file.rald
make test         # golden session tests (headless driver vs tests/*.expected)
make bless        # regenerate the golden expectations
```

The `emeraldc` binary is resolved at startup from `$EMERALDC`, then `$PATH`,
then `../emerald/bin/emeraldc` (the sibling checkout of the compiler). The
status bar shows which one is in use; without any of them the IDE falls back
to its built-in linter.

Two env vars are handy for smoke-testing: `EMERALD_IDE_SHOT=<path>` takes a
screenshot after 30 frames and exits, and `EMERALD_IDE_TAB=0|1|2` selects the
initial bottom tab.

## Layout

```
src/ide.h      core types + API
src/buffer.c   line-array text buffer, UTF-8 helpers
src/json.c     minimal JSON parser for --check --json output
src/session.c  statement splitter, locus state machine, emeraldc runner,
               built-in linter, goal env, obligation ledger, REPL
src/ui.c       raygui front end (window, panels, input, main)
headless/      scripted session driver (make headless)
tests/         golden session scripts (.script) + expectations (.expected)
vendor/        raylib 6.0, raygui v5.0, JetBrains Mono (see vendor/README.md)
```

`src/buffer.c`, `src/json.c` and `src/session.c` are pure C11 with no GUI
dependency, per SPEC.md §4 (`core/` never includes a UI header). The spec's
`headless/` session driver and golden tests are the natural next step.

## Limitations (all inherited from the spec's own open questions)

- The goal panel is derived from source text, not from a compiler env dump:
  `--env-at` (SPEC.md §2b) does not exist in the compiler yet, so inferred
  types and narrowing reasons are not shown.
- The ledger detects the two structural proof forms (`never` bindings and
  negation functions); the spec's declared-proposition forms need a
  language change (SPEC.md §8a).
- Checks are synchronous with a 10 s timeout (the largest example checks in
  ~4 ms), not a worker thread; a hung compiler is killed and reported.
