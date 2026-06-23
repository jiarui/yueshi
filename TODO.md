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
      correctness/regression cases are in `test/lex_correctness.cpp`; full
      corpus integration is pending
- [ ] Minor: bare `\r` line endings (H8.2); reject raw newlines inside short
      strings (H8.3)

## Phase 2 — Parser → Typed AST

- [ ] `AST.h` with `std::variant`-based strong node types
- [ ] **Explicit precedence layering** (14 priority levels, 2 associativities)
      — NOT pure left recursion (PEG left-recursion is precedence-unaware;
      see Lua 5.4 operator precedence table)
- [ ] Semantic actions build AST nodes from the value stack
- [ ] `ASTPrinter` (S-expression output) for debugging
- [ ] Parse Lua 5.4 official test suite ≥ 95%

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
