# yueshi

A Lua 5.4 interpreter built on
[peglib](https://github.com/jiarui/peglib), a C++20 header-only PEG library
with packrat memoization, left-recursion support, and a cut operator.

## Status

The **lexer**, **parser**, and **evaluator** are implemented. The lexer covers
full Lua 5.4 lexical syntax (numerals incl. hex floats, escape decoding,
long-bracket strings/comments, all operators and keywords) with peglib-backed
error diagnostics. The **parser** produces a typed 38-node AST (14-level
precedence ladder, suffix-loop `prefixexp`, S-expression printer) and passes
**56/56** structural unit tests (1 534 assertions) plus the **official Lua
5.4.8 test suite as a corpus** (33/33 clean parses, with a known-failure
allowlist as a regression guard). The **evaluator** is a GC-first
tree-walking interpreter: a tagged-union value model with an intrusive
mark-sweep collector (no `shared_ptr` — ownership is singular, in the Heap),
Lua 5.4 integer/float subtype arithmetic, closures, tables, multires, the
full **metatable/metamethod** system (all arithmetic/bitwise/concat/ordering/
equality events, `__index`/`__newindex` chains, `__call`, `__len`,
`__tostring`, `setmetatable`/`getmetatable`), and **goto/labels**
(`goto name` / `::name::`, backward and forward, in/out of any block or
loop, with a per-block memoized label cache), plus a minimal standard library
(`print`/`type`/`tostring`/`tonumber`/`error`/`assert`/`ipairs`/`pairs`/
`next`/`select`/`rawget`/`rawset`/`rawequal`/`rawlen`). It passes **107/107**
evaluator unit tests (6 386 assertions) and a **12/12** GC unit suite (42
assertions) that verifies unreachable cycles (and tables with metatables) are
collected while escaping closures keep their captured environment alive. All
gates are green under ASan + UBSan with leak detection. The full standard
library (`string`/`table`/`math`/`io`/`os`, real `_G`/`_ENV`) is the next
milestone.
See [TODO.md](TODO.md) for the full roadmap.

## Architecture

yueshi uses a **double-pass + interpret** architecture:

1. **Lexer** (char → token): Tokenizes Lua source into `Token` objects with
   `TokenID`, `TokenValue` (variant of int/float/string), and source position.
   Built on peglib's PEG combinator DSL.
2. **Parser** (token → AST): Parses the token stream into a typed AST using
   explicit 14-level operator precedence (not PEG left-recursion, which is
   precedence-unaware).
3. **Evaluator** (AST → values): A tree-walking interpreter over the typed
   AST. Runtime values are a tagged union (`LuaValue`); collectable objects
   (`String`/`Table`/`Closure`/`Builtin`/`Environment`) derive an intrusive
   `GCObject` header and are owned exclusively by the `Heap`, which reclaims
   them via stop-the-world mark-sweep from a root set (the live environment
   chain). No `shared_ptr`: aliasing is non-owning pointers, and cycles are
   collected by reachability — the whole point of designing GC in at the
   floor rather than bolting it on. Tables (and closures) carry a per-value
   `metatable` pointer traced by the collector; the full metamethod event set
   (arithmetic, bitwise, concat, ordering, equality, `__index`/`__newindex`,
   `__call`, `__len`, `__tostring`) routes through it.

## Project Layout

```
include/
  peglib/          peglib submodule (parsing engine)
  lua/             Lua frontend
    lex.h          Token types, Tokenizer template
    lex_conv.h     PEG grammar rules for Lua lexing
    parser.h       Parser skeleton
    parser_conv.h  Token-level PEG grammar + semantic actions
    ast.h          Typed 38-node std::variant AST
    ast_print.h    S-expression AST printer
    value.h        Runtime value model (LuaValue, GCObject, Table, ...)
    heap.h         Heap: owner of GC objects + mark-sweep collector
    numops.h       Number-aware arithmetic (Lua 5.4 int/float subtypes)
    evaluator.h    Tree-walking evaluator
    state.h        Interpreter State (owns Heap + drives lex→parse→eval)
src/
  lua/
    lex.cpp        Token string maps, operator<<
    parser.cpp     Parser implementation
    heap.cpp       Heap + GC tracer + key normalization
    numops.cpp     Arithmetic primitives
    evaluator.cpp  Evaluator + builtins
    state.cpp      State driver (run_string / run_file)
  yueshi.cpp       Interpreter CLI (yueshi <file.lua>)
  yueshic.cpp      Compiler CLI (stub — bytecode VM is M5)
test/
  test.cpp              Unit tests (doctest)
  lex_correctness.cpp   Lexer correctness / regression suite
  parser_test.cpp       Parser structural unit tests (precedence, statements)
  parser_structure.cpp  Structural navigation tests over the typed AST
  ast_check.h           Header-only typed AST accessors (used by the above)
  parser_corpus.cpp     Official-suite parser corpus acceptance
  gc_test.cpp           Heap/GC unit tests (mark-sweep, cycles, escaping envs)
  evaluator_test.cpp    Evaluator unit tests (values, operators, control flow,
                       closures, tables, multires, builtins)
  corpus/lua-5.4.8-tests/  Lua 5.4.8 official test suite (checked in)
```

## Build

```sh
git clone --recursive <repo-url> yueshi
cd yueshi
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a Lua file through the interpreter:

```sh
./build/yueshi path/to/script.lua
```

To run the lexer under ASan + UBSan (the correctness gate used by CI):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DYUESHI_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CI covers GCC 15 (Release + ASan/UBSan Debug), Clang 22 (Release), and
MSVC v145 (Release, windows-2025).

## Dependencies

- **peglib** (submodule) — C++20 PEG library
- **doctest** (bundled in peglib's `third_party/`) — unit testing
- C++20 compiler (GCC 15 / Clang 22 / MSVC v145)

## License

[Apache License 2.0](LICENSE).
