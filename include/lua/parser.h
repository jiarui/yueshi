#pragma once
#include "lua/lex.h"
#include <vector>
namespace ys
{
    namespace lua
    {
        struct Parser {
            void run();
            void next();
        protected:
            Tokenizer m_tokenizer;
        };
        
    } // namespace lua
    
    
} // namespace ys
