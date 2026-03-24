#include "tests.h"

#include <exception>
#include <iostream>

int main() {
    try {
        return RunCondVarTests();
    } catch (const std::exception& ex) {
        std::cerr << "Unhandled exception: " << ex.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unhandled exception: unknown error\n";
        return 1;
    }
}
