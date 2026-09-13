#include "stdhdr.h"
#include "f4vu.h"
#include "f4find.h"
#include "entity.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "campstr.h"
#include "team.h"
#include "unit.h"
#include "aiinput.h"
#include "tactics.h"
#include "weather.h"
#include "tmap.h"
#include "tacan.h"
#include "falcmesg.h"
#include "falcsess.h"
#include "asearch.h"
#include "boundary.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include "scenario.h"
#include <map>
#include <set>
#include <fstream>

struct UnitState { short x, y; int vehicles, supply, orders; bool flight, local; };
using UnitStates = std::map<unsigned long long, UnitState>;
static UnitStates captureUnits() {
    UnitStates result;
    VuListIterator it(AllUnitList);
    for (Unit u = static_cast<Unit>(it.GetFirst()); u; u = static_cast<Unit>(it.GetNext())) {
        if (!u->IsAggregate()) throw std::runtime_error("Detailed entity entered the aggregate campaign");
        UnitState state{}; u->GetLocation(&state.x, &state.y);
        state.vehicles = u->GetTotalVehicles(); state.supply = u->GetUnitSupply(); state.orders = u->GetUnitOrders();
        state.flight = u->IsFlight(); state.local = u->IsLocal();
        result.emplace((static_cast<unsigned long long>(u->Id().creator_) << 32) | u->Id().num_, state);
    }
    return result;
}

extern void DoCampaignLoop(int);
extern void UpdateRealUnits(CampaignTime);
extern void ReadAllRadarData();
extern void ReadAllMissileData();
extern void SetTime(unsigned long);
extern void InitBaseLists();
extern RealWeather* realWeather;

static void setPath(char* target, const std::filesystem::path& path) {
    const auto wide = path.wstring();
    BOOL substituted = FALSE;
    int size = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1, target, MAX_PATH, nullptr, &substituted);
    if (!size || substituted) throw std::runtime_error("Legacy data path cannot be represented in Windows ANSI encoding");
    if (strchr(target, '%')) throw std::runtime_error("Legacy engine paths cannot contain percent characters");
}
static void stage(const char* text) { std::cerr << "[headless] " << text << std::endl; }

static void requireFile(const std::filesystem::path& file) {
    if (!std::filesystem::is_regular_file(file) || !std::filesystem::file_size(file))
        throw std::runtime_error("Missing required data file: " + file.u8string());
}
static void validateData(const std::filesystem::path& data) {
    for (const char* ext : {"ct", "ini", "ucd", "fed", "ocd", "wcd", "fcd", "vcd", "wld", "phd", "pd", "rcd", "icd", "rwd", "vsd", "swd", "acd", "ssd", "rkt", "ddp"})
        requireFile(data / "terrdata/objects" / (std::string("Falcon4.") + ext));
    for (const char* file : {"Falcon4.AII", "Falcon4.TT", "Strings.idx", "Strings.wch", "KOREA.THR"})
        requireFile(data / "campaign/SAVE" / file);
    requireFile(data / "terrdata/korea/terrain/Theater.map");
    requireFile(data / "terrdata/korea/terrain/Theater.MEA");
    for (const char* list : {"sim/MISDATA/mistypes.lst", "sim/RADAR/radtypes.lst"}) {
        const auto path = data / list;
        requireFile(path);
        std::ifstream input(path);
        int count = 0; input >> count;
        if (!input || count <= 0 || count > 10000) throw std::runtime_error("Invalid dataset list: " + path.u8string());
        for (int i = 0; i < count; ++i) {
            std::string name;
            if (!(input >> name)) {
                // Both upstream FF6 simdata archives ship this exact off-by-one.
                // The original reader reuses slot 168 for 169; retain that
                // compatibility rule explicitly in model_support.cpp.
                if (std::string(list) == "sim/RADAR/radtypes.lst" && count == 170 && i == 169) {
                    stage("FF6 radar list: final slot reuses the preceding entry (legacy compatibility)");
                    break;
                }
                throw std::runtime_error("Truncated dataset list: " + path.u8string());
            }
            requireFile(path.parent_path() / (name + ".dat"));
        }
    }
}

