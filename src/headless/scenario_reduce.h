#pragma once
#include <filesystem>
#include <string>

struct ScenarioReduceResult {
    std::string name;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int objectives_kept = 0, objectives_dropped = 0;
    int units_kept = 0, units_dropped = 0;
    int links_removed = 0;
    int unit_refs = 0;   // unit and team references moved off dropped objectives or units
    int disconnected = 0;   // box objectives dropped as unreachable from the largest group
    std::string triggers = "team";   // none | team | theater (trigger script of the reduced scenario)
};

// Reduces a loaded campaign to a rectangular area of the grid for small comparison wars
// (migration/ADR-0001 milestone 3e). Plan lines:
//   name <scenario-name>        scenario name written into the archive (it also names the .tri)
//   box <x0> <y0> <x1> <y1>     inclusive grid cells (1 km, x east, y north) that stay
//   units land|all              land: keep ground units only; all: also squadrons on kept
//                               airbases and naval units inside the box (flights and packages
//                               are always dropped)
//   triggers none|team|theater  trigger script written as <name>.tri. team (default): the init
//                               section and the top-level trigger blocks that name no objectives
//                               and compare no force ratios, so FF's own end-game triggers
//                               (boredom, campaign day) stay; none: the init section only;
//                               theater: unchanged. The theater script lists whole-theater
//                               objectives (a missing one counts as controlled in IF_CONTROLLED
//                               A lists) and divides by aircraft counts (IF_FORCE_RATIO).
//   connected largest|all       largest (default): keep only the largest group of box objectives
//                               joined by links that tracked or wheeled units can use; an
//                               objective cut off from it could never be reached and captured
// Objectives outside the box are dropped with their links; ground units are kept or dropped with
// their brigade, decided by the brigade's (or a lone battalion's) location. References to dropped
// objectives move to the nearest kept objective of the same kind, as rebuild-objectives does.
ScenarioReduceResult ReduceScenario(const std::filesystem::path& plan);
