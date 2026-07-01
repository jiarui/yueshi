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
  label-hidden-in-nested-block) is deferred to M3.5 (the `load()` sub-
  milestone) — those checks are only testable via `load()` of strings (the
  official `goto.lua` drives them that way). v1 behavior on a static-rule violation is benign: a
  forward goto over a `local` leaves the binding untouched (nil first, prior
  value after), matching Lua's runtime semantics. Tests: evaluator 107/107
  (6 386 assertions) — green under ASan + UBSan.
- **M2.3 pcall / xpcall / error objects** — `LuaError` gains an optional
  `LuaValue m_obj` (the raw error value). `error(msg, level)` stores the
  original value as the object; `pcall` catches `LuaError` and returns
  `false, obj` (or `false, what()` for runtime-generated errors). `xpcall`
  invokes a handler before returning `false, handler-result`. `call_value`
  made public so builtins can drive function calls. Position prepending
  (level >= 1 on string messages) is deferred to M4 (needs a call stack with
  source positions). Tests: evaluator 122/122 (6 810 assertions) — green
  under ASan + UBSan.
- **M3.1 String library + pattern engine + interning + per-type metatable**
  — five parts: (A) short-string interning (Heap-owned table, pruned at sweep);
  (B) per-type string metatable (`m_string_mt` with `__index`=string lib, so
  `("hi"):upper()` works); (C) basic functions (len/sub/upper/lower/rep/
  reverse/byte/char); (D) string.format (standard conversions + custom `%a`
  hex-float + `%q` Lua quoting + `%p` pointer identity); (E) hand-rolled
  pattern engine (~600 lines, full feature set: classes/sets/anchors/
  quantifiers/captures/position-captures/back-refs/%b balanced/%f frontier/
  gsub with 3 replacement kinds/5.3.3 empty-match rule/exact error strings).
  `pack`/`unpack`/`packsize` stubbed. Tests: evaluator 134/134 (7 412),
  gc 15/15 (53), pattern 20/20 (107) — green under ASan + UBSan.
- **M3.2-3 `table` + `math` libraries** (combined milestone):
  - **table** — `insert` (positional + append, with the exact `position out
    of bounds` / `number has no integer representation` errors), `remove`
    (default-last + positional, shifts down), `concat` (with sep + i/j range,
    exact `invalid value (at index N) in table for 'concat'` error),
    `pack`/`unpack` (multires round-trip incl. `t.n`), `move` (overlapping-
    range forward/backward copy, returns dest), `sort` (recursive quicksort
    with median-of-3 pivot, insertion-sort cutoff at 16, tail-call
    elimination on the larger partition for O(log n) stack depth; comparator
    dispatches through the public `Evaluator::call_value` so a Lua comparator's
    metamethods are honored; totalness check raises `invalid order function
    for sorting` when `comp(a,b)` and `comp(b,a)` are both true for an
    adjacent pair).
  - **math** — constants (`pi`/`huge`/`maxinteger`/`mininteger`); subtype-
    preserving `abs`/`ceil`/`floor`/`min`/`max` (the chosen value's subtype
    is preserved, so `math.min(5.0, 3, 8)` returns integer `3`, while
    `math.min(0.5, 3, 8)` returns float `0.5`); all `<cmath>` wrappers
    (`exp`/`log`/`sqrt`/`sin`/`cos`/`tan`/`fmod`/`modf`/`pow`); `tointeger`
    (nil for non-integral floats / non-numbers); `math.type` ("integer"/
    "float"/nil); `random`/`randomseed` backed by a per-Evaluator
    xorshift64* PRNG (lazy-created, default seed = 1 for reproducibility —
    matches reference Lua's seed-reproducibility property, though NOT byte-
    for-byte the same sequence).
  - Tests: evaluator 134→159 (+25 cases: 13 table + 12 math, +3 298
    assertions). All green under ASan + UBSan with leak detection.

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
      label '<name>' for <goto>"`. Static scope analysis deferred to M3.5
      (the `load()` sub-milestone — those checks are only testable via
      `load()` of strings). Tests: evaluator 107/107 (6 386 assertions),
      green under ASan + UBSan.
- [x] **M2.3 pcall / xpcall / error objects** — `LuaError` gains an optional
      `LuaValue m_obj` (the raw error value); `error(msg, level)` stores the
      original value as the object; `pcall` catches `LuaError` and returns
      `false, obj` (or `false, what()` for runtime errors); `xpcall` invokes
      a handler before returning `false, handler-result`. `call_value` made
      public so builtins can drive function calls. Position prepending
      (level >= 1 on string messages) deferred to M4 (needs a call stack
      with source positions). Tests: evaluator 122/122 (6 810 assertions),
      green under ASan + UBSan.
- [ ] Error-recovery study on real-world Lua sources — peglib's recover API
      (`recover_set` / `recover_eol` / `set_recovery`) exists but no parser
      rule uses it yet; the parser stops at the first syntax error. See
      *Cross-cutting deferrals* below.
