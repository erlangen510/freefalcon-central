#pragma once
// Opt-in wall-clock section timers for the headless host (FF_HEADLESS_PROFILE=<file.json>).
// Timing only: no campaign state is read or written, so enabling it cannot change results.
#include <chrono>

namespace ff_headless {
enum ProfileSection {
    // Host loop (engine_host.cpp)
    PROFILE_CAMPAIGN_LOOP, PROFILE_UPDATE_REAL_UNITS, PROFILE_VU_THREAD, PROFILE_MAIN_THREAD,
    PROFILE_HOST_UNIT_CAPTURE, PROFILE_CONQUEST_LOG, PROFILE_WATCH_WAIT, PROFILE_WATCH_PUBLISH,
    PROFILE_DETAILED_FRAME, PROFILE_DETAILED_RECONCILE,
    // DoCampaignLoop (campaign.cpp)
    PROFILE_STARTUP, PROFILE_STAGE1_DAY_REINFORCE_STATS, PROFILE_STAGE4_SUPPLY, PROFILE_STAGE4_PLAN_GROUND,
    PROFILE_STAGE7_ORDER_GROUND, PROFILE_STAGE10_REPAIR_PRODUCE, PROFILE_STAGE10_SUPPLY,
    PROFILE_STANDARD_REBUILD, PROFILE_CHECK_TRIGGERS, PROFILE_REBUILD_ATM_LISTS, PROFILE_ATM_CALCULATIONS,
    PROFILE_SELECT_AIR_ACTIONS, PROFILE_UPDATE_PARENT_UNITS, PROFILE_DIVERT_STATUS, PROFILE_WEATHER,
    PROFILE_ATM_TASK,
    // UpdateRealUnits -> UpdateUnit, by unit kind
    PROFILE_UNITS_FLIGHT, PROFILE_UNITS_BATTALION, PROFILE_UNITS_TASK_FORCE, PROFILE_UNITS_OTHER,
    PROFILE_SECTION_COUNT
};
extern bool profileEnabled;
void ProfileAdd(ProfileSection section, long long nanoseconds);
// Largest state.json written so far, in bytes.
void ProfileSnapshotBytes(size_t bytes);
// Writes the accumulated profile atomically; a no-op unless profiling is enabled.
void ProfileWrite(unsigned long campaignMinutes, unsigned long long steps);

class ProfileScope {
public:
    explicit ProfileScope(ProfileSection section) : section(section) {
        if (profileEnabled) start = std::chrono::steady_clock::now();
    }
    ~ProfileScope() {
        if (profileEnabled) ProfileAdd(section, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
private:
    ProfileSection section;
    std::chrono::steady_clock::time_point start;
};
}

#define FF_PROFILE_JOIN2(a, b) a##b
#define FF_PROFILE_JOIN(a, b) FF_PROFILE_JOIN2(a, b)
#define FF_PROFILE(section) ff_headless::ProfileScope FF_PROFILE_JOIN(ffProfileScope, __LINE__)(ff_headless::section)
// Same, for a section chosen at run time.
#define FF_PROFILE_KIND(expression) ff_headless::ProfileScope FF_PROFILE_JOIN(ffProfileScope, __LINE__)(expression)
