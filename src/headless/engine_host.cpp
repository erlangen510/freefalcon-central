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
#include "profile.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include "scenario.h"
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <memory>
#include "watch.h"
#include "detailed.h"
#include "commander.h"
#include "theater_remap.h"
#include "objective_rebuild.h"
#include "objective_layout.h"
#include "battle_service.h"
#include "objectiv.h"
#include <random>

extern short Map_Max_X, Map_Max_Y;
static std::mt19937 detailedRandom;
long HeadlessFastRandom() { return static_cast<long>(detailedRandom()); }

int HeadlessTraceRandom(int value, const char* file, int line) {
    static const bool trace = std::getenv("FF_RANDOM_TRACE") != nullptr;
    if(trace) {
        const char* name=strrchr(file,'/');
        if(!name) name=strrchr(file,'\\');
        fprintf(stderr,"[random] %s:%d %d\n",name?name+1:file,line,value);
    }
    return value;
}

struct UnitState { short x, y; int vehicles, supply, orders, liveVehicles; bool flight, local, dead; };
using UnitStates = std::map<unsigned long long, UnitState>;
static int liveVehicleCount(Unit unit) {
    if(unit->IsDead()) return 0;
    if(!unit->Father()) return unit->GetTotalVehicles();
    int count=0;
    for(int i=0;Unit child=unit->GetUnitElement(i);++i) count+=liveVehicleCount(child);
    return count;
}
static UnitStates captureUnits() {
    UnitStates result;
    VuListIterator it(AllUnitList);
    for (Unit u = static_cast<Unit>(it.GetFirst()); u; u = static_cast<Unit>(it.GetNext())) {
        if (!u->IsAggregate() && !DetailedRegionActive()) throw std::runtime_error("Detailed entity outside managed region");
        UnitState state{}; u->GetLocation(&state.x, &state.y);
        state.vehicles = u->GetTotalVehicles(); state.supply = u->GetUnitSupply(); state.orders = u->GetUnitOrders();
        state.flight = u->IsFlight(); state.local = u->IsLocal();
        state.dead=u->IsDead();
        state.liveVehicles=liveVehicleCount(u);
        result.emplace((static_cast<unsigned long long>(u->Id().creator_) << 32) | u->Id().num_, state);
    }
    return result;
}
static unsigned long long unitStateHash(const UnitStates& units) {
    unsigned long long hash=14695981039346656037ull;
    for(const auto& [id,s]:units) {
        for(auto value:{id,static_cast<unsigned long long>(s.x),static_cast<unsigned long long>(s.y),
            static_cast<unsigned long long>(s.vehicles),static_cast<unsigned long long>(s.supply),static_cast<unsigned long long>(s.orders)}) {
            for(int i=0;i<8;++i) {hash^=(value>>(i*8))&255;hash*=1099511628211ull;}
        }
    }
    return hash;
}
static std::string unitStateJson(const UnitStates& units) {
    std::ostringstream out;out<<'[';bool comma=false;
    for(const auto& [id,s]:units) {
        if(comma)out<<',';comma=true;
        out<<"[\""<<id<<"\","<<s.x<<','<<s.y<<','<<s.vehicles<<','<<s.supply<<','<<s.orders<<','<<s.liveVehicles<<','<<(s.dead?"true":"false")<<']';
    }
    out<<']';return out.str();
}

