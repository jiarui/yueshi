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
