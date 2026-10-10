// Phase 2 experiment (wargame migration/ADR-0001 section 4): run one detailed battle from a
// checkpoint as a "battle service" and measure how cleanly its result separates from the
// legacy campaign. Experimental; not used by the observer.
//
//   battle <data-root> <checkpoint-dir> <x|auto> <y> <radius-km> <seconds> <frozen|live> <out-dir> [seed]
//
// frozen: only detailed frames, VU message pumping and region membership run; the strategic
//         schedule (DoCampaignLoop) and aggregate unit updates (UpdateRealUnits) do not.
// live:   the observer's behaviour, where the strategic campaign keeps running.
// Writes <out-dir>/result.json (result diff plus isolation audit) and <out-dir>/after (checkpoint).
#include "stdhdr.h"
#include "f4vu.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "team.h"
#include "unit.h"
#include "objectiv.h"
#include "falcsess.h"
#include "boundary.h"
#include "detailed.h"
#include "watch.h"
#include "battle_service.h"
#include "scenario.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

extern void DoCampaignLoop(int);
extern void UpdateRealUnits(CampaignTime);

namespace {
constexpr int kFramesPerTick = 250;     // 20 ms native frames per 5 s campaign tick
constexpr int kDrainFrameLimit = 15000; // 300 s for weapons and pilots in flight to finish

// Fields a campaign core would receive from a battle, plus the native serialized record.
struct Entity {
    bool unit = false;
    int team = 0;
    short x = 0, y = 0;
    int vehicles = 0, supply = 0, orders = 0, owner = 0, status = 0, fuel = 0;
    bool dead = false;
    std::vector<int> features;
    std::vector<unsigned char> record;
};
using World = std::map<unsigned long long, Entity>;

unsigned long long key(VU_ID id) { return (static_cast<unsigned long long>(id.creator_) << 32) | id.num_; }
std::string text(unsigned long long k) { return std::to_string(k >> 32) + ":" + std::to_string(k & 0xffffffffull); }

std::vector<unsigned char> serialize(CampBaseClass* e) {
    const int size = e->SaveSize();
    std::vector<unsigned char> bytes(static_cast<size_t>(size) * 2 + 4096);
    VU_BYTE* cursor = bytes.data();
    e->Save(&cursor);
    const auto written = static_cast<size_t>(cursor - bytes.data());
    if (written > static_cast<size_t>(size)) throw std::runtime_error("Native record exceeded its SaveSize");
    bytes.resize(written);
    return bytes;
}

World capture() {
    if (DetailedRegionActive()) throw std::runtime_error("Battle audit requires fully aggregated state");
    World world;
    VuListIterator units(AllUnitList);
    for (auto* e = units.GetFirst(); e; e = units.GetNext()) {
        auto* u = static_cast<UnitClass*>(e);
        Entity s; s.unit = true; s.team = u->GetTeam(); u->GetLocation(&s.x, &s.y);
        s.vehicles = u->GetTotalVehicles(); s.supply = u->GetUnitSupply(); s.orders = u->GetUnitOrders(); s.dead = u->IsDead();
        s.record = serialize(u);
        world.emplace(key(u->Id()), std::move(s));
    }
    VuListIterator objectives(AllObjList);
    for (auto* e = objectives.GetFirst(); e; e = objectives.GetNext()) {
        auto* o = static_cast<ObjectiveClass*>(e);
        Entity s; s.team = o->GetTeam(); o->GetLocation(&s.x, &s.y);
        s.owner = o->GetOwner(); s.status = o->GetObjectiveStatus(); s.supply = o->GetObjectiveSupply(); s.fuel = o->GetObjectiveFuel();
        for (int f = 0; f < o->GetTotalFeatures(); ++f) s.features.push_back(o->GetFeatureStatus(f));
        s.record = serialize(o);
        world.emplace(key(o->Id()), std::move(s));
    }
    return world;
}

// Closest hostile pair of live aggregate battalions; the region is centred on the first.
void chooseRegion(double& x, double& y, double& radius) {
    std::vector<UnitClass*> battalions;
    VuListIterator units(AllUnitList);
    for (auto* e = units.GetFirst(); e; e = units.GetNext()) {
        auto* u = static_cast<UnitClass*>(e);
        if (u->IsBattalion() && u->Real() && u->IsAggregate() && !u->IsDead() && !u->Inactive() && u->GetTotalVehicles() > 0) battalions.push_back(u);
    }
    double best = 1e30;
    for (auto* a : battalions) for (auto* b : battalions) {
        if (GetRoE(a->GetTeam(), b->GetTeam(), ROE_GROUND_FIRE) != ROE_ALLOWED) continue;
        short ax, ay, bx, by; a->GetLocation(&ax, &ay); b->GetLocation(&bx, &by);
        const double d = std::hypot(double(ax - bx), double(ay - by));
        if (d < best) { best = d; x = ax; y = ay; }
    }
    if (best > 1e29) throw std::runtime_error("No hostile battalion pair for an automatic battle region");
    // Whole metres, so the region prints and re-parses exactly.
    radius = std::round((std::min)(20.0, (std::max)(2.0, best + 2.0)) * 1000.0) / 1000.0;
}

struct Counters { unsigned long long engagements, losses, weapons, damage, missileEnds; };
Counters counters() {
    const auto& c = ff_headless::combat;
    return {c.engagements, c.losses, c.weaponsFired, c.detailedDamageMessages, c.missileEnds};
}

void pump() {
    TheCampaign.vuThread->Update(-1);
    gMainThread->Update(-1);
}

void tick(bool live) {
    if (live) { DoCampaignLoop(0); UpdateRealUnits(5 * CampaignSeconds); }
    pump();
    ReconcileDetailedRegion();
}

bool inside(const Entity& e, double x, double y, double radius) {
    return std::hypot(e.x - x, e.y - y) <= radius;
}

// Explicit result fields that differ; empty when only the native record changed.
std::vector<const char*> changedFields(const Entity& a, const Entity& b) {
    std::vector<const char*> fields;
    if (a.x != b.x || a.y != b.y) fields.push_back("location");
    if (a.vehicles != b.vehicles) fields.push_back("vehicles");
    if (a.supply != b.supply) fields.push_back("supply");
    if (a.orders != b.orders) fields.push_back("orders");
    if (a.dead != b.dead) fields.push_back("dead");
    if (a.owner != b.owner) fields.push_back("owner");
    if (a.status != b.status) fields.push_back("status");
    if (a.fuel != b.fuel) fields.push_back("fuel");
    if (a.features != b.features) fields.push_back("features");
    return fields;
}
}

