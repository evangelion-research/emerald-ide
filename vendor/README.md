# Vendored dependencies

Both are committed so the IDE builds offline; licenses are in the upstream
sources.

| Path | What | License |
|---|---|---|
| `raylib/` | [raylib](https://github.com/raysan5/raylib) 6.0, full source (built into `src/libraylib.a` by the Makefile) | zlib |
| `raygui.h` | [raygui](https://github.com/raysan5/raygui) v5.0, header-only (implementation behind `RAYGUI_IMPLEMENTATION`) | zlib |
| `fonts/JetBrainsMono-Regular.ttf` | [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) Regular — the editor's monospace font | OFL-1.1 |

raygui is header-only since v4.0: the upstream repo has no `src/raygui.c` —
everything lives in `src/raygui.h`, which is what is vendored here. The
font is loaded at runtime from `vendor/fonts/` and falls back to raylib's
default font if missing.