- [ ] Performance benchmark: parse + run a large Lua file — deferred to M5.
      See *Cross-cutting deferrals* below.

## Future Milestones (not in current scope)

The runtime core (M2.0–M2.3) is complete: a GC-first tree-walking evaluator
with the full metatable event set, goto/labels, and pcall/xpcall error
handling. The remaining work is the
standard library, advanced runtime features, and a performance tier. The
milestones below are ordered by dependency; each lists scope, what it
unblocks in the official `lua-5.4.8-tests` corpus, and explicit deferrals.

### M2.3 — pcall / xpcall / error objects

A small, high-leverage milestone done before M3 so the string-library corpus
files (`pm.lua`, `strings.lua`) can run end-to-end: both use a `checkerror`
helper built on `pcall` + `string.find` on the message (~12 + ~25
assertions). Without `pcall`, M3.1's work isn't observable against those
files. Mechanically cheap: `error()` already throws a C++ `LuaError` to the
top of `run()`; `pcall` just installs a catch at a specific frame.

- **`pcall(f, ...)`** — calls `f`; on success returns `true, ...results`; on
  error returns `false, errmsg`. Implemented as try/catch around
  `call_value`.
- **`xpcall(f, handler, ...)`** — same, but invokes `handler(err)` before
  returning `false, handler_result`.
- **`error(msg, level)` generalized** — `msg` may be **any LuaValue** (the
  error object), not just a string. If `msg` is a string and `level > 0`,
  prepend `source:line:` per Lua §2.3; `level == 0` suppresses position
  prepending. `LuaError` gains an optional `LuaValue m_obj` (the raw error
  value); `what()` renders it for the message-string case. This is what
  `pcall` returns as the second value.
- The throw-to-top-of-`run()` behavior is preserved (an uncaught error still
  aborts the chunk); `pcall` installs a catch at a specific frame. The
  `m_depth` counter is already correctly managed (decremented before
  throws), so pcall doesn't change GC/depth semantics.

**M2.3 out of scope (→ M4):** error-object metatables (a thrown table/closure
rendering via `__tostring`), and `xpcall`'s handler integrating with the
`debug.traceback` machinery once that lands.

### M3 — Standard library subset

The largest remaining milestone; delivers enough of the stdlib that real-world
Lua runs. Naturally splits into sub-milestones by library. Each sub-milestone
is independently shippable and adds its own test corpus file(s).

