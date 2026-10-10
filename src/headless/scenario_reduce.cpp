// Reduced scenarios for small comparison wars (see scenario_reduce.h). The result is saved
// before any planning, so both the legacy host and the new campaign core start the same war.
#include "stdhdr.h"
#include "f4vu.h"
#include "entity.h"
#include "classtbl.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "unit.h"
#include "objectiv.h"
#include "team.h"
#include "campmap.h"
#include "scenario_reduce.h"
#include "objective_rebuild.h"
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {

struct Box {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool contains(GridIndex x, GridIndex y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

float Distance2(GridIndex ax, GridIndex ay, Objective b) {
    GridIndex bx, by;
    b->GetLocation(&bx, &by);
    return float(ax - bx) * float(ax - bx) + float(ay - by) * float(ay - by);
}

// Every unit of the database: active (AllUnitList) and inactive reinforcements (InactiveList).
std::vector<Unit> AllUnits() {
    std::vector<Unit> units;
    for (auto* list : {static_cast<VuLinkedList*>(AllUnitList), static_cast<VuLinkedList*>(InactiveList)}) {
        VuListIterator it(list);
        for (auto* e = it.GetFirst(); e; e = it.GetNext())
            if (GetEntityClass(e) == CLASS_UNIT) units.push_back(static_cast<Unit>(e));
    }
    return units;
}

}  // namespace

ScenarioReduceResult ReduceScenario(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot open reduce plan");
    ScenarioReduceResult result;
    Box box;
    bool haveBox = false, keepAir = false, largestOnly = true;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "name") {
            if (!(in >> result.name)) throw std::runtime_error("Malformed name line: " + line);
        } else if (kind == "box") {
            if (!(in >> box.x0 >> box.y0 >> box.x1 >> box.y1) || box.x0 > box.x1 || box.y0 > box.y1)
                throw std::runtime_error("Malformed box line: " + line);
            haveBox = true;
        } else if (kind == "units") {
            std::string mode;
            in >> mode;
            if (mode != "land" && mode != "all") throw std::runtime_error("units must be land or all: " + line);
            keepAir = mode == "all";
        } else if (kind == "connected") {
            std::string mode;
            in >> mode;
            if (mode != "largest" && mode != "all") throw std::runtime_error("connected must be largest or all: " + line);
            largestOnly = mode == "largest";
        } else if (kind == "triggers") {
            std::string mode;
            in >> mode;
            if (mode != "none" && mode != "team" && mode != "theater") throw std::runtime_error("triggers must be none, team or theater: " + line);
            result.triggers = mode;
        } else {
            throw std::runtime_error("Unknown reduce plan line: " + line);
        }
    }
    if (!haveBox || result.name.empty()) throw std::runtime_error("A reduce plan needs name and box lines");
    if (result.name.size() >= 32 || strspn(result.name.c_str(), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != result.name.size())
        throw std::runtime_error("Invalid scenario name: " + result.name);
    result.x0 = box.x0; result.y0 = box.y0; result.x1 = box.x1; result.y1 = box.y1;

    // Objectives: keep the ones inside the box.
    std::vector<Objective> kept, dropped;
    {
        VuListIterator it(AllObjList);
        for (auto* e = it.GetFirst(); e; e = it.GetNext()) {
            auto* o = static_cast<Objective>(e);
            GridIndex x, y;
            o->GetLocation(&x, &y);
            (box.contains(x, y) ? kept : dropped).push_back(o);
        }
    }
    if (kept.empty()) throw std::runtime_error("The box holds no objectives");
    if (largestOnly) {
        // Ground-connected groups over the links inside the box; ties keep the earlier group.
        std::set<Objective> inBox(kept.begin(), kept.end()), seen;
        std::vector<Objective> largest;
        for (auto* first : kept) {
            if (seen.count(first)) continue;
            std::vector<Objective> group{first};
            seen.insert(first);
            for (size_t i = 0; i < group.size(); ++i)
                for (int l = 0; l < group[i]->NumLinks(); ++l) {
                    Objective n = group[i]->GetNeighbor(l);
                    if (!n || !inBox.count(n) || seen.count(n)) continue;
                    if (group[i]->GetNeighborCost(l, Tracked) >= 255 && group[i]->GetNeighborCost(l, Wheeled) >= 255) continue;
                    seen.insert(n);
                    group.push_back(n);
                }
            if (group.size() > largest.size()) largest = group;
        }
        const std::set<Objective> keep(largest.begin(), largest.end());
        std::vector<Objective> connected;
        for (auto* o : kept) {
            if (keep.count(o)) connected.push_back(o);
            else { dropped.push_back(o); ++result.disconnected; }
        }
        kept = connected;
    }
    std::set<VU_ID> droppedObjectives;
    for (auto* o : dropped) droppedObjectives.insert(o->Id());

    // Units: decide per top-level unit (brigades with all their elements, lone battalions,
    // squadrons by airbase, naval units by location); flights and packages always go.
    std::set<VU_ID> droppedUnits;
    const auto units = AllUnits();
    for (auto* u : units) {
        if (u->GetDomain() == DOMAIN_LAND) {
            if (Unit parent = u->GetUnitParent()) continue;  // decided with its brigade
            GridIndex x, y;
            u->GetLocation(&x, &y);
            if (box.contains(x, y)) continue;
            droppedUnits.insert(u->Id());
            for (int i = 0; Unit element = u->GetUnitElement(i); ++i) droppedUnits.insert(element->Id());
        } else if (u->IsSquadron()) {
            if (!keepAir || droppedObjectives.count(u->GetUnitAirbaseID()) || u->GetUnitAirbaseID() == FalconNullId)
                droppedUnits.insert(u->Id());
        } else if (u->GetDomain() == DOMAIN_SEA) {
            GridIndex x, y;
            u->GetLocation(&x, &y);
            if (!keepAir || !box.contains(x, y)) droppedUnits.insert(u->Id());
        } else {
            droppedUnits.insert(u->Id());
        }
    }
    // A battalion whose brigade is gone from the database goes too.
    for (auto* u : units)
        if (u->GetDomain() == DOMAIN_LAND && u->GetUnitParentID() != FalconNullId && droppedUnits.count(u->GetUnitParentID()))
            droppedUnits.insert(u->Id());

    // References to dropped objectives move to the nearest kept objective of the same kind.
    auto replacement = [&](VU_ID id, GridIndex x, GridIndex y, const std::function<bool(Objective)>& accept) -> VU_ID {
        if (id == FalconNullId || !droppedObjectives.count(id)) return id;
        Objective best = nullptr;
        float bestD = 0;
        for (int pass = 0; pass < 2 && !best; ++pass)
            for (auto* o : kept) {
                if (pass == 0 && !accept(o)) continue;
                const float d = Distance2(x, y, o);
                if (!best || d < bestD) { best = o; bestD = d; }
            }
        ++result.unit_refs;
        return best ? best->Id() : FalconNullId;
    };
    auto at = [](VU_ID id, GridIndex* x, GridIndex* y) {
        if (auto* e = static_cast<CampEntity>(vuDatabase->Find(id))) e->GetLocation(x, y);
    };
    auto sameType = [](VU_ID id) {
        auto* old = static_cast<Objective>(vuDatabase->Find(id));
        const int type = old ? old->GetType() : -1;
        return std::function<bool(Objective)>([type](Objective o) { return o->GetType() == type; });
    };
    const auto isPrimary = std::function<bool(Objective)>([](Objective o) { return o->IsPrimary() != 0; });
    const auto isSecondary = std::function<bool(Objective)>([](Objective o) { return o->IsSecondary() != 0; });
    for (auto* u : units) {
        if (droppedUnits.count(u->Id())) continue;
        for (auto [get, set, accept] : {
                 std::make_tuple(&UnitClass::GetUnitObjectiveID, &UnitClass::SetUnitObjective, std::function<bool(Objective)>()),
                 std::make_tuple(&UnitClass::GetUnitPrimaryObjID, &UnitClass::SetUnitPrimaryObj, isPrimary),
                 std::make_tuple(&UnitClass::GetUnitSecondaryObjID, &UnitClass::SetUnitSecondaryObj, isSecondary)}) {
            const VU_ID id = (u->*get)();
            if (!droppedObjectives.count(id)) continue;
            GridIndex x = 0, y = 0;
            at(id, &x, &y);
            (u->*set)(replacement(id, x, y, accept ? accept : sameType(id)));
        }
        const VU_ID target = u->GetTargetID();
        if (droppedObjectives.count(target) || droppedUnits.count(target)) { u->SetTargetId(FalconNullId); ++result.unit_refs; }
        if (droppedUnits.count(u->GetCargoId())) { u->SetCargoId(FalconNullId); ++result.unit_refs; }
    }
    for (int t = 0; t < NUM_TEAMS; ++t) {
        if (!TeamInfo[t]) continue;
        auto move = [&](VU_ID& id, const std::function<bool(Objective)>& accept) {
            GridIndex x = 0, y = 0;
            at(id, &x, &y);
            id = replacement(id, x, y, accept);
        };
        move(TeamInfo[t]->GetGroundAction()->actionObjective, isPrimary);
        for (auto* air : {TeamInfo[t]->GetDefensiveAirAction(), TeamInfo[t]->GetOffensiveAirAction()}) {
            move(air->actionObjective, isPrimary);
            move(air->lastActionObjective, isPrimary);
        }
        for (auto& bonus : TeamInfo[t]->bonusObjs) move(bonus, isSecondary);
    }

    // Links to dropped objectives go; the kept links keep their costs.
    for (auto* o : kept)
        for (int i = o->NumLinks() - 1; i >= 0; --i) {
            Objective n = o->GetNeighbor(i);
            if (n && !droppedObjectives.count(n->Id())) continue;
            o->RemoveObjectiveNeighbor(i);
            ++result.links_removed;
        }

    // Remove the dropped entities. Dead units are never saved; removing them from the database
    // also takes them off every campaign list before the lists and maps are rebuilt.
    for (auto* u : units) {
        if (!droppedUnits.count(u->Id())) { ++result.units_kept; continue; }
        u->SetDead(1);
        vuDatabase->Remove(u);
        ++result.units_dropped;
    }
    for (auto* o : dropped) vuDatabase->Remove(o);
    result.objectives_kept = static_cast<int>(kept.size());
    result.objectives_dropped = static_cast<int>(dropped.size());
    RefreshObjectiveProximity();
    RebuildObjectiveLists();
    RebuildParentsList();
    TheCampaign.MakeCampMap(MAP_OWNERSHIP);
    return result;
}
