//! Syntax errors produce recovered nodes; allocation and input failures remain errors.
use std::sync::OnceLock;
use tree_feller::{Child, Language, Node, ParseError};

fn parse(source: &str) -> Result<(usize, usize), ParseError> {
    static LANGUAGE: OnceLock<Language> = OnceLock::new();
    let language =
        LANGUAGE.get_or_init(|| Language::new(tree_sitter_c::LANGUAGE).expect("tables load"));
    language.parse(
        source.as_bytes(),
        |node: Node<'_>, children: &mut Vec<Child<(usize, usize)>>| {
            let mut total = (
                1,
                usize::from(node.symbol() == u16::MAX || node.is_missing()),
            );
            for child in children.drain(..) {
                total.0 += child.value.0;
                total.1 += child.value.1;
            }
            total
        },
    )
}

const MALFORMED: &[&str] = &[
    "int x = ",                    // truncated
    "int a[] = {1, 2",             // unterminated initialiser
    "char *s = \"unterminated",    // unterminated string
    "struct S { int a; ",          // unterminated struct
    "struct { int a; ",            // unterminated anonymous struct
    "int f(void) { return 0;",     // unterminated body
    "int f(void) { return 0; } }", // stray brace
    "int int x;",                  // a token that lexes but has no action here
    "@",                           // unlexable
    "int 1x = 2;",                 // a number where a declarator belongs
];

const WELL_FORMED: &[&str] = &[
    "",
    "// just a comment\n",
    "int x = 1;\n",
    "int a[] = {1, 2, 3};\nchar *s = \"text\";\n",
    "struct S { int a; };\nint f(int b) { return b + 1; }\n",
];

#[test]
fn malformed_input_produces_error_nodes() {
    for source in MALFORMED {
        let (nodes, errors) = parse(source).unwrap_or_else(|error| panic!("{source:?}: {error}"));
        assert!(nodes > 0);
        assert!(errors > 0, "{source:?}: no ERROR or missing node");
    }
}

#[test]
fn well_formed_input_has_no_error_nodes() {
    for source in WELL_FORMED {
        let (_, errors) = parse(source).unwrap_or_else(|error| panic!("{source:?}: {error}"));
        assert_eq!(errors, 0, "{source:?}");
    }
}
