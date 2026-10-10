#include "scenario.h"
#include <iostream>
#include <stdexcept>
#ifdef FF_CAMPAIGN_ENGINE
#include "detailed.h"
int RunLegacyCampaign(const std::vector<std::string>& args);
#endif

int Run(const std::vector<std::string>& args) {
    using namespace ff::headless;
    if (args.size() == 2 && args[1] == "--help") {
        std::cout << "Usage: ff-campaign inspect <scenario.cam>\n"
                     "Validates archive boundaries, metadata and compressed sections.\n"
                     "Inspect checkpoint: ff-campaign inspect-checkpoint <checkpoint-directory>\n"
                     "Damage checkpoint diagnostic: ff-campaign damage-checkpoint-test <data-root> <scenario-name> <new-output-directory> [seed]\n"
                     "Equipment fault diagnostic: ff-campaign fault-checkpoint-test <data-root> <scenario-name> <new-output-directory> [seed]\n"
                     "Run: ff-campaign run <data-root> <scenario-name> <minutes> [seed]\n"
                     "Checkpoint: ff-campaign checkpoint <data-root> <scenario-name> <new-output-directory> [seed] (advances one minute)\n"
                     "Resume: ff-campaign resume <data-root> <checkpoint-directory> <minutes> [seed]\n"
                     "Watch: ff-campaign watch <data-root> <scenario-name> <session-directory> [seed]\n"
                     "Watch resume: ff-campaign watch-resume <data-root> <checkpoint-directory> <session-directory> [seed]\n"
                     "Remap theater: ff-campaign remap-theater <data-root> <scenario-name> <remap-file> <new-output-directory> (relocate onto the data root's grid, save checkpoint)\n"
                     "Rebuild objectives: ff-campaign rebuild-objectives <data-root> <scenario-name> <objectives-file> <new-output-directory> (open-data objective set, save checkpoint)\n"
                     "Export map: ff-campaign export-map <data-root> <scenario-name> <output-directory> (loaded state, no campaign time)\n"
                     "Detail: ff-campaign detail-test <data-root> <scenario-name> <hit|miss|unarmed|air|ground|helo|sea|feature|region> [seed]\n";
        return 0;
    }
    if (args.size() >= 2 && (args[1] == "run" || args[1] == "watch" || args[1] == "export-map" || args[1] == "detail-test" || args[1] == "checkpoint" || args[1] == "resume" || args[1] == "watch-resume" || args[1] == "damage-checkpoint-test" || args[1] == "fault-checkpoint-test" || args[1] == "remap-theater" || args[1] == "rebuild-objectives")) {
#ifdef FF_CAMPAIGN_ENGINE
        return RunLegacyCampaign(args);
#else
        std::cerr << "{\"status\":\"not_implemented\",\"simulation_advanced\":false,"
                     "\"error\":\"Legacy campaign initialization and stepping are not connected\"}\n";
        return 3;
#endif
    }
    if (args.size() != 3 || (args[1] != "inspect" && args[1] != "inspect-checkpoint")) {
        std::cerr << "Usage: ff-campaign inspect <scenario.cam>\n";
        return 2;
    }
    try {
#ifdef FF_CAMPAIGN_ENGINE
        if(args[1]=="inspect-checkpoint") {
            ValidateDetailedBoundaryCheckpoint(std::filesystem::u8path(args[2]));
            std::cout<<"{\"status\":\"ok\"}\n";return 0;
        }
#endif
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
