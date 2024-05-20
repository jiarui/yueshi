#include "lua/lex.h"
using namespace peg;
auto WS = *terminal(std::set({' ', '\f', '\t', '\v'}));
auto linebreaks = terminal('\n');
auto identifiers = terminal<char>([](char c){return std::isalpha(c);});

next(Context)
//while context.ended()

