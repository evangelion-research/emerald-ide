# Emerald IDE docs

- [`../SPEC.md`](../SPEC.md) — the design this implementation follows.
- [`../README.md`](../README.md) — build, run, keybindings, layout.

The IDE is currently the "prototype the UI to feel out the interaction" step
of the spec's suggested order, implemented in raygui. The core (`src/buffer.c`,
`src/json.c`, `src/session.c`) is GUI-free so a `headless/` session driver
with golden tests can be added without touching the front end.
