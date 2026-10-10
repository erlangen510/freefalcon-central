#pragma once
#include <filesystem>

struct TheaterRemapResult {
    int objectives = 0;
    int units = 0;
    int parents = 0;
    int missing = 0;
    int links = 0;
    int unreachable_links = 0;
    int ground_impassable_links = 0;
};

// Applies `obj|unit creator num x y` lines to the loaded campaign, resets unit
// paths and recomputes all objective link costs on the current theater grid.
TheaterRemapResult RemapTheater(const std::filesystem::path& file);
