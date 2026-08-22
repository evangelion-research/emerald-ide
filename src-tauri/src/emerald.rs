//! Tree-sitter analysis of Emerald source.
//!
//! The grammar is vendored from github.com/evangelion-research/tree-sitter-emerald
//! (`src/parser.c`, compiled in build.rs; `highlights.scm`).
//!
//! Everything the proof session needs is derived here from the concrete
//! syntax tree — statement ranges for the locus, highlight spans, symbols
//! for the environment panel, and `never`-sites for the obligation ledger.

use std::sync::OnceLock;

use serde::Serialize;
use streaming_iterator::StreamingIterator;
use tree_sitter::{Language, Node, Parser, Query, QueryCursor, QueryMatch, Tree};

// ---------------------------------------------------------------- language

extern "C" {
    fn tree_sitter_emerald() -> *const ();
}

pub fn emerald_language() -> Language {
    unsafe { tree_sitter_language::LanguageFn::from_raw(tree_sitter_emerald).into() }
}

fn highlights_query() -> &'static Query {
    static QUERY: OnceLock<Query> = OnceLock::new();
    QUERY.get_or_init(|| {
        let src = include_str!("../grammar/highlights.scm");
        Query::new(&emerald_language(), src)
            .expect("vendored highlights.scm must compile against the vendored grammar")
    })
}

fn parser() -> Parser {
    let mut p = Parser::new();
    p.set_language(&emerald_language())
        .expect("language version mismatch");
    p
}

// ---------------------------------------------------------------- shapes

#[derive(Serialize, Clone)]
pub struct Stmt {
    /// node kind of the top-level statement ("function_definition", …)
    pub kind: String,
    /// byte offsets into the analysed text
    pub start: u32,
    pub end: u32,
    /// 0-based line/col of the first token
    pub start_line: u32,
    pub start_col: u32,
    pub end_line: u32,
    /// statement contains an ERROR or MISSING node
    pub error: bool,
}

#[derive(Serialize, Clone)]
pub struct Span {
    pub start: u32,
    pub end: u32,
    /// index into the capture-group palette (see capture_group)
    pub group: u16,
}

#[derive(Serialize)]
pub struct Highlights {
    pub spans: Vec<Span>,
}

#[derive(Serialize, Clone)]
pub struct Symbol {
    /// def | type | error | dim | const | binding
    pub kind: String,
    pub name: String,
    /// rendered signature / value summary
    pub detail: String,
    /// 0-based line
    pub line: u32,
    /// index of the enclosing top-level statement
    pub stmt: u32,
}

#[derive(Serialize, Clone, Debug)]
pub struct Obligation {
    /// "negation fn" | "never binding"
    pub kind: String,
    pub name: String,
    /// 0-based line of the `never` site
    pub line: u32,
    /// index of the enclosing top-level statement
    pub stmt: u32,
}

#[derive(Serialize)]
pub struct Analysis {
    pub statements: Vec<Stmt>,
    pub highlights: Highlights,
    pub symbols: Vec<Symbol>,
    pub obligations: Vec<Obligation>,
    pub has_error: bool,
}

impl Default for Analysis {
    fn default() -> Self {
        Analysis {
            statements: Vec::new(),
            highlights: Highlights { spans: Vec::new() },
            symbols: Vec::new(),
            obligations: Vec::new(),
            has_error: false,
        }
    }
}

// ------------------------------------------------------------- highlighting