void RunBattleService(const std::vector<std::string>& args) {
    double x = 0, y = 0, radius = 0;
    const bool automatic = args[4] == "auto";
    if (!automatic) { x = std::stod(args[4]); y = std::stod(args[5]); radius = std::stod(args[6]); }
    const int seconds = std::stoi(args[7]);
    if (seconds < 5 || seconds > 3600) throw std::runtime_error("Battle duration must be 5..3600 seconds");
    if (args[8] != "frozen" && args[8] != "live") throw std::runtime_error("Battle mode must be frozen or live");
    const bool live = args[8] == "live";
    const auto out = std::filesystem::absolute(std::filesystem::u8path(args[9]));
    if (std::filesystem::exists(out)) throw std::runtime_error("Battle output directory must not exist");
    if (automatic) chooseRegion(x, y, radius);
    if (!std::isfinite(x) || !std::isfinite(y) || radius < 2 || radius > 20) throw std::runtime_error("Invalid battle region");
    std::filesystem::create_directories(out);

    const CampaignTime startTime = TheCampaign.CurrentTime;
    const World before = capture();
    const Counters startCounters = counters();
    int participants = 0;
    for (const auto& [k, e] : before) participants += e.unit && !e.dead && e.vehicles > 0 && inside(e, x, y, radius);

    SetDetailedRegion(true, x, y, radius);
    const int battleFrames = seconds * 50;
    for (int frame = 1; frame <= battleFrames; ++frame) {
        AdvanceDetailedFrame();
        if (frame % kFramesPerTick == 0) tick(live);
    }
    SetDetailedRegion(false, x, y, radius);
    int drainFrames = 0;
    while (DetailedRegionActive() && drainFrames < kDrainFrameLimit) {
        AdvanceDetailedFrame();
        if (++drainFrames % kFramesPerTick == 0) tick(live);
    }
    pump();
    if (DetailedRegionActive()) throw std::runtime_error("Detailed region did not drain within the frame limit");
    const World after = capture();
    const Counters endCounters = counters();
    SaveAggregateCheckpoint(out / "after");

    // Diff and isolation audit.
    std::ostringstream diff;
    int insideChanged = 0, outsideChanged = 0, hidden = 0, created = 0, removed = 0;
    std::vector<std::string> outsideIds, hiddenIds;
    bool comma = false;
    diff << '[';
    for (const auto& [k, b] : after) {
        const auto a = before.find(k);
        if (a == before.end()) {
            ++created;
            diff << (comma ? "," : "") << "{\"id\":\"" << text(k) << "\",\"change\":\"created\",\"unit\":" << (b.unit ? "true" : "false") << '}';
            comma = true;
            continue;
        }
        const auto fields = changedFields(a->second, b);
        const bool recordChanged = a->second.record != b.record;
        if (fields.empty() && !recordChanged) continue;
        const bool in = inside(a->second, x, y, radius);
        (in ? insideChanged : outsideChanged)++;
        if (!in && outsideIds.size() < 50) outsideIds.push_back(text(k));
        if (fields.empty()) { ++hidden; if (hiddenIds.size() < 50) hiddenIds.push_back(text(k)); }
        diff << (comma ? "," : "") << "{\"id\":\"" << text(k) << "\",\"unit\":" << (b.unit ? "true" : "false")
             << ",\"inside\":" << (in ? "true" : "false") << ",\"fields\":[";
        comma = true;
        for (size_t i = 0; i < fields.size(); ++i) diff << (i ? "," : "") << '"' << fields[i] << '"';
        diff << "],\"record_bytes\":" << a->second.record.size() << ",\"changed_offsets\":[";
        for (size_t i = 0, n = 0; i < (std::min)(a->second.record.size(), b.record.size()) && n < 32; ++i)
            if (a->second.record[i] != b.record[i]) diff << (n++ ? "," : "") << i;
        diff << "],\"before\":{\"x\":" << a->second.x << ",\"y\":" << a->second.y << ",\"vehicles\":" << a->second.vehicles
             << ",\"supply\":" << a->second.supply << ",\"owner\":" << a->second.owner << ",\"dead\":" << (a->second.dead ? "true" : "false")
             << "},\"after\":{\"x\":" << b.x << ",\"y\":" << b.y << ",\"vehicles\":" << b.vehicles << ",\"supply\":" << b.supply
             << ",\"owner\":" << b.owner << ",\"dead\":" << (b.dead ? "true" : "false") << "}}";
    }
    for (const auto& [k, a] : before) {
        if (after.count(k)) continue;
        ++removed;
        diff << (comma ? "," : "") << "{\"id\":\"" << text(k) << "\",\"change\":\"removed\",\"unit\":" << (a.unit ? "true" : "false")
             << ",\"inside\":" << (inside(a, x, y, radius) ? "true" : "false") << '}';
        comma = true;
    }
    diff << ']';
    auto list = [](const std::vector<std::string>& ids) {
        std::string s = "[";
        for (size_t i = 0; i < ids.size(); ++i) s += (i ? ",\"" : "\"") + ids[i] + "\"";
        return s + "]";
    };
    std::ofstream result(out / "result.json", std::ios::binary);
    result.precision(17);  // round-trips every double
    result << "{\"status\":\"ok\",\"mode\":\"" << (live ? "live" : "frozen") << "\",\"region\":{\"x\":" << x << ",\"y\":" << y
           << ",\"radius_km\":" << radius << ",\"automatic\":" << (automatic ? "true" : "false") << "}"
           << ",\"battle_seconds\":" << seconds << ",\"drain_frames\":" << drainFrames
           << ",\"start_time_ms\":" << startTime << ",\"end_time_ms\":" << TheCampaign.CurrentTime
           << ",\"participant_units\":" << participants << ",\"entities_before\":" << before.size() << ",\"entities_after\":" << after.size()
           << ",\"combat\":{\"engagements\":" << endCounters.engagements - startCounters.engagements
           << ",\"losses\":" << endCounters.losses - startCounters.losses << ",\"weapons_fired\":" << endCounters.weapons - startCounters.weapons
           << ",\"damage_messages\":" << endCounters.damage - startCounters.damage << ",\"missile_ends\":" << endCounters.missileEnds - startCounters.missileEnds << "}"
           << ",\"audit\":{\"changed_inside\":" << insideChanged << ",\"changed_outside\":" << outsideChanged
           << ",\"record_only_changes\":" << hidden << ",\"created\":" << created << ",\"removed\":" << removed
           << ",\"outside_ids\":" << list(outsideIds) << ",\"record_only_ids\":" << list(hiddenIds) << "}"
           << ",\"diff\":" << diff.str() << "}\n";
    if (!result) throw std::runtime_error("Cannot write battle result");
    std::cout << "{\"status\":\"ok\",\"mode\":\"" << (live ? "live" : "frozen") << "\",\"participant_units\":" << participants
              << ",\"changed_inside\":" << insideChanged << ",\"changed_outside\":" << outsideChanged << ",\"record_only_changes\":" << hidden
              << ",\"created\":" << created << ",\"removed\":" << removed << ",\"losses\":" << endCounters.losses - startCounters.losses
              << ",\"weapons_fired\":" << endCounters.weapons - startCounters.weapons << ",\"result\":" << ff::headless::JsonString(out.u8string() + "/result.json") << "}" << std::endl;
}
