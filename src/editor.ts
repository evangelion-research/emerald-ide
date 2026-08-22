// The editor: a transparent <textarea> stacked over a syntax-highlighted
// <pre>, a shading layer for the accepted prefix, and a synced gutter.
//
// All tree-sitter offsets are UTF-8 bytes; JS strings are UTF-16 — every
// conversion goes through the line tables rebuilt here on each edit.

import type { Diag, Obligation, Span, Stmt } from "./ipc";

/* must match --lh-code / --pad-x / --pad-y in base.css */
const LH = 21;
const PAD_X = 16;
const PAD_Y = 12;
const INDENT = "    ";

interface UndoEntry {
  text: string;
  selStart: number;
  selEnd: number;
  time: number;
  pos: number;
  structural: boolean;
}

export interface CaretInfo {
  line: number; // 0-based
  colByte: number; // byte column
  byteOffset: number;
}

export interface SessionView {
  statements: Stmt[];
  locus: number; // last accepted statement index (-1 = none)
  failing: number; // failing statement index (-1 = none)
  obligations: Obligation[];
  diags: Diag[];
}

function utf8Len(cp: number): number {
  return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : c === '"' ? "&quot;" : "&#39;",
  );
}

function countNewlines(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) === 10) n++;
  return n;
}

export class Editor {
  private scroller: HTMLElement;
  private content: HTMLElement;
  private regionsEl: HTMLElement;
  private codeEl: HTMLElement;
  private diagsEl: HTMLElement;
  private input: HTMLTextAreaElement;
  private gutterInner: HTMLElement;

  private text = "";
  private lineStartU16: number[] = [0];
  private lineStartBytes: number[] = [0];
  private maxCols = 0;

  private chW = 8.4;
  private spans: Span[] = [];
  private view: SessionView = { statements: [], locus: -1, failing: -1, obligations: [], diags: [] };

  private undoStack: UndoEntry[] = [];
  private redoStack: UndoEntry[] = [];
  private pendingUndo: UndoEntry | null = null;

  private caretRaf = 0;

  onChange: ((text: string, caretByte: number) => void) | null = null;
  onCaretMove: ((info: CaretInfo) => void) | null = null;

  constructor() {
    this.scroller = document.getElementById("scroller")!;
    this.content = document.getElementById("content")!;
    this.regionsEl = document.getElementById("layer-regions")!;
    this.codeEl = document.getElementById("code-el")!;
    this.diagsEl = document.getElementById("layer-diags")!;
    this.input = document.getElementById("layer-input") as HTMLTextAreaElement;
    this.gutterInner = document.getElementById("gutter-inner")!;

    this.measureCharWidth();
    this.bind();
  }

  /* ------------------------------------------------------------ metrics */

  measureCharWidth(): void {
    const style = getComputedStyle(this.input);
    const canvas = document.createElement("canvas");
    const ctx = canvas.getContext("2d")!;
    ctx.font = `${style.fontSize} ${style.fontFamily}`;
    this.chW = Math.max(ctx.measureText("M".repeat(100)).width / 100, 0.5);
  }

  refreshMetrics(): void {
    this.measureCharWidth();
    this.layout();
    this.renderCode();
    this.renderGutter();
  }

  /* ------------------------------------------------------- byte ↔ utf16 */

  private rebuildTables(): void {
    const t = this.text;
    const u16: number[] = [0];
    const bytes: number[] = [0];
    let b = 0;
    let cols = 0;
    let maxCols = 0;
    for (let i = 0; i < t.length; ) {
      const cp = t.codePointAt(i)!;
      const size = cp >= 0x10000 ? 2 : 1;
      b += utf8Len(cp);
      if (cp === 10) {
        u16.push(i + 1);
        bytes.push(b);
        if (cols > maxCols) maxCols = cols;
        cols = 0;
      } else {
        cols++;
      }
      i += size;
    }
    if (cols > maxCols) maxCols = cols;
    this.lineStartU16 = u16;
    this.lineStartBytes = bytes;
    this.maxCols = maxCols;
  }