/// Map a Helix-style capture name onto the UI's flat palette.
fn capture_group(name: &str) -> u16 {
    const COMMENT: u16 = 0;
    const STRING: u16 = 1;
    const ESCAPE: u16 = 2;
    const NUMBER: u16 = 3;
    const CONSTANT: u16 = 4;
    const KEYWORD: u16 = 5;
    const FUNCTION: u16 = 6;
    const TYPE: u16 = 7;
    const BUILTIN: u16 = 8;
    const PARAMETER: u16 = 9;
    const MEMBER: u16 = 10;
    const NAMESPACE: u16 = 11;
    const OPERATOR: u16 = 12;
    const PUNCTUATION: u16 = 13;

    let g = if name.starts_with("comment") {
        COMMENT
    } else if name.starts_with("string") {
        STRING
    } else if name.starts_with("constant.character.escape") || name == "punctuation.special" {
        ESCAPE
    } else if name.starts_with("constant.numeric") {
        NUMBER
    } else if name.starts_with("constant.builtin") {
        CONSTANT
    } else if name.starts_with("keyword") {
        KEYWORD
    } else if name.starts_with("function.builtin") {
        BUILTIN
    } else if name.starts_with("function") {
        FUNCTION
    } else if name.starts_with("type.builtin") {
        BUILTIN
    } else if name.starts_with("type") {
        TYPE
    } else if name == "variable.parameter" {
        PARAMETER
    } else if name.starts_with("variable.other.member") {
        MEMBER
    } else if name == "namespace" {
        NAMESPACE
    } else if name.starts_with("operator") {
        OPERATOR
    } else {
        PUNCTUATION
    };
    g
}

/// Evaluate `#any-of?` / `#eq?` style general predicates ourselves so the
/// behaviour does not depend on which predicate handling the host crate has.
fn pattern_ok(m: &QueryMatch, query: &Query, text: &[u8]) -> bool {
    let preds = query.general_predicates(m.pattern_index);
    for pred in preds {
        let op = pred.operator.as_ref();
        let negate = op.starts_with("not-");
        let base = op.trim_start_matches("not-");
        if !matches!(base, "eq?" | "any-of?") {
            continue;
        }
        // args: [capture, value…]
        let (cap_idx, values) = match pred.args.split_first() {
            Some((tree_sitter::QueryPredicateArg::Capture(i), rest)) => {
                let vals: Vec<&str> = rest
                    .iter()
                    .filter_map(|a| match a {
                        tree_sitter::QueryPredicateArg::String(s) => Some(s.as_ref()),
                        _ => None,
                    })
                    .collect();
                (*i, vals)
            }
            _ => continue,
        };
        let mut hit = false;
        for c in m.captures {
            if c.index != cap_idx {
                continue;
            }
            let t = &text[c.node.byte_range()];
            if values.iter().any(|v| v.as_bytes() == t) {
                hit = true;
                break;
            }
        }
        let ok = if negate { !hit } else { hit };
        if !ok {
            return false;
        }
    }
    true
}

fn highlight(tree: &Tree, text: &[u8]) -> Highlights {
    let query = highlights_query();
    let mut cursor = QueryCursor::new();
    let mut spans: Vec<Span> = Vec::new();

    let mut matches = cursor.matches(query, tree.root_node(), text);
    while let Some(m) = matches.next() {
        if !pattern_ok(&m, query, text) {
            continue;
        }
        for cap in m.captures {
            let name = query.capture_names()[cap.index as usize];
            if name.is_empty() {
                continue;
            }
            let range = cap.node.byte_range();
            if range.is_empty() {
                continue;
            }
            spans.push(Span {
                start: range.start as u32,
                end: range.end as u32,
                group: capture_group(name),
            });
        }
    }

    spans.sort_by_key(|s| (s.start, s.end));
    Highlights { spans }
}

// ---------------------------------------------------------------- symbols

fn text_at<'a>(node: Node<'a>, text: &'a str) -> &'a str {
    let r = node.byte_range();
    if r.end <= text.len() {
        &text[r]
    } else {
        ""
    }
}

fn name_field<'a>(node: Node<'a>, text: &'a str) -> Option<&'a str> {
    node.child_by_field_name("name").map(|c| text_at(c, text))
}