extern void DoCampaignLoop(int);
extern void UpdateRealUnits(CampaignTime);
extern void ReadHeadlessRadarData();
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
void SaveAggregateCheckpoint(const std::filesystem::path& directory) {
    if(DetailedRegionActive()) throw std::runtime_error("Checkpoint requires fully aggregated campaign state");
    captureUnits();
    char archivePath[MAX_PATH];setPath(archivePath,directory/"checkpoint.cam");
    if(std::filesystem::exists(directory)) throw std::runtime_error("Checkpoint output directory already exists");
    std::filesystem::create_directories(directory.parent_path());
    if(!std::filesystem::create_directory(directory)) throw std::runtime_error("Checkpoint output directory already exists");
    const std::string oldDirectory=FalconCampUserSaveDirectory,oldScenario=TheCampaign.Scenario,oldSave=TheCampaign.SaveFile;
    try {
        setPath(FalconCampUserSaveDirectory,directory);
        char name[]="checkpoint";
        if(!TheCampaign.SaveCampaign(game_Campaign,name,1)) throw std::runtime_error("Native campaign save failed");
        const auto saved=ff::headless::InspectScenario(directory/"checkpoint.cam");
        if(saved.time_ms!=TheCampaign.CurrentTime) throw std::runtime_error("Saved campaign time mismatch");
        SaveDetailedBoundaryCheckpoint(directory);
        SaveCommanderCheckpoint(directory);
        SaveObjectiveLayouts(directory/"checkpoint.lay");
    } catch(...) {
        EndWriteCampFile();
        strcpy(FalconCampUserSaveDirectory,oldDirectory.c_str());strcpy(TheCampaign.Scenario,oldScenario.c_str());strcpy(TheCampaign.SaveFile,oldSave.c_str());
        throw;
    }
    strcpy(FalconCampUserSaveDirectory,oldDirectory.c_str());strcpy(TheCampaign.Scenario,oldScenario.c_str());strcpy(TheCampaign.SaveFile,oldSave.c_str());
}

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

