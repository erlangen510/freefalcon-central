// Moves a loaded FreeFalcon campaign onto a regenerated theater grid
// (wargame mapgen/theater_gen.py) and recomputes objective link costs there.
// The grid itself is already loaded from the data root's KOREA.THR; this only
// relocates entities and refreshes the per-movement-type link costs with the
// campaign editor's own LinkCampaignObjectives path search.
#include "stdhdr.h"
#include "f4vu.h"
#include "entity.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "unit.h"
#include "objectiv.h"
#include "asearch.h"
#include "campmap.h"
#include "theater_remap.h"
#include "objective_rebuild.h"
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

extern short Map_Max_X, Map_Max_Y;

TheaterRemapResult RemapTheater(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("Cannot open theater remap file");
    TheaterRemapResult result;
    std::set<VU_ID> placed;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string kind;
        uint32_t creator = 0, number = 0;
        int x = 0, y = 0;
        if (!(in >> kind >> creator >> number >> x >> y) || (kind != "obj" && kind != "unit"))
            throw std::runtime_error("Malformed theater remap line: " + line);
        if (x < 1 || y < 1 || x >= Map_Max_X - 1 || y >= Map_Max_Y - 1)
            throw std::runtime_error("Theater remap position outside the grid: " + line);
        auto* entity = vuDatabase->Find(VU_ID(creator, number));
        if (kind == "obj") {
            auto* objective = dynamic_cast<ObjectiveClass*>(entity);
            if (!objective) { ++result.missing; continue; }
            objective->SetLocation(static_cast<GridIndex>(x), static_cast<GridIndex>(y));
            ++result.objectives;
        } else {
            auto* unit = dynamic_cast<UnitClass*>(entity);
            if (!unit) { ++result.missing; continue; }
            unit->SetLocation(static_cast<GridIndex>(x), static_cast<GridIndex>(y));
            ++result.units;
        }
        placed.insert(VU_ID(creator, number));
    }
    // Parents (brigades) are not in the remap list: put each on its first
    // element. Then clear every ground/naval path planned on the old grid.
    {
        VuListIterator it(AllUnitList);
        for (auto* entity = it.GetFirst(); entity; entity = it.GetNext()) {
            auto* unit = static_cast<UnitClass*>(entity);
            if (!placed.count(unit->Id()) && unit->Father()) {
                if (Unit element = unit->GetFirstUnitElement()) {
                    GridIndex x, y;
                    element->GetLocation(&x, &y);
                    unit->SetLocation(x, y);
                    ++result.parents;
                }
            }
            unit->ResetMoves();
        }
    }
    // Nearest-objective searches (bridge control of river crossings) must see the new positions.
    RefreshObjectiveProximity();
    // Link costs depend on the cells between objectives; recompute every link
    // on the new grid. Links with no path for any movement type become
    // impassable (255) instead of keeping costs measured on the old map.
    PathClass path;
    uchar impassable[MOVEMENT_TYPES];
    memset(impassable, 255, sizeof(impassable));
    VuListIterator it(AllObjList);
    for (auto* entity = it.GetFirst(); entity; entity = it.GetNext()) {
        auto* objective = static_cast<ObjectiveClass*>(entity);
        for (int i = 0; i < objective->NumLinks(); ++i) {
            Objective neighbor = objective->GetNeighbor(i);
            if (!neighbor || neighbor->Id() < objective->Id()) continue;  // each pair once
            ++result.links;
            if (!LinkCampaignObjectives(&path, objective, neighbor)) {
                objective->AddObjectiveNeighbor(neighbor, impassable);
                neighbor->AddObjectiveNeighbor(objective, impassable);
                ++result.unreachable_links;
            }
            // Air movement always finds a path, so also report links that
            // tracked and wheeled forces can no longer use.
            if (objective->GetNeighborCost(i, Tracked) >= 255 && objective->GetNeighborCost(i, Wheeled) >= 255)
                ++result.ground_impassable_links;
        }
    }
    TheCampaign.MakeCampMap(MAP_OWNERSHIP);
    return result;
}