fn symbols_for(stmt: Node, idx: usize, text: &str) -> Vec<Symbol> {
    let mut out = Vec::new();
    let line = stmt.start_position().row as u32;

    match stmt.kind() {
        "function_definition" => {
            let name = name_field(stmt, text).unwrap_or("");
            // signature: parameters plus return/pure markers, before the body.
            let detail = match (
                stmt.child_by_field_name("parameters"),
                stmt.child_by_field_name("body"),
            ) {
                (Some(p), Some(b)) => text[p.start_byte()..b.start_byte()].trim().to_string(),
                (Some(p), None) => text_at(p, text).to_string(),
                _ => "()".into(),
            };
            out.push(Symbol {
                kind: "def".into(),
                name: name.into(),
                detail,
                line,
                stmt: idx as u32,
            });
        }
        "type_definition" => {
            let value = stmt
                .child_by_field_name("value")
                .map(|n| text_at(n, text))
                .unwrap_or("");
            out.push(Symbol {
                kind: "type".into(),
                name: name_field(stmt, text).unwrap_or("").into(),
                detail: format!("= {}", value.trim()),
                line,
                stmt: idx as u32,
            });
        }
        "error_definition" => {
            out.push(Symbol {
                kind: "error".into(),
                name: name_field(stmt, text).unwrap_or("").into(),
                detail: String::new(),
                line,
                stmt: idx as u32,
            });
        }
        "dimension_definition" => {
            let mut cur = stmt.walk();
            for n in stmt.children(&mut cur) {
                if n.kind() == "identifier" {
                    out.push(Symbol {
                        kind: "dim".into(),
                        name: text_at(n, text).into(),
                        detail: String::new(),
                        line,
                        stmt: idx as u32,
                    });
                }
            }
        }
        "const_declaration" | "annotated_declaration" | "assignment"
        | "augmented_assignment" => {
            let ty = stmt.child_by_field_name("type").map(|n| text_at(n, text));
            let detail = match ty {
                Some(t) => format!(": {}", t.trim()),
                None => String::new(),
            };
            let kind = match stmt.kind() {
                "const_declaration" => "const",
                _ => "binding",
            };
            out.push(Symbol {
                kind: kind.into(),
                name: name_field(stmt, text).unwrap_or("").into(),
                detail,
                line,
                stmt: idx as u32,
            });
        }
        _ => {}
    }
    out
}

// ------------------------------------------------------------- obligations

fn contains(outer: Node, inner: Node) -> bool {
    let o = outer.byte_range();
    let i = inner.byte_range();
    o.start <= i.start && i.end <= o.end
}

/// Every `never` primitive-type site, classified:
///   * in a function's return type      → "negation fn"   (name = function)
///   * annotating a declared binding    → "never binding" (name = binding)
fn never_sites(root: Node, text: &str) -> Vec<Obligation> {
    let mut bounds: Vec<(usize, usize)> = Vec::new();
    let mut cur = root.walk();
    for s in root.children(&mut cur) {
        bounds.push((s.start_byte(), s.end_byte()));
    }
    drop(cur);
    let stmt_of = |byte: usize| -> u32 {
        bounds
            .iter()
            .position(|(a, b)| byte >= *a && byte <= *b)
            .map(|p| p as u32)
            .unwrap_or(0)
    };

    let mut out = Vec::new();
    let mut cursor = root.walk();
    let mut visited = std::collections::HashSet::<usize>::new();
    visit(root, &mut cursor, &mut |node| {
        if node.kind() != "primitive_type" || text_at(node, text) != "never" {
            return;
        }
        let key = node.id();
        if !visited.insert(key) {
            return;
        }

        // classify via ancestors
        let mut kind: Option<&str> = None;
        let mut site_name: Option<String> = None;
        let mut anc = node.parent();
        while let Some(a) = anc {
            match a.kind() {
                "function_definition" => {
                    if let Some(rt) = a.child_by_field_name("return_type") {
                        if rt.id() == node.id() || contains(rt, node) {
                            kind = Some("negation fn");
                            site_name = a
                                .child_by_field_name("name")
                                .map(|n| text_at(n, text).to_string());
                        }
                    }
                    break;
                }
                "annotated_declaration" | "const_declaration" => {
                    if let Some(ty) = a.child_by_field_name("type") {
                        if ty.id() == node.id() || contains(ty, node) {
                            kind = Some("never binding");
                            site_name = a
                                .child_by_field_name("name")
                                .map(|n| text_at(n, text).to_string());
                        }
                    }
                    break;
                }
                "parameter" => break,
                _ => anc = a.parent(),
            }
        }

        if let Some(k) = kind {
            out.push(Obligation {
                kind: k.into(),
                name: site_name.unwrap_or_else(|| "(unnamed)".into()),
                line: node.start_position().row as u32,
                stmt: stmt_of(node.start_byte()),
            });
        }
    });

    out.sort_by_key(|o| o.line);
    out.dedup_by(|a, b| a.line == b.line && a.name == b.name);
    out
}

