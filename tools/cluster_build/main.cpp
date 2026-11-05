#include "build.hpp"
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(strata::runtime::path_utf8(std::filesystem::path(argv[i])));
    return strata::build::main(args);
}
#else
int main(int argc, char** argv) { return strata::build::main(std::vector<std::string>(argv, argv + argc)); }
#endif