- **M3.1 `string` library + pattern engine + interning + per-type metatable**
  — the highest-value piece (most Lua code uses it) and the largest single
  milestone. Five coupled parts; do them in the order listed.
  - **(A) String interning** (do first — the other parts depend on it). Short
    strings (≤ 40 bytes) are deduplicated via a Heap-owned intern table.
    Every allocation already funnels through `Heap::make_string`, so the
    change is centralized. GC interaction (decided): the intern table is a
    root, and at sweep time entries whose `String` survived only via the
    table are pruned — collect, not pin, so long-running programs don't leak
    distinct short strings. Unlocks `%p` identity, `("ab") is ("ab")`
    short-string equality, and bounds pattern-engine equality cost. (See the
    "String interning" row in Open Design Decisions.)
  - **(B) Per-type string metatable** (deferred from M2.1). A single
    `m_string_mt` on the Evaluator; `metatable_of()` returns it for string
    operands. The lib installs `__index = string_lib_table`, so
    `("hi"):upper()` routes through the existing M2.1 `__index` machinery —
    no new mechanism.
  - **(C) Basic functions** — `len`/`sub`/`upper`/`lower`/`rep`/`reverse`/
    `byte`/`char`. Genuinely thin wrappers over `<string>` + the allocator.
    `sub` uses 1-based indexing with negative-from-end + clamping; `rep`
    takes the 5.4 separator (4th arg); `byte`/`char` are byte-oriented
    (UTF-8 is the `utf8` lib, M4).
  - **(D) `string.format` (full)** — standard conversions (`%d %i %u %o %x %X
    %c %f %e %E %g %G %s` with all flags/width/precision) over `snprintf`,
    but **`%a`/`%A` (hex float), `%q` (Lua's quoting), and `%p` (pointer)
    are non-trivial**: `%a` needs a custom impl (portability/precision),
    `%q` has many edge cases (binary/unicode/NaN/number rendering), `%p`
    prints `static_cast<const void*>(string_ptr)` (correct once interning
    dedups). Error cases (`%F`, `%t`, oversized width, `%10q` "cannot have
    modifiers", missing value) match reference Lua's exact messages.
  - **(E) Pattern engine** (the meaty part, ~500–700 lines): `find`/`match`/
    `gmatch`/`gsub`. **Hand-rolled recursive matcher** (Lua patterns are NOT
    PEG — captures, back-refs `%1` inside the pattern, `%b` balanced, `%f`
    frontier don't map to PEG combinators; peglib is the wrong tool). Full
    feature set in one pass: all `%`-classes + complements, `[set]` ranges
    and complements (incl. binary bytes), anchors, quantifiers (`* + - ?`),
    captures (incl. position `()`), back-refs in pattern, `%bXY` balanced
    (incl. non-printable delimiters `%b\0\1`), `%f[set]` frontier (incl.
    ranges/complements), `gsub` with all 3 replacement kinds (string with
    `%0`–`%9`, function, table incl. metatable `__index` + `false`=delete),
    `gmatch` with init arg + the no-empty-past-end rule, the 5.3.3 empty-
    match-advance semantics, and exact malformed-pattern error strings
    (`"unfinished capture"`, `"malformed pattern (missing ']')"`, `"missing
    '[' after '%f' in pattern"`, etc.).
  - Unblocks corpus: `pm.lua`, `strings.lua`.
- **M3.2.5 `string.pack` / `unpack` / `packsize`** (next) — the isolated
  ~500-LOC mini-milestone. Currently stubbed in strlib.cpp; tpack.lua's ~150
  self-contained assertions all gate on it. Endian/align/overflow and the
  exact error strings (`"out of limits"`, `"invalid format option 'r'"`,
  `"16-byte integer"`, etc.) are the bulk of the work. No other lib needs
  it; can land cleanly between M3.2-3 and the higher-risk M3.5 refactor.
  Unblocks corpus: `tpack.lua`.
- **M3.4 `io` + `os` libraries** — needs a **file-handle / userdata** concept
  (a new `LuaValue` tag, or a `Table` with a hidden native pointer slot).
  - `io`: `write`/`read`/`open`/`close`/`lines`, plus `io.stdin`/`io.stdout`/
    `io.stderr` handles. File handles get a `__gc`-driven close (but `<close>`
    semantics itself is M4 — M3 just leaks-closes at process exit or via the
    existing `__gc` hook once M4 lands).
  - `os`: `time`/`clock`/`date`/`difftime`/`getenv`/`exit`/`execute`/`remove`/
    `rename`. Thin wrappers over POSIX/CRT.
  - Unblocks corpus: `files.lua`, parts of `all.lua`, the `os.clock` timing
    loop in `sort.lua`.
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
  - Unblocks corpus: `goto.lua` (runtime portion), `libs/`, `literals.lua`.
    **NOT `code.lua`** — that file is 100% gated on the testC `T` library
    (`T.listcode`/`T.listk`); see *Cross-cutting deferrals*.

**M3 out of scope (→ M4):** `coroutine`, `debug`, error-object metatables
(the bare pcall mechanics are M2.3), `<close>` semantics, weak tables,
finalizers.

### M4 — Full stdlib + advanced GC

Two intertwined themes: finish the runtime features the official suite needs,
and make the collector production-quality.

- **Error handling refinements** — error objects carry a metatable (so a
  thrown table/closure renders via `__tostring`); `xpcall`'s handler
  integrates with the `debug.traceback` machinery once that lands. (The bare
  `pcall`/`xpcall` mechanics and `error(msg, level)` are M2.3.) Unblocks
  corpus: `errors.lua`.
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
- **testC / ltests `T` library** — gates the entire `code.lua` corpus
  (`T.listcode`/`T.listk` inspect opcode listings). Not part of any stdlib
  milestone; yueshi has no bytecode to introspect until M5. Effective scope:
  `code.lua` stays a parse-only corpus file until M5 + a `T` shim is added.
  Separate track; pull in only if/when opcode-level corpus coverage becomes
  a goal.

## Open Design Decisions

| Decision | Choice | Rationale |
| ---      | ---    | ---       |
| Pass model | Double-pass (char → token → AST) → interpret | Cleaner lexer/parser separation; matches reference Lua |
| Precedence | Explicit layering (14 levels) | PEG left-recursion is precedence-unaware |
| Value model | Tagged union `LuaValue` + intrusive `GCObject` | GC tracing is one `switch`; matches reference Lua's `TValue` shape |
| Object ownership | `Heap`-owned (single owner), non-owning pointers elsewhere | `shared_ptr` would model co-ownership of aliased data; a single owner + reachability collector is unambiguous and collects cycles |
| GC | Stop-the-world mark-sweep, designed in at the floor | Cycles + escaping closures need reachability, not refcounting; bolting GC on later means redesigning the object layout |
| Numbers | Lua 5.4 int/float subtypes | Correct `/ // % ^` semantics; matches the lexer's `TK_INT`/`TK_FLT` split |
| String interning (M3.1) | Short-string dedup; intern table is a GC root + post-sweep prune | `%p` identity + short-string `is` equality + bounded pattern-engine equality cost. Collect (not pin) so long-running programs don't leak distinct short strings; the post-sweep prune is a small `sweep()` extension. |
| Test framework | doctest (bundled in peglib) | Header-only, zero external deps; already used by peglib upstream |
| peglib integration | Git submodule | Tracks upstream peglib improvements independently |