// conquest: JSON lines with every objective ownership change, an hourly per-team status and the
// milestones of one team holding 80% and then all of an enemy team's starting objectives.
class ConquestLog {
public:
    ConquestLog(const std::filesystem::path& file, CampaignTime start) : out(file, std::ios::binary), start(start) {
        if(!out) throw std::runtime_error("Cannot open conquest log: " + file.u8string());
        VuListIterator it(AllObjList);
        for(VuEntity* e=it.GetFirst();e;e=it.GetNext()) {
            const Control owner=static_cast<Objective>(e)->GetOwner();
            startTeam[e->Id()]=GetTeam(owner); startCountry[e->Id()]=owner; lastOwner[e->Id()]=owner;
            ++startCount[GetTeam(owner)]; ++held[GetTeam(owner)][GetTeam(owner)];
        }
        out<<"{\"type\":\"start\","<<clock(start)<<",\"teams\":[";
        bool comma=false;
        for(int t=0;t<NUM_TEAMS;++t) {
            if(!startCount[t]) continue;
            out<<(comma?",":"")<<"{\"team\":"<<t<<",\"name\":"<<ff::headless::JsonString(TeamInfo[t]->GetName())<<",\"objectives\":"<<startCount[t]<<",\"enemies\":[";
            comma=true;
            bool c2=false;
            for(int d=0;d<NUM_TEAMS;++d) if(atWar(t,d)) {out<<(c2?",":"")<<d;c2=true;}
            out<<"]}";
        }
        out<<"]}\n";out.flush();
    }
    // Records ownership changes since the last step; true once a team holds all of an enemy's objectives.
    bool step() {
        VuListIterator it(AllObjList);
        for(VuEntity* e=it.GetFirst();e;e=it.GetNext()) {
            const auto o=static_cast<Objective>(e);
            auto last=lastOwner.find(e->Id());
            if(last==lastOwner.end() || last->second==o->GetOwner()) continue;
            const int from=last->second, to=o->GetOwner(), d=startTeam[e->Id()];
            --held[GetTeam(from)][d]; ++held[GetTeam(to)][d];
            last->second=static_cast<Control>(to);
            ++captures;
            _TCHAR name[80]="";o->GetName(name,sizeof name,FALSE);
            short x,y;o->GetLocation(&x,&y);
            out<<"{\"type\":\"capture\","<<clock(TheCampaign.CurrentTime)<<",\"id\":\""<<e->Id().creator_<<':'<<e->Id().num_<<"\",\"name\":"
               <<ff::headless::JsonString(name)<<",\"objective_type\":"<<int(o->GetType())<<",\"x\":"<<x<<",\"y\":"<<y
               <<",\"from_country\":"<<from<<",\"to_country\":"<<to<<",\"from_team\":"<<int(GetTeam(from))<<",\"to_team\":"<<int(GetTeam(to))
               <<",\"start_team\":"<<d<<"}\n";
        }
        bool complete=false;
        for(int a=0;a<NUM_TEAMS;++a) for(int d=0;d<NUM_TEAMS;++d) {
            if(!atWar(a,d)) continue;
            for(int m=0;m<2;++m) {
                const bool reached=held[a][d]*(m?1:5)>=startCount[d]*(m?1:4);
                if(!reached || milestone[m][a][d]) continue;
                milestone[m][a][d]=true;
                out<<"{\"type\":\"milestone\","<<clock(TheCampaign.CurrentTime)<<",\"attacker\":"<<a<<",\"defender\":"<<d
                   <<",\"share\":"<<(m?"1.0":"0.8")<<",\"held\":"<<held[a][d]<<",\"of\":"<<startCount[d]<<"}\n";
                out.flush();
            }
            complete|=milestone[1][a][d];
        }
        return complete;
    }
    void status() {
        struct Forces {int battalions=0,groundVehicles=0,brigades=0,squadrons=0,aircraft=0,flights=0,taskForces=0,ships=0;} forces[NUM_TEAMS];
        VuListIterator it(AllUnitList);
        for(Unit u=static_cast<Unit>(it.GetFirst());u;u=static_cast<Unit>(it.GetNext())) {
            if(u->IsDead()) continue;
            auto& f=forces[u->GetTeam()];
            if(u->IsBattalion()) {++f.battalions;f.groundVehicles+=u->GetTotalVehicles();}
            else if(u->IsBrigade()) ++f.brigades;
            else if(u->IsSquadron()) {++f.squadrons;f.aircraft+=u->GetTotalVehicles();}
            else if(u->IsFlight()) ++f.flights;
            else if(u->IsTaskForce()) {++f.taskForces;f.ships+=u->GetTotalVehicles();}
        }
        // Start country -> objectives each team holds now.
        int byCountry[NUM_COUNS][NUM_TEAMS]={};
        for(const auto& [id,country]:startCountry) ++byCountry[country][GetTeam(lastOwner[id])];
        out<<"{\"type\":\"status\","<<clock(TheCampaign.CurrentTime)<<",\"captures\":"<<captures
           <<",\"combat_messages\":"<<ff_headless::combat.engagements<<",\"reported_combat_losses\":"<<ff_headless::combat.losses<<",\"teams\":[";
        bool comma=false;
        for(int t=0;t<NUM_TEAMS;++t) {
            if(!startCount[t] && !forces[t].battalions && !forces[t].squadrons) continue;
            const auto& f=forces[t];
            const auto* s=TeamInfo[t]->GetCurrentStats();
            const auto* g=TeamInfo[t]->GetGroundAction();
            _TCHAR target[80]="";
            if(const auto o=static_cast<Objective>(vuDatabase->Find(g->actionObjective))) o->GetName(target,sizeof target,FALSE);
            int owned=0;for(int d=0;d<NUM_TEAMS;++d) owned+=held[t][d];
            out<<(comma?",":"")<<"{\"team\":"<<t<<",\"objectives\":"<<owned<<",\"held_from\":[";
            comma=true;
            for(int d=0;d<NUM_TEAMS;++d) out<<(d?",":"")<<held[t][d];
            out<<"],\"battalions\":"<<f.battalions<<",\"ground_vehicles\":"<<f.groundVehicles<<",\"brigades\":"<<f.brigades
               <<",\"squadrons\":"<<f.squadrons<<",\"aircraft\":"<<f.aircraft<<",\"flights\":"<<f.flights
               <<",\"task_forces\":"<<f.taskForces<<",\"ships\":"<<f.ships
               <<",\"initiative\":"<<TeamInfo[t]->GetInitiative()<<",\"ground_action\":"<<int(g->actionType)
               <<",\"action_objective\":"<<ff::headless::JsonString(target)
               <<",\"stats\":{\"ground_vehicles\":"<<s->groundVehs<<",\"aircraft\":"<<s->aircraft<<",\"air_defense\":"<<s->airDefenseVehs
               <<",\"ships\":"<<s->ships<<",\"supply\":"<<s->supply<<",\"fuel\":"<<s->fuel<<",\"airbases\":"<<s->airbases
               <<",\"supply_level\":"<<int(s->supplyLevel)<<",\"fuel_level\":"<<int(s->fuelLevel)<<"}}";
        }
        out<<"],\"by_start_country\":{";
        comma=false;
        for(int c=0;c<NUM_COUNS;++c) {
            int total=0;for(int t=0;t<NUM_TEAMS;++t) total+=byCountry[c][t];
            if(!total) continue;
            out<<(comma?",":"")<<'"'<<c<<"\":[";comma=true;
            for(int t=0;t<NUM_TEAMS;++t) out<<(t?",":"")<<byCountry[c][t];
            out<<']';
        }
        out<<"}}\n";out.flush();
    }
    void finish(const char* reason) {
        out<<"{\"type\":\"end\","<<clock(TheCampaign.CurrentTime)<<",\"reason\":\""<<reason<<"\",\"captures\":"<<captures<<"}\n";out.flush();
    }
private:
    static bool atWar(int a,int d) {return a!=d && a!=TEAM_NEUTRAL && TeamInfo[a] && TeamInfo[d] && GetRoE(a,d,ROE_GROUND_CAPTURE)==ROE_ALLOWED;}
    std::string clock(CampaignTime t) const {
        char text[96];
        snprintf(text,sizeof text,"\"elapsed_minutes\":%lu,\"campaign_day\":%lu,\"clock\":\"%02lu:%02lu\"",
            (t-start)/CampaignMinutes,t/CampaignDay+1,t%CampaignDay/CampaignHours,t%CampaignHours/CampaignMinutes);
        return text;
    }
    std::ofstream out;
    CampaignTime start;
    std::map<VU_ID,int> startTeam;
    std::map<VU_ID,Control> startCountry,lastOwner;
    int startCount[NUM_TEAMS]={},held[NUM_TEAMS][NUM_TEAMS]={},captures=0;
    bool milestone[2][NUM_TEAMS][NUM_TEAMS]={};
};

