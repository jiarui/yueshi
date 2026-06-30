# doctest is bundled inside the peglib submodule (third_party/doctest.h).
set(doctest_include_dir ${CMAKE_SOURCE_DIR}/include/peglib/third_party)

# Shared doctest main() — defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN once.
add_library(yueshi_test_main OBJECT doctest_main.cpp)
target_include_directories(yueshi_test_main PRIVATE ${doctest_include_dir})

# Apply sanitizer flags to test translation units too, so the ASan+UBSan CI
# matrix actually instruments the code under test (lex.cpp is compiled into
# the test binary, not linked from the yueshi target).
function(yueshi_test_enable_sanitizers target)
    if(YUESHI_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# Lexer unit tests: existing cases + the correctness/regression suite.
add_executable(yueshi_lua_lex_test
    test.cpp
    lex_correctness.cpp
    ../src/lua/lex.cpp)
target_link_libraries(yueshi_lua_lex_test PRIVATE peglib yueshi_test_main)
target_include_directories(yueshi_lua_lex_test PRIVATE
    ${doctest_include_dir}
    ${CMAKE_SOURCE_DIR}/include)
yueshi_test_enable_sanitizers(yueshi_lua_lex_test)
add_test(NAME yueshi_lua_lex_test COMMAND yueshi_lua_lex_test)

# Parser unit tests: lex → Parser::parse → ASTPrinter assertions on precedence,
# left-recursion (var/functioncall/prefixexp), statements. The grammar/AST
# code lives in headers; only parser.cpp + lex.cpp compile in here.
add_executable(yueshi_lua_parser_test
    parser_test.cpp
    parser_structure.cpp
    ../src/lua/parser.cpp
    ../src/lua/lex.cpp)
target_link_libraries(yueshi_lua_parser_test PRIVATE peglib yueshi_test_main)
target_include_directories(yueshi_lua_parser_test PRIVATE
    ${doctest_include_dir}
    ${CMAKE_SOURCE_DIR}/include)
yueshi_test_enable_sanitizers(yueshi_lua_parser_test)
add_test(NAME yueshi_lua_parser_test COMMAND yueshi_lua_parser_test)

# Parser corpus acceptance: run the parser over the official Lua 5.4.8 test
# suite and assert clean parses (with an explicit known-failure allowlist).
# The corpus is checked in under test/corpus/lua-5.4.8-tests/; the path is
# baked in via YUESHI_CORPUS_DIR so the test does not depend on CWD.
add_executable(yueshi_lua_parser_corpus
    parser_corpus.cpp
    ../src/lua/parser.cpp
    ../src/lua/lex.cpp)
target_link_libraries(yueshi_lua_parser_corpus PRIVATE peglib yueshi_test_main)
target_include_directories(yueshi_lua_parser_corpus PRIVATE
    ${doctest_include_dir}
    ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(yueshi_lua_parser_corpus PRIVATE
    YUESHI_CORPUS_DIR="${CMAKE_SOURCE_DIR}/test/corpus/lua-5.4.8-tests")
yueshi_test_enable_sanitizers(yueshi_lua_parser_corpus)
add_test(NAME yueshi_lua_parser_corpus COMMAND yueshi_lua_parser_corpus)

# Heap/GC unit tests: verify the runtime memory layer (value.h + heap.cpp) in
# isolation, before the evaluator is stacked on top. heap.cpp is the only TU
# under test (the object types are header-only).
add_executable(yueshi_lua_gc_test
    gc_test.cpp
    ../src/lua/heap.cpp)
target_link_libraries(yueshi_lua_gc_test PRIVATE peglib yueshi_test_main)
target_include_directories(yueshi_lua_gc_test PRIVATE
    ${doctest_include_dir}
    ${CMAKE_SOURCE_DIR}/include)
yueshi_test_enable_sanitizers(yueshi_lua_gc_test)
add_test(NAME yueshi_lua_gc_test COMMAND yueshi_lua_gc_test)

# Evaluator unit tests: lex -> parse -> Evaluator::run, assert on returned
# LuaValues. The evaluator + heap + parser + lex TUs compile in here.
add_executable(yueshi_lua_evaluator_test
    evaluator_test.cpp
    ../src/lua/evaluator.cpp
    ../src/lua/numops.cpp
    ../src/lua/heap.cpp
    ../src/lua/parser.cpp
    ../src/lua/lex.cpp)
target_link_libraries(yueshi_lua_evaluator_test PRIVATE peglib yueshi_test_main)
target_include_directories(yueshi_lua_evaluator_test PRIVATE
    ${doctest_include_dir}
    ${CMAKE_SOURCE_DIR}/include)
yueshi_test_enable_sanitizers(yueshi_lua_evaluator_test)
add_test(NAME yueshi_lua_evaluator_test COMMAND yueshi_lua_evaluator_test)
