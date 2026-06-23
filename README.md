# yueshi

A Lua 5.4 interpreter built on
[peglib](https://github.com/jiarui/peglib), a C++20 header-only PEG library
with packrat memoization, left-recursion support, and a cut operator.

## Status

Early development. The lexer is partially implemented; the parser, AST, and
evaluator are planned. See [TODO.md](TODO.md) for the full roadmap.

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
    parser.cpp     Parser implementation (skeleton)
  yueshi.cpp       Main entry point
  yueshic.cpp      Compiler CLI
test/
  test.cpp         Unit tests (Boost.Test)
  test.lua         Test Lua source
```

## Build

```sh
git clone --recursive <repo-url> yueshi
cd yueshi
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Dependencies

- **peglib** (submodule) — C++20 PEG library
- **doctest** (bundled in peglib's `third_party/`) — unit testing
- C++20 compiler (GCC 11+ / Clang 14+ / MSVC 19.30+)

## License

[Apache License 2.0](LICENSE).
