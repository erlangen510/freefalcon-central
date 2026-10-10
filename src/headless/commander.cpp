#include "stdhdr.h"
#include "f4vu.h"
#include "entity.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "listadt.h"
#include "unit.h"
#include "flight.h"
#include "package.h"
#include "mission.h"
#include "objectiv.h"
#include "team.h"
#include "atm.h"
#include "gtm.h"
#include "gtmobj.h"
#include "scenario.h"
#include "commander.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace {
std::string id(VU_ID value) {
    if (value == FalconNullId) return "";
    return std::to_string(value.creator_) + ":" + std::to_string(value.num_);
}
std::string quoted(VU_ID value) { return "\"" + id(value) + "\""; }
bool activeTeam(int team) {
    return team > 0 && team < NUM_TEAMS && TeamInfo[team] && (TeamInfo[team]->flags & TEAM_ACTIVE);
}
template <typename T> void array(std::ostream& out, const T* values, int count) {
    out << '[';
    for (int i = 0; i < count; ++i) { if (i) out << ','; out << int(values[i]); }
    out << ']';
}
void request(std::ostream& out, const MissionRequestClass& mis) {
    out << "{\"mission\":" << int(mis.mission) << ",\"target\":" << quoted(mis.targetID)
        << ",\"requester\":" << quoted(mis.requesterID) << ",\"x\":" << mis.tx << ",\"y\":" << mis.ty
        << ",\"priority\":" << mis.priority << ",\"context\":" << int(mis.context)
        << ",\"action\":" << int(mis.action_type) << ",\"aircraft\":" << int(mis.aircraft)
        << ",\"tot_ms\":" << mis.tot << '}';
}
void airAction(std::ostream& out, const TeamAirActionType& action) {
    out << "{\"type\":" << int(action.actionType) << ",\"objective\":" << quoted(action.actionObjective)
        << ",\"start_ms\":" << action.actionStartTime << ",\"stop_ms\":" << action.actionStopTime << '}';
}
Objective primaryObjective(unsigned long long creator, unsigned long long number) {
    if (creator > UINT32_MAX || number > UINT32_MAX) return nullptr;
    VU_ID target(static_cast<uint32_t>(creator), static_cast<uint32_t>(number));
    auto* entity = static_cast<FalconEntity*>(vuDatabase->Find(target));
    if (!entity || !entity->IsObjective()) return nullptr;
    auto* objective = static_cast<Objective>(entity);
    return GetPOData(objective) ? objective : nullptr;
}
void setPriority(POData pd, int team, short value) {
    // player_priority is the original PAK-screen override (ATM air tasking,
    // saved natively); operator_priority feeds the GTM like #SET_PRIORITY.
    pd->player_priority[team] = value;
    pd->operator_priority[team] = value;
    bool player = false;
    for (int t = 0; t < NUM_TEAMS; ++t) player = player || pd->player_priority[t] >= 0;
    if (player) pd->flags |= GTMOBJ_PLAYER_SET_PRIORITY;
    else pd->flags &= ~GTMOBJ_PLAYER_SET_PRIORITY;
}
}

