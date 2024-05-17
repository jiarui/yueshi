#include "Cotext.h"
namespace peglib
{
    template <typename ParserType>
    concept Parser  = requires (ParserType parser){
        parser.operator()();
        
    };
    struct Parser{
        virtual ~Parser() {};
        virtual bool operator(Context& c) const = 0;

    };
    
} // namespace peglib
