# yueshi

A Lua 5.4 interpreter built on
[peglib](https://github.com/jiarui/peglib), a C++20 header-only PEG library
with packrat memoization, left-recursion support, and a cut operator.

## Status

The **lexer** and **parser** are feature-complete. The lexer covers full
Lua 5.4 lexical syntax (numerals incl. hex floats, escape decoding,
long-bracket strings/comments, all operators and keywords) with peglib-backed
error diagnostics. The **parser** produces a typed 38-node AST (14-level
precedence ladder, suffix-loop `prefixexp`, S-expression printer) and passes
**56/56** structural unit tests (1 534 assertions) plus the **official Lua
5.4.8 test suite as a corpus** (33/33 clean parses, with a known-failure
allowlist as a regression guard). Both gates are green under ASan + UBSan
with leak detection. The **evaluator** (tree-walking interpreter) is the next
milestone. See [TODO.md](TODO.md) for the full roadmap.

## Architecture

yueshi uses a **double-pass** architecture:

1. **Lexer** (char → token): Tokenizes Lua source into `Token` objects with
   `TokenID`, `TokenValue` (variant of int/float/string), and source position.
   Built on peglib's PEG combinator DSL.
2. **Parser** (token → AST): Parses the token stream into a typed AST using
   explicit 14-level operator precedence (not PEG left-recursion, which is
   precedence-unaware).

## Project Layout

```
include/
  peglib/          peglib submodule (parsing engine)
  lua/             Lua frontend
    lex.h          Token types, Tokenizer template
    lex_conv.h     PEG grammar rules for Lua lexing
    parser.h       Parser skeleton
    object.h       Lua object model
    state.h        Interpreter state
  yueshi.h         Public API
  ysState.h        State management
src/
  lua/
    lex.cpp        Token string maps, operator<<
    parser.cpp     Parser implementation
  yueshi.cpp       Main entry point
  yueshic.cpp      Compiler CLI
test/
  test.cpp              Unit tests (doctest)
  lex_correctness.cpp   Lexer correctness / regression suite
  parser_test.cpp       Parser structural unit tests (precedence, statements)
  parser_structure.cpp  Structural navigation tests over the typed AST
  ast_check.h           Header-only typed AST accessors (used by the above)
  parser_corpus.cpp     Official-suite parser corpus acceptance
  corpus/lua-5.4.8-tests/  Lua 5.4.8 official test suite (checked in)
  test.lua              Test Lua source
```

## Build

```sh
git clone --recursive <repo-url> yueshi
cd yueshi
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
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
