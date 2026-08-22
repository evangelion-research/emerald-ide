// Typed wrappers over the Tauri commands in src-tauri/src/lib.rs.
import { invoke } from "@tauri-apps/api/core";

export interface Stmt {
  kind: string;
  start: number; // byte offsets
  end: number;
  start_line: number; // 0-based
  start_col: number;
  end_line: number;
  error: boolean;
}

export interface Span {
  start: number;
  end: number;
  group: number;
}

export interface Symbol {
  kind: string; // def | type | error | dim | const | binding
  name: string;
  detail: string;
  line: number; // 0-based
  stmt: number;
}

export interface Obligation {
  kind: string; // "negation fn" | "never binding"
  name: string;
  line: number; // 0-based
  stmt: number;
}

export interface Analysis {
  statements: Stmt[];
  highlights: { spans: Span[] };
  symbols: Symbol[];
  obligations: Obligation[];
  has_error: boolean;
}

export interface Diag {
  kind: string;
  severity: string;
  code: string;
  file: string;
  message: string;
  expected: string;
  actual: string;
  source_line: string;
  line: number; // 1-based
  column: number;
}

export interface CheckOutcome {
  exit_code: number;
  timed_out: boolean;
  ms: number;
  diags: Diag[];
  error: string | null;
}

export interface CompilerInfo {
  path: string | null;
  source: string | null;
}

export function analyze(text: string): Promise<Analysis> {
  return invoke("analyze", { text });
}

export function check(args: {
  text: string;
  upto: number | null;
  suffix: string | null;
  dir: string | null;
  baseName: string;
  timeoutMs: number | null;
}): Promise<CheckOutcome> {
  return invoke("check", args);
}

export function resolveCompiler(): Promise<CompilerInfo> {
  return invoke("resolve_compiler");
}

export function readFile(path: string): Promise<string> {
  return invoke("read_file", { path });
}

export function writeFile(path: string, contents: string): Promise<void> {
  return invoke("write_file", { path, contents });
}
