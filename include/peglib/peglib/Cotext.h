namespace peglib
{
    template <typename ContextType>
    concept Context = requires (ContextType context) {
        ContextType::State;

    };
    struct Context {

    };
    
} // namespace peglib
