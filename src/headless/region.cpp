#include "stdhdr.h"
#include "detailed.h"
#include "simdrive.h"
#include "simbase.h"
#include "simveh.h"
#include "simfeat.h"
#include "unit.h"
#include "objectiv.h"
#include "camplist.h"
#include "classtbl.h"
#include "vehicle.h"
#include "feature.h"
#include "sms.h"
#include "rdrackdata.h"
#include "aircrft.h"
#include "airframe.h"
#include "helo.h"
#include "hdigi.h"
#include "ground.h"
#include "gndai.h"
#include "digi.h"
#include "wingorder.h"
#include "fcc.h"
#include "sensclas.h"
#include "radar.h"
#include "object.h"
#include "simweapn.h"
#include "bomb.h"
#include "guns.h"
#include "scenario.h"
#include "campweap.h"
#include "otwdrive.h"
#include "boundary.h"
#include <vector>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <map>
#include <deque>
#include <array>
extern void SyncDetailedModel(SimBaseClass*);
namespace {
bool requested=false,initialized=false;
double centerX=0,centerY=0,radius=10;
std::vector<VuBin<CampBaseClass>> members;
struct TrailPoint { unsigned long time; float x,y,z; };
struct Trail { int team; std::deque<TrailPoint> points; };
std::map<std::string,Trail> trails;
// Preserve per-pilot stores and radar power across detailed reaggregation.
// These runtime fields do not change the legacy campaign save format.
struct AircraftBoundaryState { int chaff,flare;int radarPower=-1;bool operatorHold=false;bool masterSafe=false;bool returning=false;bool hasReturnWaypoint=false;std::array<float,3> returnLocation{};int returnAction=0;VU_ID returnTarget; };
std::map<std::pair<VU_ID,int>,AircraftBoundaryState> aircraftBoundaryStates;
std::map<std::pair<VU_ID,int>,bool> helicopterSafetyStates;
struct AmmunitionStationState { int weaponId=0;std::vector<int> rounds;int gunRounds=-1;float gunRemainder=0; };
std::map<std::pair<VU_ID,int>,std::vector<AmmunitionStationState>> ammunitionBoundaryStates;
void rememberAmmunition(SimVehicleClass* vehicle) {
    auto* campaign=vehicle->GetCampaignObject();auto* sms=vehicle->GetSMS();
    if(!campaign || !campaign->IsFlight() || !sms) return;
    for(auto it=ammunitionBoundaryStates.begin();it!=ammunitionBoundaryStates.end();) {
        if(!vuDatabase->Find(it->first.first)) it=ammunitionBoundaryStates.erase(it);
        else ++it;
    }
    auto& state=ammunitionBoundaryStates[{campaign->Id(),vehicle->pilotSlot}];
    state.clear();state.resize(sms->NumHardpoints());
    for(int hp=0;hp<sms->NumHardpoints();++hp) {
        auto* station=sms->hardPoint[hp];if(!station) continue;
        state[hp].weaponId=station->weaponId;
        if(auto* gun=station->GetGun()) {
            state[hp].gunRounds=gun->numRoundsRemaining;
            state[hp].gunRemainder=gun->GetAmmunitionRemainder();
        }
        for(auto* weapon=station->weaponPointer.get();weapon;weapon=weapon->GetNextOnRail())
            if(weapon->IsLauncher()) state[hp].rounds.push_back(static_cast<BombClass*>(weapon)->LauGetRoundsOnboard());
    }
}
void restoreAmmunition(SimVehicleClass* vehicle) {
    auto* campaign=vehicle->GetCampaignObject();auto* sms=vehicle->GetSMS();
    if(!campaign || !campaign->IsFlight() || !sms) return;
    const auto found=ammunitionBoundaryStates.find({campaign->Id(),vehicle->pilotSlot});
    if(found==ammunitionBoundaryStates.end()) return;
    const auto& state=found->second;
    for(int hp=0;hp<sms->NumHardpoints() && hp<int(state.size());++hp) {
        auto* station=sms->hardPoint[hp];
        if(!station || station->weaponId!=state[hp].weaponId) continue;
        if(auto* gun=station->GetGun(); gun && state[hp].gunRounds>=0)
            gun->RestoreAmmunition(state[hp].gunRounds,state[hp].gunRemainder);
        unsigned index=0;int loadedPods=0;
        for(auto* weapon=station->weaponPointer.get();weapon;weapon=weapon->GetNextOnRail()) {
            if(!weapon->IsLauncher()) continue;
            auto* pod=static_cast<BombClass*>(weapon);
            if(index<state[hp].rounds.size()) {
                pod->LauCancelSalvo();
                pod->LauSetRoundsRemaining(min(pod->LauGetRoundsOnboard(),max(0,state[hp].rounds[index])));
            }
            if(pod->LauGetRoundsOnboard()>0) ++loadedPods;
            ++index;
        }
        if(index) station->weaponCount=loadedPods;
    }
}
void pruneTrails() {
    for(auto it=trails.begin();it!=trails.end();) {
        auto& points=it->second.points;
        while(!points.empty() && TheCampaign.CurrentTime-points.front().time>2000) points.pop_front();
        if(points.empty()) it=trails.erase(it); else ++it;
    }
}
std::string id(VU_ID value) { return std::to_string(value.creator_)+":"+std::to_string(value.num_); }
std::string label(int index) {
    if(index<0||index>=NumEntities) return "\"Unknown\"";
    const auto& c=Falcon4ClassTable[index]; const char* name="Unknown";
    if(c.dataPtr) {
        if(c.dataType==DTYPE_VEHICLE) name=static_cast<VehicleClassDataType*>(c.dataPtr)->Name;
        else if(c.dataType==DTYPE_WEAPON) name=static_cast<WeaponClassDataType*>(c.dataPtr)->Name;
        else if(c.dataType==DTYPE_FEATURE) name=static_cast<FeatureClassDataType*>(c.dataPtr)->Name;
    }
    std::wstring wide(MultiByteToWideChar(1252,0,name,-1,nullptr,0),0);
    MultiByteToWideChar(1252,0,name,-1,wide.data(),static_cast<int>(wide.size()));
    std::string utf8(WideCharToMultiByte(CP_UTF8,0,wide.c_str(),-1,nullptr,0,nullptr,nullptr),0);
    WideCharToMultiByte(CP_UTF8,0,wide.c_str(),-1,utf8.data(),static_cast<int>(utf8.size()),nullptr,nullptr);
    if(!utf8.empty()) utf8.pop_back(); return ff::headless::JsonString(utf8);
}
SMSBaseClass* stores(SimBaseClass* e) { return (e->IsAirplane()||e->IsHelicopter()||e->IsGroundVehicle()) ? static_cast<SimVehicleClass*>(e)->GetSMS() : nullptr; }
bool inside(CampBaseClass* e) {
    const double dx=e->YPos()/3280.839895-centerX,dy=e->XPos()/3280.839895-centerY;
    return dx*dx+dy*dy<=radius*radius;
}
unsigned liveWeapons() {
    if (!initialized) return 0;
    unsigned n=PendingDetailedDamage();VuListIterator it(SimDriver.objectList);
    for(auto* e=it.GetFirst();e;e=it.GetNext()) {
        auto* s=static_cast<SimBaseClass*>(e);
        // Exploding weapons still need their next native Exec to call SetDead
        // and Sleep. Returning to aggregate scheduling earlier strands them.
        if((s->IsMissile()||s->IsBomb())&&!s->IsDead()&&!s->IsSetRemoveFlag()) ++n;
    }
    return n;
}
unsigned livePilots() {
    if(!initialized) return 0;
    unsigned n=0;VuListIterator it(SimDriver.objectList);
    for(auto* e=it.GetFirst();e;e=it.GetNext()) {
        auto* actor=static_cast<SimBaseClass*>(e);
        if(actor->IsEject() && actor->IsAwake() && !actor->IsDead() && !actor->IsSetRemoveFlag()) ++n;
    }
    return n;
}
void rebuildTargets() {
    SimDriver.combinedList->Purge(); SimDriver.combinedFeatureList->Purge();
    VuListIterator objects(SimDriver.objectList),features(SimDriver.featureList),units(AllUnitList),objectives(AllObjList);
    for(auto* e=objects.GetFirst();e;e=objects.GetNext()) SimDriver.AddToCombUnitList(e);
    for(auto* e=features.GetFirst();e;e=features.GetNext()) SimDriver.AddToCombFeatList(e);
    for(auto* e=units.GetFirst();e;e=units.GetNext()) {
        auto* u=static_cast<UnitClass*>(e);
        if(u->Real()&&u->IsAggregate()&&!u->IsDead()&&!u->Inactive()) SimDriver.AddToCombUnitList(u);
    }
    for(auto* e=objectives.GetFirst();e;e=objectives.GetNext())
        if(static_cast<ObjectiveClass*>(e)->IsAggregate()) SimDriver.AddToCombFeatList(e);
}
}
bool DetailedRegionActive() { return requested || !members.empty() || liveWeapons()!=0 || livePilots()!=0; }
void RememberDetailedAircraftState(AircraftClass* aircraft) {
    rememberAmmunition(aircraft);
    auto* campaign=aircraft->GetCampaignObject();
    if(!campaign || !campaign->IsFlight()) return;
    for(auto it=aircraftBoundaryStates.begin();it!=aircraftBoundaryStates.end();) {
        if(!vuDatabase->Find(it->first.first)) it=aircraftBoundaryStates.erase(it);
        else ++it;
    }
    aircraftBoundaryStates[{campaign->Id(),aircraft->pilotSlot}]={
        max(0,aircraft->counterMeasureStation[CHAFF_STATION].weaponCount),
        max(0,aircraft->counterMeasureStation[FLARE_STATION].weaponCount)};
    auto& state=aircraftBoundaryStates[{campaign->Id(),aircraft->pilotSlot}];
    state.operatorHold=aircraft->DBrain()->IsOperatorWeaponsHold();
    state.masterSafe=aircraft->Sms->IsOperatorMasterSafe();
    state.returning=aircraft->DBrain()->IsReturningToBase();
    for(auto* wp=aircraft->waypoint;wp;wp=wp->GetNextWP())
        if(wp==aircraft->curWaypoint) {
            state.hasReturnWaypoint=true;
            wp->GetLocation(&state.returnLocation[0],&state.returnLocation[1],&state.returnLocation[2]);
            state.returnAction=wp->GetWPAction();state.returnTarget=wp->GetWPTargetID();
            break;
        }
    for(int i=0;i<aircraft->numSensors;++i) {
        auto* sensor=aircraft->sensorArray[i];
        if(sensor && sensor->Type()==SensorClass::Radar) { state.radarPower=sensor->IsOn()?1:0;break; }
    }
}
void RestoreDetailedAircraftAmmunition(AircraftClass* aircraft) {
    restoreAmmunition(aircraft);
}
void RestoreDetailedAircraftState(AircraftClass* aircraft) {
    auto* campaign=aircraft->GetCampaignObject();
    if(!campaign || !campaign->IsFlight()) return;
    const auto it=aircraftBoundaryStates.find({campaign->Id(),aircraft->pilotSlot});
    if(it==aircraftBoundaryStates.end()) return;
    aircraft->DBrain()->SetOperatorWeaponsHold(it->second.operatorHold);
    if(it->second.masterSafe) aircraft->Sms->SetOperatorMasterSafe(true);
    if(it->second.returning) {
        // Campaign replanning can insert or replace waypoints between entries.
        // Require a current landing route, preserve the actual navigation point
        // when it still exists, otherwise resolve the original AI's RTB entry.
        auto* land=aircraft->waypoint;
        while(land && land->GetWPAction()!=WP_LAND) land=land->GetNextWP();
        if(land && land->GetPrevWP()) {
            auto* destination=land->GetPrevWP();
            if(it->second.hasReturnWaypoint) for(auto* wp=aircraft->waypoint;wp;wp=wp->GetNextWP()) {
                std::array<float,3> location;
                wp->GetLocation(&location[0],&location[1],&location[2]);
                if(location==it->second.returnLocation && wp->GetWPAction()==it->second.returnAction &&
                   wp->GetWPTargetID()==it->second.returnTarget) { destination=wp;break; }
            }
            aircraft->DBrain()->SetReturnToBaseWaypoint(destination);
        }
    }
    aircraft->counterMeasureStation[CHAFF_STATION].weaponCount=min(aircraft->counterMeasureStation[CHAFF_STATION].weaponCount,it->second.chaff);
    aircraft->counterMeasureStation[FLARE_STATION].weaponCount=min(aircraft->counterMeasureStation[FLARE_STATION].weaponCount,it->second.flare);
    if(it->second.radarPower>=0) for(int i=0;i<aircraft->numSensors;++i) {
        auto* sensor=aircraft->sensorArray[i];
        if(sensor && sensor->Type()==SensorClass::Radar) { sensor->SetPower(it->second.radarPower);break; }
    }
}
void RememberDetailedHelicopterState(HelicopterClass* helicopter) {
    rememberAmmunition(helicopter);
    auto* campaign=helicopter->GetCampaignObject();
    if(!campaign || !campaign->IsFlight() || !helicopter->Sms) return;
    for(auto it=helicopterSafetyStates.begin();it!=helicopterSafetyStates.end();) {
        if(!vuDatabase->Find(it->first.first)) it=helicopterSafetyStates.erase(it);
        else ++it;
    }
    helicopterSafetyStates[{campaign->Id(),helicopter->pilotSlot}]=helicopter->Sms->IsOperatorMasterSafe();
}
void RestoreDetailedHelicopterState(HelicopterClass* helicopter) {
    restoreAmmunition(helicopter);
    auto* campaign=helicopter->GetCampaignObject();
    if(!campaign || !campaign->IsFlight() || !helicopter->Sms) return;
    const auto it=helicopterSafetyStates.find({campaign->Id(),helicopter->pilotSlot});
    if(it!=helicopterSafetyStates.end() && it->second) helicopter->Sms->SetOperatorMasterSafe(true);
}
std::string QueueDetailedAction(uint32_t creator,uint32_t number,const std::string& action) {
    const bool radarAction=action=="radar_on" || action=="radar_off";
    const bool weaponsAction=action=="weapons_hold" || action=="weapons_free";
    const bool masterAction=action=="master_safe" || action=="master_arm";
    const bool returnAction=action=="return_to_base";
    const bool jettisonAction=action.rfind("jettison_",0)==0;
    if(action!="chaff" && action!="flare" && !radarAction && !weaponsAction && !masterAction && !jettisonAction && !returnAction) return "unsupported_action";
    if(!requested) return "region_inactive";
    auto* entity=vuDatabase->Find(VU_ID(creator,number));
    auto* aircraft=dynamic_cast<AircraftClass*>(entity);
    auto* helicopter=dynamic_cast<HelicopterClass*>(entity);
    auto* actor=dynamic_cast<SimBaseClass*>(entity);
    if(!aircraft && !(masterAction && helicopter)) return "aircraft_unavailable";
    if(!actor->IsLocal() || !actor->IsAwake() || actor->IsDead() || actor->IsExploding() || actor->IsSetRemoveFlag()) return "aircraft_unavailable";
    const double dx=actor->YPos()/3280.839895-centerX,dy=actor->XPos()/3280.839895-centerY;
    if(dx*dx+dy*dy>radius*radius) return "outside_region";
    if(returnAction) {
        if(aircraft->OnGround()) return "aircraft_grounded";
        auto* landing=aircraft->waypoint;
        while(landing && landing->GetWPAction()!=WP_LAND) landing=landing->GetNextWP();
        if(!landing || !landing->GetPrevWP()) return "return_route_unavailable";
        if(aircraft->DBrain()->IsReturningToBase()) return "already_returning";
        FalconWingmanMsg message(aircraft->GetCampaignObject()->Id(),FalconLocalGame);
        message.dataBlock.from=aircraft->Id();message.dataBlock.to=AiWingman;
        message.dataBlock.command=FalconWingmanMsg::WMRTB;
        aircraft->DBrain()->ReceiveOrders(&message);
        return aircraft->DBrain()->IsReturningToBase()?"applied":"orders_unavailable";
    }
    if(jettisonAction) {
        const auto suffix=action.substr(9);
        if(suffix.empty() || suffix.size()>3 || suffix.find_first_not_of("0123456789")!=std::string::npos) return "invalid_station";
        const int hp=std::stoi(suffix);
        auto* sms=aircraft->Sms;
        if(!sms || hp<1 || hp>=sms->NumHardpoints() || !sms->hardPoint[hp]) return "invalid_station";
        if(aircraft->OnGround()) return "aircraft_grounded";
        if(sms->MasterArm()!=SMSBaseClass::Arm) return "master_safe";
        if(!sms->hardPoint[hp]->weaponPointer) return "stores_empty";
        if(!sms->CanJettisonWeapon(hp)) return "jettison_denied";
        sms->JettisonWeapon(hp);
        sms->Exec();
        return !sms->hardPoint[hp]->weaponPointer?"applied":"jettison_denied";
    }
    if(masterAction) {
        auto* sms=aircraft?aircraft->Sms:helicopter->Sms;
        if(!sms) return "stores_unavailable";
        sms->SetOperatorMasterSafe(action=="master_safe");
        sms->Exec();
        return "applied";
    }
    if(weaponsAction) {
        if(aircraft->OnGround()) return "aircraft_grounded";
        if(!aircraft->waypoint) return "orders_unavailable";
        FalconWingmanMsg message(aircraft->GetCampaignObject()->Id(),FalconLocalGame);
        message.dataBlock.from=aircraft->Id();message.dataBlock.to=AiWingman;
        message.dataBlock.command=action=="weapons_hold"?FalconWingmanMsg::WMWeaponsHold:FalconWingmanMsg::WMWeaponsFree;
        const auto state=action=="weapons_hold"?DigitalBrain::AI_WEAPONS_HOLD:DigitalBrain::AI_WEAPONS_FREE;
        auto* brain=aircraft->DBrain();
        const bool previousHold=brain->IsOperatorWeaponsHold();
        if(action=="weapons_free") brain->SetOperatorWeaponsHold(false);
        brain->ReceiveOrders(&message);
        if(brain->GetWeaponsAction()!=int(state)) {
            brain->SetOperatorWeaponsHold(previousHold);
            return "orders_unavailable";
        }
        brain->SetOperatorWeaponsHold(action=="weapons_hold");
        return "applied";
    }
    if(radarAction) {
        for(int i=0;i<aircraft->numSensors;++i) {
            auto* sensor=aircraft->sensorArray[i];
            if(sensor && sensor->Type()==SensorClass::Radar) {
                const BOOL requestedPower=action=="radar_on";
                sensor->SetPower(requestedPower);
                // Restore operation as the native radar-active order does.
                // SetPower alone leaves isEmitting latched off after shutdown.
                if(requestedPower) if(auto* radar=dynamic_cast<RadarClass*>(sensor)) radar->SetEmitting(TRUE);
                return sensor->IsOn()==requestedPower?"applied":"sensor_power_denied";
            }
        }
        return "radar_unavailable";
    }
    if(aircraft->OnGround()) return "aircraft_grounded";
    if(!(GetVehicleClassData(aircraft->Type()-VU_LAST_ENTITY_TYPE)->Flags & 0x40000000)) return "countermeasures_unavailable";
    const int station=action=="chaff"?CHAFF_STATION:FLARE_STATION;
    if(aircraft->counterMeasureStation[station].weaponCount<=0) return "stores_empty";
    auto& command=action=="chaff"?aircraft->dropChaffCmd:aircraft->dropFlareCmd;
    if(command) return "already_queued";
    command=TRUE;
    return "queued";
}
void RecordDetailedTrajectory(SimBaseClass* actor) {
    if(!actor->IsMissile() && !actor->IsBomb()) return;
    pruneTrails();
    auto& trail=trails[id(actor->Id())]; trail.team=actor->GetTeam();
    TrailPoint p{TheCampaign.CurrentTime,actor->XPos(),actor->YPos(),actor->ZPos()};
    if(!trail.points.empty() && trail.points.back().time==p.time) trail.points.back()=p;
    else trail.points.push_back(p);
}
void SetDetailedRegion(bool active,double x,double y,double r) {
    requested=active; centerX=x;centerY=y;radius=r;
    if(active&&!initialized) { InitializeDetailedRuntime(); initialized=true; }
    ReconcileDetailedRegion();
}
void ReconcileDetailedRegion() {
    if(!initialized) return;
    const bool retain=liveWeapons()!=0;
    for(auto it=members.begin();it!=members.end();) {
        auto* e=it->get();
        if(!retain&&(!requested||!inside(e)||e->IsDead())) {
            if(!e->IsAggregate()) e->Reaggregate(FalconLocalSession);
            it=members.erase(it);
        } else ++it;
    }
    if(requested) {
        // Static collision geometry is available before aircraft/ground actors wake.
        VuListIterator objects(AllObjList);
        for(auto* e=objects.GetFirst();e;e=objects.GetNext()) {
            auto* o=static_cast<ObjectiveClass*>(e);
            if(o->IsAggregate()&&o->IsLocal()&&inside(o)&&o->Deaggregate(FalconLocalSession)) {
                if(!o->IsAwake()) o->Wake();
                members.emplace_back(o);
                VuListIterator parts(o->GetComponents());
                for(auto* part=parts.GetFirst();part;part=parts.GetNext()) SyncDetailedModel(static_cast<SimBaseClass*>(part));
            }
        }
        VuListIterator units(AllUnitList);
        // Deaggregation does not alter the campaign list; native pilot/runway
        // readiness can defer a flight. It is retried at the next campaign tick.
        for(auto* e=units.GetFirst();e;e=units.GetNext()) {
            auto* u=static_cast<UnitClass*>(e);
            if(u->Real()&&!u->Inactive()&&!u->IsDead()&&u->IsAggregate()&&u->IsLocal()&&inside(u)&&u->GetTotalVehicles()>0&&u->Deaggregate(FalconLocalSession)) {
                // NEW_WAKE in the desktop engine keys off session ownership.
                // This offline host executes every local parent in its region,
                // including newly planned flights with a different owner ID.
                if(!u->IsAwake()) u->Wake();
                members.emplace_back(u);
            }
        }
    }
    rebuildTargets();
}
std::string DetailedSnapshotJson() {
    pruneTrails();
    std::ostringstream out;
    out.precision(10);
    out<<"{\"active\":"<<(DetailedRegionActive()?"true":"false")<<",\"draining\":"<<(!requested&&DetailedRegionActive()?"true":"false")
       <<",\"weapon_events\":"<<ff_headless::combat.weaponsFired<<",\"damage_events\":"<<ff_headless::combat.detailedDamageMessages
       <<",\"aircraft_weapon_events\":"<<ff_headless::combat.aircraftWeaponsFired<<",\"aircraft_damage_events\":"<<ff_headless::combat.aircraftDamageMessages
       <<",\"naval_weapon_events\":"<<ff_headless::combat.navalWeaponsFired
       <<",\"helicopter_weapon_events\":"<<ff_headless::combat.helicopterWeaponsFired
       <<",\"helicopter_damage_dealt\":"<<ff_headless::combat.helicopterDamageDealt
       <<",\"missile_ends\":"<<ff_headless::combat.missileEnds<<",\"parents\":"<<members.size()<<",\"actors\":[";
    bool comma=false;
    if(SimDriver.objectList) for(auto* list:{static_cast<VuLinkedList*>(SimDriver.objectList),static_cast<VuLinkedList*>(SimDriver.featureList)}) {
        VuListIterator it(list);
        for(auto* e=it.GetFirst();e;e=it.GetNext()) {
            auto* s=static_cast<SimBaseClass*>(e);
            if(!s->IsAwake()||s->IsSetRemoveFlag()) continue;
            if(comma) out<<',';comma=true;
            const int index=s->Type()-VU_LAST_ENTITY_TYPE;
            auto* parent=s->IsWeapon()?static_cast<SimWeaponClass*>(s)->Parent():nullptr;
            auto* campaign=s->GetCampaignObject();
            if(!campaign && parent) {
                if(parent->IsCampaign()) campaign=static_cast<CampBaseClass*>(parent);
                else if(parent->IsSim()) campaign=static_cast<SimBaseClass*>(parent)->GetCampaignObject();
            }
            const char* kind=s->IsEject()?"ejected_pilot":s->IsStatic()?"feature":s->IsMissile()?"missile":s->IsBomb()?"bomb":s->IsAirplane()?"aircraft":s->IsHelicopter()?"helicopter":s->GetDomain()==DOMAIN_SEA?"ship":"ground";
            if(s->IsBomb()) {
                auto* bomb=static_cast<BombClass*>(s);
                if(bomb->IsSetBombFlag(BombClass::IsChaff)) kind="chaff";
                else if(bomb->IsSetBombFlag(BombClass::IsFlare)) kind="flare";
                else if(bomb->IsSetBombFlag(BombClass::IsDebris)) kind="debris";
            }
            const std::string displayName=s->IsEject()?"\"Ejected pilot\"":std::string(kind)=="chaff"?"\"Chaff\"":std::string(kind)=="flare"?"\"Flare\"":std::string(kind)=="debris"?"\"Debris\"":label(index);
            out<<"{\"id\":\""<<id(s->Id())<<"\",\"campaign_id\":\""<<(campaign?id(campaign->Id()):"")<<"\",\"parent_id\":"<<ff::headless::JsonString(parent?id(parent->Id()):"")<<",\"class_id\":"<<index
               <<",\"name\":"<<displayName<<",\"native_class_name\":"<<label(index)<<",\"kind\":\""<<kind<<"\",\"team\":"<<int(parent?parent->GetTeam():s->GetTeam())<<",\"x\":"<<s->YPos()/3280.839895<<",\"y\":"<<s->XPos()/3280.839895
               <<",\"altitude_m\":"<<-s->ZPos()*.3048<<",\"yaw\":"<<s->Yaw()<<",\"pitch\":"<<s->Pitch()<<",\"roll\":"<<s->Roll()
               <<",\"vx_mps\":"<<s->YDelta()*.3048<<",\"vy_mps\":"<<s->XDelta()*.3048<<",\"vz_mps\":"<<-s->ZDelta()*.3048
               <<",\"strength\":"<<s->Strength()<<",\"max_strength\":"<<s->MaxStrength()<<",\"destroyed\":"<<((s->IsDead()||s->IsExploding())?"true":"false")
               <<",\"weapons\":[";
            bool weaponComma=false;
            auto* sms=stores(s);
            if(sms) for(int hp=0;hp<sms->NumHardpoints();++hp) {
                auto* station=sms->hardPoint[hp]; if(!station || !station->weaponId) continue;
                int pods=0,rounds=0,queued=0;
                for(auto* weapon=station->weaponPointer.get();weapon;weapon=weapon->GetNextOnRail()) {
                    if(!weapon->IsLauncher()) continue;
                    auto* pod=static_cast<BombClass*>(weapon);
                    ++pods;rounds+=pod->LauGetRoundsOnboard();queued+=pod->LauGetQueuedRounds();
                }
                // Reserving the last rounds discounts weaponCount before actual
                // launch. A mounted pod and its ammunition must remain visible.
                if(!station->weaponCount && !pods && !station->GetGun()) continue;
                if(weaponComma) out<<',';weaponComma=true;
                out<<"{\"id\":"<<station->weaponId<<",\"count\":"<<station->weaponCount<<",\"station\":"<<hp;
                if(auto* aircraft=dynamic_cast<AircraftClass*>(s)) {
                    const bool supported=hp>0 && station->weaponPointer && !station->GetGun() &&
                        (station->GetRackDataFlags() & RDF_SELECTIVE_JETT_RACK);
                    out<<",\"jettison_supported\":"<<(supported?"true":"false")
                       <<",\"jettison_available\":"<<(supported && !aircraft->OnGround() && aircraft->Sms->MasterArm()==SMSBaseClass::Arm && aircraft->Sms->CanJettisonWeapon(hp)?"true":"false");
                }
                if(pods) out<<",\"pod_count\":"<<pods<<",\"rounds\":"<<rounds<<",\"queued_rounds\":"<<queued;
                if(station->GetGun()) out<<",\"rounds\":"<<station->GetGun()->numRoundsRemaining;
                out<<"}";
            }
            out<<"],\"target_id\":";
            auto* mover=s->IsMover()?static_cast<SimMoverClass*>(s):nullptr;
            auto* target=mover?mover->targetPtr:nullptr;
            out<<ff::headless::JsonString(target?id(target->BaseData()->Id()):"");
            out<<",\"target_range_m\":";
            if(target && s->IsBomb()) {
                // Bomb target wrappers do not maintain mover scan geometry;
                // their zero-initialized localData range is not a measurement.
                const double dx=s->XPos()-target->BaseData()->XPos();
                const double dy=s->YPos()-target->BaseData()->YPos();
                const double dz=s->ZPos()-target->BaseData()->ZPos();
                const double range=std::sqrt(dx*dx+dy*dy+dz*dz);
                if(std::isfinite(range)) out<<range*.3048; else out<<"null";
            }
            else if(target && target->localData && std::isfinite(target->localData->range)) out<<target->localData->range*.3048;
            else out<<"null";
            out<<",\"sensors\":[";
            bool sensorComma=false;
            if(mover) for(int sensorIndex=0;sensorIndex<mover->numSensors;++sensorIndex) {
                auto* sensor=mover->sensorArray[sensorIndex];if(!sensor) continue;
                if(sensorComma) out<<',';sensorComma=true;
                auto* locked=sensor->CurrentTarget();
                out<<"{\"type\":"<<int(sensor->Type())<<",\"powered\":"<<(sensor->IsOn()?"true":"false")
                   <<",\"target_id\":"<<ff::headless::JsonString(locked?id(locked->BaseData()->Id()):"");
                if(auto* radar=dynamic_cast<RadarClass*>(sensor)) {
                    int trackState=SensorClass::NoTrack;
                    if(locked) for(auto* contact=mover->targetList;contact;contact=contact->next)
                        if(contact->BaseData()==locked->BaseData()) {
                            trackState=int(contact->localData->sensorState[SensorClass::Radar]);
                            break;
                        }
                    const bool tracking=radar->IsOn() && radar->IsEmitting() && locked && trackState>=SensorClass::SensorTrack;
                    out<<",\"emitting\":"<<(radar->IsEmitting()?"true":"false")
                       <<",\"track_state\":"<<trackState<<",\"tracking\":"<<(tracking?"true":"false");
                }
                out<<"}";
            }
            out<<"]";
            if(auto* ground=dynamic_cast<GroundClass*>(s); ground && ground->gai) {
                out<<",\"ground_ai\":{\"movement\":"<<int(ground->gai->moveState)
                   <<",\"surface_capable\":"<<(ground->isGroundCapable?"true":"false")<<",\"air_capable\":"<<(ground->isAirCapable?"true":"false")
                   <<",\"fixed_position\":"<<((ground->gai->moveFlags & GNDAI_MOVE_FIXED_POSITIONS)?"true":"false")<<"}";
            }
            if(s->IsHelicopter()) {
                auto* helicopter=static_cast<HelicopterClass*>(s);
                out<<",\"pilot_slot\":"<<int(helicopter->pilotSlot)
                   <<",\"master_arm\":"<<int(helicopter->Sms->MasterArm())
                   <<",\"operator_master_safe\":"<<(helicopter->Sms->IsOperatorMasterSafe()?"true":"false");
                out<<",\"helicopter_ai\":{\"mode\":"<<int(helicopter->hBrain->curMode)
                   <<",\"selected_station\":"<<helicopter->Sms->CurHardpoint()
                   <<",\"commanded_agl_m\":";
                if(helicopter->hBrain->hasAltitudeCommand) out<<helicopter->hBrain->commandedAltitudeAGL*0.3048;
                else out<<"null";
                out<<",\"rocket_aim_valid\":"<<(helicopter->hBrain->rocketAimValid?"true":"false");
                if(helicopter->hBrain->rocketAimValid)
                    out<<",\"rocket_aim_error_m\":"<<helicopter->hBrain->rocketAimError*0.3048
                       <<",\"rocket_aim_tolerance_m\":"<<helicopter->hBrain->rocketAimTolerance*0.3048;
                out<<"}";
            }
            if(s->IsAirplane()) {
                auto* aircraft=static_cast<AircraftClass*>(s);
                bool returnAvailable=false;
                for(auto* wp=aircraft->waypoint;wp;wp=wp->GetNextWP())
                    if(wp->GetWPAction()==WP_LAND && wp->GetPrevWP()) {returnAvailable=true;break;}
                out<<",\"atc_status\":"<<int(aircraft->DBrain()->ATCStatus());
                out<<",\"is_tanker\":"<<(aircraft->DBrain()->IsTanker()?"true":"false")
                   <<",\"refuel_status\":"<<int(aircraft->DBrain()->RefuelStatus())
                   <<",\"refuel_contact\":"<<(aircraft->af->IsSet(AirframeClass::Refueling)?"true":"false")
                   <<",\"tanker_id\":"<<ff::headless::JsonString(aircraft->DBrain()->Tanker()==FalconNullId?"":id(aircraft->DBrain()->Tanker()));
                out<<",\"on_ground\":"<<(aircraft->OnGround()?"true":"false")<<",\"gear_position\":"<<aircraft->af->gearPos;
                out<<",\"returning_to_base\":"<<(aircraft->DBrain()->IsReturningToBase()?"true":"false")
                   <<",\"return_route_available\":"<<(returnAvailable && !aircraft->OnGround()?"true":"false");
                if(aircraft->curWaypoint) {
                    float wx,wy,wz;aircraft->curWaypoint->GetLocation(&wx,&wy,&wz);
                    out<<",\"navigation_target\":{\"x\":"<<wy/3280.839895<<",\"y\":"<<wx/3280.839895<<",\"action\":"<<aircraft->curWaypoint->GetWPAction()<<"}";
                }

                out<<",\"total_weight_kg\":"<<aircraft->af->Mass()*GRAVITY*0.45359237
                   <<",\"fuel_kg\":"<<(aircraft->af->Fuel()+aircraft->af->ExternalFuel())*0.45359237
                   <<",\"store_drag_index\":"<<aircraft->af->GetDragIndex()
                   <<",\"lateral_load_kg_m\":"<<aircraft->af->assymetry*0.45359237*0.3048;
                auto* groundTarget=aircraft->DBrain()->GetGroundTarget();
                out<<",\"ground_target_id\":"<<ff::headless::JsonString(groundTarget?id(groundTarget->BaseData()->Id()):"");
                out<<",\"pilot_slot\":"<<int(aircraft->pilotSlot)
                   <<",\"ai_mode\":"<<int(aircraft->DBrain()->GetCurrentMode())
                   <<",\"ai_weapons_action\":"<<aircraft->DBrain()->GetWeaponsAction()
                   <<",\"operator_weapons_hold\":"<<(aircraft->DBrain()->IsOperatorWeaponsHold()?"true":"false")
                   <<",\"operator_master_safe\":"<<(aircraft->Sms->IsOperatorMasterSafe()?"true":"false")
                   <<",\"ai_missile_wait_ms\":"<<(aircraft->DBrain()->GetMissileShotTime()>SimLibElapsedTime?aircraft->DBrain()->GetMissileShotTime()-SimLibElapsedTime:0)
                   <<",\"ai_bvr_tactic\":"<<int(aircraft->DBrain()->GetBvrTactic())
                   <<",\"ai_radar_spiked\":"<<(aircraft->DBrain()->IsRadarSpiked()?"true":"false")
                   <<",\"ai_max_air_weapon_range_m\":"<<aircraft->DBrain()->GetMaxAAWeaponRange()*.3048
                   <<",\"master_arm\":"<<int(aircraft->Sms->MasterArm())
                   <<",\"selected_station\":"<<aircraft->Sms->CurHardpoint()
                   <<",\"weapon_in_range\":"<<(aircraft->FCC->inRange?"true":"false")
                   <<",\"fire_control_target_id\":"<<ff::headless::JsonString(aircraft->FCC->TargetPtr()?id(aircraft->FCC->TargetPtr()->BaseData()->Id()):"")
                   <<",\"countermeasures\":{\"chaff\":"<<aircraft->counterMeasureStation[CHAFF_STATION].weaponCount
                   <<",\"flare\":"<<aircraft->counterMeasureStation[FLARE_STATION].weaponCount<<"}";
            }
            out<<"}";
        }
    }
    out<<"],\"tracers\":[";comma=false;
    if(initialized) {
        VuListIterator it(SimDriver.objectList);
        for(auto* e=it.GetFirst();e;e=it.GetNext()) {
            auto* actor=static_cast<SimBaseClass*>(e);auto* sms=stores(actor);if(!sms) continue;
            for(int hp=0;hp<sms->NumHardpoints();++hp) {
                auto* station=sms->hardPoint[hp];auto* gun=station?station->GetGun():nullptr;
                if(!gun||!gun->bullet) continue;
                for(int i=0;i<gun->numTracers;++i) {
                    const auto& b=gun->bullet[i];if(!b.flying) continue;
                    if(comma) out<<',';comma=true;
                    out<<"{\"parent_id\":\""<<id(actor->Id())<<"\",\"team\":"<<int(actor->GetTeam())<<",\"x\":"<<b.y/3280.839895<<",\"y\":"<<b.x/3280.839895<<",\"altitude_m\":"<<-b.z*.3048
                       <<",\"vx_mps\":"<<b.ydot*.3048<<",\"vy_mps\":"<<b.xdot*.3048<<",\"vz_mps\":"<<-b.zdot*.3048<<"}";
                }
            }
        }
    }
    out<<"],\"trails\":[";comma=false;
    for(const auto& pair:trails) {
        if(pair.second.points.size()<2) continue;
        if(comma) out<<',';comma=true;
        out<<"{\"id\":\""<<pair.first<<"\",\"team\":"<<pair.second.team<<",\"points\":[";
        bool pointComma=false;
        for(const auto& p:pair.second.points) {
            if(pointComma) out<<',';pointComma=true;
            out<<'['<<p.y/3280.839895<<','<<p.x/3280.839895<<','<<-p.z*.3048<<']';
        }
        out<<"]}";
    }
    out<<"]}";return out.str();
}

void FinishDetailedDrain() { if (!requested && !members.empty() && liveWeapons()==0) ReconcileDetailedRegion(); }
