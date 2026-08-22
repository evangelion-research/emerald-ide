use std::path::Path;

fn main() {
    let grammar = Path::new("grammar/src");
    println!("cargo:rerun-if-changed=grammar/src/parser.c");
    cc::Build::new()
        .include(grammar)
        .file(grammar.join("parser.c"))
        .flag_if_supported("-Wno-unused-parameter")
        .flag_if_supported("-Wno-unused-but-set-variable")
        .warnings(false)
        .compile("tree_sitter_emerald");
}
