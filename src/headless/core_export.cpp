// Scenario export for the independent campaign core (migration/ADR-0001, sim/).
//
// export-map writes core-data.json next to catalog.json: the class tables, tactics, AI inputs and
// the entity state that the core's ported rules read, exactly as loaded and before any campaign
// time. It is an interchange format only; nothing in the legacy host reads it back.
#include "stdhdr.h"
#include "f4vu.h"
#include "entity.h"
#include "classtbl.h"
#include "campaign.h"
#include "cmpclass.h"
#include "camplist.h"
#include "unit.h"
#include "battalion.h"
#include "brigade.h"
#include "squadron.h"
#include "objectiv.h"
#include "feature.h"
#include "vehicle.h"
#include "team.h"
#include "atm.h"
#include "mission.h"
#include "pilot.h"
#include "gtm.h"
#include "gtmobj.h"
#include "tactics.h"
#include "cmpevent.h"
#include "aiinput.h"
#include "campwp.h"
#include "scenario.h"
#include "falcsess.h"
#include "weaplist.h"
#include "playerop.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <set>
#include <vector>
#include <cstdio>

extern int VisualDetectionRange[OtherDam];
extern uchar RoEData[ROE_NAVAL_BOMBARD + 1][War + 1];
extern int OrderPriority[GORD_LAST];
extern uchar DefaultDamageMods[OtherDam + 1];
extern EventClass** CampEvents;
extern short CE_Events;
extern SquadronStoresDataType* SquadronStoresDataTable;
extern bool g_bPowerGrid;
extern int g_npercentage_available_aircraft;
extern int g_nminimum_available_aircraft;
extern float g_fClimbRatio;
extern int g_nNoWPRefuelNeeded;
extern bool g_bAddIngressWP;
extern bool g_bTankerWaypoints;
extern bool g_bLargeStrike;
extern bool g_bOldSamActivity;
extern bool g_bPrecisionWaypoints;
extern bool g_bFireOntheMove;
extern bool g_bRealisticAttrition;
extern bool g_bLoadoutSquadStoreResupply;
extern int g_nFlightVisualBonus;
extern float g_fIdentFactor;
extern int ALT_TO_BUILD_RANGES_TO;
extern int g_nNoPlayerPlay;

extern short NumSquadTypes;

// Falcon4.AII inputs defined in aiinput.cpp that no header declares here.
extern short IMMEDIATE_MIN_TIME;
extern short IMMEDIATE_MAX_TIME;
extern short LONGRANGE_MIN_TIME;
extern short LONGRANGE_MAX_TIME;
extern short ATM_ASSIGN_RATIO;
extern short MAX_FLYMISSION_THREAT;
extern short MAX_FLYMISSION_HIGHTHREAT;
extern short MAX_FLYMISSION_NOTHREAT;
extern short MIN_SEADESCORT_THREAT;
extern short MIN_AVOID_THREAT;
extern short MIN_AP_DISTANCE;
extern short BREAKPOINT_DISTANCE;
extern short ATM_TARGET_CLEAR_RADIUS;
extern short LOITER_DIST;
extern short SWEEP_DISTANCE;
extern short MINIMUM_BDA_PRIORITY;
extern short MINIMUM_AWACS_DISTANCE;
extern short MAXIMUM_AWACS_DISTANCE;
extern short MINIMUM_JSTAR_DISTANCE;
extern short MAXIMUM_JSTAR_DISTANCE;
extern short MINIMUM_TANKER_DISTANCE;
extern short MAXIMUM_TANKER_DISTANCE;
extern short MINIMUM_ECM_DISTANCE;
extern short MAXIMUM_ECM_DISTANCE;
extern short FIRST_COLONEL;
extern short FIRST_COMMANDER;
extern short FIRST_WINGMAN;
extern short LAST_WINGMAN;
extern short MAX_AA_STR_FOR_ESCORT;
extern short ONCALL_CAS_FLIGHTS_PER_REQUEST;
extern short MIN_TASK_AIR;
extern short MIN_PLAN_AIR;
extern short VICTORY_CHECK_TIME;
extern short FLIGHT_COMBAT_CHECK_INTERVAL;
extern short AIR_UPDATE_CHECK_INTERVAL;
extern short FLIGHT_COMBAT_RATE;
extern short PACKAGE_CYCLES_TO_WAIT;
extern short AIR_PATH_MAX;
extern float MIN_IGNORE_RANGE;
extern short MAX_SAR_DIST;
extern long PILOT_ASSIGN_TIME;
extern short AIRCRAFT_TURNAROUND_TIME_MINUTES;
extern short MAX_OFFENSES;
extern short MIN_OFFENSIVE_UNITS;
extern short MINIMUM_ADJUSTED_LEVEL;
extern short MINIMUM_VIABLE_LEVEL;
extern short MAX_ASSIGNMENT_DIST;
extern short BRIGADE_BREAK_BASE;
extern short ROLE_SCORE_MODIFIER;
extern short MIN_REPAIR_OBJECTIVES;
extern short MINIMUM_EXP_TO_FIRE_PREGUIDE;
extern short MIN_TASK_NAVAL;
extern short MIN_PLAN_NAVAL;
extern short NAVAL_COMBAT_CHECK_INTERVAL;
extern short NAVAL_COMBAT_RATE;
extern short LOW_ALTITUDE_CUTOFF;
extern short NEW_CLOUD_CHANCE;
extern short DEL_CLOUD_CHANCE;
extern short PLAYER_BUBBLE_MOVERS;
extern short FALCON_PLAYER_TEAM;
extern short CAMP_RESYNC_TIME;
extern short STANDARD_EVENT_LENGTH;
extern short PRIORITY_EVENT_LENGTH;
extern float REAGREGATION_RATIO;
extern float SIM_BUBBLE_SIZE;
extern int F4_GENERIC_US_TRUCK_TYPE_SMALL;
extern int F4_GENERIC_US_TRUCK_TYPE_LARGE;
extern int F4_GENERIC_US_TRUCK_TYPE_TRAILER;
extern int F4_GENERIC_OPFOR_TRUCK_TYPE_SMALL;
extern int F4_GENERIC_OPFOR_TRUCK_TYPE_LARGE;
extern int F4_GENERIC_OPFOR_TRUCK_TYPE_TRAILER;
extern int SWITCH_TRACTORS_IN_TE;
extern float MIN_DEAD_PCT;
extern float FLOTDrawDistance;
extern int FLOTSortDirection;
extern int TheaterXPosition;
extern int TheaterYPosition;

