// App shell: wires the editor, proof session and panels together; owns
// files, keybindings and the status bar.

import { getCurrentWebview } from "@tauri-apps/api/webview";
import { open as openFileDialog, save as saveFileDialog } from "@tauri-apps/plugin-dialog";

import { Editor } from "./editor";
import { Panels } from "./panels";
import { ProofSession } from "./session";
import * as ipc from "./ipc";
import { initThemeSelect } from "./theme";

const WELCOME = `# Emerald — accepted-prefix proof session
# ⌘↓ advance · ⌘↑ retract · ⌘→ check to cursor · ⌘⏎ check all

import math

type Shape = Circle | Square

def area(s: Shape) -> float pure {
    match s {
        circle -> { return 3.14159 * circle.r ** 2.0 },
        square -> { return square.side ** 2.0 }
    }
}

def contradiction(p: int) -> never pure {
    return fail("unreachable")
}

x: never = contradiction(1)
`;

const $ = (id: string) => document.getElementById(id)!;

function esc(s: string): string {
  return s.replace(/[&<>"]/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : "&quot;",
  );
}

class App {
  private editor = new Editor();
  private session = new ProofSession();
  private panels: Panels;

  private filePath: string | null = null;
  private compilerInfo: ipc.CompilerInfo = { path: null, source: null };

  constructor() {
    this.panels = new Panels(this.session, this.editor, (line) => {
      this.editor.scrollToLine(line);
      this.editor.focus();
    });

    this.editor.onChange = (text, caretByte) => {
      $("dirty-dot").classList.remove("hidden");
      this.session.setText(text, caretByte);
    };
    this.editor.onCaretMove = () => this.renderStatus();

    this.session.onUpdate = () => {
      const s = this.session.state();
      this.editor.setSessionView({
        statements: s.analysis?.statements ?? [],
        locus: s.locus,
        failing: s.failing,
        obligations: s.analysis?.obligations ?? [],
        diags: s.diags,
      });
      this.editor.applySpans(s.analysis?.highlights.spans ?? []);
      this.panels.render();
      this.renderStatus();
    };

    this.bindToolbar();
    this.bindKeys();
    void this.bindDragDrop();

    initThemeSelect($("theme-select") as HTMLSelectElement);

    document.fonts?.ready.then(() => this.editor.refreshMetrics());

    this.editor.setText(WELCOME);
    this.editor.focus();
    void this.boot();
  }

  /* ------------------------------------------------------------- boot */

  private async boot(): Promise<void> {
    try {
      this.compilerInfo = await ipc.resolveCompiler();
    } catch {
      this.compilerInfo = { path: null, source: null };
    }
    this.session.setCompiler(this.compilerInfo);
    this.session.setText(this.editor.getText(), 0, { immediateCheck: true });
    this.renderStatus();
  }

  /* ---------------------------------------------------------- toolbar */

  private bindToolbar(): void {
    $("btn-advance").addEventListener("click", () => this.session.advance());
    $("btn-retract").addEventListener("click", () => this.session.retract());
    $("btn-goto").addEventListener("click", () =>
      this.session.gotoCursor(this.editor.caretInfo().byteOffset),
    );
    $("btn-checkall").addEventListener("click", () => this.session.checkAll());
  }

  /* ------------------------------------------------------------- keys */

  private bindKeys(): void {
    window.addEventListener("keydown", (e) => {
      if (!(e.metaKey || e.ctrlKey)) return;
      const k = e.key.toLowerCase();
      switch (k) {
        case "arrowdown":
          e.preventDefault();
          this.session.advance();
          break;
        case "arrowup":
          e.preventDefault();
          this.session.retract();
          break;
        case "arrowright":
          e.preventDefault();
          this.session.gotoCursor(this.editor.caretInfo().byteOffset);
          break;
        case "enter":
          e.preventDefault();
          this.session.checkAll();
          break;
        case ".":
          e.preventDefault();
          this.session.interrupt();
          break;
        case "1":
        case "2":
        case "3":
        case "4":
        case "5":
          e.preventDefault();
          this.panels.showTab(["goal", "symbols", "ledger", "diagnostics", "repl"][Number(k) - 1]);
          break;
        case "[":
          e.preventDefault();
          this.panels.cycleTab(-1);
          break;
        case "]":
          e.preventDefault();
          this.panels.cycleTab(1);
          break;
        case "s":
          e.preventDefault();
          if (e.shiftKey) void this.saveAs();
          else void this.save();
          break;
        case "o":
          e.preventDefault();
          void this.open();
          break;
        case "n":
          e.preventDefault();
          this.newFile();
          break;
        case "z":
          if (document.activeElement === ($("layer-input") as HTMLTextAreaElement)) {
            e.preventDefault();
            if (e.shiftKey) this.editor.redo();
            else this.editor.undo();
          }
          break;
      }
    });
  }

  /* ------------------------------------------------------------ files */

  private dirOf(path: string): string | null {
    const i = path.lastIndexOf("/");
    return i > 0 ? path.slice(0, i) : null;
  }

  private baseName(path: string): string {
    return path.slice(path.lastIndexOf("/") + 1) || "untitled.rald";
  }

  private async open(path?: string): Promise<void> {
    let target = path;
    if (!target) {
      const picked = await openFileDialog({
        multiple: false,
        directories: false,
        filters: [{ name: "Emerald", extensions: ["rald", "emerald", "em"] }],
      });
      if (!picked) return;
      target = picked;
    }
    try {
      const text = await ipc.readFile(target);
      this.filePath = target;
      this.editor.setText(text);
      $("dirty-dot").classList.add("hidden");
      this.updateCrumb();
      this.session.setFileContext(this.dirOf(target), this.baseName(target));
      this.session.setText(text, 0, { immediateCheck: true });
    } catch (e) {
      this.flash(`cannot open ${target}: ${e}`);
    }
  }

  private async save(): Promise<void> {
    if (!this.filePath) return this.saveAs();
    try {
      await ipc.writeFile(this.filePath, this.editor.getText());
      $("dirty-dot").classList.add("hidden");
    } catch (e) {
      this.flash(`cannot save: ${e}`);
    }
  }

  private async saveAs(): Promise<void> {
    const picked = await saveFileDialog({
      defaultPath: this.filePath ?? "untitled.rald",
      filters: [{ name: "Emerald", extensions: ["rald", "emerald", "em"] }],
    });
    if (!picked) return;
    this.filePath = picked;
    await this.save();
    this.updateCrumb();
    this.session.setFileContext(this.dirOf(this.filePath), this.baseName(this.filePath));
    this.session.setText(this.editor.getText(), this.editor.caretInfo().byteOffset, {
      immediateCheck: true,
    });
  }

  private newFile(): void {
    this.filePath = null;
    this.editor.setText("");
    $("dirty-dot").classList.add("hidden");
    this.updateCrumb();
    this.session.setFileContext(null, "untitled.rald");
    this.session.setText("", 0, { immediateCheck: true });
  }

  private updateCrumb(): void {
    $("file-crumb").textContent = this.filePath ? this.baseName(this.filePath) : "untitled.rald";
  }

  /* --------------------------------------------------------- drag-drop */

  private async bindDragDrop(): Promise<void> {
    try {
      await getCurrentWebview().onDragDropEvent((event) => {
        if (event.payload.type === "drop" && event.payload.paths.length > 0) {
          void this.open(event.payload.paths[0]);
        }
      });
    } catch {
      /* drag-drop unavailable */
    }
  }

  /* ----------------------------------------------------------- status */

  private renderStatus(): void {
    const s = this.session.state();
    const n = s.analysis?.statements.length ?? 0;

    const pill = $("session-pill");
    pill.className = `pill ${s.status === "idle" ? "ok" : s.status}`;
    pill.textContent =
      s.status === "checking"
        ? "checking"
        : s.status === "failed"
          ? "failed"
          : s.status === "offline"
            ? "offline"
            : "accepted";

    const caret = this.editor.caretInfo();
    $("st-left").textContent = `${caret.line + 1}:${caret.colByte + 1}`;

    $("st-right").innerHTML =
      `<span class="v">locus <span class="v">${s.locus + 1}/${n}</span></span>` +
      `<span class="v">check <span class="v">${this.session.ms} ms</span></span>` +
      `<span class="compiler">${esc(this.compilerInfo.source ?? "no emeraldc")}</span>`;
  }

  private flash(msg: string): void {
    $("st-left").textContent = msg;
  }
}

new App();
