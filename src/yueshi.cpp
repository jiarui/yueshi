#include <cstdio>
#include <iostream>
#include <string>

#include "lua/state.h"

// yueshi: the interpreter CLI. Usage: yueshi <file.lua>
// Tokenizes, parses, and evaluates the file; runtime errors are printed as
// `lua: <msg>` (mirroring reference Lua).
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.lua>\n", argv[0]);
        return 1;
    }
    ys::lua::State state;
    return state.run_file(argv[1]) ? 0 : 1;
}
