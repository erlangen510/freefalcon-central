#include "scenario.h"
#include "utils/lzss.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ff::headless {
namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::size_t MaxBytes = 64 * 1024 * 1024;
struct Reader {
    const Bytes& bytes;
    std::size_t pos = 0;
    void require(std::size_t n) const {
        if (pos > bytes.size() || n > bytes.size() - pos)
            throw std::runtime_error("truncated scenario section");
    }
    void skip(std::size_t n) { require(n); pos += n; }
    std::uint32_t number(std::size_t n) {
        require(n);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < n; ++i) value |= std::uint32_t(bytes[pos++]) << (i * 8);
        return value;
    }
    std::string text(std::size_t n) {
        require(n);
        std::string value(bytes.begin() + pos, bytes.begin() + pos + n);
        pos += n;
        auto end = value.find('\0');
        if (end != std::string::npos) value.resize(end);
        return value;
    }
};
Bytes Expand(const Bytes& bytes, std::size_t prefix, std::uint32_t size) {
    if (size > MaxBytes || prefix > bytes.size())
        throw std::runtime_error("invalid expanded size");
    if (size == 0) {
        if (prefix != bytes.size()) throw std::runtime_error("unexpected empty compressed payload");
        return {};
    }
    Bytes out(size);
    const int consumed = LZSS_ExpandChecked(bytes.data() + prefix,
        static_cast<int>(bytes.size() - prefix), out.data(), static_cast<int>(size));
    if (consumed < 0 || static_cast<std::size_t>(consumed) != bytes.size() - prefix)
        throw std::runtime_error("invalid or truncated LZSS payload");
    return out;
}
}

ScenarioInfo InspectScenario(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open scenario file");
    const auto size = input.tellg();
    if (size < 8 || size > static_cast<std::streamoff>(MaxBytes))
        throw std::runtime_error("invalid scenario file size");
    Bytes bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("cannot read scenario file");
    return InspectScenarioBytes(bytes);
}

ScenarioInfo InspectScenarioBytes(const Bytes& bytes) {
    if (bytes.size() > MaxBytes) throw std::runtime_error("scenario file too large");
    Reader input{bytes};
    const auto directory = input.number(4);
    if (directory < 4 || directory > bytes.size()) throw std::runtime_error("invalid directory offset");
    input.pos = directory;
    const auto count = input.number(4);
    if (count == 0 || count > 64) throw std::runtime_error("invalid section count");
    ScenarioInfo result;
    std::map<std::string, Bytes> sections;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto length = input.number(1);
        if (length == 0) throw std::runtime_error("empty section name");
        auto name = input.text(length);
        if (name.size() != length || name.find_first_of("/\\:") != std::string::npos)
            throw std::runtime_error("invalid section name");
        const auto offset = input.number(4), size = input.number(4);
        if (offset < 4 || offset > directory || size > directory - offset)
            throw std::runtime_error("section outside payload area");
        const auto dot = name.rfind('.');
        if (dot == std::string::npos) throw std::runtime_error("section has no extension");
        auto extension = name.substr(dot + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!sections.emplace(extension, Bytes(bytes.begin() + offset, bytes.begin() + offset + size)).second)
            throw std::runtime_error("duplicate section extension");
        result.sections.push_back({name, offset, size});
        if (size) ranges.emplace_back(offset, offset + size);
    }
    if (input.pos != bytes.size()) throw std::runtime_error("unexpected data after directory");
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second) throw std::runtime_error("overlapping sections");
    auto section = [&](const char* name) -> const Bytes& {
        const auto found = sections.find(name);
        if (found == sections.end()) throw std::runtime_error(std::string("missing section: ") + name);
        return found->second;
    };
    const auto& version = section("ver");
    if (version.empty() || version.size() > 3) throw std::runtime_error("invalid format version");
    for (auto digit : version) {
        if (digit < '0' || digit > '9') throw std::runtime_error("invalid format version");
        result.version = result.version * 10 + digit - '0';
    }
    // Version-specific prefix follows CampaignClass::Decode. Do not silently
    // interpret unknown save formats as one of these layouts.
    if (result.version != 73 && result.version != 99)
        throw std::runtime_error("unsupported scenario format version (expected 73 or 99)");
    Reader cmp{section("cmp")};
    if (cmp.number(4) != cmp.bytes.size() - 4)
        throw std::runtime_error("campaign section size mismatch");
    const auto expanded_size = cmp.number(4);
    const auto expanded = Expand(cmp.bytes, cmp.pos, expanded_size);
    Reader state{expanded};
    result.time_ms = state.number(4);
    state.skip(12); // Tactical start/limit/victory points.
    state.skip(4 + 4 + 32 + 32 + 4 + 32 + 4 + 8 * 222);
    state.skip(16); // Major event / resupply / repair / reinforcement clocks.
    state.skip(14); // Timestamp, group, force ratios, brief.
    result.width = static_cast<std::uint16_t>(state.number(2));
    result.height = static_cast<std::uint16_t>(state.number(2));
    result.current_day = state.number(1);
    result.active_teams = state.number(1);
    state.skip(10); // Day/conditions/experience/bullseye fields.
    result.theater = state.text(40);
    result.scenario = state.text(40);
    state.skip(40); // Saved filename.
    result.title = state.text(40);
    if (!result.width || !result.height || result.theater.empty())
        throw std::runtime_error("invalid campaign metadata");
    Reader units{section("uni")};
    if (units.number(4) != units.bytes.size() - 4)
        throw std::runtime_error("unit section size mismatch");
    result.units = static_cast<std::uint16_t>(units.number(2));
    const auto unit_size = units.number(4);
    Expand(units.bytes, units.pos, unit_size);
    if (!result.units || result.units > 32767 || !unit_size)
        throw std::runtime_error("invalid recorded unit count");
    Reader objectives{section("obj")};
    result.objectives = static_cast<std::uint16_t>(objectives.number(2));
    const auto objective_size = objectives.number(4);
    if (objectives.number(4) != objectives.bytes.size() - 10)
        throw std::runtime_error("objective section size mismatch");
    Expand(objectives.bytes, objectives.pos, objective_size);
    if (!result.objectives || result.objectives > 32767 || !objective_size)
        throw std::runtime_error("invalid recorded objective count");
    Reader deltas{section("obd")};
    if (deltas.number(4) != deltas.bytes.size() - 4)
        throw std::runtime_error("objective delta section size mismatch");
    const auto delta_count = deltas.number(2);
    const auto delta_size = deltas.number(4);
    Expand(deltas.bytes, deltas.pos, delta_size);
    if ((delta_count == 0) != (delta_size == 0)) throw std::runtime_error("inconsistent objective deltas");
    section("tea");
    return result;
}

std::string JsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32 || c >= 127) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}

std::string ToJson(const ScenarioInfo& info) {
    std::ostringstream out;
    out << "{\"status\":\"inspected\",\"simulation_advanced\":false,\"format_version\":" << info.version
        << ",\"campaign_time_ms\":" << info.time_ms << ",\"theater\":" << JsonString(info.theater)
        << ",\"scenario\":" << JsonString(info.scenario) << ",\"title\":" << JsonString(info.title)
        << ",\"map_width\":" << info.width << ",\"map_height\":" << info.height
        << ",\"current_day\":" << info.current_day << ",\"active_teams\":" << info.active_teams
        << ",\"recorded_unit_count\":" << info.units << ",\"recorded_objective_count\":" << info.objectives
        << ",\"section_count\":" << info.sections.size() << '}';
    return out.str();
}
}
