// Replaces a loaded campaign's objective set with one generated from open data
// (wargame mapgen/objectives.py). FreeFalcon objective classes stay the
// behaviour templates; this only decides which objectives exist, where, of
// which class, with which owner, priority, name and links.
//
// Plan lines:
//   obj <key> <creator:num|-> <class> <x> <y> <owner> <primary> <secondary> <priority> <name...>
//   keep <key> <creator:num>          an FF objective that stays as it is (outside Korea)
//   link <key> <key>                  every objective link (old links are cleared)
//   radar <key> <r0..r7>              radar arc ratios (tan of the masking angle per 45 deg octant)
//   layout <key> <rotation> <x> <y>   site layout: class template rotated (deg) and shifted (ft east, north)
//   feature <key> <index> <x> <y> <facing>  explicit placement of one class feature (ft, deg)
// Every FF objective neither reused nor kept is dropped after unit and team
// references to it have moved to the nearest suitable objective.
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
#include "asearch.h"
#include "campmap.h"
#include "cmpradar.h"
#include "campterr.h"
#include "find.h"
#include "path.h"
#include "name.h"
#include "sim/include/atcbrain.h"
#include "objective_rebuild.h"
#include "objective_layout.h"
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

extern short Map_Max_X, Map_Max_Y;
extern unsigned short* NameIndex;
extern short NameEntries;
extern _TCHAR* NameStream;
extern char FalconCampaignSaveDirectory[];
extern void InitTheaterLists(void);
extern void DisposeTheaterLists(void);

namespace {

struct PlannedObjective {
    std::string key, reuse, name;
    int classId = 0, x = 0, y = 0, owner = 0, priority = 0;
    bool primary = false, secondary = false;
};

VU_ID ParseId(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) throw std::runtime_error("Malformed objective id: " + text);
    return VU_ID(std::stoul(text.substr(0, colon)), std::stoul(text.substr(colon + 1)));
}

void RequireObjectiveClass(int classId) {
    if (classId <= 0 || classId >= NumEntities || Falcon4ClassTable[classId].dataType != DTYPE_OBJECTIVE)
        throw std::runtime_error("Not an objective class: " + std::to_string(classId));
}

// Airbases and airstrips get their air traffic brain from the class at construction;
// rebuild it whenever the class changes so runway data matches the new layout.
void ResetBrain(Objective o) {
    delete o->brain;
    o->brain = nullptr;
    if (o->GetType() == TYPE_AIRBASE || o->GetType() == TYPE_AIRSTRIP) {
        if (o->GetType() == TYPE_AIRBASE) o->SetTacan(1);
        o->brain = new ATCBrain(o);
    }
}

Objective CreateObjective(int classId) {
    // As NewObjective(), but constructed with the final class (airbase brains, feature status).
    auto* o = new ObjectiveClass(classId + VU_LAST_ENTITY_TYPE);
    o->SetSendCreate(VuEntity::VU_SC_DONT_SEND);
    if (vuDatabase->Insert(o) != VU_SUCCESS) { delete o; throw std::runtime_error("Objective insert failed"); }
    return o;
}

float Distance2(Objective a, Objective b) {
    GridIndex ax, ay, bx, by;
    a->GetLocation(&ax, &ay);
    b->GetLocation(&bx, &by);
    return float(ax - bx) * float(ax - bx) + float(ay - by) * float(ay - by);
}

Objective Nearest(Objective from, const std::vector<Objective>& pool, const std::function<bool(Objective)>& accept) {
    Objective best = nullptr;
    float bestD = 0;
    for (auto* o : pool) {
        if (!accept(o)) continue;
        const float d = Distance2(from, o);
        if (!best || d < bestD) { best = o; bestD = d; }
    }
    return best;
}

}  // namespace

void RefreshObjectiveProximity() {
    DisposeTheaterLists();
    InitTheaterLists();
    VuListIterator it(AllObjList);
    for (auto* e = it.GetFirst(); e; e = it.GetNext())
        if (e->VuState() == VU_MEM_ACTIVE) ObjProxList->ForcedInsert(e);
}

