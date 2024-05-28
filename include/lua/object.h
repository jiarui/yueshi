#include <cstddef>
namespace ys
{
    namespace lua
    {
        struct Object {
            Object* next;
            std::byte type;
            std::byte marked;
        };

        struct String : Object
        {
            std::byte extra;
            void* ud;
        };

        struct Table : Object {

        };
        
        
    } // namespace lua
    
    
} // namespace ys
