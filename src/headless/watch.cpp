#include "stdhdr.h"
#include "f4vu.h"
#include "entity.h"
#include "classtbl.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "unit.h"
#include "flight.h"
#include "objectiv.h"
#include "vehicle.h"
#include "feature.h"
#include "team.h"
#include "campterr.h"
#include "campwp.h"
#include "scenario.h"
#include "boundary.h"
#include "watch.h"
#include <sstream>
#include <fstream>
#include <thread>
#include <map>

namespace {
std::string id(VU_ID value) {
    return std::to_string(value.creator_) + ":" + std::to_string(value.num_);
}
// FF6 text tables use legacy Western text, independent of host Windows locale.
std::string text(const char* value, size_t capacity = 256) {
    if (!value) return "\"\"";
    int n = static_cast<int>(strnlen(value, capacity));
    if (!n) return "\"\"";
    std::wstring wide(MultiByteToWideChar(1252, 0, value, n, nullptr, 0), 0);
    MultiByteToWideChar(1252, 0, value, n, wide.data(), static_cast<int>(wide.size()));
    std::string utf8(WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return ff::headless::JsonString(utf8);
}
void common(std::ostream& out, CampEntity e) {
    short x, y; e->GetLocation(&x, &y);
    out << "\"id\":\"" << id(e->Id()) << "\",\"x\":" << x << ",\"y\":" << y
        << ",\"team\":" << int(e->GetTeam()) << ",\"owner\":" << int(e->GetOwner());
}
}

CampaignWatch::CampaignWatch(const std::filesystem::path& path) : directory(path) {
    std::filesystem::create_directories(directory);
    // Keep diagnostics in the session; never open a second console window.
    _wfreopen((directory / "engine.log").c_str(), L"w", stderr);
    _wfreopen((directory / "report.json").c_str(), L"w", stdout);
}

void CampaignWatch::write(const char* name, const std::string& contents) {
    auto target = directory / name;
    auto temp = target; temp += ".tmp";
    { std::ofstream out(temp, std::ios::binary); out << contents; out.close();
      if (!out) throw std::runtime_error("Observer file write failed"); }
    // A short reader may temporarily hold the old file on Windows.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("Observer file replacement failed");
}

void CampaignWatch::initialize(const char* scenario) {
    std::ostringstream out;
    out << "{\"schema\":1,\"scenario\":" << text(scenario) << ",\"width\":" << Map_Max_X
        << ",\"height\":" << Map_Max_Y << ",\"cell_km\":1,\"teams\":[";
    for (int i = 0; i < NUM_TEAMS; ++i) {
        if (i) out << ',';
        out << "{\"id\":" << i << ",\"name\":" << text(TeamInfo[i]->GetName()) << '}';
    }
    out << "],\"vehicles\":{";
    bool comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_VEHICLE || !Falcon4ClassTable[i].dataPtr) continue;
        auto* v = GetVehicleClassData(i);
        if (comma) out << ','; comma = true;
        out << '"' << i << "\":{\"name\":" << text(v->Name, sizeof(v->Name)) << ",\"max_speed_kph\":" << v->MaxSpeed << ",\"weapons\":[";
        bool wc = false;
        for (int hp = 0; hp < HARDPOINT_MAX; ++hp) {
            // 255 means a menu of compatible weapons, not an installed weapon.
            if (v->Weapon[hp] <= 0 || !v->Weapons[hp] || v->Weapons[hp] == 255) continue;
            if (wc) out << ','; wc = true;
            out << "{\"id\":" << v->Weapon[hp] << ",\"nominal_shots\":" << int(v->Weapons[hp]) << '}';
        }
        out << "]}";
    }
    out << "},\"weapons\":{"; comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_WEAPON || !Falcon4ClassTable[i].dataPtr) continue;
        auto* w = static_cast<WeaponClassDataType*>(Falcon4ClassTable[i].dataPtr);
        if (comma) out << ','; comma = true;
        out << '"' << (w - WeaponDataTable) << "\":{\"name\":" << text(w->Name, sizeof(w->Name)) << ",\"range_km\":" << w->Range << '}';
    }
    out << "},\"objectives\":["; comma = false;
    VuListIterator it(AllObjList);
    for (Objective o = static_cast<Objective>(it.GetFirst()); o; o = static_cast<Objective>(it.GetNext())) {
        if (comma) out << ','; comma = true;
        char name[256]{}; o->GetName(name, 255, 0);
        out << '{'; common(out, o);
        out << ",\"name\":" << text(name) << ",\"class\":" << text(o->GetObjectiveClassData()->Name, 20) << ",\"features\":[";
        for (int f = 0; f < o->GetTotalFeatures(); ++f) {
            if (f) out << ',';
            int index = o->GetFeatureID(f);
            auto* feature = index > 0 && index < NumEntities && Falcon4ClassTable[index].dataType == DTYPE_FEATURE ? GetFeatureClassData(index) : nullptr;
            // ObjectiveClass::Deaggregate uses (&simY, &simX): the first
            // stored offset is east, the second north (find.cpp grid mapping).
            float north = 0, east = 0, z = 0; o->GetFeatureOffset(f, &east, &north, &z);
            out << "{\"name\":" << (feature ? text(feature->Name, 20) : "\"Feature\"")
                << ",\"dx\":" << east * 0.0003048 << ",\"dy\":" << north * 0.0003048 << '}';
        }
        out << "]}";
    }
    out << "]}";
    write("catalog.json", out.str());
    // Native grid: x=east, y=north. Preserve each cell, including roads/rail.
    std::string cells;
    cells.reserve(static_cast<size_t>(Map_Max_X) * Map_Max_Y);
    for (int y = 0; y < Map_Max_Y; ++y)
        for (int x = 0; x < Map_Max_X; ++x)
            cells.push_back(static_cast<char>(int(GetCover(x, y)) | (int(GetRelief(x, y)) << 4)
                | (GetRoad(x, y) ? 64 : 0) | (GetRail(x, y) ? 128 : 0)));
    write("terrain.bin", cells);
    last = heartbeat = Clock::now();
    publish();
}

