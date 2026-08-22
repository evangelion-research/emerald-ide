// The proof session: locus state machine over top-level statements.
//
// SPEC.md §1 — statements are the unit of advance; every edit retracts the
// locus to the last statement fully before the edit; checks re-run from
// scratch each time with a generation counter for cancellation (§10).

import type { Analysis, CheckOutcome, Diag } from "./ipc";
import { analyze, check } from "./ipc";

export type Status = "offline" | "idle" | "checking" | "failed";

const ANALYZE_DEBOUNCE_MS = 40;
const CHECK_DEBOUNCE_MS = 220;

export interface SessionState {
  status: Status;
  locus: number;
  failing: number;
  diags: Diag[];
  ms: number;
  analysis: Analysis | null;
}

export class ProofSession {
  private gen = 0;
  private analyzeTimer: number | null = null;
  private checkTimer: number | null = null;

  private text = "";
  private dir: string | null = null;
  private baseName = "untitled.rald";
  private compilerAvailable = true;

  status: Status = "checking";
  locus = -1;
  failing = -1;
  diags: Diag[] = [];
  ms = 0;
  analysis: Analysis | null = null;
  compilerPath: string | null = null;
  compilerSource: string | null = null;

  onUpdate: (() => void) | null = null;

  setCompiler(info: { path: string | null; source: string | null }): void {
    this.compilerPath = info.path;
    this.compilerSource = info.source;
    this.compilerAvailable = info.path !== null;
    if (!this.compilerAvailable && this.status === "checking") {
      this.status = "offline";
      this.deriveParseDiags();
    }
    this.notify();
  }

  setFileContext(dir: string | null, baseName: string): void {
    this.dir = dir;
    this.baseName = baseName;
  }

  /** Editor text changed (user edit or programmatic load). */
  setText(text: string, _caretByte: number, opts?: { immediateCheck?: boolean }): void {
    const edited = text !== this.text;
    this.text = text;

    if (edited) {
      // §1d — retract to the last statement that ends before the edit point
      this.retractToEdit(_caretByte);
    }

    this.scheduleAnalyze();
    if (opts?.immediateCheck) {
      this.clearTimer("checkTimer");
      void this.runCheck();
    } else {
      this.scheduleCheck();
    }
  }

  /* ---------------------------------------------------------- commands */

  advance(): void {
    if (!this.analysis) return;
    if (this.locus + 1 >= this.analysis.statements.length) return;
    this.locus += 1;
    this.notify();
    void this.runCheck();
  }

  retract(): void {
    if (this.locus < 0) return;
    this.locus -= 1;
    // answered locally, no compiler round-trip (SPEC §1e)
    this.dropDiagsAtOrAfter(this.locus + 1);
    this.failing = -1;
    if (this.status === "failed") this.status = "idle";
    this.notify();
    this.scheduleCheck();
  }

  gotoCursor(caretByte: number): void {
    if (!this.analysis || this.analysis.statements.length === 0) return;
    let idx = -1;
    const stmts = this.analysis.statements;
    for (let i = 0; i < stmts.length; i++) {
      if (stmts[i].start <= caretByte) idx = i;
      else break;
    }
    if (idx < 0) idx = 0;
    this.locus = idx;
    this.notify();
    void this.runCheck();
  }

  checkAll(): void {
    if (!this.analysis) return;
    this.locus = Math.max(this.analysis.statements.length - 1, -1);
    this.notify();
    void this.runCheck();
  }

  interrupt(): void {
    this.gen++; // stale replies are discarded (§10)
    this.status = this.compilerAvailable ? "idle" : "offline";
    this.clearTimer("checkTimer");
    this.notify();
  }

  /** REPL: check `print(expr)` against the accepted prefix. */
  async evalExpression(expr: string): Promise<CheckOutcome> {
    return check({
      text: this.text,
      upto: this.acceptedEnd(),
      suffix: `print(${expr})`,
      dir: this.dir,
      baseName: this.baseName,
      timeoutMs: null,
    });
  }

  state(): SessionState {
    return {
      status: this.status,
      locus: this.locus,
      failing: this.failing,
      diags: this.diags,
      ms: this.ms,
      analysis: this.analysis,
    };
  }

  acceptedEnd(): number | null {
    if (!this.analysis || this.locus < 0) return null;
    return this.analysis.statements[this.locus].end;
  }

  /* ------------------------------------------------------------ engine */

