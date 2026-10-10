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
                     "Conquest: ff-campaign conquest <data-root> <scenario-name> <max-days> <log.jsonl> [seed] [end-game] (stops once a team holds every starting objective of an enemy; with end-game also at the trigger script's #END_GAME)\n"
                     "Checkpoint: ff-campaign checkpoint <data-root> <scenario-name> <new-output-directory> [seed] (advances one minute)\n"
                     "Checkpoint after: ff-campaign checkpoint-after <data-root> <scenario-name> <minutes> <new-output-directory> [seed]\n"
                     "Resume: ff-campaign resume <data-root> <checkpoint-directory> <minutes> [seed]\n"
                     "Battle (experimental): ff-campaign battle <data-root> <checkpoint-directory> <x|auto> <y> <radius-km> <seconds> <frozen|live> <new-output-directory> [seed]\n"
                     "Watch: ff-campaign watch <data-root> <scenario-name> <session-directory> [seed]\n"
                     "Watch resume: ff-campaign watch-resume <data-root> <checkpoint-directory> <session-directory> [seed]\n"
                     "Remap theater: ff-campaign remap-theater <data-root> <scenario-name> <remap-file> <new-output-directory> (relocate onto the data root's grid, save checkpoint)\n"
                     "Rebuild objectives: ff-campaign rebuild-objectives <data-root> <scenario-name> <objectives-file> <new-output-directory> (open-data objective set, save checkpoint)\n"
                     "Reduce scenario: ff-campaign reduce-scenario <data-root> <scenario-name> <plan-file> <new-output-directory> (small comparison war: <name>.cam and <name>.tri)\n"
                     "Export map: ff-campaign export-map <data-root> <scenario-name> <output-directory> (loaded state, no campaign time)\n"
                     "Export checkpoint: ff-campaign export-checkpoint <data-root> <checkpoint-dir> <output-directory>\n"
                     "Detail: ff-campaign detail-test <data-root> <scenario-name> <hit|miss|unarmed|air|ground|helo|sea|feature|region> [seed]\n";
        return 0;
    }
    if (args.size() >= 2 && (args[1] == "run" || args[1] == "conquest" || args[1] == "checkpoint-after" || args[1] == "battle" || args[1] == "watch" || args[1] == "export-map" || args[1] == "export-checkpoint" || args[1] == "detail-test" || args[1] == "checkpoint" || args[1] == "resume" || args[1] == "watch-resume" || args[1] == "damage-checkpoint-test" || args[1] == "fault-checkpoint-test" || args[1] == "remap-theater" || args[1] == "rebuild-objectives" || args[1] == "reduce-scenario")) {
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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
// Unhandled native faults (access violations in the legacy engine) print a symbolized
// stack to stderr before the process dies, so long unattended runs leave evidence.
static LONG WINAPI ReportCrash(EXCEPTION_POINTERS* info) {
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    const auto* record = info->ExceptionRecord;
    fprintf(stderr, "{\"status\":\"crash\",\"code\":\"%08lx\",\"address\":\"%p\",\"access\":%llu,\"target\":\"%llx\"}\n",
            record->ExceptionCode, record->ExceptionAddress,
            record->NumberParameters > 0 ? static_cast<unsigned long long>(record->ExceptionInformation[0]) : 0ull,
            record->NumberParameters > 1 ? static_cast<unsigned long long>(record->ExceptionInformation[1]) : 0ull);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Rip, 0, AddrModeFlat};
    frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
    frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
        alignas(SYMBOL_INFO) char bytes[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(bytes);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        if (!SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol)) strcpy(symbol->Name, "?");
        IMAGEHLP_LINE64 line{sizeof(line)}; DWORD displacement = 0;
        const bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &displacement, &line);
        fprintf(stderr, "  %llx %s+%llu %s:%lu\n", frame.AddrPC.Offset, symbol->Name, offset,
                hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0ul);
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(ReportCrash);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(std::filesystem::path(argv[i]).u8string());
    return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv, argv + argc)); }
#endif
