#include "ysState.h"
#include "peglib.h"


int main(int argc, char *argv[]) {
    auto state = ys::ysState();
    state.loadFile(argv[1]);
    state.close();
    return 0;
}