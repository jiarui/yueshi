# yueshi TODO

yueshi is a Lua 5.4 interpreter built on
[peglib](https://github.com/jiarui/peglib), a C++20 header-only PEG library.

## Done

- peglib submodule integrated (grammar + lexer infrastructure)
- Token type and TokenID enum (Lua 5.4 full keyword + operator set)
- Basic lexer grammar (names, numerals, comments, strings, operators)
- Tokenizer with semantic actions (keyword disambiguation, int/float parsing)
- CMake build with doctest (bundled in peglib submodule)
- Long-bracket strings (`[[ ... ]]`, `[==[ ... ]==]`) and comments
- All operators including `//`, `<<`, `>>`, `~`, `...`, `::`
- Hex integers / hex floats / decimal floats / exponents (`0xFF`, `0x1.8p3`, `1e-4`)
- Numeric literal sign correctness (signs are unary operators, not part of the literal)
- Escape sequence decoding (`\n`, `\u{XXXX}`, `\xAB`, `\ddd`, `\z`)
- `do` keyword + `Token::operator==` is const (satisfies peglib `PegValue` for token-level parsing)
- peglib-backed lexer diagnostics (`take_error()` → `file:line:col: error: expected X`)
- Token source ranges (`start` / `end` byte offsets)
- Eager `Tokenizer::tokenize()` returning `std::vector<Token>` (with trailing `TK_EOS`)
- CI: GCC 15, Clang 22, MSVC v145, plus a GCC 15 ASan+UBSan Debug gate
- Parser: 38-node `std::variant` AST, 14-level precedence ladder, suffix-loop
  `prefixexp` (no left recursion), `ASTPrinter` S-expression output, parser_test 34/34
- Lexer gaps closed via the official suite: shebang first-line, `\ddd` 1–3 digits,
  raw-newline rejection in short strings, escaped `\\` / `\z` / line-continuation,
  hex floats with no integer part (`0x.0p-3`)
- Official Lua 5.4.8 test suite integrated as a parser corpus
  (`test/parser_corpus.cpp` over `test/corpus/lua-5.4.8-tests/`): 33/33 clean,
  with a known-failure allowlist as the regression guard

## Phase 1 — Lexer (double-pass)

- [x] `Token { TokenID, TokenValue, SourceRange }` — source range tracking
- [x] Complete Lua 5.4 lexing:
      - [x] Long-bracket strings (`[[ ... ]]`, `[==[ ... ]==]`)
      - [x] Long-bracket comments
      - [x] All operators including `//`, `<<`, `>>`, `~`, `...`, `::`
      - [x] Hex floats and exponents (`0xA23p-4`)
      - [x] Unicode escape sequences (`\u{XXXX}`)
- [ ] `TokenStream` API (`peek(n)` / `next()` / `expect(TokenID)`) — deferred to
      Phase 2; `tokenize()` + peglib `Context<Token>` cover the parser's needs
- [x] Keyword vs name disambiguation via `cut`
- [ ] Comprehensive lexer test suite (Lua 5.4 reference lexer corpus) —
      correctness/regression cases are in `test/lex_correctness.cpp`; the
      official suite now lexes clean as part of the parser corpus
      (`test/parser_corpus.cpp`, 33/33), which surfaced and fixed several
      real lexer gaps (shebang, `\ddd` 1–3 digits, raw newlines in short
      strings, `\\` / `\z` / line-continuation escapes, hex floats with no
      integer part). A dedicated lexer-only corpus run is still pending.
- [x] Minor: reject raw newlines inside short strings (H8.3) — short-string
      scan now excludes `\n`/`\r`, so an unterminated string errors at its real
      location instead of running on to a later quote
- [ ] Minor: bare `\r` line endings (H8.2)

## Phase 2 — Parser → Typed AST

- [x] `AST.h` with `std::variant`-based strong node types
- [x] **Explicit precedence layering** (14 priority levels, 2 associativities)
      — NOT pure left recursion (PEG left-recursion is precedence-unaware;
      see Lua 5.4 operator precedence table)
- [x] Semantic actions build AST nodes from the value stack
- [x] `ASTPrinter` (S-expression output) for debugging
- [x] Parse the Lua 5.4 official test suite — corpus checked in under
      `test/corpus/lua-5.4.8-tests/` and driven by `test/parser_corpus.cpp`,
      which lex+parses every `.lua` and asserts clean parses via an explicit
      known-failure allowlist (no fixed %-gate; the point is to expose the
      parser's real state and catch regressions). Currently 33/33 clean.

## Phase 3 — Validation

- [ ] Error-recovery study on real-world Lua sources
- [ ] Performance benchmark: parse 1 MB Lua file (target: cut release keeps
      memory bounded)

## Future Milestones (not in current scope)

- **M2: Tree-walking evaluator** — `LuaValue` (nil/bool/number/string/table),
  closures, environments, metatables, basic operators
- **M3: Standard library subset** — `print`, `string`, `table`, `math`, `io`,
  `os`, optional `coroutine`
- **M4: Full stdlib + GC** — mark-sweep or ref-count + cycle detection,
  `pcall` error handling, complete official test suite
- **M5: Bytecode VM** — register-based compiler + VM for performance parity
  with reference Lua

## Open Design Decisions

| Decision | Choice | Rationale |
| ---      | ---    | ---       |
| Pass model | Double-pass (char → token → AST) | Cleaner lexer/parser separation; matches reference Lua |
| Precedence | Explicit layering (14 levels) | PEG left-recursion is precedence-unaware |
| Test framework | doctest (bundled in peglib) | Header-only, zero external deps; already used by peglib upstream |
| peglib integration | Git submodule | Tracks upstream peglib improvements independently |
