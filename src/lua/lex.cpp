#include "lua/lex.h"
using namespace peg;
using namespace ys::lua;

using namespace peg;

auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
auto linebreak = terminal('\n');
auto not_linebreak = terminal<char>([](char c){return c!='\n';});
Rule<std::string::value_type> names = terminal<char>([](char c){return std::isalpha(c) || c == '_';}) 
        >> *terminal<char>([](char c){return std::isalnum(c) || c=='_';});
auto digit = terminal('0', '9');
auto xdigit = terminal<char>([](char c){return std::isxdigit(c);});
auto fractional = -(terminal('+') | '-') >> ((*digit >> terminal('.') >> +digit) | (+digit >> terminal('.') >> *digit));
auto decimal = -(terminal('+') | '-') >> +digit;

auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -(terminal('.') >> +xdigit) >> 
            -((terminal('p') | 'P') >> decimal);
Rule<std::string::value_type> numeral = hexdecimal | ((fractional | decimal) >> -((terminal('e') | 'E') >> -(decimal)));

Rule<std::string::value_type> comment = terminal('-') >> '-' >> *not_linebreak >> linebreak;

Token Tokenizer::next() {
    return {};
}