ObjectiveRebuildResult RebuildObjectives(const std::filesystem::path& file, const std::filesystem::path& keys) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot open objective plan");
    std::vector<PlannedObjective> planned;
    std::vector<std::pair<std::string, std::string>> keeps, links;
    std::map<std::string, std::vector<float>> radar;
    std::map<std::string, ObjectiveLayout> siteLayouts;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "obj") {
            PlannedObjective p;
            int primary = 0, secondary = 0;
            if (!(in >> p.key >> p.reuse >> p.classId >> p.x >> p.y >> p.owner >> primary >> secondary >> p.priority))
                throw std::runtime_error("Malformed objective line: " + line);
            std::getline(in >> std::ws, p.name);
            p.primary = primary != 0;
            p.secondary = secondary != 0;
            RequireObjectiveClass(p.classId);
            if (p.x < 1 || p.y < 1 || p.x >= Map_Max_X - 1 || p.y >= Map_Max_Y - 1 || p.owner < 0 || p.owner >= NUM_TEAMS)
                throw std::runtime_error("Objective outside the grid or with an invalid owner: " + line);
            planned.push_back(p);
        } else if (kind == "keep") {
            std::string key, id;
            if (!(in >> key >> id)) throw std::runtime_error("Malformed keep line: " + line);
            keeps.emplace_back(key, id);
        } else if (kind == "link") {
            std::string a, b;
            if (!(in >> a >> b)) throw std::runtime_error("Malformed link line: " + line);
            links.emplace_back(a, b);
        } else if (kind == "radar") {
            std::string key;
            std::vector<float> arcs(NUM_RADAR_ARCS);
            in >> key;
            for (auto& r : arcs) in >> r;
            if (!in) throw std::runtime_error("Malformed radar line: " + line);
            radar[key] = arcs;
        } else if (kind == "layout") {
            std::string key;
            float rotation = 0, x = 0, y = 0;
            if (!(in >> key >> rotation >> x >> y)) throw std::runtime_error("Malformed layout line: " + line);
            auto& l = siteLayouts[key];
            l.rotation = rotation;
            l.shiftX = x;
            l.shiftY = y;
        } else if (kind == "feature") {
            std::string key;
            ObjectiveLayout::Feature f{};
            if (!(in >> key >> f.index >> f.x >> f.y >> f.facing)) throw std::runtime_error("Malformed feature line: " + line);
            siteLayouts[key].features.push_back(f);
        } else if (kind != "parent") {
            throw std::runtime_error("Unknown objective plan line: " + line);
        }
    }

    fprintf(stderr, "[rebuild] plan parsed\n"); fflush(stderr);
    ObjectiveRebuildResult result;
    std::vector<Objective> existing;
    {
        VuListIterator it(AllObjList);
        for (auto* e = it.GetFirst(); e; e = it.GetNext()) existing.push_back(static_cast<Objective>(e));
    }
    std::map<std::string, Objective> byKey;
    std::set<Objective> used;
    for (const auto& [key, id] : keeps) {
        auto* o = dynamic_cast<ObjectiveClass*>(vuDatabase->Find(ParseId(id)));
        if (!o) throw std::runtime_error("Kept objective not found: " + id);
        byKey[key] = o;
        used.insert(o);
        ++result.kept;
    }
    for (const auto& p : planned) {
        Objective o = nullptr;
        if (p.reuse != "-") {
            o = dynamic_cast<ObjectiveClass*>(vuDatabase->Find(ParseId(p.reuse)));
            if (!o || used.count(o)) throw std::runtime_error("Reused objective missing or reused twice: " + p.reuse);
            if (o->Type() - VU_LAST_ENTITY_TYPE != p.classId) {
                o->SetObjectiveClass(p.classId + VU_LAST_ENTITY_TYPE);
                ResetBrain(o);
                ++result.class_changes;
            }
            ++result.modified;
        } else {
            o = CreateObjective(p.classId);
            o->SetObjectiveSupply(100);
            o->SetObjectiveFuel(100);
            ++result.created;
        }
        o->SetLocation(static_cast<GridIndex>(p.x), static_cast<GridIndex>(p.y));
        o->SetOwner(static_cast<Control>(p.owner));
        o->SetObjectiveOldown(static_cast<Control>(p.owner));
        o->SetObjectivePriority(static_cast<PriorityLevel>(p.priority));
        byKey[p.key] = o;
        used.insert(o);
    }
    fprintf(stderr, "[rebuild] objectives placed\n"); fflush(stderr);
    // Names: the table is rebuilt in one piece (kept objectives keep their text); adding
    // thousands of names one by one would overflow the engine's 16-bit name offsets.
    {
        std::vector<std::string> text;
        std::vector<Objective> named;
        for (const auto& [key, id] : keeps) {
            Objective o = byKey[key];
            if (!o->GetObjectiveNameID()) continue;
            _TCHAR buffer[128]{};
            ReadNameString(o->GetObjectiveNameID(), buffer, 127);
            text.emplace_back(buffer);
            named.push_back(o);
        }
        for (const auto& p : planned) {
            text.push_back(p.name.substr(0, 40));
            named.push_back(byKey[p.key]);
        }
        std::vector<const _TCHAR*> pointers;
        for (const auto& t : text) pointers.push_back(t.c_str());
        if (!ReplaceNameTable(static_cast<int>(pointers.size()), pointers.data()))
            throw std::runtime_error("Objective names exceed the name table");
        {
            VuListIterator it(AllObjList);
            for (auto* e = it.GetFirst(); e; e = it.GetNext()) static_cast<Objective>(e)->SetObjectiveNameID(0);
        }
        for (size_t i = 0; i < named.size(); ++i) named[i]->SetObjectiveNameID(static_cast<short>(i + 2));
        if (!SaveNames(TheCampaign.TheaterName)) throw std::runtime_error("Name table save failed");
        // SaveNames writes the .idx, but OpenCampFile returns the cached read-only .wch handle for
        // writing, so the name text is written here (same folder as the .idx).
        const auto text_path = std::filesystem::path(FalconCampaignSaveDirectory) /  // ANSI code page, as setPath stores it
                               (std::string(TheCampaign.TheaterName) + ".wch");
        std::ofstream stream(text_path, std::ios::binary | std::ios::trunc);
        stream.write(NameStream, NameIndex[NameEntries - 1]);
        if (!stream) throw std::runtime_error("Name text save failed: " + text_path.u8string());
    }
    std::vector<Objective> survivors(used.begin(), used.end());
    std::vector<Objective> dropped;
    for (auto* o : existing)
        if (!used.count(o)) dropped.push_back(o);

    // Primary/secondary status follows from type and priority (IsPrimary/IsSecondary).
    for (const auto& p : planned) {
        Objective o = byKey[p.key];
        result.primaries += o->IsPrimary() ? 1 : 0;
        result.secondaries += o->IsSecondary() ? 1 : 0;
        if ((o->IsPrimary() != 0) != p.primary || (o->IsSecondary() != 0) != p.secondary) ++result.flag_mismatches;
    }

    fprintf(stderr, "[rebuild] flags checked\n"); fflush(stderr);
    // References to dropped objectives move to the nearest surviving objective of the same
    // kind (primary, secondary, airbase for squadrons, else the same type).
    std::set<VU_ID> droppedIds;
    for (auto* o : dropped) droppedIds.insert(o->Id());
    auto replacement = [&](VU_ID id, const std::function<bool(Objective)>& accept) -> VU_ID {
        if (id == FalconNullId || !droppedIds.count(id)) return id;
        auto* old = static_cast<Objective>(vuDatabase->Find(id));
        Objective best = old ? Nearest(old, survivors, accept) : nullptr;
        if (!best && old) best = Nearest(old, survivors, [](Objective) { return true; });
        ++result.unit_refs;
        return best ? best->Id() : FalconNullId;
    };
    auto sameType = [&](VU_ID id) {
        auto* old = static_cast<Objective>(vuDatabase->Find(id));
        const int type = old ? old->GetType() : -1;
        return std::function<bool(Objective)>([type](Objective o) { return o->GetType() == type; });
    };
    auto isPrimary = std::function<bool(Objective)>([](Objective o) { return o->IsPrimary() != 0; });
    auto isSecondary = std::function<bool(Objective)>([](Objective o) { return o->IsSecondary() != 0; });
    {
        VuListIterator it(AllUnitList);
        for (auto* e = it.GetFirst(); e; e = it.GetNext()) {
            auto* u = static_cast<UnitClass*>(e);
            if (u->IsSquadron()) {
                const VU_ID base = u->GetUnitAirbaseID();
                const Team team = u->GetTeam();
                const VU_ID moved = replacement(base, [team](Objective o) {
                    return (o->GetType() == TYPE_AIRBASE || o->GetType() == TYPE_AIRSTRIP) && GetTTRelations(o->GetTeam(), team) == Allied;
                });
                if (moved != base) u->SetUnitAirbase(moved);
                if (auto* airbase = static_cast<Objective>(vuDatabase->Find(moved))) {
                    GridIndex x, y;
                    airbase->GetLocation(&x, &y);
                    u->SetLocation(x, y);
                    ++result.squadrons;
                }
                continue;
            }
            const VU_ID objective = u->GetUnitObjectiveID();
            if (droppedIds.count(objective)) u->SetUnitObjective(replacement(objective, sameType(objective)));
            const VU_ID primary = u->GetUnitPrimaryObjID();
            if (droppedIds.count(primary)) u->SetUnitPrimaryObj(replacement(primary, isPrimary));
            const VU_ID secondary = u->GetUnitSecondaryObjID();
            if (droppedIds.count(secondary)) u->SetUnitSecondaryObj(replacement(secondary, isSecondary));
            if (droppedIds.count(u->GetTargetID())) { u->SetTargetId(FalconNullId); ++result.unit_refs; }
        }
    }
    fprintf(stderr, "[rebuild] unit references moved\n"); fflush(stderr);
    for (int t = 0; t < NUM_TEAMS; ++t) {
        if (!TeamInfo[t]) continue;
        auto* ground = TeamInfo[t]->GetGroundAction();
        ground->actionObjective = replacement(ground->actionObjective, isPrimary);
        for (auto* air : {TeamInfo[t]->GetDefensiveAirAction(), TeamInfo[t]->GetOffensiveAirAction()}) {
            air->actionObjective = replacement(air->actionObjective, isPrimary);
            air->lastActionObjective = replacement(air->lastActionObjective, isPrimary);
        }
        for (auto& bonus : TeamInfo[t]->bonusObjs) bonus = replacement(bonus, isSecondary);
    }

    fprintf(stderr, "[rebuild] team references moved\n"); fflush(stderr);
    // Links: clear everything, then link the plan's pairs with path costs on the current grid.
    for (auto* o : existing)
        while (o->NumLinks() > 0) o->RemoveObjectiveNeighbor(0);
    fprintf(stderr, "[rebuild] links cleared\n"); fflush(stderr);
    // Dropped objectives leave the database lazily (garbage collection on a later VU update) and
    // moved ones sit at stale keys in the proximity grid; rebuild it before the link path costs,
    // which ask it which objective controls each river crossing (GetMovementCost).
    for (auto* o : dropped) vuDatabase->Remove(o);
    RefreshObjectiveProximity();
    fprintf(stderr, "[rebuild] dropped removed\n"); fflush(stderr);
    result.dropped = static_cast<int>(dropped.size());
    PathClass path;
    uchar impassable[MOVEMENT_TYPES];
    memset(impassable, 255, sizeof(impassable));
    if (getenv("REBUILD_TRACE")) {
        int bridges = 0, nearestOther = 0, nearestNone = 0, blocked = 0, noRoad = 0, dry = 0;
        for (auto* o : survivors) {
            if (o->GetType() != TYPE_BRIDGE) continue;
            ++bridges;
            GridIndex x, y;
            o->GetLocation(&x, &y);
            Objective n = FindNearestObjective(x, y, NULL);
            if (!n) ++nearestNone; else if (n != o) ++nearestOther;
            if (!GetRoad(x, y)) ++noRoad;
            if (GetCover(x, y) != Water) ++dry;
            if (GetMovementCost(x, y, Tracked, PATH_BASIC, Here) > MAX_COST) ++blocked;
        }
        fprintf(stderr, "[rebuild] bridges %d nearest-other %d nearest-none %d no-road %d not-water %d blocked %d\n",
                bridges, nearestOther, nearestNone, noRoad, dry, blocked);
    }
    for (const auto& [a, b] : links) {
        auto ia = byKey.find(a), ib = byKey.find(b);
        if (ia == byKey.end() || ib == byKey.end()) throw std::runtime_error("Link to an unknown objective: " + a + " " + b);
        if (ia->second == ib->second) continue;
        ++result.links;
        if (!LinkCampaignObjectives(&path, ia->second, ib->second)) {
            ia->second->AddObjectiveNeighbor(ib->second, impassable);
            ib->second->AddObjectiveNeighbor(ia->second, impassable);
            ++result.unreachable_links;
        }
    }
    for (auto* o : survivors)
        for (int i = 0; i < o->NumLinks(); ++i)
            if (o->GetNeighbor(i) && o->GetNeighbor(i)->Id() > o->Id() && o->GetNeighborCost(i, Tracked) >= 255 &&
                o->GetNeighborCost(i, Wheeled) >= 255)
                ++result.ground_impassable_links;

    fprintf(stderr, "[rebuild] links built\n"); fflush(stderr);
    // Radar masking arcs from the open-data elevation model, where the class has a sensor.
    for (const auto& [key, arcs] : radar) {
        auto it = byKey.find(key);
        if (it == byKey.end()) throw std::runtime_error("Radar arcs for an unknown objective: " + key);
        Objective o = it->second;
        if (!(o->SamSite() || o->RadarSite() || o->GetElectronicDetectionRange(LowAir) > 0)) continue;
        delete o->static_data.radar_data;
        o->static_data.radar_data = new RadarRangeClass;
        for (int arc = 0; arc < NUM_RADAR_ARCS; ++arc)
            o->static_data.radar_data->SetArcRatio(arc, arcs[arc] > MINIMUM_RADAR_RATIO ? arcs[arc] : MINIMUM_RADAR_RATIO);
        ++result.radar_sites;
    }

    fprintf(stderr, "[rebuild] radar arcs set\n"); fflush(stderr);
    // Site layouts last: the class of every objective is final now.
    for (auto& [key, l] : siteLayouts) {
        auto it = byKey.find(key);
        if (it == byKey.end()) throw std::runtime_error("Layout for an unknown objective: " + key);
        l.classId = it->second->Type() - VU_LAST_ENTITY_TYPE;
        SetObjectiveLayout(it->second, l);
    }
    fprintf(stderr, "[rebuild] site layouts %zu\n", siteLayouts.size()); fflush(stderr);
    RebuildObjectiveLists();
    RebuildParentsList();
    TheCampaign.MakeCampMap(MAP_OWNERSHIP);
    if (!keys.empty()) {
        std::ofstream out(keys);
        for (const auto& [key, o] : byKey)
            out << key << ' ' << o->Id().creator_ << ':' << o->Id().num_ << "\n";
        if (!out) throw std::runtime_error("Cannot write objective keys");
    }
    return result;
}