namespace {

std::string id(VU_ID value) { return std::to_string(value.creator_) + ":" + std::to_string(value.num_); }
std::string quoted(VU_ID value) { return value == FalconNullId ? std::string("\"\"") : "\"" + id(value) + "\""; }

std::string name(const char* value, size_t capacity) {
    std::string s(value, strnlen(value, capacity));
    for (auto& c : s) if (static_cast<unsigned char>(c) >= 128) c = '?';
    return ff::headless::JsonString(s);
}

template <class T> void array(std::ostream& out, const T* values, int count) {
    out << '[';
    for (int i = 0; i < count; ++i) out << (i ? "," : "") << static_cast<long long>(values[i]);
    out << ']';
}

void classInfo(std::ostream& out, int index) {
    const auto& info = Falcon4ClassTable[index].vuClassData.classInfo_;
    out << "\"domain\":" << int(info[VU_DOMAIN]) << ",\"class\":" << int(info[VU_CLASS]) << ",\"type\":" << int(info[VU_TYPE])
        << ",\"stype\":" << int(info[VU_STYPE]) << ",\"sptype\":" << int(info[VU_SPTYPE]);
}

// ObjectiveClass::GetObjFlags is declared but never defined; rebuild the word from its accessors.
unsigned long objectiveFlags(Objective o) {
    unsigned long f = 0;
    if (o->IsFrontline()) f |= O_FRONTLINE;
    if (o->IsSecondline()) f |= O_SECONDLINE;
    if (o->IsThirdline()) f |= O_THIRDLINE;
    if (o->Jammed()) f |= O_JAMMED;
    if (o->IsBeach()) f |= O_BEACH;
    if (o->ManualSet()) f |= O_MANUAL_SET;
    if (o->MountainSite()) f |= O_MOUNTAIN_SITE;
    if (o->SamSite()) f |= O_SAM_SITE;
    if (o->ArtillerySite()) f |= O_ARTILLERY_SITE;
    if (o->AmbushCAPSite()) f |= O_AMBUSHCAP_SITE;
    if (o->BorderSite()) f |= O_BORDER_SITE;
    if (o->CommandoSite()) f |= O_COMMANDO_SITE;
    if (o->FlatSite()) f |= O_FLAT_SITE;
    if (o->RadarSite()) f |= O_RADAR_SITE;
    if (o->NeedRepair()) f |= O_NEED_REPAIR;
    if (o->Abandoned()) f |= O_ABANDONED;
    if (o->HasNCTR()) f |= O_HAS_NCTR;
    if (o->IsGCI()) f |= O_IS_GCI;
    return f;
}

void constants(std::ostream& out) {
    struct Item { const char* key; double value; };
    const Item items[] = {
        {"PRIMARY_OBJ_PRIORITY", PRIMARY_OBJ_PRIORITY}, {"SECONDARY_OBJ_PRIORITY", SECONDARY_OBJ_PRIORITY},
        {"MAXLINKS_FROM_SO_OFFENSIVE", MAXLINKS_FROM_SO_OFFENSIVE}, {"MAXLINKS_FROM_SO_DEFENSIVE", MAXLINKS_FROM_SO_DEFENSIVE},
        {"MIN_TASK_GROUND", MIN_TASK_GROUND}, {"MIN_PLAN_GROUND", MIN_PLAN_GROUND}, {"MIN_RESUPPLY", MIN_RESUPPLY},
        {"MORALE_REGAIN_RATE", MORALE_REGAIN_RATE}, {"REGAIN_RATE_MULTIPLIER_FOR_TE", REGAIN_RATE_MULTIPLIER_FOR_TE},
        {"FOOT_MOVE_CHECK_INTERVAL", FOOT_MOVE_CHECK_INTERVAL}, {"TRACKED_MOVE_CHECK_INTERVAL", TRACKED_MOVE_CHECK_INTERVAL},
        {"GROUND_COMBAT_CHECK_INTERVAL", GROUND_COMBAT_CHECK_INTERVAL}, {"GROUND_UPDATE_CHECK_INTERVAL", GROUND_UPDATE_CHECK_INTERVAL},
        {"GROUND_COMBAT_RATE", GROUND_COMBAT_RATE}, {"GROUND_PATH_MAX", GROUND_PATH_MAX},
        {"OBJ_GROUND_PATH_MAX_SEARCH", OBJ_GROUND_PATH_MAX_SEARCH}, {"OBJ_GROUND_PATH_MAX_COST", OBJ_GROUND_PATH_MAX_COST},
        {"MIN_FULL_OFFENSIVE_INITIATIVE", MIN_FULL_OFFENSIVE_INITIATIVE}, {"MIN_COUNTER_ATTACK_INITIATIVE", MIN_COUNTER_ATTACK_INITIATIVE},
        {"MIN_SECURE_INITIATIVE", MIN_SECURE_INITIATIVE}, {"ACTION_PREP_TIME", static_cast<double>(ACTION_PREP_TIME)},
        {"OFFENSIVE_REINFORCEMENT_TIME", static_cast<double>(OFFENSIVE_REINFORCEMENT_TIME)},
        {"DEFENSIVE_REINFORCEMENT_TIME", static_cast<double>(DEFENSIVE_REINFORCEMENT_TIME)},
        {"CONSOLIDATE_REINFORCEMENT_TIME", static_cast<double>(CONSOLIDATE_REINFORCEMENT_TIME)},
        {"NAVAL_MOVE_CHECK_INTERVAL", NAVAL_MOVE_CHECK_INTERVAL}, {"FLIGHT_MOVE_CHECK_INTERVAL", FLIGHT_MOVE_CHECK_INTERVAL},
        {"MAX_GROUND_SEARCH", MAX_GROUND_SEARCH}, {"MAX_AIR_SEARCH", MAX_AIR_SEARCH}, {"MAX_BAI_DIST", MAX_BAI_DIST},
        {"MINIMUM_STRENGTH", MINIMUM_STRENGTH}, {"MAX_DAMAGE_TRIES", MAX_DAMAGE_TRIES},
        {"MIN_REINFORCE_TIME", MIN_REINFORCE_TIME}, {"INITIATIVE_LEAK_PER_HOUR", INITIATIVE_LEAK_PER_HOUR},
        {"BARCAP_REQUEST_INTERVAL", BARCAP_REQUEST_INTERVAL}, {"MIN_RECALCULATE_STATISTICS", MIN_RECALCULATE_STATISTICS},
        {"StartOffBonusRepl", StartOffBonusRepl}, {"StartOffBonusSup", StartOffBonusSup}, {"StartOffBonusFuel", StartOffBonusFuel},
        {"ActionRate", ActionRate}, {"ActionTimeOut", ActionTimeOut}, {"DataRateModRepl", DataRateModRepl},
        {"DataRateModSup", DataRateModSup}, {"RelSquadBonus", RelSquadBonus}, {"NoActionBonusProd", NoActionBonusProd},
        {"NoTypeBonusRepl", NoTypeBonusRepl}, {"NewInitiativePoints", NewInitiativePoints}, {"CampBugFixes", CampBugFixes},
        {"HitChanceAir", HitChanceAir}, {"HitChanceGround", HitChanceGround}, {"PowerGrid", g_bPowerGrid ? 1 : 0}, {"NoPlayerPlay", g_nNoPlayerPlay},
        {"LOWAIR_RANGE_MODIFIER", LOWAIR_RANGE_MODIFIER}, {"DayZero", static_cast<double>(TheCampaign.DayZero)},
        {"CampaignEnemyGroundExperience", static_cast<double>(PlayerOptions.CampaignEnemyGroundExperience())},
            // The remaining Falcon4.AII inputs (aiinput.cpp).
        {"IMMEDIATE_MIN_TIME", static_cast<double>(IMMEDIATE_MIN_TIME)},
        {"IMMEDIATE_MAX_TIME", static_cast<double>(IMMEDIATE_MAX_TIME)},
        {"LONGRANGE_MIN_TIME", static_cast<double>(LONGRANGE_MIN_TIME)},
        {"LONGRANGE_MAX_TIME", static_cast<double>(LONGRANGE_MAX_TIME)},
        {"ATM_ASSIGN_RATIO", static_cast<double>(ATM_ASSIGN_RATIO)},
        {"MAX_FLYMISSION_THREAT", static_cast<double>(MAX_FLYMISSION_THREAT)},
        {"MAX_FLYMISSION_HIGHTHREAT", static_cast<double>(MAX_FLYMISSION_HIGHTHREAT)},
        {"MAX_FLYMISSION_NOTHREAT", static_cast<double>(MAX_FLYMISSION_NOTHREAT)},
        {"MIN_SEADESCORT_THREAT", static_cast<double>(MIN_SEADESCORT_THREAT)},
        {"MIN_AVOID_THREAT", static_cast<double>(MIN_AVOID_THREAT)},
        {"MIN_AP_DISTANCE", static_cast<double>(MIN_AP_DISTANCE)},
        {"BREAKPOINT_DISTANCE", static_cast<double>(BREAKPOINT_DISTANCE)},
        {"ATM_TARGET_CLEAR_RADIUS", static_cast<double>(ATM_TARGET_CLEAR_RADIUS)},
        {"LOITER_DIST", static_cast<double>(LOITER_DIST)},
        {"SWEEP_DISTANCE", static_cast<double>(SWEEP_DISTANCE)},
        {"MINIMUM_BDA_PRIORITY", static_cast<double>(MINIMUM_BDA_PRIORITY)},
        {"MINIMUM_AWACS_DISTANCE", static_cast<double>(MINIMUM_AWACS_DISTANCE)},
        {"MAXIMUM_AWACS_DISTANCE", static_cast<double>(MAXIMUM_AWACS_DISTANCE)},
        {"MINIMUM_JSTAR_DISTANCE", static_cast<double>(MINIMUM_JSTAR_DISTANCE)},
        {"MAXIMUM_JSTAR_DISTANCE", static_cast<double>(MAXIMUM_JSTAR_DISTANCE)},
        {"MINIMUM_TANKER_DISTANCE", static_cast<double>(MINIMUM_TANKER_DISTANCE)},
        {"MAXIMUM_TANKER_DISTANCE", static_cast<double>(MAXIMUM_TANKER_DISTANCE)},
        {"MINIMUM_ECM_DISTANCE", static_cast<double>(MINIMUM_ECM_DISTANCE)},
        {"MAXIMUM_ECM_DISTANCE", static_cast<double>(MAXIMUM_ECM_DISTANCE)},
        {"FIRST_COLONEL", static_cast<double>(FIRST_COLONEL)},
        {"FIRST_COMMANDER", static_cast<double>(FIRST_COMMANDER)},
        {"FIRST_WINGMAN", static_cast<double>(FIRST_WINGMAN)},
        {"LAST_WINGMAN", static_cast<double>(LAST_WINGMAN)},
        {"MAX_AA_STR_FOR_ESCORT", static_cast<double>(MAX_AA_STR_FOR_ESCORT)},
        {"ONCALL_CAS_FLIGHTS_PER_REQUEST", static_cast<double>(ONCALL_CAS_FLIGHTS_PER_REQUEST)},
        {"MIN_TASK_AIR", static_cast<double>(MIN_TASK_AIR)},
        {"MIN_PLAN_AIR", static_cast<double>(MIN_PLAN_AIR)},
        {"VICTORY_CHECK_TIME", static_cast<double>(VICTORY_CHECK_TIME)},
        {"FLIGHT_COMBAT_CHECK_INTERVAL", static_cast<double>(FLIGHT_COMBAT_CHECK_INTERVAL)},
        {"AIR_UPDATE_CHECK_INTERVAL", static_cast<double>(AIR_UPDATE_CHECK_INTERVAL)},
        {"FLIGHT_COMBAT_RATE", static_cast<double>(FLIGHT_COMBAT_RATE)},
        {"PACKAGE_CYCLES_TO_WAIT", static_cast<double>(PACKAGE_CYCLES_TO_WAIT)},
        {"AIR_PATH_MAX", static_cast<double>(AIR_PATH_MAX)},
        {"MIN_IGNORE_RANGE", static_cast<double>(MIN_IGNORE_RANGE)},
        {"MAX_SAR_DIST", static_cast<double>(MAX_SAR_DIST)},
        {"PILOT_ASSIGN_TIME", static_cast<double>(PILOT_ASSIGN_TIME)},
        {"AIRCRAFT_TURNAROUND_TIME_MINUTES", static_cast<double>(AIRCRAFT_TURNAROUND_TIME_MINUTES)},
        {"MAX_OFFENSES", static_cast<double>(MAX_OFFENSES)},
        {"MIN_OFFENSIVE_UNITS", static_cast<double>(MIN_OFFENSIVE_UNITS)},
        {"MINIMUM_ADJUSTED_LEVEL", static_cast<double>(MINIMUM_ADJUSTED_LEVEL)},
        {"MINIMUM_VIABLE_LEVEL", static_cast<double>(MINIMUM_VIABLE_LEVEL)},
        {"MAX_ASSIGNMENT_DIST", static_cast<double>(MAX_ASSIGNMENT_DIST)},
        {"BRIGADE_BREAK_BASE", static_cast<double>(BRIGADE_BREAK_BASE)},
        {"ROLE_SCORE_MODIFIER", static_cast<double>(ROLE_SCORE_MODIFIER)},
        {"MIN_REPAIR_OBJECTIVES", static_cast<double>(MIN_REPAIR_OBJECTIVES)},
        {"MINIMUM_EXP_TO_FIRE_PREGUIDE", static_cast<double>(MINIMUM_EXP_TO_FIRE_PREGUIDE)},
        {"MIN_TASK_NAVAL", static_cast<double>(MIN_TASK_NAVAL)},
        {"MIN_PLAN_NAVAL", static_cast<double>(MIN_PLAN_NAVAL)},
        {"NAVAL_COMBAT_CHECK_INTERVAL", static_cast<double>(NAVAL_COMBAT_CHECK_INTERVAL)},
        {"NAVAL_COMBAT_RATE", static_cast<double>(NAVAL_COMBAT_RATE)},
        {"LOW_ALTITUDE_CUTOFF", static_cast<double>(LOW_ALTITUDE_CUTOFF)},
        {"NEW_CLOUD_CHANCE", static_cast<double>(NEW_CLOUD_CHANCE)},
        {"DEL_CLOUD_CHANCE", static_cast<double>(DEL_CLOUD_CHANCE)},
        {"PLAYER_BUBBLE_MOVERS", static_cast<double>(PLAYER_BUBBLE_MOVERS)},
        {"FALCON_PLAYER_TEAM", static_cast<double>(FALCON_PLAYER_TEAM)},
        {"CAMP_RESYNC_TIME", static_cast<double>(CAMP_RESYNC_TIME)},
        {"STANDARD_EVENT_LENGTH", static_cast<double>(STANDARD_EVENT_LENGTH)},
        {"PRIORITY_EVENT_LENGTH", static_cast<double>(PRIORITY_EVENT_LENGTH)},
        {"REAGREGATION_RATIO", static_cast<double>(REAGREGATION_RATIO)},
        {"SIM_BUBBLE_SIZE", static_cast<double>(SIM_BUBBLE_SIZE)},
        {"F4_GENERIC_US_TRUCK_TYPE_SMALL", static_cast<double>(F4_GENERIC_US_TRUCK_TYPE_SMALL)},
        {"F4_GENERIC_US_TRUCK_TYPE_LARGE", static_cast<double>(F4_GENERIC_US_TRUCK_TYPE_LARGE)},
        {"F4_GENERIC_US_TRUCK_TYPE_TRAILER", static_cast<double>(F4_GENERIC_US_TRUCK_TYPE_TRAILER)},
        {"F4_GENERIC_OPFOR_TRUCK_TYPE_SMALL", static_cast<double>(F4_GENERIC_OPFOR_TRUCK_TYPE_SMALL)},
        {"F4_GENERIC_OPFOR_TRUCK_TYPE_LARGE", static_cast<double>(F4_GENERIC_OPFOR_TRUCK_TYPE_LARGE)},
        {"F4_GENERIC_OPFOR_TRUCK_TYPE_TRAILER", static_cast<double>(F4_GENERIC_OPFOR_TRUCK_TYPE_TRAILER)},
        {"SWITCH_TRACTORS_IN_TE", static_cast<double>(SWITCH_TRACTORS_IN_TE)},
        {"MIN_DEAD_PCT", static_cast<double>(MIN_DEAD_PCT)},
        {"FLOTDrawDistance", static_cast<double>(FLOTDrawDistance)},
        {"FLOTSortDirection", static_cast<double>(FLOTSortDirection)},
        {"TheaterXPosition", static_cast<double>(TheaterXPosition)},
        {"TheaterYPosition", static_cast<double>(TheaterYPosition)},
    };
    out << '{';
    for (size_t i = 0; i < sizeof items / sizeof *items; ++i) out << (i ? "," : "") << '"' << items[i].key << "\":" << items[i].value;
    out << '}';
}

void waypoints(std::ostream& out, Unit u) {
    out << '[';
    int n = 0;
    for (WayPoint w = u->GetFirstUnitWP(); w; w = w->GetNextWP()) {
        GridIndex x, y; w->GetWPLocation(&x, &y);
        out << (n++ ? "," : "") << "{\"x\":" << x << ",\"y\":" << y << ",\"action\":" << w->GetWPAction()
            << ",\"flags\":" << w->GetWPFlags() << ",\"arrive\":" << w->GetWPArrivalTime()
            << ",\"depart\":" << w->GetWPDepartureTime() << '}';
    }
    out << ']';
}

void unit(std::ostream& out, Unit u) {
    GridIndex x, y, dx, dy; u->GetLocation(&x, &y); u->GetUnitDestination(&dx, &dy);
    char text[256]{}; u->GetName(text, 255, FALSE);
    out << "{\"id\":\"" << id(u->Id()) << "\",\"camp_id\":" << u->GetCampID() << ",\"class_id\":" << (u->Type() - VU_LAST_ENTITY_TYPE)
        << ",\"name\":" << name(text, sizeof text) << ",\"owner\":" << int(u->GetOwner()) << ",\"x\":" << x << ",\"y\":" << y
        << ",\"dest\":[" << dx << ',' << dy << "],\"flags\":" << u->GetUnitFlags() << ",\"real\":" << (u->Real() ? 1 : 0)
        << ",\"father\":" << (u->Father() ? 1 : 0) << ",\"parent\":" << (u->Parent() ? 1 : 0)
        << ",\"roster\":";
    int roster[VEHICLE_GROUPS_PER_UNIT];
    for (int v = 0; v < VEHICLE_GROUPS_PER_UNIT; ++v) roster[v] = u->Father() ? 0 : u->GetNumVehicles(v);
    array(out, roster, VEHICLE_GROUPS_PER_UNIT);
    out << ",\"last_check\":" << u->GetLastCheck() << ",\"moved\":" << u->GetUnitMoved() << ",\"losses\":" << int(u->GetLosses())
        << ",\"tactic\":" << u->GetUnitTactic() << ",\"reinforcement\":" << u->GetUnitReinforcementLevel() << ",\"odds\":" << u->GetOdds()
        << ",\"target\":" << quoted(u->GetTargetID()) << ",\"cargo\":" << quoted(u->GetCargoId())
        << ",\"spotted\":" << static_cast<unsigned short>(u->GetSpotted()) << ",\"spot_time\":" << u->GetSpotTime()
        << ",\"emitting\":" << (u->IsEmitting() ? 1 : 0) << ",\"waypoints\":";
    waypoints(out, u);
    WayPoint current = u->GetCurrentUnitWP();
    int currentIndex = 0;
    for (WayPoint w = u->GetFirstUnitWP(); w; w = w->GetNextWP()) { ++currentIndex; if (w == current) break; }
    out << ",\"current_wp\":" << (current ? currentIndex : 0);
    if (u->GetDomain() == DOMAIN_LAND) {
        auto* g = static_cast<GroundUnitClass*>(u);
        out << ",\"orders\":" << int(g->GetOrders()) << ",\"division\":" << g->GetUnitDivision()
            << ",\"objective\":" << quoted(g->GetUnitObjectiveID()) << ",\"secondary\":" << quoted(g->GetUnitSecondaryObjID())
            << ",\"primary\":" << quoted(g->GetUnitPrimaryObjID());
    }
    if (u->IsBattalion()) {
        auto* b = static_cast<BattalionClass*>(u);
        out << ",\"supply\":" << b->GetUnitSupply() << ",\"fatigue\":" << b->GetUnitFatigue() << ",\"morale\":" << b->GetUnitMorale()
            << ",\"heading\":" << int(b->GetUnitHeading()) << ",\"final_heading\":" << int(b->GetFinalHeading())
            << ",\"position\":" << b->GetUnitPosition() << ",\"fullstrength\":" << int(b->GetFullStrength())
            << ",\"last_move\":" << b->last_move << ",\"last_combat\":" << b->last_combat
            << ",\"last_resupply\":" << b->GetLastResupplyTime() << ",\"brigade\":" << quoted(b->parent_id)
            << ",\"last_obj\":" << quoted(b->last_obj) << ",\"path\":{\"position\":" << b->path->GetCurrentPosition() << ",\"directions\":[";
        for (int i = 0; i < b->path->GetLength(); ++i) out << (i ? "," : "") << b->path->GetDirection(i);
        out << "]}";
    }
    if (u->IsBrigade()) {
        out << ",\"elements\":[";
        int n = 0;
        for (Unit e = u->GetFirstUnitElement(); e; e = u->GetNextUnitElement()) out << (n++ ? "," : "") << '"' << id(e->Id()) << '"';
        out << ']';
    }
    if (u->IsSquadron()) {
        auto* s = static_cast<SquadronClass*>(u);
        UnitClassDataType* uc = u->GetUnitClassData();
        int want = 0, got = 0;
        for (int i = 0; uc && i < MAXIMUM_WEAPTYPES; ++i) {
            want += SquadronStoresDataTable[uc->SpecialIndex].Stores[i];
            got += s->GetUnitStores(i);
        }
        out << ",\"stores_want\":" << want << ",\"stores_got\":" << got << ",\"fuel_lb\":" << s->GetSquadronFuel()
            << ",\"last_resupply\":" << s->GetLastResupplyTime() << ",\"airbase\":" << quoted(s->GetUnitAirbaseID());
        // The whole squadron record (SquadronClass): stores as [weapon, count] pairs, pilots, schedule.
        out << ",\"squadron\":{\"fuel\":" << s->GetSquadronFuel() << ",\"specialty\":" << s->GetUnitSpecialty()
            << ",\"player_type\":" << TheCampaign.IsValidAircraftType(u)
            << ",\"hot_spot\":" << quoted(s->GetHotSpot()) << ",\"aa_kills\":" << s->GetAAKills() << ",\"ag_kills\":" << s->GetAGKills()
            << ",\"as_kills\":" << s->GetASKills() << ",\"an_kills\":" << s->GetANKills() << ",\"missions_flown\":" << s->GetMissionsFlown()
            << ",\"mission_score\":" << s->GetMissionScore() << ",\"total_losses\":" << int(s->GetTotalLosses())
            << ",\"pilot_losses\":" << int(s->GetPilotLosses()) << ",\"assigned\":" << int(s->GetAssigned())
            << ",\"patch\":" << int(s->GetPatchID()) << ",\"last_resupply\":" << s->GetLastResupply()
            << ",\"last_resupply_time\":" << s->GetLastResupplyTime() << ",\"retask_at\":" << s->squadronRetaskAt << ",\"rating\":";
        int rating[ARO_OTHER];
        for (int i = 0; i < ARO_OTHER; ++i) rating[i] = s->GetRating(i);
        array(out, rating, ARO_OTHER);
        out << ",\"schedule\":[";
        for (int i = 0; i < VEHICLE_GROUPS_PER_UNIT; ++i) out << (i ? "," : "") << s->GetSchedule(i);
        out << "],\"stores\":[";
        int n = 0;
        for (int i = 0; i < MAXIMUM_WEAPTYPES; ++i)
            if (s->GetUnitStores(i)) out << (n++ ? "," : "") << '[' << i << ',' << int(s->GetUnitStores(i)) << ']';
        out << "],\"pilots\":[";
        for (int i = 0; i < PILOTS_PER_SQUADRON; ++i) {
            const PilotClass* p = s->GetPilotData(i);
            out << (i ? "," : "") << '[' << p->pilot_id << ',' << int(p->pilot_skill_and_rating) << ',' << int(p->pilot_status) << ','
                << int(p->aa_kills) << ',' << int(p->ag_kills) << ',' << int(p->as_kills) << ',' << int(p->an_kills) << ','
                << p->missions_flown << ']';
        }
        out << "]}";
    }
    out << '}';
}

} // namespace