std::string CommanderSnapshotJson() {
    std::ostringstream out;
    out << "{\"teams\":[";
    bool comma = false;
    for (int t = 1; t < NUM_TEAMS; ++t) {
        if (!activeTeam(t)) continue;
        TeamClass* team = TeamInfo[t];
        if (comma) out << ','; comma = true;
        const TeamStatusType* stats = team->GetCurrentStats();
        const TeamGndActionType* ground = team->GetGroundAction();
        out << "{\"team\":" << t << ",\"initiative\":" << team->GetInitiative()
            << ",\"supply_avail\":" << team->GetSupplyAvail() << ",\"fuel_avail\":" << team->GetFuelAvail()
            << ",\"replacements_avail\":" << team->GetReplacementsAvail() << ",\"reinforcement\":" << team->GetReinforcement()
            << ",\"stats\":{\"aircraft\":" << stats->aircraft << ",\"ground\":" << stats->groundVehs
            << ",\"air_defense\":" << stats->airDefenseVehs << ",\"ships\":" << stats->ships
            << ",\"airbases\":" << stats->airbases << ",\"supply_level\":" << int(stats->supplyLevel)
            << ",\"fuel_level\":" << int(stats->fuelLevel) << '}'
            << ",\"ground_action\":{\"type\":" << int(ground->actionType) << ",\"objective\":" << quoted(ground->actionObjective)
            << ",\"tempo\":" << int(ground->actionTempo) << ",\"points\":" << int(ground->actionPoints)
            << ",\"start_ms\":" << ground->actionTime << ",\"timeout_ms\":" << ground->actionTimeout << '}'
            << ",\"offensive_air\":";
        airAction(out, *team->GetOffensiveAirAction());
        out << ",\"defensive_air\":";
        airAction(out, *team->GetDefensiveAirAction());
        out << ",\"stance\":"; array(out, team->stance, NUM_TEAMS);
        // Pending requests are the commander's current reasons to fly, before packaging.
        std::vector<MissionRequestClass*> pending;
        int delayed = 0;
        if (ATM atm = team->atm) {
            for (ListNode n = atm->requestList ? atm->requestList->GetFirstElement() : nullptr; n; n = n->GetNext())
                if (n->GetUserData()) pending.push_back(static_cast<MissionRequestClass*>(n->GetUserData()));
            for (ListNode n = atm->delayedList ? atm->delayedList->GetFirstElement() : nullptr; n; n = n->GetNext()) ++delayed;
            std::stable_sort(pending.begin(), pending.end(), [](auto* a, auto* b) { return a->priority > b->priority; });
            out << ",\"atm\":{\"squadrons\":" << atm->squadrons << ",\"cycle\":" << int(atm->cycle)
                << ",\"pending\":" << pending.size() << ",\"delayed\":" << delayed << ",\"requests\":[";
            for (size_t i = 0; i < pending.size() && i < 12; ++i) { if (i) out << ','; request(out, *pending[i]); }
            out << "]}";
        }
        if (GTM gtm = team->gtm)
            out << ",\"gtm\":{\"top_priority\":" << gtm->topPriority << ",\"priority_objective\":" << gtm->priorityObj << '}';
        out << '}';
    }
    out << "],\"primary_objectives\":[";
    comma = false;
    VuListIterator poit(POList);
    for (Objective o = GetFirstObjective(&poit); o; o = GetNextObjective(&poit)) {
        POData pd = GetPOData(o);
        if (!pd) continue;
        if (comma) out << ','; comma = true;
        out << "{\"id\":" << quoted(o->Id()) << ",\"base_priority\":" << int(o->GetObjectivePriority())
            << ",\"scripted\":" << ((pd->flags & GTMOBJ_SCRIPTED_PRIORITY) ? "true" : "false") << ",\"ground\":";
        array(out, pd->ground_priority, NUM_TEAMS);
        out << ",\"air\":"; array(out, pd->air_priority, NUM_TEAMS);
        out << ",\"player\":"; array(out, pd->player_priority, NUM_TEAMS);
        out << ",\"operator\":"; array(out, pd->operator_priority, NUM_TEAMS);
        out << ",\"assigned\":"; array(out, pd->ground_assigned, NUM_TEAMS);
        out << '}';
    }
    out << "],\"packages\":[";
    comma = false;
    VuListIterator it(AllUnitList);
    for (Unit u = static_cast<Unit>(it.GetFirst()); u; u = static_cast<Unit>(it.GetNext())) {
        if (!u->IsPackage() || u->IsDead()) continue;
        auto* package = static_cast<PackageClass*>(u);
        if (comma) out << ','; comma = true;
        out << "{\"id\":" << quoted(u->Id()) << ",\"team\":" << int(u->GetTeam())
            << ",\"takeoff_ms\":" << package->GetTakeoff() << ",\"request\":";
        request(out, *package->GetMissionRequest());
        out << ",\"flights\":[";
        bool fc = false;
        for (int e = 0; e < MAX_UNIT_CHILDREN; ++e) {
            Unit flight = package->GetUnitElement(e);
            if (!flight) continue;
            if (fc) out << ','; fc = true;
            out << quoted(flight->Id());
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string ApplyCommanderCommand(const std::string& command) {
    std::istringstream in(command);
    std::string verb, extra;
    unsigned long long creator = 0, number = 0;
    int team = 0, value = -1;
    if (!(in >> verb >> team >> creator >> number)) return "malformed";
    if (verb == "priority") { if (!(in >> value) || value < 0 || value > 100) return "invalid_priority"; }
    else if (verb != "reset") return "unknown_command";
    if (in >> extra) return "malformed";
    if (!activeTeam(team)) return "inactive_team";
    Objective objective = primaryObjective(creator, number);
    if (!objective) return "not_primary_objective";
    setPriority(GetPOData(objective), team, static_cast<short>(value));
    // The loop recalculates only every PlanGroundTime (6 h in FF6). Recalculate
    // this team now so the override, or its removal, reaches hourly consumers.
    TeamInfo[team]->gtm->DoCalculations();
    fprintf(stderr, "[commander] %s team=%d objective=%llu:%llu value=%d\n", verb.c_str(), team, creator, number, value);
    return "applied";
}

void SaveCommanderCheckpoint(const std::filesystem::path& directory) {
    // player_priority is in the native .pol; the GTM override needs a sidecar.
    std::ofstream out(directory / "commander-priorities.txt", std::ios::binary);
    out << "commander-priorities 1\n";
    VuListIterator poit(POList);
    for (Objective o = GetFirstObjective(&poit); o; o = GetNextObjective(&poit)) {
        POData pd = GetPOData(o);
        if (!pd) continue;
        for (int t = 0; t < NUM_TEAMS; ++t)
            if (pd->operator_priority[t] >= 0)
                out << t << ' ' << o->Id().creator_ << ' ' << o->Id().num_ << ' ' << pd->operator_priority[t] << '\n';
    }
    out.close();
    if (!out) throw std::runtime_error("Commander priority checkpoint write failed");
}

void LoadCommanderCheckpoint(const std::filesystem::path& directory) {
    std::ifstream in(directory / "commander-priorities.txt", std::ios::binary);
    if (!in) return; // Older checkpoints predate commander overrides.
    std::string magic; int version = 0;
    if (!(in >> magic >> version) || magic != "commander-priorities" || version != 1)
        throw std::runtime_error("Invalid commander priority checkpoint");
    int team, value; unsigned long long creator, number;
    while (in >> team >> creator >> number >> value) {
        if (team < 0 || team >= NUM_TEAMS || value < 0 || value > 100) throw std::runtime_error("Invalid commander priority entry");
        Objective objective = primaryObjective(creator, number);
        if (!objective) { fprintf(stderr, "[commander] saved override for %llu:%llu skipped: not a primary objective\n", creator, number); continue; }
        GetPOData(objective)->operator_priority[team] = static_cast<short>(value);
    }
    if (!in.eof()) throw std::runtime_error("Invalid commander priority checkpoint");
}
