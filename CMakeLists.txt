cmake_minimum_required(VERSION 3.22)
project(yueshi CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# peglib submodule (header-only INTERFACE target with C++20 propagation)
set(PEGLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PEGLIB_BUILD_LUA_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(include/peglib)

option(YUESHI_ENABLE_SANITIZERS "Enable ASan + UBSan (GCC/Clang Debug builds)" OFF)

add_executable(yueshi)
target_link_libraries(yueshi PRIVATE peglib)
target_include_directories(yueshi PUBLIC include)
target_sources(yueshi PRIVATE
    src/yueshi.cpp
    src/lua/state.cpp
    src/lua/evaluator.cpp
    src/lua/strlib.cpp
    src/lua/pattern.cpp
    src/lua/numops.cpp
    src/lua/heap.cpp
    src/lua/parser.cpp
    src/lua/lex.cpp)

add_executable(yueshic)
target_link_libraries(yueshic PRIVATE peglib)
target_include_directories(yueshic PUBLIC include)
target_sources(yueshic PRIVATE
    src/yueshic.cpp
    src/lua/state.cpp
    src/lua/evaluator.cpp
    src/lua/strlib.cpp
    src/lua/pattern.cpp
    src/lua/numops.cpp
    src/lua/heap.cpp
    src/lua/parser.cpp
    src/lua/lex.cpp)

# Apply sanitizer flags to every target that links yueshi's translation units.
# MSVC's ASan story is separate and not wired here; this option targets GCC/Clang
# (the sanitizer CI matrix uses GCC 15).
function(yueshi_enable_sanitizers target)
    if(YUESHI_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

yueshi_enable_sanitizers(yueshi)
yueshi_enable_sanitizers(yueshic)

option(YUESHI_BUILD_TESTS "Build unit tests (doctest, bundled in peglib submodule)" ON)
enable_testing()
if(YUESHI_BUILD_TESTS)
    add_subdirectory(test)
endif()