  private retractToEdit(byte: number): void {
    if (!this.analysis) return;
    let keep = -1;
    const stmts = this.analysis.statements;
    for (let i = 0; i < stmts.length; i++) {
      if (stmts[i].end <= byte) keep = i;
      else break;
    }
    if (keep < this.locus) {
      this.locus = keep;
      this.dropDiagsAtOrAfter(keep + 1);
      this.failing = -1;
      if (this.status === "failed") this.status = "idle";
    }
  }

  private dropDiagsAtOrAfter(stmtIndex: number): void {
    if (!this.analysis) return;
    const stmts = this.analysis.statements;
    const cut = stmtIndex < stmts.length ? stmts[stmtIndex].start : Number.MAX_SAFE_INTEGER;
    this.diags = this.diags.filter((d) => {
      const line0 = d.line - 1;
      const st = stmts.find((s) => line0 >= s.start_line && line0 <= s.end_line);
      return !st || st.start < cut;
    });
  }

  private scheduleAnalyze(): void {
    this.clearTimer("analyzeTimer");
    this.analyzeTimer = window.setTimeout(() => {
      this.analyzeTimer = null;
      void this.runAnalyze();
    }, ANALYZE_DEBOUNCE_MS);
  }

  private scheduleCheck(): void {
    this.clearTimer("checkTimer");
    this.checkTimer = window.setTimeout(() => {
      this.checkTimer = null;
      void this.runCheck();
    }, CHECK_DEBOUNCE_MS);
  }

  private clearTimer(which: "analyzeTimer" | "checkTimer"): void {
    if (which === "analyzeTimer" && this.analyzeTimer !== null) {
      clearTimeout(this.analyzeTimer);
      this.analyzeTimer = null;
    }
    if (which === "checkTimer" && this.checkTimer !== null) {
      clearTimeout(this.checkTimer);
      this.checkTimer = null;
    }
  }

  private async runAnalyze(): Promise<void> {
    const g = ++this.gen;
    let result: Analysis;
    try {
      result = await analyze(this.text);
    } catch {
      return;
    }
    if (g !== this.gen) return;

    this.analysis = result;
    if (this.locus >= result.statements.length) {
      this.locus = result.statements.length - 1;
    }
    if (!this.compilerAvailable) this.deriveParseDiags();
    this.notify();
  }

  private async runCheck(): Promise<void> {
    if (!this.compilerAvailable) {
      this.deriveParseDiags();
      return;
    }
    const upto = this.acceptedEnd();
    if (upto === null) {
      this.diags = [];
      this.failing = -1;
      this.status = "idle";
      this.notify();
      return;
    }

    const g = ++this.gen;
    this.status = "checking";
    this.notify();

    let outcome: CheckOutcome;
    try {
      outcome = await check({
        text: this.text,
        upto,
        suffix: null,
        dir: this.dir,
        baseName: this.baseName,
        timeoutMs: null,
      });
    } catch {
      if (g !== this.gen) return;
      this.status = "offline";
      this.notify();
      return;
    }
    if (g !== this.gen) return;

    this.ms = outcome.ms;
    this.diags = outcome.diags;

    if (outcome.error && outcome.exit_code === -1 && !outcome.timed_out) {
      this.status = "offline";
    } else if (outcome.diags.length > 0) {
      this.status = "failed";
      this.failing = this.statementOfDiag(outcome.diags[0]);
    } else {
      this.status = "idle";
      this.failing = -1;
    }
    this.notify();
  }

  private statementOfDiag(d: Diag): number {
    if (!this.analysis) return this.locus;
    const line0 = d.line - 1;
    const stmts = this.analysis.statements;
    for (let i = 0; i < stmts.length; i++) {
      if (line0 >= stmts[i].start_line && line0 <= stmts[i].end_line) return i;
    }
    return Math.min(this.locus, Math.max(stmts.length - 1, 0));
  }

  /** No emeraldc: fall back to tree-sitter error nodes as diagnostics. */
  private deriveParseDiags(): void {
    if (!this.analysis) return;
    this.diags = this.analysis.statements
      .filter((s) => s.error)
      .map((s) => ({
        kind: "parse",
        severity: "error",
        code: "E-SYNTAX",
        file: "",
        message: "syntax error (tree-sitter)",
        expected: "",
        actual: "",
        source_line: "",
        line: s.start_line + 1,
        column: s.start_col + 1,
      }));
    this.failing = this.diags.length ? this.statementOfDiag(this.diags[0]) : -1;
    this.status = this.diags.length ? "failed" : "idle";
  }

  private notify(): void {
    this.onUpdate?.();
  }
}