void WriteCoreExport(const std::filesystem::path& directory);

int RunLegacyCampaign(const std::vector<std::string>& args) {
    // CampaignThread and SimLoop normally configure this per-thread. The
    // headless host replaces both loops and must preserve their numeric mode.
    _controlfp(_RC_CHOP, MCW_RC);
    unsigned completedSteps = 0;
    CampaignTime loadedTime = 0;
    bool campaignLoaded = false;
    std::unique_ptr<CampaignWatch> watch;
    try {
        // conquest <data-root> <scenario> <max-days> <log.jsonl> [seed]
        const bool conquest = args[1] == "conquest";
        // checkpoint-after <data-root> <scenario> <minutes> <new-output-directory> [seed]
        const bool checkpointAfter = args[1] == "checkpoint-after";
        // battle <data-root> <checkpoint-dir> <x|auto> <y> <radius-km> <seconds> <frozen|live> <out-dir> [seed]
        const bool battle = args[1] == "battle";
        const size_t seedIndex = battle ? 10 : (conquest || checkpointAfter) ? 6 : 5;
        if (args.size() != seedIndex && args.size() != seedIndex + 1) { std::cerr << "{\"status\":\"error\",\"simulation_advanced\":false,\"error\":\"Usage: ff-campaign run <data-root> <scenario-name> <minutes> [seed]\"}\n"; return 2; }
        const auto data = std::filesystem::absolute(std::filesystem::u8path(args[2]));
        const auto conquestLogFile = conquest ? std::filesystem::absolute(std::filesystem::u8path(args[5])) : std::filesystem::path();
        // export-checkpoint <data-root> <checkpoint-dir> <output-directory>: export-map of a saved checkpoint.
        const bool exportMap = args[1] == "export-map" || args[1] == "export-checkpoint";
        // Resolved before the host changes into the data root.
        const auto exportDirectory = exportMap ? std::filesystem::absolute(std::filesystem::u8path(args[4])) : std::filesystem::path();
        const bool interactive = args[1] == "watch" || args[1] == "watch-resume" || exportMap;
        if (interactive) watch = std::make_unique<CampaignWatch>(std::filesystem::absolute(std::filesystem::u8path(args[4])));
        const auto wallStart = std::chrono::steady_clock::now();
        size_t parsed = 0;
        const bool damageCheckpoint=args[1]=="damage-checkpoint-test" || args[1]=="fault-checkpoint-test";
        const bool diagnostic = args[1] == "detail-test" || damageCheckpoint;
        const bool checkpoint=args[1]=="checkpoint" || checkpointAfter || damageCheckpoint, resume=args[1]=="resume" || args[1]=="watch-resume" || args[1]=="export-checkpoint" || battle;
        // remap-theater <data-root> <scenario> <remap.txt> <new-output-directory>
        // rebuild-objectives <data-root> <scenario> <objectives.txt> <new-output-directory>
        const bool rebuild=args[1]=="rebuild-objectives";
        const bool remap=args[1]=="remap-theater" || rebuild;
        if(remap && args.size()!=6) throw std::runtime_error("Usage: ff-campaign " + args[1] + " <data-root> <scenario-name> <plan-file> <new-output-directory>");
        const auto checkpointDirectory=(checkpoint||resume)?std::filesystem::absolute(std::filesystem::u8path(args[checkpointAfter?5:checkpoint?4:3])):remap?std::filesystem::absolute(std::filesystem::u8path(args[5])):std::filesystem::path();
        if(checkpoint||resume) {
            char archivePath[MAX_PATH];
            setPath(archivePath,checkpointDirectory / "checkpoint.cam");
        }
        if((checkpoint||remap) && std::filesystem::exists(checkpointDirectory)) throw std::runtime_error("Checkpoint output directory must not already exist");
        const bool fixedDuration = diagnostic || (checkpoint && !checkpointAfter) || remap || battle;
        const int minutes = interactive ? 10080 : fixedDuration ? 1 : std::stoi(args[4], &parsed) * (conquest ? 1440 : 1);
        if (!interactive && !fixedDuration && parsed != args[4].size()) throw std::runtime_error("Invalid duration");
        unsigned seed = 1;
        if (args.size() == seedIndex + 1 && !remap) {
            const auto value = std::stoull(args[seedIndex], &parsed);
            if (parsed != args[seedIndex].size() || value > UINT_MAX || args[seedIndex][0] == '-') throw std::runtime_error("Invalid seed");
            seed = static_cast<unsigned>(value);
        }
        if (conquest ? minutes < 1440 || minutes > 365 * 1440 : minutes < 1 || minutes > 10080)
            throw std::runtime_error(conquest ? "Duration must be 1..365 days" : "Duration must be 1..10080 minutes");
        char scenario[64];
        const auto scenarioName=resume?std::string("checkpoint"):args[3];
        if (scenarioName.empty() || scenarioName.size() >= sizeof(scenario)) throw std::runtime_error("Invalid scenario name");
        strcpy(scenario, scenarioName.c_str());
        if (strspn(scenario, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(scenario)) throw std::runtime_error("Use a scenario basename, such as save0");
        const auto inspected = ff::headless::InspectScenario((resume?checkpointDirectory:data / "campaign/SAVE") / (std::string(scenario) + ".cam"));
        validateData(data);
        // Native history files share this data root. The handle lives until exit.
        HANDLE dataLock = CreateFileW((data / ".headless.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dataLock == INVALID_HANDLE_VALUE) throw std::runtime_error("Data root is in use by another campaign process");
        setPath(FalconDataDirectory, data);
        setPath(FalconCampaignSaveDirectory, data / "campaign/SAVE");
        setPath(FalconCampUserSaveDirectory, resume?checkpointDirectory:data / "campaign/SAVE");
        setPath(FalconObjectDataDir, data / "terrdata/objects");
        setPath(FalconTerrainDataDir, data / "terrdata/korea");
        std::filesystem::current_path(data);
        srand(seed);
        detailedRandom.seed(seed);
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
        ReadHeadlessRadarData();
        ReadAllMissileData();
        stage("load campaign entities");
        if (!TheCampaign.LoadCampaign(game_Campaign, scenario)) throw std::runtime_error("Campaign load failed");
        if(resume) { LoadDetailedBoundaryCheckpoint(checkpointDirectory); LoadCommanderCheckpoint(checkpointDirectory); }
        // Site layouts are saved next to the scenario (<scenario>.lay); none means class templates.
        const int siteLayouts = LoadObjectiveLayouts((resume?checkpointDirectory:data / "campaign/SAVE") / (std::string(scenario) + ".lay"));
        TheCampaign.Flags |= CAMP_RUNNING;
        for (int t = 0; t < NUM_TEAMS; ++t) {
            if (!TeamInfo[t] || !TeamInfo[t]->atm || !TeamInfo[t]->gtm || !TeamInfo[t]->ntm)
                throw std::runtime_error("Campaign requires all eight team managers");
        }
        int unitCount = 0, objectiveCount = 0;
        { VuListIterator it(AllUnitList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) ++unitCount; }
        { VuListIterator it(AllObjList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) ++objectiveCount; }
        fprintf(stderr, "loaded units=%d objectives=%d site_layouts=%d theater=%s scenario=%s time=%lu\n", unitCount, objectiveCount, siteLayouts,TheCampaign.TheaterName, TheCampaign.Scenario, TheCampaign.CurrentTime);
        if (rebuild) {
            // Replace the objective set from an open-data plan and save before any planning.
            // Plan key -> objective id, for the viewer; moved into the checkpoint once it exists.
            auto keys = checkpointDirectory;
            keys += ".objective-keys.tmp";
            const auto r = RebuildObjectives(std::filesystem::absolute(std::filesystem::u8path(args[4])), keys);
            SaveAggregateCheckpoint(checkpointDirectory);
            std::filesystem::rename(keys, checkpointDirectory / "objective-keys.txt");
            int objectiveTotal = 0;
            { VuListIterator it(AllObjList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) ++objectiveTotal; }
            std::cout << "{\"status\":\"ok\",\"objectives\":" << objectiveTotal << ",\"kept\":" << r.kept << ",\"modified\":" << r.modified
                      << ",\"created\":" << r.created << ",\"dropped\":" << r.dropped << ",\"class_changes\":" << r.class_changes
                      << ",\"links\":" << r.links << ",\"unreachable_links\":" << r.unreachable_links
                      << ",\"ground_impassable_links\":" << r.ground_impassable_links << ",\"radar_sites\":" << r.radar_sites
                      << ",\"unit_refs\":" << r.unit_refs << ",\"squadrons\":" << r.squadrons << ",\"primaries\":" << r.primaries
                      << ",\"secondaries\":" << r.secondaries << ",\"flag_mismatches\":" << r.flag_mismatches
                      << ",\"site_layouts\":" << ObjectiveLayoutCount() << "}" << std::endl;
            std::cout.flush(); std::cerr.flush(); std::_Exit(0);
        }
        if (remap) {
            // Relocate onto the regenerated grid and save before any planning.
            const auto moved = RemapTheater(std::filesystem::absolute(std::filesystem::u8path(args[4])));
            SaveAggregateCheckpoint(checkpointDirectory);
            std::cout << "{\"status\":\"ok\",\"objectives\":" << moved.objectives << ",\"units\":" << moved.units
                      << ",\"parents\":" << moved.parents << ",\"missing\":" << moved.missing << ",\"links\":" << moved.links
                      << ",\"unreachable_links\":" << moved.unreachable_links << ",\"ground_impassable_links\":" << moved.ground_impassable_links << ",\"theater\":[" << Map_Max_X << ',' << Map_Max_Y << "]}" << std::endl;
            std::cout.flush(); std::cerr.flush(); std::_Exit(0);
        }
        const CampaignTime start = TheCampaign.CurrentTime;
        loadedTime = start; campaignLoaded = true;
        const auto before = captureUnits();
        // Objective owners at the start, to report captures (ownership changes).
        std::map<VU_ID,int> startOwners;
        { VuListIterator it(AllObjList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) startOwners[e->Id()] = static_cast<ObjectiveClass*>(e)->GetOwner(); }
        auto previous = before;
        std::set<unsigned long long> moved, createdFlights, strengthChanged, supplyChanged, ordersChanged;
        stage("startup campaign planning");
#ifdef FF_DETAILED_ENGINE
        if (diagnostic) {
            RunDetailedDiagnostic(damageCheckpoint?(args[1]=="fault-checkpoint-test"?"air-fault-checkpoint":"air-damage-checkpoint"):args[4]);
            if(damageCheckpoint) SaveAggregateCheckpoint(checkpointDirectory);
            std::cout.flush(); std::cerr.flush(); std::_Exit(0);
        }
#endif
        if (exportMap) {
            // Map viewer export: the scenario exactly as loaded, before startup
            // planning or any campaign time.
            watch->initialize(scenario);
            watch->publish("snapshot");
            WriteCoreExport(exportDirectory);
            std::cout.flush(); std::cerr.flush(); std::_Exit(0);
        }
#ifdef FF_DETAILED_ENGINE
        if (battle) {
            RunBattleService(args);
            std::cout.flush(); std::cerr.flush(); std::_Exit(0);
        }
#endif
        if(!resume) DoCampaignLoop(1);
        if (watch) watch->initialize(scenario);
        std::unique_ptr<ConquestLog> conquestLog;
        if (conquest) { conquestLog = std::make_unique<ConquestLog>(conquestLogFile, start); conquestLog->status(); }
        const char* stopReason = "duration";
        CampaignTime nextCampaignTick = start + 5000;
        for (int second = 0; second < minutes * 60;) {
            if (watch) {
                FF_PROFILE(PROFILE_WATCH_WAIT);
                if (!watch->waitForStep()) break;
            }
            const unsigned frameMs = watch ? watch->stepMilliseconds() : 5000;
            if (DetailedRegionActive()) { FF_PROFILE(PROFILE_DETAILED_FRAME); AdvanceDetailedFrame(); }
            else SetTime(TheCampaign.CurrentTime + frameMs);
            if (TheCampaign.CurrentTime < nextCampaignTick) continue;
            nextCampaignTick += 5000; second += 5;
            { FF_PROFILE(PROFILE_CAMPAIGN_LOOP); DoCampaignLoop(0); }
            { FF_PROFILE(PROFILE_UPDATE_REAL_UNITS); UpdateRealUnits(5 * CampaignSeconds); }
            { FF_PROFILE(PROFILE_VU_THREAD); TheCampaign.vuThread->Update(-1); }
            { FF_PROFILE(PROFILE_MAIN_THREAD); gMainThread->Update(-1); }
            if (DetailedRegionActive()) { FF_PROFILE(PROFILE_DETAILED_RECONCILE); ReconcileDetailedRegion(); }
            {
                FF_PROFILE(PROFILE_HOST_UNIT_CAPTURE);
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
            }
            ++completedSteps;
            if (second % 3600 == 0) ff_headless::ProfileWrite((TheCampaign.CurrentTime - start) / CampaignMinutes, completedSteps);
            if (conquestLog) {
                FF_PROFILE(PROFILE_CONQUEST_LOG);
                const bool complete = conquestLog->step();
                if (second % 3600 == 0 || complete) conquestLog->status();
                if (complete) { stopReason = "conquest"; break; }
            }
        }
        ff_headless::ProfileWrite((TheCampaign.CurrentTime - start) / CampaignMinutes, completedSteps);
        if (conquestLog) conquestLog->finish(stopReason);
        if (watch) watch->publish("stopped");
        UnitStates savedUnits;
        if(checkpoint) {
            SaveAggregateCheckpoint(checkpointDirectory);
            savedUnits=captureUnits();
        }
        int local = 0, flights = 0;
        for (const auto& [id, state] : previous) { local += state.local; flights += state.flight; }
        std::cout << "{";
        if(checkpoint) std::cout<<"\"saved_unit_states\":"<<unitStateJson(savedUnits)<<',';
        if(checkpoint||resume) std::cout<<"\"initial_unit_states\":"<<unitStateJson(before)<<",\"final_unit_states\":"<<unitStateJson(previous)<<',';
        std::cout << "\"status\":\"ok\",\"simulation_advanced\":" << (completedSteps ? "true" : "false") << ",\"start_ms\":" << start
                  << ",\"end_ms\":" << TheCampaign.CurrentTime
                  << ",\"scenario\":" << ff::headless::JsonString(scenario)
                  << ",\"seed\":" << seed << ",\"format_version\":" << inspected.version
                  << (conquest ? std::string(",\"stop_reason\":\"") + stopReason + "\"" : std::string())
                  << ",\"completed_steps\":" << completedSteps << ",\"step_seconds\":5"
                  << ",\"all_units_aggregate\":" << (DetailedRegionActive() ? "false" : "true")
                  << ",\"objectives\":" << objectiveCount
                  << ",\"units_before\":" << before.size() << ",\"units_after\":" << previous.size()
                  << ",\"initial_unit_state_hash\":\""<<unitStateHash(before)<<"\",\"final_unit_state_hash\":\""<<unitStateHash(previous)<<"\""
                  << ",\"local_units\":" << local << ",\"flights_after\":" << flights
                  << ",\"moved_units\":" << moved.size() << ",\"created_flights\":" << createdFlights.size()
                  << [&] {
                         // Captures: objectives whose owner differs from the start of this run.
                         std::ostringstream ids; int n = 0;
                         VuListIterator it(AllObjList);
                         for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) {
                             auto o = startOwners.find(e->Id());
                             if (o == startOwners.end() || o->second == static_cast<ObjectiveClass*>(e)->GetOwner()) continue;
                             ids << (n++ ? "," : "") << '"' << e->Id().creator_ << ':' << e->Id().num_ << '"';
                         }
                         return ",\"captured_objectives\":" + std::to_string(n) + ",\"captured_ids\":[" + ids.str() + "]";
                     }()
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
        if (watch) { try { watch->error(error.what()); } catch (...) {} }
        std::cerr << "{\"status\":\"error\",\"simulation_advanced\":" << ((completedSteps || (campaignLoaded && TheCampaign.CurrentTime != loadedTime)) ? "true" : "false")
                  << ",\"completed_steps\":" << completedSteps << ",\"error\":" << ff::headless::JsonString(error.what()) << "}" << std::endl;
        std::_Exit(1);
    }
}
