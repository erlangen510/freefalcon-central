#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ff::headless {
struct Section {
    std::string name;
    std::uint32_t offset, size;
};
struct ScenarioInfo {
    int version = 0;
    std::uint32_t time_ms = 0;
    std::uint16_t width = 0, height = 0, units = 0, objectives = 0;
    unsigned current_day = 0, active_teams = 0;
    std::string theater, scenario, title;
    std::vector<Section> sections;
};
// Reads and validates container boundaries and compressed campaign sections.
// Does not instantiate VU entities or run the campaign model.
ScenarioInfo InspectScenario(const std::filesystem::path& path);
ScenarioInfo InspectScenarioBytes(const std::vector<unsigned char>& bytes);
std::string JsonString(const std::string& value);
std::string ToJson(const ScenarioInfo& info);
}
