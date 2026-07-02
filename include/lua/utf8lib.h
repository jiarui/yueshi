#pragma once

// yueshi utf8 library (M3.6): UTF-8 codec + the utf8.* functions.
// charpattern/len/offset/codepoint/char/codes. Self-contained, no M4 deps.

namespace ys
{
    namespace lua
    {
        class Evaluator;

        void install_utf8_lib(Evaluator& ev);
    }
}
