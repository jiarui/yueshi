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
- **M2.1 Metatables + metamethods** — per-table (and per-closure) metatable
  pointer on the GC object, traced by the collector (two-line `trace()` change).
  Full event set: arithmetic (`__add`/`__sub`/`__mul`/`__div`/`__idiv`/`__mod`/
  `__pow`), bitwise (`__band`/`__bor`/`__bxor`/`__shl`/`__shr`), `__unm`/
  `__bnot`, `__concat`, `__eq`/`__lt`/`__le` (with `>`/`>=`/`~=` derived and
  `__le`'s `not (b<a)` fallback via `__lt`), `__index`/`__newindex` (iterative
  chain, raw-slot-wins), `__call`, `__len`, `__tostring`. Arith/bit fallbacks
  use non-throwing type predicates (`both_numbers`/`both_ints`) + the existing
  throwing compute (so `n//0` stays a hard error, not a metamethod trigger).
  `setmetatable`/`getmetatable` builtins; `raw*` family confirmed to bypass.
  evaluator_test 96/96 (5 898 assertions) + gc_test 12/12 (42 assertions) —
  green under ASan + UBSan (leak-checked, incl. metatable edges).
- **M2.2 goto/labels** — `goto Name` / `::Name::` execute at runtime. New
  `Flow::Goto` joins `Break`/`Return`; a third control signal carries the
  label name + source offset through `Control`. `eval_block` is index-driven
  with a per-Block memoized label cache (`m_labels`, cleared per `run()`):
  each block scans its statement list once, last-wins on duplicate labels.
  A goto whose label lives in its own block resumes there (forward or
  backward); otherwise the signal propagates through `Do`/`If`/the four loop
  statements (each got a one-line `if (c.flow == Flow::Goto) return c;` so a
  goto out of a loop doesn't infinitely re-loop) to an enclosing block, or to
  `call_value`/`run`, which report `"no visible label '<name>' for <goto>"`.
  Static scope analysis (jump-into-local-scope, repeated-label-at-compile,
  label-hidden-in-nested-block) is deferred to M3 — those checks are only
  useful/testable once `load()` lands (the official `goto.lua` drives them via
  `load()` of strings). v1 behavior on a static-rule violation is benign: a
  forward goto over a `local` leaves the binding untouched (nil first, prior
  value after), matching Lua's runtime semantics. Tests: evaluator 107/107
  (6 386 assertions) — green under ASan + UBSan.

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
- [x] **M2.1 Metatables + metamethods** — per-table (and per-closure) metatable
      pointer on the GC object, traced by the collector (a two-line `trace()`
      change in `heap.cpp`). Full event set: arithmetic (`__add`/`__sub`/`__mul`/
      `__div`/`__idiv`/`__mod`/`__pow`), bitwise (`__band`/`__bor`/`__bxor`/
      `__shl`/`__shr`), `__unm`/`__bnot`, `__concat`, `__eq`/`__lt`/`__le`
      (with `>`/`>=`/`~=` derived and `__le`'s `not (b<a)` fallback via `__lt`),
      `__index`/`__newindex` (iterative chain, raw-slot-wins), `__call`,
      `__len`, `__tostring`. Arith/bit fallbacks use non-throwing type
      predicates (`both_numbers`/`both_ints`) + the existing throwing compute,
      so `n//0` stays a hard error rather than a metamethod trigger.
      `setmetatable`/`getmetatable` builtins; the `raw*` family confirmed to
      bypass metamethods. Tests: evaluator 96/96 (5 898 assertions), GC 12/12
      (42 assertions), both green under ASan + UBSan (leak-checked, incl. the
      new table/closure→metatable GC edges).
- [x] **M2.2 goto/labels** — `goto Name` / `::Name::` execute at runtime. New
      `Flow::Goto` joins `Break`/`Return`; a third control signal carries the
      label name + source offset through `Control`. `eval_block` is index-
      driven with a per-Block memoized label cache (`m_labels`, cleared per
      `run()`): each block scans its statement list once, last-wins on
      duplicate labels. A goto whose label lives in its own block resumes there
      (forward or backward); otherwise the signal propagates through `Do`/`If`/
      the four loop statements (each got a one-line `if (c.flow==Flow::Goto)
      return c;` so a goto out of a loop doesn't infinitely re-loop) to an
      enclosing block, or to `call_value`/`run`, which report `"no visible
      label '<name>' for <goto>"`. Static scope analysis deferred to M3.
      Tests: evaluator 107/107 (6 386 assertions), green under ASan + UBSan.
- [ ] Error-recovery study on real-world Lua sources — peglib's recover API
      (`recover_set` / `recover_eol` / `set_recovery`) exists but no parser
      rule uses it yet; the parser stops at the first syntax error. See
      *Cross-cutting deferrals* below.
- [ ] Performance benchmark: parse + run a large Lua file — deferred to M5.
      See *Cross-cutting deferrals* below.

## Future Milestones (not in current scope)

The runtime core (M2.0–M2.2) is complete: a GC-first tree-walking evaluator
with the full metatable event set and goto/labels. The remaining work is the
standard library, advanced runtime features, and a performance tier. The
milestones below are ordered by dependency; each lists scope, what it
unblocks in the official `lua-5.4.8-tests` corpus, and explicit deferrals.

### M3 — Standard library subset

The largest remaining milestone; delivers enough of the stdlib that real-world
Lua runs. Naturally splits into sub-milestones by library. Each sub-milestone
is independently shippable and adds its own test corpus file(s).

- **M3.1 `string` library + per-type string metatable** — the highest-value
  piece (most Lua code uses it). Two parts:
  - The basic functions: `len`/`sub`/`upper`/`lower`/`rep`/`reverse`/`byte`/
    `char`/`format`. Thin wrappers over `<string>` / `snprintf`.
  - The **pattern-matching engine**: `find`/`match`/`gmatch`/`gsub`. Lua
    patterns are a mini-regex (character classes, `%a`, `*`/`+`/`-`/`?`,
    anchors `^`/`$`, captures, balanced `%b()`/`%b[]`, frontier `%f[set]`).
    This is the meaty part — a real PEG-ish matcher, likely built on peglib
    or hand-rolled over the subject string.
  - Per-type **string metatable** (deferred from M2.1): populated by `string`,
    so `("hi"):upper()` works. Wires into the existing `__index` machinery —
    a string operand with no per-value metatable falls back to the per-type
    one. Unblocks corpus: `pm.lua`, `strings.lua`.
- **M3.2 `table` library** — `insert`/`remove`/`concat`/`pack`/`unpack`/`move`/
  `sort`. `sort` dispatches through the existing `call_value` (so it honors a
  comparator's metamethods for free). `pack`/`unpack` round out multires
  handling. Unblocks corpus: `sort.lua`, `tpack.lua`.
- **M3.3 `math` library** — constants (`pi`/`huge`/`maxinteger`/`mininteger`)
  + functions (`abs`/`ceil`/`floor`/`exp`/`log`/`pow`/`sqrt`/`sin`/`cos`/`tan`/
  `min`/`max`/`fmod`/`modf`/`random`/`randomseed`/`tointeger`/`type`). Almost
  entirely thin wrappers over `<cmath>`; the only subtlety is `random` (state
  lives in the interpreter). Unblocks corpus: `math.lua`, parts of `literals.lua`.
- **M3.4 `io` + `os` libraries** — needs a **file-handle / userdata** concept
  (a new `LuaValue` tag, or a `Table` with a hidden native pointer slot).
  - `io`: `write`/`read`/`open`/`close`/`lines`, plus `io.stdin`/`io.stdout`/
    `io.stderr` handles. File handles get a `__gc`-driven close (but `<close>`
    semantics itself is M4 — M3 just leaks-closes at process exit or via the
    existing `__gc` hook once M4 lands).
  - `os`: `time`/`clock`/`date`/`difftime`/`getenv`/`exit`/`execute`/`remove`/
    `rename`. Thin wrappers over POSIX/CRT.
  - Unblocks corpus: `files.lua`, parts of `all.lua`.
- **M3.5 `_G`/`_ENV` + `load()`/`dofile()` + `require`** — the load-bearing
  piece for the rest of the suite:
  - **`_G`/`_ENV`**: real global-table semantics. Currently globals live in
    the root `Environment`; `_ENV` is Lua 5.4's name for the globals upvalue.
    Low-risk refactor: make `_ENV` an explicit upvalue of every chunk, `_G`
    its initial value. `_ENV = {}` swapping then works for free.
  - **`load()`/`loadstring()`/`dofile()`**: compile-from-source. Re-enters
    the existing lex→parse→eval pipeline. Unlocks:
    - The **deferred static goto-scope analysis** (8 checks the official
      `goto.lua` drives via `load()` of strings: forward-jump-into-local-
      scope, repeated-label, label-hidden-in-nested-block, repeat-until-
      variable, etc.). v1 of goto (M2.2) runs these programs with benign
      runtime behavior; `load()` is what makes the compile-time errors both
      useful and testable.
    - Runtime compile errors as message strings (real Lua's `load` returns
      `nil, errmsg`).
  - **`require`/`package`**: `package.path`/`package.cpath` resolution and
    module caching. Needed for any real-world script.
  - Unblocks corpus: `code.lua`, `goto.lua` (runtime portion), `libs/`.

**M3 out of scope (→ M4):** `coroutine`, `debug`, `pcall`/`xpcall` error
objects, `<close>` semantics, weak tables, finalizers.

### M4 — Full stdlib + advanced GC

Two intertwined themes: finish the runtime features the official suite needs,
and make the collector production-quality.

- **Error handling** — `pcall`/`xpcall` (currently `error()` throws C++
  `LuaError` to the top of `run()`; `pcall` needs to catch it at the Lua
  level and return `nil, msg`). `error()`'s argument may be any value (with
  the `level` argument controlling position info); error objects carry a
  metatable (`__tostring` for messages). Unblocks corpus: `errors.lua`.
- **`coroutine` library** — `create`/`resume`/`yield`/`status`/`wrap`/
  `isyieldable`/`running`. The hard part: a tree-walking evaluator runs on the
  C++ stack, so a Lua coroutine needs either a separate C++ coroutine per Lua
  coroutine (C++20 stackful via `boost::context` or a hand-rolled fiber) or
  a continuation-passing transform. Decision deferred — likely C++20
  coroutines wrapping the evaluator's `call_value`. Unblocks corpus:
  `coroutine.lua`.
- **`debug` library** — `getinfo`/`traceback`/`getlocal`/`setlocal`/
  `getupvalue`/`setupvalue`/`upvalueid`/`upvaluejoin`. Requires the evaluator
  to retain enough frame metadata (call stack of `{closure, env, pc-as-
  stmt-index}`) to introspect. Unblocks corpus: `db.lua`, the upvalue-identity
  portion of `goto.lua`.
- **Finalizers + `<close>`** — `__gc` metamethod on tables/closures (run at
  sweep time for the next-cycle-dead objects); the `<close>` attribute on
  locals (run the `__close` metamethod at scope exit, including goto-jumps-out-
  of-scope — this is the `<close>` × goto interaction that's a no-op in M2.2).
- **Advanced GC** — incremental mark-sweep (the current collector is stop-the-
  world at statement boundaries, fine for correctness but pauses matter once
  real programs run). Generational mode optional. `collectgarbage()` builtin
  with `collect`/`count`/`stop`/`restart`/`step`/`setpause`/`setstepmul`/
  `isrunning`/`generational`/`incremental` options. Unblocks corpus: `gc.lua`,
  `gengc.lua`, `tracegc.lua`.
- **Weak tables** — `__mode` metamethod (`"k"`/`"v"`/`"kv"`); the collector
  clears weak references before sweep. Small extension to the existing tracer.
- **`utf8` library** — `len`/`offset`/`codepoint`/`char`/`codes` + the
  `utf8.charpattern` pattern. Small, self-contained; mostly UTF-8 decoding.
  Unblocks corpus: `utf8.lua`.

**M4 success criterion:** the complete official `lua-5.4.8-tests` suite runs
(with an allowlist for platform-specific timing/`os` quirks).

### M5 — Bytecode VM (performance tier)

The tree-walker stays as the reference interpreter; M5 adds a register-based
compiler + VM for performance parity with reference Lua.

- **Bytecode design** — register-based (matches Lua 5.x; a stack machine
  would need 5–10× more dispatches). A prototype frame is a fixed-size cell
  array; instructions reference registers by index.
- **Compiler (AST → bytecode)** — a separate pass over the typed AST. Closures
  become upvalue-capturing prototypes (the GC already models `Closure`+
  `Environment`; upvalues become first-class GC objects shared across
  closures, enabling the `debug.upvalueid`/`upvaluejoin` semantics).
- **VM (bytecode → values)** — a dispatch loop over the bytecode. Reuses the
  existing `LuaValue`/`Heap`/`numops` layers unchanged — only the evaluation
  strategy changes, not the value model or GC.
- **Perf benchmark** — the deferred "parse + run 1 MB" benchmark lands here,
  comparing against reference Lua on the official suite and real-world
  programs (e.g. a JSON parser, a regex-on-patterns workload). Target:
  within ~3× of reference Lua on compute-heavy code (the tree-walker is
  ~50–100× slower today, as expected for an AST interpreter).

### Cross-cutting deferrals (not milestone-bound)

- **Parser error recovery** (peglib's `recover_set`/`recover_eol`/
  `set_recovery`) — currently the parser stops at the first syntax error.
  Most valuable once a REPL/LSP surfaces multiple errors per file; no natural
  milestone home, pull in when the use case appears.
- **Performance benchmark of the parser itself** — the current corpus's
  largest file is ~42 KB; pure parser throughput has no M2 impact. Becomes
  meaningful as part of the M5 perf suite against reference Lua.

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