void CampaignWatch::command() {
    std::ifstream in(directory / "control.txt");
    unsigned long long seq, steps;
    int rate, pause, stop;
    if (!(in >> seq >> rate >> pause >> steps >> stop) || seq <= sequence) return;
    if ((rate != 1 && rate != 5 && rate != 20 && rate != 100) || pause < 0 || pause > 1 || stop < 0 || stop > 1 || steps < requestedSteps || steps - requestedSteps > 100) return;
    if (speed != rate || paused != bool(pause)) credit = 0;
    sequence = seq; speed = rate; paused = pause != 0; requestedSteps = steps; stopping = stop != 0;
    heartbeat = Clock::now();
}

bool CampaignWatch::waitForStep() {
    for (;;) {
        command();
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - last).count(); last = now;
        if (stopping || now - heartbeat > std::chrono::seconds(20)) { publish("stopped"); return false; }
        if (now - sent >= std::chrono::milliseconds(500)) publish();
        if (paused) {
            credit = 0;
            if (consumedSteps < requestedSteps) { ++consumedSteps; return true; }
        } else {
            consumedSteps = requestedSteps;
            credit += (elapsed < 0.5 ? elapsed : 0.5) * speed;
            if (credit >= 5) { credit -= 5; return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void CampaignWatch::publish(const char* status) {
    std::ostringstream out;
    out << "{\"schema\":1,\"frame\":" << ++frame << ",\"status\":" << text(status)
        << ",\"time_ms\":" << TheCampaign.CurrentTime << ",\"ack\":" << sequence
        << ",\"speed\":" << speed << ",\"paused\":" << (paused ? "true" : "false")
        << ",\"combat_messages\":" << ff_headless::combat.engagements << ",\"reported_losses\":" << ff_headless::combat.losses << ",\"units\":[";
    bool comma = false;
    VuListIterator it(AllUnitList);
    for (Unit u = static_cast<Unit>(it.GetFirst()); u; u = static_cast<Unit>(it.GetNext())) {
        if (comma) out << ','; comma = true;
        char name[256]{}; u->GetName(name, 255, 0);
        short dx, dy; u->GetUnitDestination(&dx, &dy);
        const char* kind = u->IsFlight() ? "flight" : u->IsSquadron() ? "squadron" : u->IsBattalion() ? "battalion" : u->IsTaskForce() ? "naval" : "formation";
        out << '{'; common(out, u);
        out << ",\"name\":" << text(name) << ",\"class\":" << text(u->GetUnitClassName(), 20)
            << ",\"kind\":" << text(kind) << ",\"domain\":" << int(u->GetDomain())
            << ",\"vehicles\":" << u->GetTotalVehicles() << ",\"supply\":" << u->GetUnitSupply()
            << ",\"morale\":" << u->GetUnitMorale() << ",\"orders\":" << u->GetUnitOrders()
            << ",\"mission\":" << int(u->GetUnitMission()) << ",\"destination\":[" << dx << ',' << dy << "],\"equipment\":[";
        std::map<int, int> equipment;
        if (u->IsFlight() || u->IsSquadron()) {
            // Air-unit roster slots represent aircraft availability, not
            // different vehicle classes. Native flight loading uses type 0.
            equipment[u->GetVehicleID(0)] = u->GetTotalVehicles();
        } else if (!u->Father()) {
            int groups = TeamInfo[u->GetTeam()]->max_vehicle[u->GetRClass()];
            for (int g = 0; g < groups && g < VEHICLE_GROUPS_PER_UNIT; ++g)
                if (u->GetNumVehicles(g)) equipment[u->GetVehicleID(g)] += u->GetNumVehicles(g);
        }
        bool ec = false;
        for (const auto& item : equipment) {
            if (!item.second) continue;
            if (ec) out << ','; ec = true;
            out << "{\"id\":" << item.first << ",\"count\":" << item.second << '}';
        }
        out << "],\"loadouts\":[";
        if (u->IsFlight()) {
            auto* flight = static_cast<FlightClass*>(u);
            for (int a = 0; a < flight->GetLoadouts(); ++a) {
                if (a) out << ',';
                out << '['; bool wc = false;
                const auto& loadout = flight->GetLoadout()[a];
                for (int hp = 0; hp < HARDPOINT_MAX; ++hp) {
                    if (!loadout.WeaponID[hp] || !loadout.WeaponCount[hp]) continue;
                    if (wc) out << ','; wc = true;
                    out << "{\"id\":" << loadout.WeaponID[hp] << ",\"count\":" << int(loadout.WeaponCount[hp]) << '}';
                }
                out << ']';
            }
        }
        out << "],\"route\":[";
        int n = 0;
        for (WayPoint wp = u->GetCurrentUnitWP(); wp && n < 64; wp = wp->GetNextWP(), ++n) {
            short x, y; wp->GetWPLocation(&x, &y);
            if (n) out << ','; out << '[' << x << ',' << y << ']';
        }
        out << "]}";
    }
    out << "],\"objectives\":["; comma = false;
    VuListIterator oi(AllObjList);
    for (Objective o = static_cast<Objective>(oi.GetFirst()); o; o = static_cast<Objective>(oi.GetNext())) {
        if (comma) out << ','; comma = true;
        out << "{\"id\":\"" << id(o->Id()) << "\",\"team\":" << int(o->GetTeam()) << ",\"status\":" << int(o->GetObjectiveStatus())
            << ",\"supply\":" << o->GetObjectiveSupply() << ",\"fuel\":" << o->GetObjectiveFuel() << ",\"feature_status\":[";
        for (int f = 0; f < o->GetTotalFeatures(); ++f) { if (f) out << ','; out << o->GetFeatureStatus(f); }
        out << "]}";
    }
    out << "]}";
    write("state.json", out.str());
    sent = Clock::now();
}

void CampaignWatch::error(const char* message) {
    write("state.json", "{\"status\":\"error\",\"error\":" + ff::headless::JsonString(message) + "}");
}
