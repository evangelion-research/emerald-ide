// Right rail: goal panel, symbol table, obligation ledger, diagnostics, REPL.

import type { CheckOutcome } from "./ipc";
import type { ProofSession, SessionState } from "./session";
import type { Editor } from "./editor";

const KIND_LABEL: Record<string, string> = {
  def: "def",
  type: "type",
  error: "error",
  dim: "dim",
  const: "const",
  binding: "let",
};

function esc(s: string): string {
  return s.replace(/[&<>"]/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : "&quot;",
  );
}

export class Panels {
  private activeTab = "goal";
  private els: Record<string, HTMLElement> = {};
  private replLog: HTMLElement;
  private replInput: HTMLInputElement;

  constructor(
    private session: ProofSession,
    private editor: Editor,
    private jumpToLine: (line: number) => void,
  ) {
    for (const id of ["goal", "symbols", "ledger", "diagnostics", "repl"]) {
      this.els[id] = document.getElementById(`panel-${id}`)!;
    }
    this.replLog = document.getElementById("repl-log")!;
    this.replInput = document.getElementById("repl-input") as HTMLInputElement;

    document.querySelectorAll<HTMLElement>(".rtab").forEach((tab) => {
      tab.addEventListener("click", () => this.showTab(tab.dataset.tab!));
    });

    document.getElementById("repl-form")!.addEventListener("submit", (e) => {
      e.preventDefault();
      const expr = this.replInput.value.trim();
      if (!expr) return;
      this.replInput.value = "";
      void this.runRepl(expr);
    });
  }

  showTab(name: string): void {
    if (!(name in this.els)) return;
    this.activeTab = name;
    document.querySelectorAll<HTMLElement>(".rtab").forEach((t) => {
      t.classList.toggle("active", t.dataset.tab === name);
    });
    for (const [id, el] of Object.entries(this.els)) {
      el.classList.toggle("active", id === name);
    }
    if (name === "repl") this.replInput.focus();
    else this.editor.focus();
  }

  cycleTab(dir: 1 | -1): void {
    const order = ["goal", "symbols", "ledger", "diagnostics", "repl"];
    const i = order.indexOf(this.activeTab);
    this.showTab(order[(i + dir + order.length) % order.length]);
  }

  render(): void {
    const s = this.session.state();
    this.renderGoal(s);
    this.renderSymbols(s);
    this.renderLedger(s);
    this.renderDiags(s);
  }

  /* -------------------------------------------------------------- goal */

  private renderGoal(s: SessionState): void {
    const el = this.els.goal;
    const a = s.analysis;
    if (!a || a.statements.length === 0) {
      el.innerHTML = `<div class="empty-note">No statements yet.</div>`;
      return;
    }

    const caret = this.editor.caretInfo();
    let context = "top level";
    for (const sym of a.symbols) {
      if (sym.kind !== "def") continue;
      const st = a.statements[sym.stmt];
      if (st && caret.line >= st.start_line && caret.line <= st.end_line) {
        context = `${sym.name} ${sym.detail}`;
        break;
      }
    }

    const env = a.symbols.filter((sym) => sym.stmt <= s.locus);
    const parts: string[] = [];
    parts.push(`<h3>context</h3>`);
    parts.push(`<div class="goal-context">${esc(context)}</div>`);

    if (s.status === "failed" && s.diags.length > 0) {
      const d = s.diags[0];
      parts.push(`<h3>residual goal</h3>`);
      parts.push(`<div class="goal-residual">${esc(d.message)}</div>`);
      parts.push(this.typeDiff(d.expected, d.actual));
    } else if (s.status === "checking") {
      parts.push(`<h3>residual goal</h3><div class="empty-note">checking…</div>`);
    } else if (s.locus >= 0) {
      parts.push(
        `<h3>residual goal</h3><div class="empty-note">nothing to prove — ${s.locus + 1}/${a.statements.length} statements accepted</div>`,
      );
    } else {
      parts.push(
        `<h3>residual goal</h3><div class="empty-note">advance into the buffer to start the session</div>`,
      );
    }

    parts.push(`<h3>environment · ${env.length}</h3>`);
    if (env.length === 0) {
      parts.push(`<div class="empty-note">empty</div>`);
    } else {
      for (const sym of env.slice(-60)) {
        parts.push(this.envRow(sym, a.statements[sym.stmt]?.start_line ?? 0, false));
      }
    }
    el.innerHTML = parts.join("");
    this.bindJumps(el);
  }

  /* ----------------------------------------------------------- symbols */

  private renderSymbols(s: SessionState): void {
    const el = this.els.symbols;
    const a = s.analysis;
    if (!a || a.symbols.length === 0) {
      el.innerHTML = `<div class="empty-note">No symbols.</div>`;
      return;
    }
    const parts: string[] = [`<h3>${a.symbols.length} symbols</h3>`];
    for (const sym of a.symbols) {
      parts.push(
        this.envRow(sym, a.statements[sym.stmt]?.start_line ?? 0, sym.stmt > s.locus),
      );
    }
    el.innerHTML = parts.join("");
    this.bindJumps(el);
  }

  private envRow(sym: { kind: string; name: string; detail: string }, line: number, dim: boolean): string {
    return `<div class="env-row${dim ? " dim" : ""}" data-kind="${esc(sym.kind)}" data-line="${line}"><span class="kbadge">${KIND_LABEL[sym.kind] ?? esc(sym.kind)}</span><span class="env-name">${esc(sym.name)}</span><span class="env-detail">${esc(sym.detail)}</span></div>`;
  }

  private typeDiff(expected: string, actual: string): string {
    if (!expected && !actual) return "";
    const exp = expected ? `<span class="exp">expected&nbsp; ${esc(expected)}</span>` : "";
    const act = actual ? `<span class="act">actual&nbsp;&nbsp;&nbsp; ${esc(actual)}</span>` : "";
    return `<div class="diag-types">${exp}${act}</div>`;
  }

  /* ------------------------------------------------------------ ledger */

  private renderLedger(s: SessionState): void {
    const el = this.els.ledger;
    const a = s.analysis;
    if (!a || a.obligations.length === 0) {
      el.innerHTML = `<div class="empty-note">No obligations. Emerald proofs are <code>never</code>-bindings and negation functions; they appear here as you write them.</div>`;
      return;
    }

    let proved = 0;
    let failed = 0;
    const rows: string[] = [];
    for (const o of a.obligations) {
      let verdict: string;
      if (o.stmt === s.failing) {
        verdict = "failed";
        failed++;
      } else if (o.stmt <= s.locus) {
        verdict = "proved";
        proved++;
      } else {
        verdict = "unchecked";
      }
      rows.push(
        `<div class="obl-row" data-line="${o.line}"><span class="verdict ${verdict}">${verdict}</span><span class="obl-kind">${esc(o.kind)}</span><span class="obl-name">${esc(o.name)}</span><span class="obl-line">:${o.line + 1}</span></div>`,
      );
    }
    const summary = `${proved} proved · ${failed} failed · ${a.obligations.length - proved - failed} unchecked`;
    el.innerHTML = `<div class="ledger-summary">${summary}</div>${rows.join("")}`;
    this.bindJumps(el);
  }

  /* ------------------------------------------------------- diagnostics */

  private renderDiags(s: SessionState): void {
    const el = this.els.diagnostics;
    if (s.diags.length === 0) {
      el.innerHTML =
        s.status === "offline"
          ? `<div class="empty-note">emeraldc not found — tree-sitter parse checks only.</div>`
          : `<div class="empty-note">No diagnostics.</div>`;
      return;
    }
    const parts: string[] = [];
    for (const d of s.diags.slice(0, 100)) {
      parts.push(
        `<div class="diag-row" data-line="${d.line - 1}">
          <div class="diag-sev">${esc(d.severity)}${d.code ? ` · ${esc(d.code)}` : ""}</div>
          <div class="diag-msg">${esc(d.message)}</div>
          ${this.typeDiff(d.expected, d.actual)}
          <div class="diag-loc">line ${d.line}, col ${d.column}</div>
        </div>`,
      );
    }
    el.innerHTML = parts.join("");
    this.bindJumps(el);
  }

  private bindJumps(el: HTMLElement): void {
    el.querySelectorAll<HTMLElement>("[data-line]").forEach((row) => {
      row.addEventListener("click", () => {
        this.jumpToLine(Number(row.dataset.line));
      });
    });
  }

  /* -------------------------------------------------------------- repl */

  private async runRepl(expr: string): Promise<void> {
    const entry = document.createElement("div");
    entry.className = "repl-entry";
    entry.innerHTML = `<div class="in">&gt; ${esc(expr)}</div><div class="out-ok">…</div>`;
    this.replLog.appendChild(entry);
    this.replLog.scrollTop = this.replLog.scrollHeight;

    try {
      const out: CheckOutcome = await this.session.evalExpression(expr);
      const outEl = entry.querySelector(".out-ok")!;
      if (out.error) {
        outEl.className = "out-err";
        outEl.textContent = out.error;
      } else if (out.diags.length > 0) {
        outEl.className = "out-err";
        outEl.textContent = out.diags
          .map((d) => `${d.message}${d.expected ? ` (expected ${d.expected})` : ""}`)
          .join("\n");
      } else {
        outEl.className = "out-ok";
        outEl.textContent = `accepted · ${out.ms} ms`;
      }
    } catch (e) {
      const outEl = entry.querySelector(".out-ok")!;
      outEl.className = "out-err";
      outEl.textContent = String(e);
    }
  }
}