void WriteCoreExport(const std::filesystem::path& directory) {
    std::ostringstream out;
    out << "{\"format\":\"ff-core-export\",\"version\":1,\"scenario\":" << ff::headless::JsonString(TheCampaign.Scenario)
        << ",\"theater\":" << ff::headless::JsonString(TheCampaign.TheaterName)
        << ",\"campaign\":{\"time_ms\":" << TheCampaign.CurrentTime << ",\"current_day\":" << int(TheCampaign.CurrentDay)
        << ",\"last_major_event\":" << TheCampaign.lastMajorEvent << ",\"flags\":" << TheCampaign.Flags
        << ",\"map\":[" << Map_Max_X << ',' << Map_Max_Y << "],\"player_team\":" << int(FalconLocalSession->GetTeam()) << "}";
    out << ",\"constants\":"; constants(out);
    out << ",\"visual_detection_range\":"; array(out, VisualDetectionRange, OtherDam);
    out << ",\"order_priority\":"; array(out, OrderPriority, GORD_LAST);
    out << ",\"default_damage_mods\":"; array(out, DefaultDamageMods, OtherDam + 1);
    out << ",\"roe\":[";
    for (int t = 0; t <= ROE_NAVAL_BOMBARD; ++t) { out << (t ? "," : ""); array(out, RoEData[t], War + 1); }
    out << "]";

    // Class tables. Keys are class table indexes, as units and objectives refer to them.
    out << ",\"unit_classes\":{";
    bool comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_UNIT || !Falcon4ClassTable[i].dataPtr) continue;
        auto* c = static_cast<UnitClassDataType*>(Falcon4ClassTable[i].dataPtr);
        out << (comma ? "," : "") << '"' << i << "\":{"; comma = true;
        classInfo(out, i);
        out << ",\"name\":" << name(c->Name, sizeof c->Name) << ",\"elements\":"; array(out, c->NumElements, VEHICLE_GROUPS_PER_UNIT);
        out << ",\"vehicle_type\":"; array(out, c->VehicleType, VEHICLE_GROUPS_PER_UNIT);
        out << ",\"vehicle_kind\":[";
        for (int v = 0; v < VEHICLE_GROUPS_PER_UNIT; ++v) out << (v ? "," : "") << int(c->VehicleClass[v][2]);
        out << "],\"flags\":" << c->Flags << ",\"movement_type\":" << int(c->MovementType) << ",\"movement_speed\":" << c->MovementSpeed
            << ",\"max_range\":" << c->MaxRange << ",\"fuel\":" << c->Fuel << ",\"rate\":" << c->Rate << ",\"role\":" << int(c->Role)
            << ",\"special_index\":" << c->SpecialIndex << ",\"radar_vehicle\":" << int(c->RadarVehicle) << ",\"scores\":";
        array(out, c->Scores, MAXIMUM_ROLES);
        out << ",\"hit_chance\":"; array(out, c->HitChance, MOVEMENT_TYPES);
        out << ",\"strength\":"; array(out, c->Strength, MOVEMENT_TYPES);
        out << ",\"range\":"; array(out, c->Range, MOVEMENT_TYPES);
        out << ",\"detection\":"; array(out, c->Detection, MOVEMENT_TYPES);
        out << ",\"damage_mod\":"; array(out, c->DamageMod, OtherDam + 1);
        out << '}';
    }
    out << "},\"vehicle_classes\":{"; comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_VEHICLE || !Falcon4ClassTable[i].dataPtr) continue;
        auto* v = GetVehicleClassData(i);
        out << (comma ? "," : "") << '"' << i << "\":{"; comma = true;
        classInfo(out, i);
        out << ",\"name\":" << name(v->Name, sizeof v->Name) << ",\"hit_points\":" << v->HitPoints << ",\"flags\":" << v->Flags
            << ",\"max_speed\":" << v->MaxSpeed << ",\"high_alt\":" << v->HighAlt << ",\"low_alt\":" << v->LowAlt
            << ",\"cruise_alt\":" << v->CruiseAlt << ",\"fuel_econ\":" << v->FuelEcon << ",\"visible_flags\":" << v->VisibleFlags
            << ",\"rack_flags\":" << v->RackFlags << ",\"callsign\":[" << int(v->CallsignIndex) << ',' << int(v->CallsignSlots) << ']'
            << ",\"hit_chance\":";
        array(out, v->HitChance, MOVEMENT_TYPES);
        out << ",\"strength\":"; array(out, v->Strength, MOVEMENT_TYPES);
        out << ",\"range\":"; array(out, v->Range, MOVEMENT_TYPES);
        out << ",\"detection\":"; array(out, v->Detection, MOVEMENT_TYPES);
        out << ",\"weapon\":"; array(out, v->Weapon, HARDPOINT_MAX);
        out << ",\"weapons\":"; array(out, v->Weapons, HARDPOINT_MAX);
        out << ",\"damage_mod\":"; array(out, v->DamageMod, OtherDam + 1);
        out << '}';
    }
    // Weapons are keyed by WeaponDataTable index, which vehicle weapon slots use.
    out << "},\"weapon_classes\":{"; comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_WEAPON || !Falcon4ClassTable[i].dataPtr) continue;
        auto* w = static_cast<WeaponClassDataType*>(Falcon4ClassTable[i].dataPtr);
        out << (comma ? "," : "") << '"' << (w - WeaponDataTable) << "\":{"; comma = true;
        classInfo(out, i);
        out << ",\"name\":" << name(w->Name, sizeof w->Name) << ",\"strength\":" << w->Strength << ",\"damage_type\":" << int(w->DamageType)
            << ",\"range\":" << w->Range << ",\"flags\":" << w->Flags << ",\"fire_rate\":" << int(w->FireRate)
            << ",\"rariety\":" << int(w->Rariety) << ",\"guidance_flags\":" << w->GuidanceFlags << ",\"collective\":" << int(w->Collective)
            << ",\"blast_radius\":" << w->BlastRadius << ",\"max_alt\":" << int(w->MaxAlt) << ",\"hit_chance\":";
        array(out, w->HitChance, MOVEMENT_TYPES);
        out << '}';
    }
    out << "},\"objective_classes\":{"; comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_OBJECTIVE || !Falcon4ClassTable[i].dataPtr) continue;
        auto* c = static_cast<ObjClassDataType*>(Falcon4ClassTable[i].dataPtr);
        out << (comma ? "," : "") << '"' << i << "\":{"; comma = true;
        classInfo(out, i);
        out << ",\"name\":" << name(c->Name, sizeof c->Name) << ",\"data_rate\":" << c->DataRate << ",\"deag_distance\":" << c->DeagDistance
            << ",\"radar_feature\":" << int(c->RadarFeature) << ",\"detection\":";
        array(out, c->Detection, MOVEMENT_TYPES);
        out << ",\"damage_mod\":"; array(out, c->DamageMod, OtherDam + 1);
        out << ",\"features\":[";
        for (int f = 0; f < c->Features; ++f) {
            const FeatureEntry& e = FeatureEntryDataTable[c->FirstFeature + f];
            out << (f ? "," : "") << "{\"class_id\":" << e.Index << ",\"flags\":" << e.Flags << ",\"value\":" << int(e.Value) << '}';
        }
        out << "]}";
    }
    out << "},\"feature_classes\":{"; comma = false;
    for (int i = 0; i < NumEntities; ++i) {
        if (Falcon4ClassTable[i].dataType != DTYPE_FEATURE || !Falcon4ClassTable[i].dataPtr) continue;
        auto* f = GetFeatureClassData(i);
        out << (comma ? "," : "") << '"' << i << "\":{"; comma = true;
        classInfo(out, i);
        out << ",\"repair_time\":" << f->RepairTime << ",\"flags\":" << f->Flags << ",\"hit_points\":" << f->HitPoints << ",\"damage_mod\":";
        array(out, f->DamageMod, OtherDam + 1);
        out << '}';
    }
    out << "},\"tactics\":[";
    for (int t = 0; t < TotalTactics; ++t) {
        const TacticData& d = TacticsTable[t];
        out << (t ? "," : "") << "{\"id\":" << d.id << ",\"name\":" << name(d.name, sizeof d.name) << ",\"team\":" << int(d.team)
            << ",\"domain\":" << int(d.domainType) << ",\"unit_size\":" << int(d.unitSize) << ",\"min_range\":" << d.minRangeToDest
            << ",\"max_range\":" << d.maxRangeToDest << ",\"dist_to_front\":" << d.distToFront << ",\"actions\":";
        array(out, d.actionList, 10);
        out << ",\"broken\":" << int(d.broken) << ",\"engaged\":" << int(d.engaged) << ",\"combat\":" << int(d.combat)
            << ",\"losses\":" << int(d.losses) << ",\"retreating\":" << int(d.retreating) << ",\"owned\":" << int(d.owned)
            << ",\"airborne\":" << int(d.airborne) << ",\"marine\":" << int(d.marine) << ",\"min_odds\":" << int(d.minOdds)
            << ",\"role\":" << int(d.role) << ",\"fuel\":" << int(d.fuel) << ",\"weapons\":" << int(d.weapons)
            << ",\"priority\":" << int(d.priority) << ",\"formation\":" << int(d.formation) << ",\"special\":" << int(d.special) << '}';
    }
    out << "],\"first_ground_tactic\":" << FirstGroundTactic << ",\"ground_tactics\":" << GroundTactics;

    // Air planning tables: the mission table (mission.cpp MissionData), the default priority
    // tables of air actions (team.cpp) and each squadron type's stores (SquadronStoresDataTable).
    out << ",\"mission_data\":[";
    for (int m = 0; m < AMIS_OTHER; ++m) {
        const MissionDataType& d = MissionData[m];
        out << (m ? "," : "") << '[' << int(d.type) << ',' << int(d.target) << ',' << int(d.skill) << ',' << int(d.mission_profile) << ','
            << int(d.target_profile) << ',' << int(d.target_desc) << ',' << int(d.routewp) << ',' << int(d.targetwp) << ','
            << d.minalt << ',' << d.maxalt << ',' << d.missionalt << ',' << d.separation << ',' << d.loitertime << ',' << int(d.str) << ','
            << int(d.min_time) << ',' << int(d.max_time) << ',' << int(d.escorttype) << ',' << int(d.mindistance) << ','
            << int(d.mintime) << ',' << int(d.caps) << ',' << d.flags << ']';
    }
    out << "],\"priority_tables\":{\"objtype\":[";
    for (int a = 0; a < TAT_CAS; ++a) {
        int row[MAX_TGTTYPE];
        for (int i = 0; i < MAX_TGTTYPE; ++i) row[i] = DefaultObjtypePriority[a][i];
        out << (a ? "," : "");
        array(out, row, MAX_TGTTYPE);
    }
    out << "],\"unittype\":[";
    for (int a = 0; a < TAT_CAS; ++a) {
        int row[MAX_UNITTYPE];
        for (int i = 0; i < MAX_UNITTYPE; ++i) row[i] = DefaultUnittypePriority[a][i];
        out << (a ? "," : "");
        array(out, row, MAX_UNITTYPE);
    }
    out << "],\"mission\":[";
    for (int a = 0; a < TAT_CAS; ++a) {
        int row[AMIS_OTHER];
        for (int i = 0; i < AMIS_OTHER; ++i) row[i] = DefaultMissionPriority[a][i];
        out << (a ? "," : "");
        array(out, row, AMIS_OTHER);
    }
    out << "]},\"squadron_stores\":[";
    for (int t = 0; t < NumSquadTypes; ++t) {
        const SquadronStoresDataType& d = SquadronStoresDataTable[t];
        out << (t ? "," : "") << "{\"infinite_ag\":" << int(d.infiniteAG) << ",\"infinite_aa\":" << int(d.infiniteAA)
            << ",\"infinite_gun\":" << int(d.infiniteGun) << ",\"stores\":[";
        int n = 0;
        for (int i = 0; i < MAXIMUM_WEAPTYPES; ++i)
            if (d.Stores[i]) out << (n++ ? "," : "") << '[' << i << ',' << int(d.Stores[i]) << ']';
        out << "]}";
    }
    out << ']';

    // Flight planning inputs: the runtime configuration switches the planner reads, the weapon lists
    // vehicle hardpoints load from (each up to its first empty entry), and the terrain MEA table as
    // TheMap.Setup reads it (Theater.map header, Theater.MEA rows stored bottom up).
    out << ",\"air_planning\":{\"config\":{\"percentage_available_aircraft\":" << g_npercentage_available_aircraft
        << ",\"minimum_available_aircraft\":" << g_nminimum_available_aircraft << ",\"climb_ratio\":" << g_fClimbRatio
        << ",\"no_wp_refuel_needed\":" << g_nNoWPRefuelNeeded << ",\"add_ingress_wp\":" << int(g_bAddIngressWP)
        << ",\"tanker_waypoints\":" << int(g_bTankerWaypoints) << ",\"large_strike\":" << int(g_bLargeStrike)
        << ",\"old_sam_activity\":" << int(g_bOldSamActivity) << ",\"precision_waypoints\":" << int(g_bPrecisionWaypoints)
        << ",\"fire_on_the_move\":" << int(g_bFireOntheMove) << ",\"alt_to_build_ranges_to\":" << ALT_TO_BUILD_RANGES_TO
        << ",\"realistic_attrition\":" << int(g_bRealisticAttrition) << ",\"loadout_squad_store_resupply\":" << int(g_bLoadoutSquadStoreResupply)
        << ",\"flight_visual_bonus\":" << g_nFlightVisualBonus << ",\"ident_factor\":" << g_fIdentFactor << '}';
    out << ",\"valid_aircraft_types\":{\"count\":" << TheCampaign.NumberOfValidTypes << ",\"types\":[";
    for (int i = 0; TheCampaign.ValidAircraftTypes && i < TheCampaign.NumberOfValidTypes && TheCampaign.NumberOfValidTypes < 8888; ++i)
        out << (i ? "," : "") << TheCampaign.ValidAircraftTypes[i];
    out << "]},\"weapon_lists\":{";
    {
        std::set<int> lists;
        for (int i = 0; i < NumEntities; ++i) {
            if (Falcon4ClassTable[i].dataType != DTYPE_VEHICLE || !Falcon4ClassTable[i].dataPtr) continue;
            auto* v = GetVehicleClassData(i);
            for (int hp = 0; hp < HARDPOINT_MAX; ++hp)
                if (v->Weapons[hp] == 255) lists.insert(v->Weapon[hp]);
        }
        bool first = true;
        for (int l : lists) {
            out << (first ? "" : ",") << '"' << l << "\":[";
            first = false;
            for (int e = 0; e < MAX_WEAPONS_IN_LIST; ++e) {
                out << (e ? "," : "") << '[' << GetListEntryWeapon(l, e) << ',' << GetListEntryWeapons(l, e) << ']';
                if (!GetListEntryWeapon(l, e)) break;
            }
            out << ']';
        }
    }
    out << "},\"mea\":";
    {
        std::ifstream header("terrdata/korea/terrain/Theater.map", std::ios::binary);
        float feetPerPost = 0, ftToCell = 0;
        int width = 0, height = 0;
        header.read(reinterpret_cast<char*>(&feetPerPost), sizeof feetPerPost);
        header.read(reinterpret_cast<char*>(&width), sizeof width);
        header.read(reinterpret_cast<char*>(&height), sizeof height);
        header.read(reinterpret_cast<char*>(&ftToCell), sizeof ftToCell);
        std::ifstream data("terrdata/korea/terrain/Theater.MEA", std::ios::binary);
        std::vector<short> values(static_cast<size_t>(width) * height);
        for (int row = height - 1; row >= 0 && header; --row)
            data.read(reinterpret_cast<char*>(&values[static_cast<size_t>(row) * width]), width * sizeof(short));
        if (!header || !data) throw std::runtime_error("terrain MEA table unreadable for the core export");
        char factor[32];
        std::snprintf(factor, sizeof factor, "%.9g", ftToCell);
        out << "{\"width\":" << width << ",\"height\":" << height << ",\"ft_to_cell\":" << factor << ",\"values\":[";
        for (size_t i = 0; i < values.size(); ++i) out << (i ? "," : "") << values[i];
        out << "]}";
    }
    out << '}';

    out << ",\"teams\":[";
    for (int t = 0; t < NUM_TEAMS; ++t) {
        TeamClass* team = TeamInfo[t];
        out << (t ? "," : "");
        if (!team) { out << "null"; continue; }
        const TeamGndActionType* g = team->GetGroundAction();
        const TeamStatusType* s = team->GetCurrentStats();
        out << "{\"id\":" << t << ",\"cteam\":" << int(team->cteam) << ",\"flags\":" << team->flags << ",\"name\":" << name(team->name, sizeof team->name)
            << ",\"initiative\":" << team->GetInitiative() << ",\"supply\":" << team->GetSupplyAvail() << ",\"fuel\":" << team->GetFuelAvail()
            << ",\"replacements\":" << team->GetReplacementsAvail() << ",\"reinforcement\":" << team->GetReinforcement()
            << ",\"stance\":";
        array(out, team->stance, NUM_TEAMS);
        out << ",\"member\":"; array(out, team->member, NUM_COUNS);
        out << ",\"max_vehicle\":"; array(out, team->max_vehicle, 4);
        out << ",\"experience\":{\"air\":" << int(team->airExperience) << ",\"air_defense\":" << int(team->airDefenseExperience)
            << ",\"ground\":" << int(team->groundExperience) << ",\"naval\":" << int(team->navalExperience) << "}"
            << ",\"player_rating\":" << team->playerRating << ",\"last_player_mission\":" << team->lastPlayerMission
            << ",\"ground_action\":{\"type\":" << int(g->actionType) << ",\"time\":" << g->actionTime << ",\"timeout\":" << g->actionTimeout
            << ",\"objective\":" << quoted(g->actionObjective) << ",\"tempo\":" << int(g->actionTempo) << ",\"points\":" << int(g->actionPoints) << '}'
            << ",\"stats\":[" << s->airDefenseVehs << ',' << s->aircraft << ',' << s->groundVehs << ',' << s->ships << ',' << s->supply << ','
            << s->fuel << ',' << s->airbases << ',' << int(s->supplyLevel) << ',' << int(s->fuelLevel) << ']'
            << ",\"start_stats\":[" << team->startStats.airDefenseVehs << ',' << team->startStats.aircraft << ',' << team->startStats.groundVehs << ','
            << team->startStats.ships << ',' << team->startStats.supply << ',' << team->startStats.fuel << ',' << team->startStats.airbases << ']'
            << ",\"bonuses\":[";
        for (int b = 0; b < MAX_BONUSES; ++b)
            out << (b ? "," : "") << '[' << quoted(team->bonusObjs[b]) << ',' << team->bonusTime[b] << ']';
        out << "],\"gtm_flags\":" << (team->gtm ? team->gtm->flags : 0);
        // Air planning state: actions, player priorities and the air tasking manager's persistent
        // part (the request lists are not saved by the legacy campaign either).
        auto action = [&](const TeamAirActionType* a) {
            out << "{\"start\":" << a->actionStartTime << ",\"stop\":" << a->actionStopTime << ",\"objective\":" << quoted(a->actionObjective)
                << ",\"last_objective\":" << quoted(a->lastActionObjective) << ",\"type\":" << int(a->actionType) << '}';
        };
        int objtype[MAX_TGTTYPE], unittype[MAX_UNITTYPE], mission[AMIS_OTHER];
        for (int i = 0; i < MAX_TGTTYPE; ++i) objtype[i] = team->GetObjTypePriority(i);
        for (int i = 0; i < MAX_UNITTYPE; ++i) unittype[i] = team->GetUnitTypePriority(i);
        for (int i = 0; i < AMIS_OTHER; ++i) mission[i] = team->GetMissionPriority(i);
        out << ",\"air\":{\"offensive\":"; action(team->GetOffensiveAirAction());
        out << ",\"defensive\":"; action(team->GetDefensiveAirAction());
        out << ",\"objtype_priority\":"; array(out, objtype, MAX_TGTTYPE);
        out << ",\"unittype_priority\":"; array(out, unittype, MAX_UNITTYPE);
        out << ",\"mission_priority\":"; array(out, mission, AMIS_OTHER);
        if (AirTaskingManagerClass* atm = team->atm) {
            out << ",\"atm\":{\"flags\":" << atm->flags << ",\"average_ca_strength\":" << atm->averageCAStrength
                << ",\"average_ca_missions\":" << atm->averageCAMissions << ",\"sample_cycles\":" << int(atm->sampleCycles)
                << ",\"current_ca_missions\":" << int(atm->currentCAMissions) << ",\"missions_to_fill\":" << atm->missionsToFill
                << ",\"missions_filled\":" << atm->missionsFilled << ",\"supply_base\":" << int(atm->supplyBase)
                << ",\"cycle\":" << int(atm->cycle) << ",\"schedule_time\":" << atm->scheduleTime << ",\"airbases\":[";
            int n = 0;
            for (ATMAirbaseClass* ab = atm->airbaseList; ab; ab = ab->next) {
                int schedule[ATM_MAX_CYCLES];
                for (int i = 0; i < ATM_MAX_CYCLES; ++i) schedule[i] = ab->schedule[i];
                out << (n++ ? "," : "") << "{\"id\":" << quoted(ab->id) << ",\"usage\":" << int(ab->usage) << ",\"schedule\":";
                array(out, schedule, ATM_MAX_CYCLES);
                out << '}';
            }
            out << "]}";
        }
        out << "}}";
    }
    // The trigger script as the legacy CheckTriggers reads it (loose file or packed in the .cam).
    std::string script;
    if (FILE* fp = OpenCampFile(TheCampaign.Scenario, "tri", "r")) {
        char buffer[4096];
        for (size_t n; (n = fread(buffer, 1, sizeof buffer, fp)) > 0;) script.append(buffer, n);
        CloseCampFile(fp);
    }
    out << "],\"triggers\":" << ff::headless::JsonString(script);
    out << ",\"events\":[";
    for (int e = 0; CampEvents && e < CE_Events; ++e) out << (e ? "," : "") << (CampEvents[e] ? CampEvents[e]->flags : 0);
    out << ']';

    // Legacy iteration orders: position of each entity in its VU lists (push-front lists, newest
    // first) and in the proximity grids (rows, then columns, then insertion order), or -1.
    auto listOrder = [](auto* list) {
        std::map<VU_ID, int> order;
        if (!list) return order;
        VuListIterator it(list);
        for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) order.emplace(e->Id(), static_cast<int>(order.size()));
        return order;
    };
    auto gridOrder = [](VuGridTree* grid) {
        std::map<VU_ID, int> order;
        if (!grid) return order;
        VuGridIterator it(grid, 0.0F, 0.0F, 1.0e12F);
        for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) order.emplace(e->Id(), static_cast<int>(order.size()));
        return order;
    };
    auto at = [](const std::map<VU_ID, int>& order, VU_ID id) { const auto it = order.find(id); return it == order.end() ? -1 : it->second; };
    const auto objectiveList = listOrder(AllObjList), objectiveGrid = gridOrder(ObjProxList), campList = listOrder(AllCampList);
    out << ",\"objectives\":[";
    comma = false;
    {
        VuListIterator it(AllObjList);
        for (Objective o = static_cast<Objective>(it.GetFirst()); o; o = static_cast<Objective>(it.GetNext())) {
            out << (comma ? "," : "") << "{\"id\":\"" << id(o->Id()) << "\",\"camp_id\":" << o->GetCampID()
                << ",\"class_id\":" << (o->Type() - VU_LAST_ENTITY_TYPE) << ",\"owner\":" << int(o->GetOwner())
                << ",\"old_owner\":" << int(o->GetObjectiveOldown()) << ",\"obj_flags\":" << objectiveFlags(o)
                << ",\"status\":" << int(o->GetObjectiveStatus()) << ",\"supply\":" << o->GetObjectiveSupply()
                << ",\"fuel\":" << o->GetObjectiveFuel() << ",\"losses\":" << o->GetObjectiveSupplyLosses()
                << ",\"priority\":" << o->GetObjectivePriority() << ",\"repair_time\":" << o->GetObjectiveRepairTime()
                << ",\"score\":" << o->GetObjectiveScore() << ",\"local_data\":" << o->static_data.local_data
                << ",\"parent\":" << quoted(o->GetObjectiveParentID()) << ",\"feature_status\":[";
            for (int f = 0; f < o->GetTotalFeatures(); ++f) out << (f ? "," : "") << o->GetFeatureStatus(f);
            // Every link with all movement-type costs, in link order (unresolved links kept as "").
            out << "],\"links\":[";
            for (int n = 0; n < o->static_data.links; ++n) {
                out << (n ? "," : "") << "{\"to\":" << quoted(o->link_data[n].id) << ",\"costs\":";
                array(out, o->link_data[n].costs, MOVEMENT_TYPES);
                out << '}';
            }
            out << "],\"order\":[" << at(objectiveList, o->Id()) << ',' << at(objectiveGrid, o->Id()) << ',' << at(campList, o->Id())
                << "],\"emitting\":" << (o->IsEmitting() ? 1 : 0);
            // Terrain radar arcs (CampaignRadarClass), for the radar coverage map: count, then ratios.
            if (o->HasRadarRanges()) {
                out << ",\"radar_arcs\":[" << o->GetNumberOfArcs();
                for (int a = 0; a < 8; ++a) { char r[32]; std::snprintf(r, sizeof r, "%.9g", o->GetArcRatio(a)); out << ',' << r; }
                out << ']';
            }
            if (o->IsPrimary()) {
                if (POData pd = GetPOData(o)) {
                    out << ",\"po\":{\"flags\":" << int(pd->flags) << ",\"ground_priority\":";
                    array(out, pd->ground_priority, NUM_TEAMS);
                    out << ",\"ground_assigned\":"; array(out, pd->ground_assigned, NUM_TEAMS);
                    out << ",\"air_priority\":"; array(out, pd->air_priority, NUM_TEAMS);
                    out << ",\"operator_priority\":"; array(out, pd->operator_priority, NUM_TEAMS);
                    out << ",\"player_priority\":"; array(out, pd->player_priority, NUM_TEAMS);
                    out << '}';
                }
            }
            out << '}';
            comma = true;
        }
    }
    // Every unit, active or not; "lists" records membership of the legacy unit lists.
    out << "],\"units\":[";
    comma = false;
    auto listed = [](VuFilteredList* list) {
        std::set<VU_ID> ids;
        if (!list) return ids;
        VuListIterator it(list);
        for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) ids.insert(e->Id());
        return ids;
    };
    const auto real = listed(AllRealList), parents = listed(AllParentList), inactive = listed(InactiveList);
    const auto unitList = listOrder(AllUnitList), realList = listOrder(AllRealList), parentList = listOrder(AllParentList),
               inactiveList = listOrder(InactiveList), unitGrid = gridOrder(RealUnitProxList), airList = listOrder(AllAirList);
    std::set<VU_ID> written;
    auto emit = [&](Unit u) {
        if (!written.insert(u->Id()).second) return;
        out << (comma ? "," : "");
        unit(out, u);
        out.seekp(-1, std::ios_base::cur);
        out << ",\"lists\":[" << (real.count(u->Id()) ? 1 : 0) << ',' << (parents.count(u->Id()) ? 1 : 0) << ','
            << (inactive.count(u->Id()) ? 1 : 0) << "],\"order\":[" << at(unitList, u->Id()) << ',' << at(realList, u->Id()) << ','
            << at(parentList, u->Id()) << ',' << at(inactiveList, u->Id()) << ',' << at(unitGrid, u->Id()) << ',' << at(campList, u->Id()) << ',' << at(airList, u->Id())
            << "],\"radar\":[" << u->GetRadarMode() << ',' << (u->IsBattalion() ? static_cast<BattalionClass*>(u)->GetSearchMode() : 0) << "]}";
        comma = true;
    };
    { VuListIterator it(AllUnitList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) emit(static_cast<Unit>(e)); }
    { VuListIterator it(InactiveList); for (VuEntity* e = it.GetFirst(); e; e = it.GetNext()) emit(static_cast<Unit>(e)); }
    out << "]}";

    const auto target = directory / "core-data.json";
    std::ofstream file(target, std::ios::binary);
    file << out.str();
    if (!file) throw std::runtime_error("core-data.json write failed");
}
