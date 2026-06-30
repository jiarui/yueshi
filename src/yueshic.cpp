#include <cstdio>

// yueshic: the compiler CLI. The bytecode VM (register-based compiler + VM) is
// milestone M5; until then yueshic is a stub. It links the runtime sources so
// the build stays consistent, but performs no compilation yet.
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.lua>\n", argv[0]);
        return 1;
    }
    std::fprintf(stderr, "yueshic: bytecode compilation not yet implemented (M5)\n");
    return 1;
}
