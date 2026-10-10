#pragma once
#include <filesystem>

struct ObjectiveRebuildResult {
    int kept = 0;          // FF objectives kept unchanged (outside the rebuilt area)
    int modified = 0;      // FF objectives reused by a new objective (same id, new place/class/name)
    int created = 0;
    int dropped = 0;
    int class_changes = 0;
    int links = 0;
    int unreachable_links = 0;
    int ground_impassable_links = 0;
    int radar_sites = 0;
    int unit_refs = 0;     // unit and team references moved off dropped objectives
    int squadrons = 0;     // squadrons placed on their (new) airbase
    int primaries = 0;
    int secondaries = 0;
    int flag_mismatches = 0;  // planned primary/secondary flag differs from the engine's rule
};

// Rebuilds the objective proximity grid from the active objectives. VuGridTree::Move re-inserts
// a moved entity keyed by its old second coordinate (it runs before the position changes), so
// objectives moved or created after load are missing from nearest-objective searches until this
// runs. Those searches decide which bridge controls a river crossing in link path costs.
void RefreshObjectiveProximity();

// Applies a mapgen/objectives.py plan (`obj`, `keep`, `link`, `radar` lines) to the loaded
// campaign: reuses, creates and drops objectives, relinks them on the current grid, moves
// unit/team references off dropped objectives and rebuilds the objective lists.
// `keys`, when given, receives "<plan key> <creator:num>" lines for the viewer.
ObjectiveRebuildResult RebuildObjectives(const std::filesystem::path& file, const std::filesystem::path& keys = {});
