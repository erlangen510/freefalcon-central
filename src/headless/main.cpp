#include "scenario.h"
#include <iostream>
#include <stdexcept>

int Run(const std::vector<std::string>& args) {
    using namespace ff::headless;
    if (args.size() == 2 && args[1] == "--help") {
        std::cout << "Usage: ff-campaign inspect <scenario.cam>\n"
                     "Validates archive boundaries, metadata and compressed sections.\n"
                     "This first-stage host does not advance the campaign simulation.\n";
        return 0;
    }
    if (args.size() >= 2 && args[1] == "run") {
        std::cerr << "{\"status\":\"not_implemented\",\"simulation_advanced\":false,"
                     "\"error\":\"Legacy campaign initialization and stepping are not connected\"}\n";
        return 3;
    }
    if (args.size() != 3 || args[1] != "inspect") {
        std::cerr << "Usage: ff-campaign inspect <scenario.cam>\n";
        return 2;
    }
    try {
        std::cout << ToJson(InspectScenario(std::filesystem::u8path(args[2]))) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"status\":\"error\",\"simulation_advanced\":false,\"error\":"
                  << JsonString(error.what()) << "}\n";
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(std::filesystem::path(argv[i]).u8string());
    return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv, argv + argc)); }
#endif
