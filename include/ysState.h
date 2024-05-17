namespace ys {
    struct ysObject {

    };


    struct ysState : public ysObject{
    public:
        ysState();
        int loadFile(const char* file_name);
        void close();
    };
}