int RunLegacyCampaign(const std::vector<std::string>& args) {
    unsigned completedSteps = 0;
    try {
        if (args.size() != 5 && args.size() != 6) { std::cerr << "{\"status\":\"error\",\"simulation_advanced\":false,\"error\":\"Usage: ff-campaign run <data-root> <scenario-name> <minutes> [seed]\"}\n"; return 2; }
        const auto data = std::filesystem::absolute(std::filesystem::u8path(args[2]));
        const auto wallStart = std::chrono::steady_clock::now();
        size_t parsed = 0;
        const int minutes = std::stoi(args[4], &parsed);
        if (parsed != args[4].size()) throw std::runtime_error("Invalid duration");
        unsigned seed = 1;
        if (args.size() == 6) {
            const auto value = std::stoull(args[5], &parsed);
            if (parsed != args[5].size() || value > UINT_MAX || args[5][0] == '-') throw std::runtime_error("Invalid seed");
            seed = static_cast<unsigned>(value);
        }
        if (minutes < 1 || minutes > 10080) throw std::runtime_error("Duration must be 1..10080 minutes");
        char scenario[64];
        if (args[3].empty() || args[3].size() >= sizeof(scenario)) throw std::runtime_error("Invalid scenario name");
        strcpy(scenario, args[3].c_str());
        if (strspn(scenario, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(scenario)) throw std::runtime_error("Use a scenario basename, such as save0");
        const auto inspected = ff::headless::InspectScenario(data / "campaign/SAVE" / (std::string(scenario) + ".cam"));
        validateData(data);
        setPath(FalconDataDirectory, data);
        setPath(FalconCampaignSaveDirectory, data / "campaign/SAVE");
        setPath(FalconCampUserSaveDirectory, data / "campaign/SAVE");
        setPath(FalconObjectDataDir, data / "terrdata/objects");
        setPath(FalconTerrainDataDir, data / "terrdata/korea");
        std::filesystem::current_path(data);
        srand(seed);
        ASD = new AS_DataClass;
        stage("class tables and campaign AI");
        ReadCampAIInputs("Falcon4");
        if (!LoadClassTable("Falcon4")) throw std::runtime_error("Class table load failed");
        stage("VU database and local session");
        InitVU();
        FalconLocalSession->SetCountry(FALCON_PLAYER_TEAM);
        campCritical = F4CreateCriticalSection("headless campaign");
        TheCampaign.Reset();
        FalconMessageFilter filter(FalconEvent::CampaignThread, 0);
        TheCampaign.vuThread = new VuThread(&filter, F4_EVENT_QUEUE_SIZE * 4);
        InitBaseLists();
        ReadIndex("Strings");
        LoadPriorityTables();
        if (!LoadTactics("Falcon4")) throw std::runtime_error("Tactics load failed");
        stage("terrain, weather and weapon datasets");
        char terrain[MAX_PATH]; setPath(terrain, data / "terrdata/korea/terrain");
        TheMap.Setup(terrain);
        realWeather = new WeatherClass;
        gTacanList = new TacanList;
        ReadAllRadarData();
        ReadAllMissileData();
        stage("load campaign entities");
        if (!TheCampaign.LoadCampaign(game_Campaign, scenario)) throw std::runtime_error("Campaign load failed");
        TheCampaign.Flags |= CAMP_RUNNING;
        for (int t = 0; t < NUM_TEAMS; ++t) {
            if (!TeamInfo[t] || !TeamInfo[t]->atm || !TeamInfo[t]->gtm || !TeamInfo[t]->ntm)
                throw std::runtime_error("Campaign requires all eight team managers");
        }
        int unitCount = 0, objectiveCount = 0;
        { VuListIterator it(AllUnitList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) ++unitCount; }
        { VuListIterator it(AllObjList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) ++objectiveCount; }
        fprintf(stderr, "loaded units=%d objectives=%d theater=%s scenario=%s time=%lu\n", unitCount, objectiveCount, TheCampaign.TheaterName, TheCampaign.Scenario, TheCampaign.CurrentTime);
        const CampaignTime start = TheCampaign.CurrentTime;
        const auto before = captureUnits();
        auto previous = before;
        std::set<unsigned long long> moved, createdFlights, strengthChanged, supplyChanged, ordersChanged;
        stage("startup campaign planning");
        DoCampaignLoop(1);
        for (int second = 0; second < minutes * 60; second += 5) {
            SetTime(TheCampaign.CurrentTime + 5 * CampaignSeconds);
            DoCampaignLoop(0);
            UpdateRealUnits(5 * CampaignSeconds);
            TheCampaign.vuThread->Update(-1);
            gMainThread->Update(-1);
            const auto current = captureUnits();
            for (const auto& [id, state] : current) {
                const auto old = previous.find(id);
                if (old == previous.end()) { if (state.flight) createdFlights.insert(id); continue; }
                if (old->second.x != state.x || old->second.y != state.y) moved.insert(id);
                if (old->second.vehicles != state.vehicles) strengthChanged.insert(id);
                if (old->second.supply != state.supply) supplyChanged.insert(id);
                if (old->second.orders != state.orders) ordersChanged.insert(id);
            }
            previous = current;
            ++completedSteps;
        }
        int local = 0, flights = 0;
        for (const auto& [id, state] : previous) { local += state.local; flights += state.flight; }
        std::cout << "{\"status\":\"ok\",\"simulation_advanced\":true,\"start_ms\":" << start
                  << ",\"end_ms\":" << TheCampaign.CurrentTime
                  << ",\"scenario\":" << ff::headless::JsonString(scenario)
                  << ",\"seed\":" << seed << ",\"format_version\":" << inspected.version
                  << ",\"completed_steps\":" << completedSteps << ",\"step_seconds\":5"
                  << ",\"all_units_aggregate\":true"
                  << ",\"objectives\":" << objectiveCount
                  << ",\"units_before\":" << before.size() << ",\"units_after\":" << previous.size()
                  << ",\"local_units\":" << local << ",\"flights_after\":" << flights
                  << ",\"moved_units\":" << moved.size() << ",\"created_flights\":" << createdFlights.size()
                  << ",\"strength_changed_units\":" << strengthChanged.size()
                  << ",\"supply_changed_units\":" << supplyChanged.size()
                  << ",\"orders_changed_units\":" << ordersChanged.size()
                  << ",\"combat_messages\":" << ff_headless::combat.engagements
                  << ",\"reported_combat_losses\":" << ff_headless::combat.losses
                  << ",\"wall_seconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count()
                  << "}" << std::endl;
        std::cout.flush(); std::cerr.flush();
        std::_Exit(0);
    } catch (const std::exception& error) {
        std::cerr << "{\"status\":\"error\",\"simulation_advanced\":" << (completedSteps ? "true" : "false")
                  << ",\"completed_steps\":" << completedSteps << ",\"error\":" << ff::headless::JsonString(error.what()) << "}" << std::endl;
        std::_Exit(1);
    }
}
