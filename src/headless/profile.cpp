#include "profile.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace ff_headless {
namespace {
const char* const names[PROFILE_SECTION_COUNT] = {
    "host.campaign_loop", "host.update_real_units", "host.vu_thread", "host.main_thread",
    "host.unit_capture", "host.conquest_log", "host.watch_wait", "host.watch_publish",
    "host.detailed_frame", "host.detailed_reconcile",
    "loop.startup", "loop.stage1_day_reinforce_stats", "loop.stage4_supply", "loop.stage4_plan_ground",
    "loop.stage7_order_ground", "loop.stage10_repair_produce", "loop.stage10_supply",
    "loop.standard_rebuild", "loop.check_triggers", "loop.rebuild_atm_lists", "loop.atm_calculations",
    "loop.select_air_actions", "loop.update_parent_units", "loop.divert_status", "loop.weather",
    "loop.atm_task",
    "units.flight", "units.battalion", "units.task_force", "units.other",
};
struct Totals { unsigned long long calls = 0; long long nanoseconds = 0, maxNanoseconds = 0; };
Totals totals[PROFILE_SECTION_COUNT];
size_t snapshotBytes = 0;
const auto processStart = std::chrono::steady_clock::now();

// Wide lookup: the path may contain non-ASCII characters outside the ANSI code page.
std::filesystem::path profilePath() {
    const wchar_t* value = _wgetenv(L"FF_HEADLESS_PROFILE");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}
}

bool profileEnabled = !profilePath().empty();

void ProfileAdd(ProfileSection section, long long nanoseconds) {
    auto& t = totals[section];
    ++t.calls;
    t.nanoseconds += nanoseconds;
    if (nanoseconds > t.maxNanoseconds) t.maxNanoseconds = nanoseconds;
}

void ProfileSnapshotBytes(size_t bytes) {
    if (bytes > snapshotBytes) snapshotBytes = bytes;
}

void ProfileWrite(unsigned long campaignMinutes, unsigned long long steps) {
    if (!profileEnabled) return;
    const auto path = profilePath();
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - processStart).count();
        out << "{\"wall_seconds\":" << wall << ",\"campaign_minutes\":" << campaignMinutes << ",\"steps\":" << steps
            << ",\"max_snapshot_bytes\":" << snapshotBytes << ",\"sections\":{";
        for (int s = 0; s < PROFILE_SECTION_COUNT; ++s) {
            const auto& t = totals[s];
            out << (s ? "," : "") << '"' << names[s] << "\":{\"calls\":" << t.calls << ",\"seconds\":" << t.nanoseconds * 1e-9
                << ",\"max_ms\":" << t.maxNanoseconds * 1e-6 << '}';
        }
        out << "}}\n";
    }
    std::error_code ignored;
    std::filesystem::rename(temp, path, ignored);
}
}