  /** UTF-8 byte offset → UTF-16 index. */
  byteToU16(byte: number): number {
    const bs = this.lineStartBytes;
    let lo = 0;
    let hi = bs.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (bs[mid] <= byte) lo = mid;
      else hi = mid - 1;
    }
    let idx = this.lineStartU16[lo];
    let b = bs[lo];
    const t = this.text;
    while (b < byte && idx < t.length) {
      const cp = t.codePointAt(idx)!;
      b += utf8Len(cp);
      idx += cp >= 0x10000 ? 2 : 1;
    }
    return idx;
  }

  /** UTF-16 index → UTF-8 byte offset. */
  u16ToByte(idx: number): number {
    const us = this.lineStartU16;
    let lo = 0;
    let hi = us.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (us[mid] <= idx) lo = mid;
      else hi = mid - 1;
    }
    let byte = this.lineStartBytes[lo];
    const t = this.text;
    for (let i = us[lo]; i < idx && i < t.length; ) {
      const cp = t.codePointAt(i)!;
      byte += utf8Len(cp);
      i += cp >= 0x10000 ? 2 : 1;
    }
    return byte;
  }

  /* ------------------------------------------------------------- layout */

  private layout(): void {
    const w = Math.max(
      (this.maxCols + 12) * this.chW + PAD_X * 2,
      this.scroller.clientWidth,
    );
    const h = Math.max(
      this.lineStartU16.length * LH + PAD_Y * 2 + 48,
      this.scroller.clientHeight,
    );
    this.content.style.width = `${w}px`;
    this.content.style.height = `${h}px`;
  }

  /* -------------------------------------------------------------- input */

  private bind(): void {
    this.input.addEventListener("beforeinput", () => {
      this.pendingUndo = {
        text: this.text,
        selStart: this.input.selectionStart ?? 0,
        selEnd: this.input.selectionEnd ?? 0,
        time: performance.now(),
        pos: this.input.selectionStart ?? 0,
        structural: false,
      };
    });

    this.input.addEventListener("input", () => {
      this.afterUserEdit(false);
    });

    this.input.addEventListener("keydown", (e) => {
      if (e.isComposing || e.metaKey || e.ctrlKey) return;
      if (e.key === "Tab") {
        e.preventDefault();
        if (e.shiftKey) this.dedent();
        else this.indent();
      } else if (e.key === "Enter") {
        e.preventDefault();
        this.autoIndentEnter();
      }
    });

    this.scroller.addEventListener("scroll", () => {
      this.gutterInner.style.transform = `translateY(${-this.scroller.scrollTop}px)`;
    });

    window.addEventListener("resize", () => this.layout());

    document.addEventListener("selectionchange", () => {
      if (document.activeElement !== this.input || this.caretRaf) return;
      this.caretRaf = requestAnimationFrame(() => {
        this.caretRaf = 0;
        this.emitCaret();
      });
    });
  }

  private afterUserEdit(structural: boolean): void {
    const prev = this.pendingUndo;
    this.pendingUndo = null;
    const now = performance.now();
    const pos = this.input.selectionStart ?? 0;

    if (prev) {
      prev.structural = structural;
      const last = this.undoStack[this.undoStack.length - 1];
      const coalesce =
        !structural &&
        last !== undefined &&
        now - last.time < 600 &&
        Math.abs(pos - last.pos) <= 2;
      if (!coalesce) this.undoStack.push(prev);
      else last.time = now;
      if (this.undoStack.length > 500) this.undoStack.shift();
      this.redoStack.length = 0;
    }

    this.text = this.input.value;
    this.rebuildTables();
    this.layout();
    this.renderGutter();
    this.renderRegions();
    this.onChange?.(this.text, this.u16ToByte(pos));
  }

  private emitCaret(): void {
    const s = this.input.selectionStart ?? 0;
    const byte = this.u16ToByte(s);
    const line = this.lineForByte(byte);
    const col = byte - this.lineStartBytes[line];
    this.onCaretMove?.({ line, colByte: col, byteOffset: byte });
  }

  private lineForByte(byte: number): number {
    const bs = this.lineStartBytes;
    let lo = 0;
    let hi = bs.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (bs[mid] <= byte) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  }

  /* ----------------------------------------------------------- edit ops */

  private splice(start: number, end: number, insert: string): void {
    this.input.focus();
    this.input.setSelectionRange(start, end);
    document.execCommand("insertText", false, insert);
  }

  private indent(): void {
    const s = this.input.selectionStart ?? 0;
    const e = this.input.selectionEnd ?? 0;
    if (s === e) {
      this.splice(s, e, INDENT);
      return;
    }
    const ls = this.text.lastIndexOf("\n", s - 1) + 1;
    let le = this.text.indexOf("\n", e);
    if (le === -1) le = this.text.length;
    const block = this.text.slice(ls, le);
    const out = block
      .split("\n")
      .map((l) => (l.length ? INDENT + l : l))
      .join("\n");
    this.splice(ls, le, out);
    const added = countNewlines(out) * INDENT.length + INDENT.length;
    this.input.setSelectionRange(s + INDENT.length, Math.min(e + added, ls + out.length));
  }

  private dedent(): void {
    const s = this.input.selectionStart ?? 0;
    const e = this.input.selectionEnd ?? 0;
    const ls = this.text.lastIndexOf("\n", s - 1) + 1;
    let le = this.text.indexOf("\n", e);
    if (le === -1) le = this.text.length;
    const lines = this.text.slice(ls, le).split("\n");
    let removedFirst = 0;
    let removedTotal = 0;
    const out = lines
      .map((l, i) => {
        let n = 0;
        while (n < 4 && n < l.length && l[n] === " ") n++;
        if (i === 0) removedFirst = n;
        removedTotal += n;
        return l.slice(n);
      })
      .join("\n");
    this.splice(ls, le, out);
    this.input.setSelectionRange(Math.max(ls, s - removedFirst), Math.max(ls, e - removedTotal));
  }

  private autoIndentEnter(): void {
    const s = this.input.selectionStart ?? 0;
    const before = this.text.slice(0, s);
    const lineStart = before.lastIndexOf("\n") + 1;
    const indent = (/^\s*/.exec(before.slice(lineStart)) || [""])[0];
    const opens = /\{\s*$/.test(before.trimEnd()) ? INDENT : "";
    this.splice(s, this.input.selectionEnd ?? s, "\n" + indent + opens);
  }

  undo(): void {
    const entry = this.undoStack.pop();
    if (!entry) return;
    this.redoStack.push({
      text: this.text,
      selStart: this.input.selectionStart ?? 0,
      selEnd: this.input.selectionEnd ?? 0,
      time: 0,
      pos: 0,
      structural: true,
    });
    this.restore(entry.text, entry.selStart, entry.selEnd);
  }

  redo(): void {
    const entry = this.redoStack.pop();
    if (!entry) return;
    this.undoStack.push({
      text: this.text,
      selStart: this.input.selectionStart ?? 0,
      selEnd: this.input.selectionEnd ?? 0,
      time: 0,
      pos: 0,
      structural: true,
    });
    this.restore(entry.text, entry.selStart, entry.selEnd);
  }

  private restore(text: string, selStart: number, selEnd: number): void {
    this.text = text;
    this.input.value = text;
    this.rebuildTables();
    this.layout();
    this.renderGutter();
    this.renderCode();
    this.renderRegions();
    const s = Math.min(selStart, text.length);
    const e = Math.min(selEnd, text.length);
    this.input.setSelectionRange(s, e);
    this.onChange?.(text, this.u16ToByte(s));
  }

  /* ------------------------------------------------------------- public */

  getText(): string {
    return this.text;
  }

  setText(text: string): void {
    this.text = text;
    this.input.value = text;
    this.undoStack.length = 0;
    this.redoStack.length = 0;
    this.rebuildTables();
    this.layout();
    this.renderGutter();
    this.renderCode();
    this.renderRegions();
    this.input.setSelectionRange(0, 0);
    this.scroller.scrollTop = 0;
    this.syncGutter();
  }

  applySpans(spans: Span[]): void {
    this.spans = spans;
    this.renderCode();
  }

  setSessionView(view: SessionView): void {
    this.view = view;
    this.renderRegions();
    this.renderDiags();
    this.renderGutter();
  }

  caretInfo(): CaretInfo {
    const s = this.input.selectionStart ?? 0;
    const byte = this.u16ToByte(s);
    const line = this.lineForByte(byte);
    return { line, colByte: byte - this.lineStartBytes[line], byteOffset: byte };
  }

  scrollToLine(line: number): void {
    const target = line * LH + PAD_Y - this.scroller.clientHeight / 2;
    this.scroller.scrollTop = Math.max(0, target);
    this.syncGutter();
  }

  focus(): void {
    this.input.focus();
  }

  /* ---------------------------------------------------------- rendering */

  private renderCode(): void {
    const t = this.text;
    if (!t) {
      this.codeEl.textContent = "";
      return;
    }
    const color = new Int16Array(t.length).fill(-1);
    for (const span of this.spans) {
      if (span.end <= span.start) continue;
      const a = this.byteToU16(span.start);
      const b = Math.min(this.byteToU16(span.end), t.length);
      if (a >= b) continue;
      color.fill(span.group, a, b);
    }
    const parts: string[] = [];
    let run = -2;
    let start = 0;
    for (let i = 0; i <= t.length; i++) {
      const g = i < t.length ? color[i] : -3;
      if (g !== run) {
        if (i > start) {
          const chunk = escapeHtml(t.slice(start, i));
          parts.push(run < 0 ? chunk : `<span class="g${run}">${chunk}</span>`);
        }
        run = g;
        start = i;
      }
    }
    this.codeEl.innerHTML = parts.join("");
  }

  private renderRegions(): void {
    const { statements, locus, failing } = this.view;
    const parts: string[] = [];

    const caretLine = this.lineForByte(this.u16ToByte(this.input.selectionStart ?? 0));
    parts.push(
      `<div class="rg caret-line" style="top:${caretLine * LH + PAD_Y}px;height:${LH}px"></div>`,
    );

    for (let i = 0; i < statements.length; i++) {
      const st = statements[i];
      let cls: string | null = null;
      if (i === failing) cls = "fail";
      else if (i <= locus) cls = "ok";
      if (!cls) continue;
      for (let row = st.start_line; row <= st.end_line; row++) {
        parts.push(`<div class="rg ${cls}" style="top:${row * LH + PAD_Y}px;height:${LH}px"></div>`);
      }
    }
    this.regionsEl.innerHTML = parts.join("");
  }

  private renderDiags(): void {
    const parts: string[] = [];
    for (const d of this.view.diags) {
      const row = d.line - 1;
      if (row < 0) continue;
      parts.push(`<div class="dg" style="top:${row * LH + PAD_Y + LH - 5}px;height:4px"></div>`);
    }
    this.diagsEl.innerHTML = parts.join("");
  }

  private renderGutter(): void {
    const n = this.lineStartU16.length;
    const { statements, locus, obligations, diags } = this.view;

    const obligLines = new Set<number>();
    for (const o of obligations) obligLines.add(o.line);
    const diagLines = new Set<number>();
    for (const d of diags) diagLines.add(d.line - 1);

    const stmtAtLine = new Map<number, number>();
    for (let i = 0; i < statements.length; i++) stmtAtLine.set(statements[i].start_line, i);

    const parts: string[] = [];
    for (let row = 0; row < n; row++) {
      let cls = "gl";
      if (stmtAtLine.get(row) === locus) cls += " m-locus";
      if (obligLines.has(row)) cls += " m-oblig";
      if (diagLines.has(row)) cls += " m-diag";
      parts.push(`<div class="${cls}"><span class="mark"></span>${row + 1}</div>`);
    }
    this.gutterInner.innerHTML = parts.join("");
    this.syncGutter();
  }

  private syncGutter(): void {
    this.gutterInner.style.transform = `translateY(${-this.scroller.scrollTop}px)`;
  }
}
