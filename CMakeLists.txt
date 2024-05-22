cmake_minimum_required(VERSION 3.1)
project(yueshi CXX)
set(CMAKE_CXX_STANDARD 20)

add_executable(yueshi)
target_include_directories(yueshi PUBLIC include/peglib/include)
target_include_directories(yueshi PUBLIC include)
target_sources(yueshi PRIVATE src/yueshi.cpp src/ysState.cpp src/lua/lex.cpp)

add_executable(yueshic)
target_include_directories(yueshic PUBLIC include/peglib/include PUBLIC include)
target_sources(yueshic PRIVATE src/yueshic.cpp src/ysState.cpp src/lua/lex.cpp)

enable_testing()
add_subdirectory(test)