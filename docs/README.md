# Emerald IDE docs

- [`../SPEC.md`](../SPEC.md) — the design this implementation follows.
- [`../README.md`](../README.md) — build, run, keybindings, layout.

The IDE is a pure Tauri app: the proof-session core lives in the Rust backend
(`src-tauri/src/`) behind `analyze` / `check` commands, and the webview
frontend (`src/`, TypeScript) renders the editor, panels, and REPL. There is
no C code outside the vendored tree-sitter grammar in `src-tauri/grammar/`,
which is compiled into the binary by `src-tauri/build.rs`.
