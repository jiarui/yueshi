#include "ysState.h"
#include "lua/lex.h"
#include <fstream>
#include <iterator>
ys::ysState::ysState() {}

int ys::ysState::loadFile(const char *file_name)
{
    std::ifstream input_steam(file_name);
    std::string str((std::istreambuf_iterator<char>(input_steam)), std::istreambuf_iterator<char>());
    ys::lua::Tokenizer t(str);
    t.next();
    return 0;
}

void ys::ysState::close()
{
    return;
}