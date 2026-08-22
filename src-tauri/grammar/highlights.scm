; Comments and literals
(comment) @comment
(string) @string
(f_string) @string
(escape_sequence) @constant.character.escape
(escaped_brace) @constant.character.escape
(interpolation ["{" "}"] @punctuation.special)
(integer) @constant.numeric.integer
(float) @constant.numeric.float
(boolean) @constant.builtin.boolean
(none) @constant.builtin
(refl) @constant.builtin

; Definitions and bindings
(function_definition name: (identifier) @function)
(type_definition name: (type_identifier) @type)
(error_definition name: (type_identifier) @type)
(type_parameter name: (identifier) @type.parameter)
(parameter name: (identifier) @variable.parameter)
(const_declaration name: (identifier) @variable)
(annotated_declaration name: (identifier) @variable)
(dimension_definition name: (identifier) @type)
(field_definition name: (identifier) @variable.other.member)
(record_field name: (identifier) @variable.other.member)
(pattern_field name: (identifier) @variable.other.member)

; References
(type_identifier) @type
(primitive_type) @type.builtin
(generic_type name: ["list" "seq" "Tensor" "Fin" "Eq"] @type.builtin)
((generic_type name: (type_identifier) @type.builtin)
 (#any-of? @type.builtin "Result" "Chan" "Task"))
(call_expression function: (identifier) @function)
(call_expression function: (attribute_expression attribute: (identifier) @function.method))
(attribute_expression attribute: (identifier) @variable.other.member)
(import_alias name: (identifier) @namespace)
(dotted_name (identifier) @namespace)

; Keywords
[
  "def" "type" "error" "dim" "const" "pure" "partial"
] @keyword.storage.type
[
  "if" "elif" "else" "match" "catch"
] @keyword.control.conditional
[
  "while" "for" "in"
] @keyword.control.repeat
[
  "return" "try"
] @keyword.control.return
[
  (break_statement)
  (continue_statement)
  (pass_statement)
] @keyword.control.return
[
  "import" "from" "as"
] @keyword.control.import
[
  "and" "or" "not" "in"
] @keyword.operator

; Operators and punctuation
[
  "=" "+=" "-=" "*=" "/="
  "+" "-" "*" "/" "//" "%" "**"
  "==" "!=" "<" "<=" ">" ">="
  "|" "&" "^" "<<" ">>>" "|>" ">>" "->" "=>" "?"
] @operator
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," "." ":" ";"] @punctuation.delimiter

; Conventional builtin calls
((call_expression function: (identifier) @function.builtin)
 (#any-of? @function.builtin
  "append" "append_file" "arange" "argmax" "argv" "astype" "chan"
  "chan_close" "chan_len" "chr" "dict" "dtype" "eprint" "exit" "exp"
  "expand" "file_exists" "filter" "float" "flush" "freeze" "full"
  "gc_collect" "gc_stats" "input" "int" "item" "join" "len" "log"
  "map" "matmul" "max" "mean" "ndim" "now" "ones" "ord" "permute"
  "pp_format" "pprint" "pprint_err" "print" "rand" "randn" "range"
  "read_all" "read_file" "read_file_opt" "read_line" "recv" "reduce"
  "relu" "reshape" "run" "seed_rand" "send" "set" "shape" "sleep"
  "slice" "spawn" "sqrt" "str" "sum" "tan" "tanh" "task_done"
  "task_stats" "task_yield" "tensor" "thaw" "transpose" "tslice"
  "write_err" "write_file" "write_out" "zeros"))