/// Pre-order DFS with a callback, driven by one reusable cursor.
fn visit(node: Node, cursor: &mut tree_sitter::TreeCursor, f: &mut impl FnMut(Node)) {
    f(node);
    if cursor.goto_first_child() {
        loop {
            let child = cursor.node();
            visit(child, cursor, f);
            if !cursor.goto_next_sibling() {
                break;
            }
        }
        cursor.goto_parent();
    }
}

// ---------------------------------------------------------------- entry

pub fn analyze(text: &str) -> Analysis {
    let tree = match parser().parse(text, None) {
        Some(t) => t,
        None => return Analysis::default(),
    };

    let mut analysis = Analysis::default();

    let mut cur = tree.root_node().walk();
    let top: Vec<_> = tree.root_node().children(&mut cur).collect();
    drop(cur);

    for (idx, s) in top.iter().enumerate() {
        analysis.statements.push(Stmt {
            kind: s.kind().to_string(),
            start: s.start_byte() as u32,
            end: s.end_byte() as u32,
            start_line: s.start_position().row as u32,
            start_col: s.start_position().column as u32,
            end_line: s.end_position().row as u32,
            error: s.has_error(),
        });
        analysis.symbols.extend(symbols_for(*s, idx, text));
    }

    analysis.obligations = never_sites(tree.root_node(), text);
    analysis.highlights = highlight(&tree, text.as_bytes());
    analysis.has_error = tree.root_node().has_error();

    analysis
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "\
import math

type Shape = Circle | Square

def area(s: Shape) -> float pure {
    match s {
        circle -> { return 3.14159 * circle.r ** 2.0 },
        square -> { return square.side ** 2.0 }
    }
}

def contradiction(p: int) -> never pure {
    return fail(\"unreachable\")
}

x: never = contradiction(1)
";

    #[test]
    fn parses_statements_symbols_and_never_sites() {
        let a = analyze(SAMPLE);
        assert!(!a.has_error, "sample must parse cleanly");
        let kinds: Vec<&str> = a.statements.iter().map(|s| s.kind.as_str()).collect();
        assert_eq!(
            kinds,
            vec![
                "import_statement",
                "type_definition",
                "function_definition",
                "function_definition",
                "annotated_declaration",
            ]
        );

        let defs: Vec<&Symbol> = a.symbols.iter().filter(|s| s.kind == "def").collect();
        assert_eq!(defs.len(), 2);
        assert_eq!(defs[0].name, "area");
        assert!(defs[0].detail.starts_with("(s: Shape)"), "{}", defs[0].detail);
        let types: Vec<&Symbol> = a.symbols.iter().filter(|s| s.kind == "type").collect();
        assert_eq!(types.len(), 1);
        assert_eq!(types[0].detail.trim(), "= Circle | Square");

        // one negation fn + one never binding
        assert_eq!(a.obligations.len(), 2, "{:?}", a.obligations);
        assert!(a
            .obligations
            .iter()
            .any(|o| o.kind == "negation fn" && o.name == "contradiction"));
        assert!(a
            .obligations
            .iter()
            .any(|o| o.kind == "never binding" && o.name == "x"));

        // highlights cover keywords and comments
        assert!(!a.highlights.spans.is_empty());
    }

    #[test]
    fn broken_source_reports_error_nodes() {
        let a = analyze("def f( {");
        assert!(a.has_error);
        assert!(a.statements.iter().any(|s| s.error));
    }

    #[test]
    fn highlights_are_monotonic() {
        let a = analyze(SAMPLE);
        let spans = &a.highlights.spans;
        for w in spans.windows(2) {
            assert!(w[0].start <= w[1].start);
            assert!(w[0].end <= w[1].end.max(w[1].start));
        }
    }
}
