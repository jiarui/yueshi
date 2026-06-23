# doctest is bundled inside the peglib submodule (third_party/doctest.h).
set(doctest_include_dir ${CMAKE_SOURCE_DIR}/include/peglib/third_party)

# Shared doctest main() — defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN once.
add_library(yueshi_test_main OBJECT doctest_main.cpp)
target_include_directories(yueshi_test_main PRIVATE ${doctest_include_dir})

add_executable(yueshi_lua_lex_test
    test.cpp
    ../src/lua/lex.cpp)
target_link_libraries(yueshi_lua_lex_test PRIVATE peglib yueshi_test_main)
target_include_directories(yueshi_lua_lex_test PRIVATE
    ${doctest_include_dir}
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME yueshi_lua_lex_test COMMAND yueshi_lua_lex_test)
