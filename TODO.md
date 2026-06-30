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
  `prefixexp` (no left recursion), `ASTPrinter` S-expression output, parser_test 56/56
  (1 534 assertions) — green under ASan + UBSan (leak-checked)
- Lexer gaps closed via the official suite: shebang first-line, `\ddd` 1–3 digits,
  raw-newline rejection in short strings, escaped `\\` / `\z` / line-continuation,
  hex floats with no integer part (`0x.0p-3`)
- Official Lua 5.4.8 test suite integrated as a parser corpus
  (`test/parser_corpus.cpp` over `test/corpus/lua-5.4.8-tests/`): 33/33 clean,
  with a known-failure allowlist as the regression guard
- **M2.0 Tree-walking evaluator** — GC-first runtime: tagged-union `LuaValue`,
  intrusive `GCObject` header on every collectable, `Heap`-owned objects with
  stop-the-world mark-sweep (no `shared_ptr`; cycles collected by reachability),
  Lua 5.4 int/float subtype arithmetic, closures, tables, multires, 14 builtins.
  evaluator_test 83/83 (2 581 assertions) + gc_test 9/9 (32 assertions) —
  green under ASan + UBSan (leak-checked). `yueshi <file.lua>` runs end-to-end.

## Phase 1 — Lexer (double-pass)

- [x] `Token { TokenID, TokenValue, SourceRange }` — source range tracking
- [x] Complete Lua 5.4 lexing:
      - [x] Long-bracket strings (`[[ ... ]]`, `[==[ ... ]==]`)
      - [x] Long-bracket comments
      - [x] All operators including `//`, `<<`, `>>`, `~`, `...`, `::`
      - [x] Hex floats and exponents (`0xA23p-4`)
      - [x] Unicode escape sequences (`\u{XXXX}`)
- [~] `TokenStream` API (`peek(n)` / `next()` / `expect(TokenID)`) — **won't-do**;
      superseded by eager `tokenize()` + peglib `Context<Token>` driving a
      declarative token-level PEG. A tree-walking evaluator (M2) walks the AST,
      not tokens, so this hand-written cursor API has no consumer.
- [x] Keyword vs name disambiguation via `cut`
- [x] Comprehensive lexer test suite — `test/lex_correctness.cpp` holds 22
      named correctness/regression cases, and the official suite is exercised
      on every `.lua` as part of the parser corpus (`test/parser_corpus.cpp`
      lexes+checks 33/33), which surfaced and fixed several real lexer gaps
      (shebang, `\ddd` 1–3 digits, raw newlines in short strings, `\\` / `\z`
      / line-continuation escapes, hex floats with no integer part).
- [x] Minor: reject raw newlines inside short strings (H8.3) — short-string
      scan now excludes `\n`/`\r`, so an unterminated string errors at its real
      location instead of running on to a later quote
- [x] Minor: bare `\r` line endings (H8.2) — `linebreak` now matches `\r\n`,
      `\n`, and lone `\r`; `not_linebreak` excludes `\r` so comments and short
      strings stop at a CR-only line ending (covered by `H8.2` regression cases)

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

- [x] **M2.0 Tree-walking evaluator** — GC-first runtime implemented and green.
      Runtime model (`value.h`): a tagged-union `LuaValue` (nil/bool/int/float/
      string/table/closure/builtin); collectable objects derive an intrusive
      `GCObject` header and are owned exclusively by the `Heap`. No
      `shared_ptr` anywhere — aliasing is non-owning pointers, so cycles and
      escaping closures are collected by reachability (mark-sweep), not
      refcounting. Stop-the-world mark-sweep (`heap.h`/`heap.cpp`) reclaims
      unreachable objects between statements from the live environment chain.
      Lua 5.4 integer/float subtype arithmetic (`numops.h`): `/`→float, `//`/
      `%` floor, `^`→float, integer overflow wrap, bitwise int-only (errors on
      floats), cross-subtype `1==1.0`, `3.0`≠`3` string form. Control flow
      (if/elseif/else, while, repeat-until, numeric-for, generic-for, break,
      return, do/end), closures with lexical capture, varargs, multires (last-
      expr expansion, `Paren` truncation), tables (positional/named/bracketed
      ctor, get/set, `#` border, aliasing), method dispatch (`obj:m()` with
      implicit `self`), and 14 builtins (`print`/`type`/`tostring`/`tonumber`/
      `error`/`assert`/`ipairs`/`pairs`/`next`/`select`/`rawget`/`rawset`/
      `rawequal`/`rawlen`). Tests: evaluator 83/83 (2 581 assertions), GC 9/9
      (32 assertions), both green under ASan + UBSan with leak detection.
      The `yueshi <file.lua>` CLI runs Lua end-to-end.
- [ ] Error-recovery study on real-world Lua sources — deferred to M4
      (pcall + error handling). peglib's recover API (`recover_set` /
      `recover_eol` / `set_recovery`) exists but no parser rule uses it yet;
      the parser currently stops at the first syntax error. Most valuable once
      a REPL/LSP surfaces multiple errors per file.
- [ ] Performance benchmark: parse 1 MB Lua file (target: cut release keeps
      memory bounded) — deferred to M5 (bytecode VM, perf parity with
      reference Lua). The current corpus's largest file is ~42 KB; pure parser
      throughput has no M2 impact, and a benchmark is most meaningful as part
      of a full perf suite against reference Lua.

## Future Milestones (not in current scope)

- **M2.x: Metatables + goto/labels** — metamethods (arithmetic, index, call),
  `setmetatable`/`getmetatable`, and goto/label resolution (currently raise a
  clear "not yet supported" runtime error). Build on the M2.0 GC foundation.
- **M3: Standard library subset** — `string`, `table`, `math`, `io`, `os`,
  optional `coroutine`, real `_G`/`_ENV` table semantics (globals are modeled
  via the root Environment for now).
- **M4: Full stdlib + advanced GC** — incremental/generational GC + finalizers
  (the `<close>` attribute is a no-op in M2.0), `pcall`/`error` objects,
  complete official test suite.
- **M5: Bytecode VM** — register-based compiler + VM for performance parity
  with reference Lua.

## Open Design Decisions

| Decision | Choice | Rationale |
| ---      | ---    | ---       |
| Pass model | Double-pass (char → token → AST) → interpret | Cleaner lexer/parser separation; matches reference Lua |
| Precedence | Explicit layering (14 levels) | PEG left-recursion is precedence-unaware |
| Value model | Tagged union `LuaValue` + intrusive `GCObject` | GC tracing is one `switch`; matches reference Lua's `TValue` shape |
| Object ownership | `Heap`-owned (single owner), non-owning pointers elsewhere | `shared_ptr` would model co-ownership of aliased data; a single owner + reachability collector is unambiguous and collects cycles |
| GC | Stop-the-world mark-sweep, designed in at the floor | Cycles + escaping closures need reachability, not refcounting; bolting GC on later means redesigning the object layout |
| Numbers | Lua 5.4 int/float subtypes | Correct `/ // % ^` semantics; matches the lexer's `TK_INT`/`TK_FLT` split |
| Test framework | doctest (bundled in peglib) | Header-only, zero external deps; already used by peglib upstream |
| peglib integration | Git submodule | Tracks upstream peglib improvements independently |
