#pragma once
#include <filesystem>
#include <string>
#include <vector>

class ObjectiveClass;

// Per-objective site layouts (wargame mapgen): the class template stays the source of the
// feature list, values and point lists; a layout only moves them.
//
// File lines (feet, x east, y north, degrees clockwise from north):
//   layout <creator:num> <class> <rotation> <shift-x> <shift-y>
//   feature <index> <x> <y> <facing>      placement of one feature of the preceding layout
// The rotation and shift move the whole template (features, runway/taxi/parking points and
// runway headings); a feature line then places that feature explicitly.
struct ObjectiveLayout {
    struct Feature { int index; float x, y; int facing; };
    int classId = 0;
    float rotation = 0, shiftX = 0, shiftY = 0;
    std::vector<Feature> features;
};

// Applies a layout to an objective (replacing any earlier one) and records it for saving.
void SetObjectiveLayout(ObjectiveClass* objective, const ObjectiveLayout& layout);
// Loads and applies a layout file; a missing file means no layouts. Returns layouts applied.
int LoadObjectiveLayouts(const std::filesystem::path& file);
// Writes the layouts of every live objective whose class still matches.
void SaveObjectiveLayouts(const std::filesystem::path& file);
int ObjectiveLayoutCount();
