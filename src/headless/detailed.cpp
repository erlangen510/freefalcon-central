#include "stdhdr.h"
#include "missile.h"
#include "misslist.h"
#include "campweap.h"
#include "weaplist.h"
#include "classtbl.h"
#include "camplist.h"
#include "unit.h"
#include "update.h"
#include "team.h"
#include "simveh.h"
#include "object.h"
#include "geometry.h"
#include "simdrive.h"
#include "boundary.h"
#include "otwdrive.h"
#include "sensclas.h"
#include "radar.h"
#include "visual.h"
#include "mvrdef.h"
#include "ground.h"
#include "gndai.h"
#include "helo.h"
#include "helimm.h"
#include "hdigi.h"
#include "digi.h"
#include "sms.h"
#include "guns.h"
#include "falcdmg.h"
#include <sstream>
#include "drawbsp.h"
#include "fcc.h"
#include "mission.h"
#include "flight.h"
#include "squadron.h"
#include "campwp.h"
#include "objectiv.h"
#include "simfeat.h"
#include "arfrmdat.h"
#include "aircrft.h"
#include "airframe.h"
#include "wingorder.h"
#include "atcbrain.h"
#include "tankbrn.h"
#include "msginc/tankermsg.h"
#include <map>
#include <set>
#include <array>
#include "bombdata.h"
#include "bomb.h"
#include "bombfunc.h"
extern void ReadDigitalBrainData();
extern short NumWeaponTypes;
extern void DoCampaignLoop(int);
extern void UpdateRealUnits(CampaignTime);
extern void SetTime(unsigned long);
extern void F4FdirCacheInsert(const char*,const char*,int,int);
#include "playerop.h"
#include "detailed.h"
#include <stdexcept>
#include <cstdio>
#include <cmath>
#include <iostream>
#include "tod.h"
#include "timemgr.h"
#include "timerthread.h"
#include "scenario.h"

extern void InitializeDetailedLists();
extern void SyncDetailedModel(SimBaseClass*);
extern void DispatchDetailedMessages();
extern short NumSimWeaponEntries;
extern short NumRocketTypes;
extern void GraphicsDataPoolInitializeStorage();
extern void LoadDetailedTerrain(const char*);
extern void LoadDetailedMaterials(const char*);
extern void LoadDetailedModels(const char*);
extern char FalconObjectDataDir[];
extern char FalconTerrainDataDir[];
extern float SimLibLastMajorFrameTime;
extern void CalcTransformMatrix(SimBaseClass*);

void InitializeDetailedRuntime() {
    static bool ready = false;
    if (ready) return;
    LoadDetailedTerrain((std::string(FalconTerrainDataDir) + "/terrain").c_str());
    LoadDetailedMaterials(FalconTerrainDataDir);
    TheTimeManager.Setup(2000, 100);
    TheTimeManager.SetTime(TheCampaign.CurrentTime);
    std::string weather = std::string(FalconTerrainDataDir) + "/weather";
    TheTimeOfDay.Setup(weather.data());
    if (!TheTimeOfDay.IsReady()) throw std::runtime_error("Native time-of-day data did not initialize");
    LoadDetailedModels((std::string(FalconObjectDataDir) + "/KoreaObj.Dxh").c_str());
    GraphicsDataPoolInitializeStorage();
    InitializeDetailedLists();
    SimMoverDefinition::ReadSimMoverDefinitionData();
    { F4FdirCacheInsert("formdat.fil", "sim/acdata/formdata/formdat.fil", 0, 0); ReadDigitalBrainData(); ReadAllAirframeData(); ReadAllBombData(); }
    OTWDriver.SetActive(TRUE);
    PlayerOptions.ObjDeaggLevel = 100;
    PlayerOptions.BldDeaggLevel = 5;
    SimLibElapsedTime = TheCampaign.CurrentTime;
    SimLibMajorFrameTime = SimLibMinorFrameTime = 0.02f;
    SimLibLastMajorFrameTime = 0.02f;
    SimLibMinorFrameRate = 50;
    SimLibMinorPerMajor = 1;
    ready = true;
}
void AdvanceDetailedFrame() {
    // The host owns pacing, but original model gates still consult the legacy
    // pause/compression flag (notably ground steering). Every executed fixed
    // frame advances at native 1x; restore the host's idle state afterwards.
    struct NativeFrameClock {
        int previous;
        NativeFrameClock():previous(gameCompressionRatio) { gameCompressionRatio=1; }
        ~NativeFrameClock() { gameCompressionRatio=previous; }
    } clock;
    SetTime(TheCampaign.CurrentTime + 20);
    SimLibElapsedTime = TheCampaign.CurrentTime;
    SimLibFrameElapsed = static_cast<float>(SimLibElapsedTime);
    UPDATE_SIM_ELAPSED_SECONDS;
    TheTimeManager.SetTime(TheCampaign.CurrentTime);
    SimDriver.Cycle();
    DispatchDetailedDamage();
    DispatchDetailedMessages();
    FinishDetailedDrain();
    ++SimLibFrameCount;
}

extern void RunCountermeasureDiagnostic();
void RunBombDamageDiagnostic(AircraftClass* aircraft, bool hit) {
    auto* sms=aircraft->Sms;
    if(!sms->FindWeaponClass(wcBombWpn)) throw std::runtime_error("Bomb damage fixture requires native stores");
    auto* prototype=dynamic_cast<BombClass*>(sms->GetCurrentWeapon());
    if(!prototype) throw std::runtime_error("Selected native store is not a bomb");
    Unit ground=nullptr;VuListIterator units(AllUnitList);
    for(auto* e=units.GetFirst();e;e=units.GetNext()) {
        auto* u=static_cast<UnitClass*>(e);
        if(u->IsBattalion() && u->IsAggregate() && !u->IsDead() && u->GetTotalVehicles()>0) { ground=u;break; }
    }
    if(!ground || !ground->Deaggregate(FalconLocalSession) || !ground->Wake()) throw std::runtime_error("No native bomb damage ground fixture");
    VuListIterator components(ground->GetComponents());
    GroundClass* victim=nullptr;
    for(auto* e=components.GetFirst();e;e=components.GetNext()) {
        auto* actor=dynamic_cast<GroundClass*>(e);
        if(!actor || !actor->gai) continue;
        actor->gai->moveFlags |= GNDAI_MOVE_FIXED_POSITIONS;
        actor->gai->moveState=GNDAI_MOVE_HALTED;
        if(!victim && actor->Strength()>0) victim=actor;
    }
    if(!victim) throw std::runtime_error("No live stationary bomb victim");
    VuBin<GroundClass> retainedVictim(victim);
    const float initial=victim->Strength();
    // Controlled vertical drop over a stationary native unit, or ten NM away.
    // After Start, no bomb/target position or damage value is modified.
    vector position={victim->XPos()+(hit?0.0f:10.0f*NM_TO_FT),victim->YPos(),0};
    position.z=OTWDriver.GetGroundLevel(position.x,position.y)-1000.0f;
    vector velocity={0,0,0};
    VuBin<BombClass> bomb(static_cast<BombClass*>(InitABomb(aircraft,prototype->GetWeaponId(),0)));
    bomb->Start(&position,&velocity,sms->hardPoint[sms->CurHardpoint()]->GetWeaponData()->cd);
    vuDatabase->Insert(bomb.get());bomb->Wake();
    const auto identity=bomb->Id();const auto key=std::make_pair(identity.creator_,identity.num_);
    aircraft->DBrain()->SetOperatorWeaponsHold(true);
    const std::string initialSnapshot=DetailedSnapshotJson();
    std::string resultSnapshot;
    unsigned frames=0;
    for(;frames<3000 && !ff_headless::combat.projectileEnds[key];++frames) {
        AdvanceDetailedFrame();
        if(resultSnapshot.empty() && victim->Strength()<initial) resultSnapshot=DetailedSnapshotJson();
    }
    if(resultSnapshot.empty()) resultSnapshot=DetailedSnapshotJson();
    for(int i=0;i<250;++i) AdvanceDetailedFrame();
    const float final=victim->Strength();
    const auto attributed=ff_headless::combat.projectileDamage[key];
    const auto victimEvents=ff_headless::combat.projectileVictimDamage[{identity.creator_,identity.num_,victim->Id().creator_,victim->Id().num_}];
    fprintf(stderr,"[bomb-damage] hit=%d initial=%.1f final=%.1f attributed=%u ends=%u frames=%u\n",int(hit),initial,final,attributed,ff_headless::combat.projectileEnds[key],frames);
    if(ff_headless::combat.projectileEnds[key]!=1 || vuDatabase->Find(identity)) throw std::runtime_error("Damage-test bomb did not terminate and retire");
    if(hit ? !(final<initial && victimEvents>0) : (final!=initial || victimEvents!=0)) throw std::runtime_error("Native bomb proximity damage did not match hit/miss geometry");
    std::cout<<"{\"status\":\"ok\",\"case\":\""<<(hit?"bomb-ground-hit":"bomb-ground-miss")<<"\",\"initial_strength\":"<<initial<<",\"final_strength\":"<<final<<",\"attributed_damage\":"<<attributed<<",\"victim_damage_events\":"<<victimEvents<<",\"terminal_events\":1,\"frames\":"<<frames<<",\"victim_id\":\""<<victim->Id().creator_<<":"<<victim->Id().num_<<"\",\"initial_detail\":"<<initialSnapshot<<",\"result_detail\":"<<resultSnapshot<<"}"<<std::endl;
}
void RunLegacyRocketDiagnostic(AircraftClass* aircraft) {
    auto* sms=aircraft->Sms;
    if(!sms->FindWeaponClass(wcRocketWpn)) throw std::runtime_error("Legacy rocket fixture has no rockets");
    auto* pod=dynamic_cast<BombClass*>(sms->GetCurrentWeapon());
    if(!pod || !pod->IsLauncher() || pod->GetType()!=TYPE_ROCKET) throw std::runtime_error("Legacy RKT pod was not created as launcher");
    const int weaponId=pod->GetWeaponId(),projectileId=pod->LauGetWeaponId();
    int mappedId=0,mappedRounds=0;
    for(int i=0;i<NumRocketTypes;++i) if(RocketDataTable[i].weaponId==weaponId) { mappedId=RocketDataTable[i].nweaponId;mappedRounds=RocketDataTable[i].weaponCount; }
    const int before=pod->LauGetRoundsRemaining();
    if(projectileId!=mappedId || before!=mappedRounds || before<2) throw std::runtime_error("Legacy pod differs from original RKT mapping");
    VuBin<BombClass> retainedPod(pod);
    sms->SetMasterArm(SMSBaseClass::Arm);sms->SetAGBPair(false);
    sms->LaunchRocket();sms->Exec();
    VuBin<MissileClass> projectile;
    int created=0;
    VuListIterator objects(SimDriver.objectList);
    for(auto* e=objects.GetFirst();e;e=objects.GetNext()) {
        auto* m=dynamic_cast<MissileClass*>(e);
        if(m && m->Parent()==aircraft) { projectile.reset(m);++created; }
    }
    bool podOnRail=false;
    for(int hp=0;hp<sms->NumHardpoints();++hp) for(auto* weapon=sms->hardPoint[hp]->weaponPointer.get();weapon;weapon=weapon->GetNextOnRail()) if(weapon==pod) podOnRail=true;
    if(!podOnRail || vuDatabase->Find(pod->Id())) throw std::runtime_error("Legacy launcher left its aircraft rail");
    if(created!=1 || !projectile || projectile->GetWeaponId()!=mappedId || !projectile->IsAwake()) throw std::runtime_error("Legacy pod did not create exactly its mapped projectile");
    aircraft->DBrain()->SetOperatorWeaponsHold(true);sms->Exec();
    if(pod->LauGetRoundsRemaining()!=before-1 || pod->LauIsFiring()) throw std::runtime_error("Legacy pod cancellation lost unspent ammunition");
    const float x=projectile->XPos(),y=projectile->YPos(),z=projectile->ZPos();
    for(int frame=0;frame<50;++frame) AdvanceDetailedFrame();
    const float moved=std::sqrt((projectile->XPos()-x)*(projectile->XPos()-x)+(projectile->YPos()-y)*(projectile->YPos()-y)+(projectile->ZPos()-z)*(projectile->ZPos()-z));
    if(!std::isfinite(moved) || moved<100 || pod->LauGetRoundsRemaining()!=before-1) throw std::runtime_error("Mapped rocket did not advance in native flight or held ammunition changed");
    std::cout<<"{\"status\":\"ok\",\"case\":\"rocket-legacy-pod\",\"pod_id\":"<<weaponId<<",\"projectile_id\":"<<projectileId<<",\"rounds_before\":"<<before<<",\"rounds_after\":"<<pod->LauGetRoundsRemaining()<<",\"flight_m\":"<<moved*0.3048f<<"}"<<std::endl;
}

void RunRocketHoldDiagnostic(AircraftClass* aircraft) {
    auto dryWeight=[](AircraftClass* ac) { return ac->af->Mass()*GRAVITY-ac->af->Fuel()-ac->af->ExternalFuel(); };
    const float initialDryWeight=dryWeight(aircraft);
    const float initialAsymmetry=aircraft->af->assymetry,initialDrag=aircraft->af->GetDragIndex();
    auto* sms=aircraft->Sms;
    if(!sms->FindWeaponClass(wcRocketWpn)) throw std::runtime_error("Rocket hold fixture requires loaded native pods");
    sms->SetMasterArm(SMSBaseClass::Arm);sms->SetAGBPair(false);
    auto* pod=dynamic_cast<BombClass*>(sms->GetCurrentWeapon());
    fprintf(stderr,"[rocket-hold] hp=%d weapon=%d bomb=%d launcher=%d\n",sms->CurHardpoint(),sms->GetCurrentWeapon()?sms->GetCurrentWeapon()->GetWeaponId():-1,pod!=nullptr,pod?pod->IsLauncher():0);
    if(!pod || !pod->IsLauncher()) throw std::runtime_error("Selected rocket is not a native launcher");
    auto rockets=[&]() { int n=0;VuListIterator it(SimDriver.objectList);for(auto* e=it.GetFirst();e;e=it.GetNext()) { auto* m=dynamic_cast<MissileClass*>(e);if(m && m->Parent()==aircraft) ++n; }return n; };
    auto pods=[&]() { int n=0;for(int hp=0;hp<sms->NumHardpoints();++hp) if(sms->hardPoint[hp]->GetWeaponClass()==wcRocketWpn) n+=sms->hardPoint[hp]->weaponCount;return n; };
    const int before=pod->LauGetRoundsRemaining(),objects=rockets(),podsBefore=pods();
    const int podStation=sms->CurHardpoint();
    sms->SetOperatorMasterSafe(true);sms->SetMasterArm(SMSBaseClass::Arm);
    sms->LaunchRocket();sms->Exec();
    if(sms->MasterArm()!=SMSBaseClass::Safe || rockets()!=objects || pods()!=podsBefore)
        throw std::runtime_error("AI arm request bypassed operator master safe");
    sms->SetOperatorMasterSafe(false);
    for(auto armState : {SMSBaseClass::Safe,SMSBaseClass::Sim}) {
        sms->SetCurrentWeapon(podStation,pod);sms->SetMasterArm(armState);
        sms->LaunchRocket();sms->Exec();
        if(rockets()!=objects || pod->LauIsFiring() || pod->LauGetRoundsOnboard()!=before || pods()!=podsBefore)
            throw std::runtime_error("Safe/Sim master arm allowed a real rocket request");
        sms->SetMasterArm(SMSBaseClass::Arm);sms->LaunchRocket();
        if(!pod->LauIsFiring()) throw std::runtime_error("Armed salvo was not queued for master-arm cancellation");
        sms->SetMasterArm(armState);sms->Exec();
        if(rockets()!=objects || pod->LauIsFiring() || sms->IsFiringRockets() || pod->LauGetRoundsOnboard()!=before || pods()!=podsBefore)
            throw std::runtime_error("Safe/Sim master arm did not cancel queued rockets without ammunition loss");
        sms->SetMasterArm(SMSBaseClass::Arm);sms->Exec();
        if(rockets()!=objects) throw std::runtime_error("Old rocket salvo revived on arming");
    }
    sms->SetCurrentWeapon(podStation,pod);
    const auto initialDetail=DetailedSnapshotJson();
    sms->LaunchRocket();
    const auto queuedDetail=DetailedSnapshotJson();
    sms->FreeWeaponGraphics();sms->AddWeaponGraphics();
    if(fabs(dryWeight(aircraft)-initialDryWeight)>0.5f)
        throw std::runtime_error("Queued rocket graphics refresh changed onboard payload weight");
    if(fabs(aircraft->af->assymetry-initialAsymmetry)>0.5f || fabs(aircraft->af->GetDragIndex()-initialDrag)>0.01f)
        throw std::runtime_error("Queued graphics refresh changed lateral loading or drag");
    sms->Exec();
    if(before<3 || rockets()!=objects+1 || !pod->LauIsFiring()) throw std::runtime_error("Native rocket salvo did not start");
    aircraft->DBrain()->SetOperatorWeaponsHold(true);
    // Exercise the native scheduler past its deadline without moving projectiles.
    SetTime(TheCampaign.CurrentTime+10000);SimLibElapsedTime=TheCampaign.CurrentTime;
    sms->Exec();
    if(rockets()!=objects+1 || pod->LauIsFiring() || sms->IsFiringRockets() || pod->LauGetRoundsRemaining()!=before-1 || pods()!=podsBefore || pod->targetPtr)
        throw std::runtime_error("Operator hold did not cancel native rocket salvo without consuming rounds");
    const auto heldDetail=DetailedSnapshotJson();
    sms->FindWeaponClass(wcRocketWpn);sms->LaunchRocket();sms->Exec();
    if(rockets()!=objects+1 || sms->IsFiringRockets() || pods()!=podsBefore || pod->LauGetRoundsRemaining()!=before-1) throw std::runtime_error("Held direct rocket request was accepted or changed inventory");
    aircraft->DBrain()->SetOperatorWeaponsHold(false);sms->Exec();
    if(rockets()!=objects+1) throw std::runtime_error("Cancelled rocket salvo revived on free");
    sms->FindWeaponClass(wcRocketWpn);sms->LaunchRocket();sms->Exec();
    if(rockets()!=objects+2) throw std::runtime_error("Fresh rocket request did not resume fire");
    auto* gun=aircraft->Guns;
    if(!gun || aircraft->OnGround() || gun->unlimitedAmmo) throw std::runtime_error("Gun inventory fixture requires an airborne finite-ammunition gun");
    const int gunBefore=gun->numRoundsRemaining;
    int gunStation=-1;
    for(int hp=0;hp<sms->NumHardpoints();++hp)
        if(sms->hardPoint[hp] && sms->hardPoint[hp]->GetGun()==gun) gunStation=hp;
    if(gunStation<0) throw std::runtime_error("Gun inventory fixture has no mounted station");
    aircraft->fireGun=1;
    for(int tick=0;tick<7;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        aircraft->DoWeapons();DispatchDetailedMessages();
    }
    aircraft->fireGun=0;
    const int gunRemaining=gun->numRoundsRemaining;
    if(gunRemaining<=0 || gunRemaining>=gunBefore) throw std::runtime_error("Native gun inventory fixture did not expend rounds");
    // Preserve the real partially used pod through native campaign recreation.
    // Drain only the already launched projectiles while the carrier is held
    // stationary, so this inventory fixture does not depend on its AI mission.
    sms->SetOperatorMasterSafe(true);sms->Exec();
    for(int tick=0;tick<1000;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        aircraft->DoWeapons();DispatchDetailedMessages();
    }
    if(gun->numFlying || gun->numRoundsRemaining!=gunRemaining) throw std::runtime_error("Gun inventory fixture failed to drain airborne bullets while Safe");
    const float gunRemainder=gun->GetAmmunitionRemainder();
    const int remaining=pod->LauGetRoundsOnboard();
    auto podInventory=[](SMSBaseClass* stores) {
        std::vector<std::array<int,4>> inventory;
        for(int hp=0;hp<stores->NumHardpoints();++hp) {
            if(!stores->hardPoint[hp]) continue;
            int rail=0;
            for(auto* w=stores->hardPoint[hp]->weaponPointer.get();w;w=w->GetNextOnRail()) {
                if(!w->IsLauncher()) continue;
                auto* p=static_cast<BombClass*>(w);
                if(p->LauIsFiring()) throw std::runtime_error("Reentry inventory contains a queued salvo");
                inventory.push_back({hp,rail++,w->GetWeaponId(),p->LauGetRoundsOnboard()});
            }
        }
        return inventory;
    };
    const auto expectedInventory=podInventory(sms);
    const float partialDryWeight=dryWeight(aircraft);
    float launchedWeight=0;
    VuListIterator launchedWeapons(SimDriver.objectList);
    for(auto* e=launchedWeapons.GetFirst();e;e=launchedWeapons.GetNext()) {
        auto* m=dynamic_cast<MissileClass*>(e);
        if(m && m->Parent()==aircraft) launchedWeight+=WeaponDataTable[m->GetWeaponId()].Weight;
    }
    if(launchedWeight<=0 || fabs(initialDryWeight-partialDryWeight-launchedWeight)>0.5f)
        throw std::runtime_error("Native rocket release did not subtract actual projectile weight");
    const auto reentryBeforeDetail=DetailedSnapshotJson();
    auto* flight=static_cast<FlightClass*>(aircraft->GetCampaignObject());
    const int pilot=aircraft->pilotSlot;
    const auto originalId=aircraft->Id();
    VuBin<AircraftClass> original(aircraft);
    for(int tick=0;tick<12000;++tick) {
        std::vector<VuBin<MissileClass>> airborne;
        VuListIterator missiles(SimDriver.objectList);
        for(auto* e=missiles.GetFirst();e;e=missiles.GetNext()) {
            auto* m=dynamic_cast<MissileClass*>(e);
            if(m && m->Parent()==aircraft && m->IsAwake() && !m->IsDead() && !m->IsSetRemoveFlag()) airborne.emplace_back(m);
        }
        if(airborne.empty()) break;
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        for(auto& m:airborne) m->Exec();
        DispatchDetailedMessages();
        if(tick==11999) throw std::runtime_error("Inventory fixture rockets failed to terminate");
    }
    if(!flight->Reaggregate(FalconLocalSession)) throw std::runtime_error("Rocket inventory fixture failed native reaggregation");
    const int campaignGunGroup=gun->IsTracer()?10:1;
    const int partialCampaignCount=flight->GetUnitWeaponCount(gunStation,flight->GetAdjustedPlayerSlot(pilot));
    fprintf(stderr,"[partial-gun-campaign] rounds=%d group=%d count=%d\n",gunRemaining,campaignGunGroup,partialCampaignCount);
    if(partialCampaignCount!=(gunRemaining+campaignGunGroup-1)/campaignGunGroup)
        throw std::runtime_error("Campaign gun loadout did not reflect native partial expenditure");
    if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Rocket inventory fixture failed native deaggregation");
    // VuIterator unlocks its collection on destruction. Destroy it before any
    // Reaggregate call frees and replaces the flight's component collection.
    auto findRecreatedPilot=[&]() -> AircraftClass* {
        VuListIterator parts(flight->GetComponents());
        for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
            auto* ac=dynamic_cast<AircraftClass*>(e);
            if(ac && ac->pilotSlot==pilot) return ac;
        }
        return nullptr;
    };
    AircraftClass* recreated=findRecreatedPilot();
    if(!recreated || recreated->Id()==originalId) throw std::runtime_error("Rocket carrier was not recreated");
    if(!recreated->IsAwake()) recreated->Wake();
    fprintf(stderr,"[rocket-mass-reentry] initial=%.3f partial=%.3f recreated=%.3f\n",initialDryWeight,partialDryWeight,dryWeight(recreated));
    if(fabs(dryWeight(recreated)-partialDryWeight)>0.5f)
        throw std::runtime_error("Native recreation changed rocket payload weight without changing inventory");
    fprintf(stderr,"[gun-reentry] initial=%d expected=%d restored=%d\n",gunBefore,gunRemaining,recreated->Guns?recreated->Guns->numRoundsRemaining:-1);
    if(!recreated->Guns || recreated->Guns->numRoundsRemaining!=gunRemaining || recreated->Guns->GetAmmunitionRemainder()!=gunRemainder)
        throw std::runtime_error("Native recreation refilled or rounded gun ammunition");
    auto* restoredPod=dynamic_cast<BombClass*>(recreated->Sms->hardPoint[podStation]->weaponPointer.get());
    fprintf(stderr,"[rocket-reentry] before=%d expected=%d restored=%d\n",before,remaining,restoredPod?restoredPod->LauGetRoundsOnboard():-1);
    if(remaining>=before || remaining<=0 || !restoredPod || !restoredPod->IsLauncher() || restoredPod->LauGetRoundsOnboard()!=remaining || restoredPod->LauIsFiring())
        throw std::runtime_error("Native recreation refilled or lost a partially used rocket pod");
    if(podInventory(recreated->Sms)!=expectedInventory)
        throw std::runtime_error("Native recreation changed another pod or rail inventory");
    fprintf(stderr,"[rocket-reentry] all pod inventories matched; exporting snapshot\n");
    const auto recreatedDetail=DetailedSnapshotJson();
    VuBin<AircraftClass> partiallyUsed(recreated);
    recreated->Sms->SetOperatorMasterSafe(false);recreated->fireGun=1;
    for(int tick=0;tick<10000 && recreated->Guns->numRoundsRemaining>0;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        recreated->DoWeapons();DispatchDetailedMessages();
    }
    if(recreated->Guns->numRoundsRemaining!=0) throw std::runtime_error("Native gun did not exhaust its remaining ammunition");
    recreated->fireGun=0;recreated->Sms->SetOperatorMasterSafe(true);
    for(int tick=0;tick<1000;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        recreated->DoWeapons();DispatchDetailedMessages();
    }
    if(recreated->Guns->numFlying) throw std::runtime_error("Exhausted gun still has airborne bullets at regional exit");
    if(!flight->Reaggregate(FalconLocalSession)) throw std::runtime_error("Empty gun fixture failed native reaggregation");
    const int campaignPilot=flight->GetAdjustedPlayerSlot(pilot);
    fprintf(stderr,"[empty-gun-campaign] pilot=%d count=%d\n",campaignPilot,flight->GetUnitWeaponCount(gunStation,campaignPilot));
    if(flight->GetUnitWeaponCount(gunStation,campaignPilot)!=0)
        throw std::runtime_error("Campaign loadout retained ammunition for an exhausted gun");
    for(auto movement:{Air,NoMove}) for(int range=0;range<3;++range) {
        int selectedStation=-1;
        flight->GetBestVehicleWeapon(campaignPilot,DefaultDamageMods,movement,range,&selectedStation);
        if(selectedStation==gunStation) throw std::runtime_error("Aggregate weapon selection chose an exhausted gun");
    }
    if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Empty gun fixture failed native deaggregation");
    AircraftClass* emptyCarrier=findRecreatedPilot();
    if(!emptyCarrier || emptyCarrier->Id()==recreated->Id()) throw std::runtime_error("Empty gun carrier was not recreated");
    if(!emptyCarrier->IsAwake()) emptyCarrier->Wake();
    if(!emptyCarrier->Guns || emptyCarrier->Guns->numRoundsRemaining!=0)
        throw std::runtime_error("Native recreation refilled an empty gun");
    emptyCarrier->Sms->SetOperatorMasterSafe(false);emptyCarrier->fireGun=1;
    for(int tick=0;tick<100;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        emptyCarrier->DoWeapons();DispatchDetailedMessages();
        if(emptyCarrier->Guns->numRoundsRemaining!=0 || emptyCarrier->Guns->numFlying)
            throw std::runtime_error("Recreated empty gun generated ammunition or bullets");
    }
    emptyCarrier->fireGun=0;
    const auto emptyDetail=DetailedSnapshotJson();
    VuBin<AircraftClass> emptyGunCarrier(emptyCarrier);
    auto* depletedSms=emptyCarrier->Sms;
    auto* depletedPod=dynamic_cast<BombClass*>(depletedSms->hardPoint[podStation]->weaponPointer.get());
    if(!depletedPod || !depletedPod->IsLauncher()) throw std::runtime_error("Empty pod fixture has no mounted launcher");
    const int expendedPodRounds=depletedPod->LauGetRoundsOnboard();
    const float preSalvoAsymmetry=emptyCarrier->af->assymetry,preSalvoDrag=emptyCarrier->af->GetDragIndex();
    float podX,podY,podZ;
    depletedSms->hardPoint[podStation]->GetPosition(&podX,&podY,&podZ);
    const float expectedAsymmetryChange=-expendedPodRounds*WeaponDataTable[depletedPod->LauGetWeaponId()].Weight*podY;
    depletedSms->SetAGBPair(false);depletedSms->SetCurrentWeapon(podStation,depletedPod);
    emptyCarrier->FCC->SetTarget(nullptr);depletedSms->LaunchRocket();
    unsigned drainedRockets=0;
    std::set<VU_ID> finalSalvo;
    for(int tick=0;tick<12000;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        depletedSms->Exec();
        std::vector<VuBin<MissileClass>> airborne;
        VuListIterator objects(SimDriver.objectList);
        for(auto* e=objects.GetFirst();e;e=objects.GetNext()) {
            auto* m=dynamic_cast<MissileClass*>(e);
            if(m && m->Parent()==emptyCarrier && m->IsAwake() && !m->IsDead() && !m->IsSetRemoveFlag()) {
                finalSalvo.insert(m->Id());airborne.emplace_back(m);
            }
        }
        for(auto& m:airborne) m->Exec();
        DispatchDetailedMessages();
        if(!depletedPod->LauGetRoundsOnboard() && !depletedSms->IsFiringRockets() && airborne.empty()) break;
        if(tick==11999) throw std::runtime_error("Final native pod salvo failed to empty and drain");
    }
    for(const auto& projectile:finalSalvo) drainedRockets+=ff_headless::combat.projectileEnds[{projectile.creator_,projectile.num_}];
    if(int(finalSalvo.size())!=expendedPodRounds || drainedRockets!=finalSalvo.size())
        throw std::runtime_error("Pod exhaustion did not launch and terminate every remaining rocket");
    depletedSms->SetOperatorMasterSafe(true);depletedSms->Exec();
    const float exhaustedDryWeight=dryWeight(emptyCarrier);
    const float exhaustedAsymmetry=emptyCarrier->af->assymetry;
    const float exhaustedDrag=emptyCarrier->af->GetDragIndex();
    if(fabs(expectedAsymmetryChange)<1 || fabs(exhaustedAsymmetry-preSalvoAsymmetry-expectedAsymmetryChange)>0.5f || fabs(exhaustedDrag-preSalvoDrag)>0.01f)
        throw std::runtime_error("Native pod firing disagrees with actual lateral ammunition position or changes container drag");
    const auto emptyPodBefore=DetailedSnapshotJson();
    if(!flight->Reaggregate(FalconLocalSession) || !flight->Deaggregate(FalconLocalSession))
        throw std::runtime_error("Empty pod carrier failed native recreation");
    AircraftClass* emptyPodCarrier=findRecreatedPilot();
    if(!emptyPodCarrier || emptyPodCarrier->Id()==emptyCarrier->Id()) throw std::runtime_error("Empty pod carrier was not recreated");
    if(!emptyPodCarrier->IsAwake()) emptyPodCarrier->Wake();
    fprintf(stderr,"[rocket-load-reentry] asymmetry=%.3f->%.3f drag=%.3f->%.3f\n",exhaustedAsymmetry,emptyPodCarrier->af->assymetry,exhaustedDrag,emptyPodCarrier->af->GetDragIndex());
    if(fabs(emptyPodCarrier->af->assymetry-exhaustedAsymmetry)>0.5f || fabs(emptyPodCarrier->af->GetDragIndex()-exhaustedDrag)>0.01f)
        throw std::runtime_error("Empty pod recreation changed lateral loading or drag");
    if(fabs(dryWeight(emptyPodCarrier)-exhaustedDryWeight)>0.5f)
        throw std::runtime_error("Empty pod recreation restored expended rocket weight");
    emptyPodCarrier->Sleep();emptyPodCarrier->Wake();
    if(fabs(emptyPodCarrier->af->assymetry-exhaustedAsymmetry)>0.5f || fabs(emptyPodCarrier->af->GetDragIndex()-exhaustedDrag)>0.01f)
        throw std::runtime_error("Native sleep/wake changed lateral loading or drag");
    if(fabs(dryWeight(emptyPodCarrier)-exhaustedDryWeight)>0.5f)
        throw std::runtime_error("Native sleep/wake duplicated remaining ammunition weight");
    auto* emptyPod=dynamic_cast<BombClass*>(emptyPodCarrier->Sms->hardPoint[podStation]->weaponPointer.get());
    fprintf(stderr,"[empty-pod-reentry] station=%d fired=%d mounted=%d rounds=%d\n",podStation,expendedPodRounds,emptyPod!=nullptr,emptyPod?emptyPod->LauGetRoundsOnboard():-1);
    if(!emptyPod || !emptyPod->IsLauncher() || emptyPod->LauGetRoundsOnboard()!=0)
        throw std::runtime_error("Native recreation removed or refilled an attached empty rocket pod");
    const auto firesBeforeEmptyRequest=ff_headless::combat.weaponsFired;
    emptyPodCarrier->Sms->SetOperatorMasterSafe(false);
    for(int tick=0;tick<100;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        emptyPodCarrier->Sms->SetCurrentWeapon(podStation,emptyPod);
        emptyPodCarrier->Sms->LaunchRocket();emptyPodCarrier->Sms->Exec();
        DispatchDetailedMessages();
        if(emptyPod->LauIsFiring() || emptyPod->LauGetRoundsOnboard()!=0 ||
           emptyPodCarrier->Sms->hardPoint[podStation]->weaponCount!=0 || ff_headless::combat.weaponsFired!=firesBeforeEmptyRequest)
            throw std::runtime_error("Recreated empty pod generated rockets or became selectable");
    }
    const auto emptyPodDetail=DetailedSnapshotJson();
    auto* jettSms=emptyPodCarrier->Sms;
    int jettStation=-1;
    BombClass* jettPod=nullptr;
    for(int hp=1;hp<jettSms->NumHardpoints();++hp) {
        auto* candidate=dynamic_cast<BombClass*>(jettSms->hardPoint[hp]->weaponPointer.get());
        if(candidate && candidate->IsLauncher() && candidate->LauGetRoundsOnboard()>0) {
            jettStation=hp;jettPod=candidate;break;
        }
    }
    if(!jettPod) throw std::runtime_error("Jettison fixture requires a remaining loaded pod");
    VuBin<BombClass> detachedPod(jettPod);
    const int jettRounds=jettPod->LauGetRoundsOnboard();
    const float jettWeight=WeaponDataTable[jettSms->hardPoint[jettStation]->weaponId].Weight+
        jettRounds*WeaponDataTable[jettPod->LauGetWeaponId()].Weight+
        (jettSms->hardPoint[jettStation]->GetRack()?WeaponDataTable[jettSms->hardPoint[jettStation]->GetRackId()].Weight:0.0f);
    const float beforeJettWeight=dryWeight(emptyPodCarrier);
    const float beforeJettMoment=emptyPodCarrier->af->assymetry;
    const float beforeJettDrag=emptyPodCarrier->af->GetDragIndex();
    float jettX,jettY,jettZ;
    jettSms->hardPoint[jettStation]->GetPosition(&jettX,&jettY,&jettZ);
    const float jettDrag=WeaponDataTable[jettSms->hardPoint[jettStation]->weaponId].DragIndex+
        (jettSms->hardPoint[jettStation]->GetRack()?WeaponDataTable[jettSms->hardPoint[jettStation]->GetRackId()].DragIndex:0.0f);
    jettSms->SetAGBPair(false);
    jettSms->SetCurrentWeapon(jettStation,jettPod);
    jettSms->LaunchRocket();
    if(!jettPod->LauIsFiring()) throw std::runtime_error("Jettison fixture did not reserve a salvo");
    // This inventory fixture never integrates the recreated airframe. Supply
    // the positive-G level-flight state required by the native jettison gate.
    emptyPodCarrier->af->nzcgb=1.0f;
    emptyPodCarrier->SetYPR(emptyPodCarrier->Yaw(),0,0);
    emptyPodCarrier->SetDelta(emptyPodCarrier->af->MaxVcas(),0,0);
    jettSms->JettisonWeapon(jettStation);
    const int jettResult=!jettSms->hardPoint[jettStation]->weaponPointer;
    fprintf(stderr,"[rocket-jettison] station=%d result=%d mounted=%d queued=%d weight_loss=%.3f expected=%.3f\n",
        jettStation,jettResult,jettSms->hardPoint[jettStation]->weaponPointer.get()!=nullptr,
        jettPod->LauIsFiring(),beforeJettWeight-dryWeight(emptyPodCarrier),jettWeight);
    if(!jettResult || jettSms->hardPoint[jettStation]->weaponPointer)
        throw std::runtime_error("Native selective weapon jettison did not remove loaded pod");
    if(fabs(beforeJettWeight-dryWeight(emptyPodCarrier)-jettWeight)>0.5f)
        throw std::runtime_error("Jettison retained rocket ammunition weight");
    if(fabs(beforeJettMoment-emptyPodCarrier->af->assymetry-jettWeight*jettY)>0.5f ||
       fabs(beforeJettDrag-emptyPodCarrier->af->GetDragIndex()-jettDrag)>0.01f)
        throw std::runtime_error("Jettison retained lateral loading or removed contained rocket drag");
    if(jettPod->LauIsFiring() || jettPod->targetPtr)
        throw std::runtime_error("Detached pod retained firing reservation or target");
    for(int tick=0;tick<100;++tick) {
        SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
        jettSms->Exec();DispatchDetailedMessages();
    }
    if(ff_headless::combat.weaponsFired!=firesBeforeEmptyRequest)
        throw std::runtime_error("Jettisoned pod continued firing");
    const float postJettWeight=dryWeight(emptyPodCarrier);
    const float postJettMoment=emptyPodCarrier->af->assymetry;
    const float postJettDrag=emptyPodCarrier->af->GetDragIndex();
    VuBin<AircraftClass> jettisonedCarrier(emptyPodCarrier);
    if(!flight->Reaggregate(FalconLocalSession))
        throw std::runtime_error("Jettisoned carrier failed reaggregation");
    if(flight->GetUnitWeaponCount(jettStation,flight->GetAdjustedPlayerSlot(pilot))!=0)
        throw std::runtime_error("Campaign retained jettisoned pod inventory");
    if(!flight->Deaggregate(FalconLocalSession))
        throw std::runtime_error("Jettisoned carrier failed deaggregation");
    AircraftClass* postJettCarrier=findRecreatedPilot();
    if(!postJettCarrier || postJettCarrier->Id()==emptyPodCarrier->Id())
        throw std::runtime_error("Jettisoned carrier was not recreated");
    if(!postJettCarrier->IsAwake()) postJettCarrier->Wake();
    fprintf(stderr,"[rocket-jettison-reentry] weight=%.3f->%.3f moment=%.3f->%.3f drag=%.3f->%.3f mounted=%d\n",
        postJettWeight,dryWeight(postJettCarrier),postJettMoment,postJettCarrier->af->assymetry,
        postJettDrag,postJettCarrier->af->GetDragIndex(),postJettCarrier->Sms->hardPoint[jettStation]->weaponPointer.get()!=nullptr);
    if(postJettCarrier->Sms->hardPoint[jettStation]->weaponPointer || postJettCarrier->Sms->hardPoint[jettStation]->weaponCount)
        throw std::runtime_error("Recreation restored a jettisoned pod");
    if(fabs(dryWeight(postJettCarrier)-postJettWeight)>0.5f ||
       fabs(postJettCarrier->af->assymetry-postJettMoment)>0.5f ||
       fabs(postJettCarrier->af->GetDragIndex()-postJettDrag)>0.01f)
        throw std::runtime_error("Jettisoned carrier recreation changed store mass moment or drag");
    const auto postJettDetail=DetailedSnapshotJson();
    std::cout<<"{\"jettison_reentry_verified\":true,\"jettison_station\":"<<jettStation
             <<",\"post_jettison_actor_id\":\""<<postJettCarrier->Id().creator_<<":"<<postJettCarrier->Id().num_
             <<"\",\"post_jettison_detail\":"<<postJettDetail
             <<",\"loaded_pod_jettison_verified\":true,\"jettisoned_rounds\":"<<jettRounds<<",\"empty_pod_verified\":true,\"empty_pod_actor_id\":\""<<emptyPodCarrier->Id().creator_<<":"<<emptyPodCarrier->Id().num_
             <<"\",\"expended_pod_rounds\":"<<expendedPodRounds<<",\"empty_pod_before_detail\":"<<emptyPodBefore<<",\"empty_pod_detail\":"<<emptyPodDetail<<",";
    std::cout<<"\"status\":\"ok\",\"case\":\"rocket-hold\",\"empty_gun_verified\":true,\"empty_actor_id\":\""<<emptyCarrier->Id().creator_<<":"<<emptyCarrier->Id().num_<<"\",\"empty_detail\":"<<emptyDetail<<",\"inventory_restored\":true,\"recreated_actor_id\":\""<<recreated->Id().creator_<<":"<<recreated->Id().num_<<"\",\"reentry_before_detail\":"<<reentryBeforeDetail<<",\"recreated_detail\":"<<recreatedDetail<<",\"rounds_before\":"<<before<<",\"new_rockets\":2,\"salvo_cancelled\":true,\"actor_id\":\""<<aircraft->Id().creator_<<":"<<aircraft->Id().num_<<"\",\"pod_station\":"<<podStation<<",\"initial_detail\":"<<initialDetail<<",\"queued_detail\":"<<queuedDetail<<",\"held_detail\":"<<heldDetail<<"}"<<std::endl;
}

void RunBombHoldDiagnostic(AircraftClass* aircraft, bool flightTest=false) {
    for(int frame=0;frame<250;++frame) AdvanceDetailedFrame();
    auto* sms=aircraft->Sms;
    if(!sms->FindWeaponClass(wcBombWpn)) throw std::runtime_error("Bomb hold fixture requires native bomb stores");
    aircraft->FCC->SetMasterMode(FireControlComputer::AirGroundBomb);
    sms->SetMasterArm(SMSBaseClass::Arm);
    sms->SetAGBPair(false);sms->SetAGBRippleCount(3);sms->SetAGBRippleInterval(100);
    // Native SMS Exec refreshes the count for a newly selected weapon.
    sms->Exec();
    auto stores=[&]() { int n=0;for(int hp=0;hp<sms->NumHardpoints();++hp) if(sms->hardPoint[hp]->GetWeaponClass()==wcBombWpn) n+=sms->hardPoint[hp]->weaponCount;return n; };
    auto bombs=[&]() { int n=0;VuListIterator objects(SimDriver.objectList);for(auto* e=objects.GetFirst();e;e=objects.GetNext()) { auto* b=dynamic_cast<BombClass*>(e);if(b && b->Parent()==aircraft && !b->IsSetBombFlag(BombClass::IsChaff bitor BombClass::IsFlare bitor BombClass::IsDebris)) ++n; }return n; };
    const int before=stores(),objectsBefore=bombs();
    const int dropped=sms->DropBomb(TRUE);
    fprintf(stderr,"[bomb-hold-fixture] before=%d after=%d old_objects=%d new_objects=%d dropped=%d ripple=%d\n",before,stores(),objectsBefore,bombs(),dropped,sms->CurRippleCount());
    if(before<3 || !dropped || sms->CurRippleCount()<1 || stores()!=before-1 || bombs()!=objectsBefore+1)
        throw std::runtime_error("Native bomb launch did not create a scheduled ripple fixture");
    aircraft->DBrain()->SetOperatorWeaponsHold(true);
    if(flightTest) {
        if(objectsBefore!=0) throw std::runtime_error("Bomb flight fixture already has airborne bombs");
        VuBin<BombClass> projectile;
        VuListIterator objects(SimDriver.objectList);
        for(auto* e=objects.GetFirst();e;e=objects.GetNext()) {
            auto* bomb=dynamic_cast<BombClass*>(e);
            if(bomb && bomb->Parent()==aircraft && !bomb->IsSetBombFlag(BombClass::IsChaff bitor BombClass::IsFlare bitor BombClass::IsDebris)) { projectile.reset(bomb);break; }
        }
        if(!projectile) throw std::runtime_error("Missing native dropped bomb");
        const auto identity=projectile->Id();
        const auto key=std::make_pair(identity.creator_,identity.num_);
        const float startX=projectile->XPos(),startY=projectile->YPos(),startZ=projectile->ZPos();
        std::string flightSnapshot;
        unsigned frames=0;
        for(;frames<6000 && !ff_headless::combat.projectileEnds[key];++frames) {
            AdvanceDetailedFrame();
            if(!std::isfinite(projectile->XPos()) || !std::isfinite(projectile->YPos()) || !std::isfinite(projectile->ZPos()))
                throw std::runtime_error("Native bomb flight produced non-finite coordinates");
            if(frames==249) flightSnapshot=DetailedSnapshotJson();
        }
        if(ff_headless::combat.projectileEnds[key]!=1 || flightSnapshot.empty()) throw std::runtime_error("Bomb did not terminate exactly once after native flight");
        const auto end=ff_headless::combat.projectileEndState.at(key);
        const float distance=std::hypot(end[0]-startX,end[1]-startY);
        const float terrainZ=OTWDriver.GetGroundLevel(end[0],end[1]);
        if(distance<100 || end[2]<=startZ || !std::isfinite(terrainZ)) throw std::runtime_error("Bomb trajectory did not descend and travel");
        for(int cleanup=0;cleanup<250;++cleanup) AdvanceDetailedFrame();
        fprintf(stderr,"[bomb-flight-end] frames=%u distance=%.1f descent=%.1f agl=%.1f code=%d db=%d ends=%u bombs=%d stores=%d expected=%d awake=%d dead=%d remove=%d\n",frames,distance,end[2]-startZ,terrainZ-end[2],int(end[3]),vuDatabase->Find(identity)?1:0,ff_headless::combat.projectileEnds[key],bombs(),stores(),before-1,projectile->IsAwake(),projectile->IsDead(),projectile->IsSetRemoveFlag());
        if(vuDatabase->Find(identity) || ff_headless::combat.projectileEnds[key]!=1 || bombs()!=0 || stores()!=before-1)
            throw std::runtime_error("Bomb cleanup, single termination or held inventory failed");
        std::cout<<"{\"status\":\"ok\",\"case\":\"bomb-flight\",\"frames\":"<<frames<<",\"horizontal_m\":"<<distance*.3048<<",\"descent_m\":"<<(end[2]-startZ)*.3048<<",\"end_agl_m\":"<<(terrainZ-end[2])*.3048<<",\"end_code\":"<<int(end[3])<<",\"terminal_events\":1,\"cleaned_up\":true,\"bomb_id\":\""<<identity.creator_<<":"<<identity.num_<<"\",\"flight_detail\":"<<flightSnapshot<<"}"<<std::endl;
        return;
    }
    const int heldStores=stores(),heldBombs=bombs();
    // Exercise the original SMS schedule in isolation, beyond its next drop.
    SetTime(TheCampaign.CurrentTime+10000);SimLibElapsedTime=TheCampaign.CurrentTime;
    sms->Exec();
    if(stores()!=heldStores || bombs()!=heldBombs || sms->CurRippleCount()!=0)
        throw std::runtime_error("Operator hold failed to cancel native bomb ripple");
    if(sms->DropBomb(FALSE) || stores()!=heldStores || bombs()!=heldBombs)
        throw std::runtime_error("Explicit bomb release bypassed operator hold");
    aircraft->DBrain()->SetOperatorWeaponsHold(false);
    sms->Exec();
    if(stores()!=heldStores || bombs()!=heldBombs)
        throw std::runtime_error("Clearing operator hold resumed a cancelled ripple");
    if(!sms->FindWeaponClass(wcBombWpn) || !sms->DropBomb(FALSE) || stores()!=heldStores-1 || bombs()!=heldBombs+1)
        throw std::runtime_error("Native bomb release failed after operator hold cleared");
    if(!sms->FindWeaponClass(wcBombWpn)) throw std::runtime_error("Missing bomb for master-safe ripple fixture");
    sms->Exec();
    if(!sms->DropBomb(TRUE) || sms->CurRippleCount()<1 || stores()!=heldStores-2 || bombs()!=heldBombs+2)
        throw std::runtime_error("Master-safe bomb fixture did not queue a real ripple");
    sms->SetOperatorMasterSafe(true);sms->SetMasterArm(SMSBaseClass::Arm);sms->Exec();
    if(sms->MasterArm()!=SMSBaseClass::Safe || sms->CurRippleCount()!=0 || sms->DropBomb(FALSE) || stores()!=heldStores-2 || bombs()!=heldBombs+2)
        throw std::runtime_error("Master safe did not cancel bomb ripple and block AI re-arm");
    sms->SetOperatorMasterSafe(false);sms->Exec();
    if(stores()!=heldStores-2 || bombs()!=heldBombs+2) throw std::runtime_error("Cancelled safe bomb ripple revived on Arm");
    std::cout<<"{\"status\":\"ok\",\"case\":\"bomb-hold\",\"stores_before\":"<<before<<",\"stores_after\":"<<stores()<<",\"new_bombs\":3,\"ripple_cancelled\":true,\"master_safe_cancelled\":true}"<<std::endl;
}
static std::string groundRetargetAssignedSnapshot,groundRetargetClearedSnapshot,groundRetargetActor;
void RunGroundRetargetDiagnostic(AircraftClass* aircraft) {
    auto* brain=aircraft->DBrain();
    VuListIterator objectives(AllObjList);
    auto* target=static_cast<FalconEntity*>(objectives.GetFirst());
    if(!target || !aircraft->waypoint || aircraft->OnGround())
        throw std::runtime_error("Missing airborne ground-retarget fixture");
    // Hold one observer reference so a stale brain reference can be inspected
    // after the actual native weapons-free handler reevaluates its AG target.
    auto* observed=new SimObjectType(target);
    observed->Reference();
    const auto previousMission=brain->missionClass;
    brain->missionClass=DigitalBrain::AGMission;
    FalconWingmanMsg message(aircraft->GetCampaignObject()->Id(),FalconLocalGame);
    message.dataBlock.from=aircraft->Id();message.dataBlock.to=AiWingman;
    message.dataBlock.command=FalconWingmanMsg::WMWeaponsFree;
    for(int cycle=0;cycle<128;++cycle) {
        brain->SetGroundTargetPtr(observed);
        if(cycle==0) {
            groundRetargetAssignedSnapshot=DetailedSnapshotJson();
            groundRetargetActor=std::to_string(aircraft->Id().creator_)+":"+std::to_string(aircraft->Id().num_);
        }
        if(observed->refCount!=2) throw std::runtime_error("Unexpected ground-target ownership before command");
        brain->AiSetWeaponsAction(&message,DigitalBrain::AI_WEAPONS_FREE);
        if(observed->refCount!=1) throw std::runtime_error("Weapons-free leaked the previous ground-target reference");
    }
    brain->SetGroundTarget(NULL);
    auto* savedWaypoint=aircraft->curWaypoint;
    auto* terminalWaypoint=aircraft->waypoint;
    while(terminalWaypoint->GetNextWP()) terminalWaypoint=terminalWaypoint->GetNextWP();
    auto* flight=static_cast<FlightClass*>(aircraft->GetCampaignObject());
    auto* savedCampaignWaypoint=flight->GetCurrentUnitWP();
    for(int cycle=0;cycle<128;++cycle) {
        brain->SetGroundTargetPtr(observed);
        aircraft->curWaypoint=terminalWaypoint;
        brain->SelectNextWaypoint();
        if(brain->GetGroundTarget() || observed->refCount!=1)
            throw std::runtime_error("Terminal waypoint abandoned the previous ground-target reference");
        if(!aircraft->curWaypoint) throw std::runtime_error("Terminal waypoint failed to select return route");
    }
    aircraft->curWaypoint=savedWaypoint;
    flight->SetCurrentUnitWP(savedCampaignWaypoint);
    groundRetargetClearedSnapshot=DetailedSnapshotJson();
    observed->Release();
    Unit groundUnit=nullptr;
    VuListIterator units(AllUnitList);
    for(auto* entity=units.GetFirst();entity;entity=units.GetNext()) {
        auto* unit=static_cast<UnitClass*>(entity);
        if(unit->IsBattalion() && unit->IsAggregate() && !unit->IsDead() && unit->GetTotalVehicles()>0) { groundUnit=unit;break; }
    }
    if(!groundUnit || !groundUnit->Deaggregate(FalconLocalSession) || !groundUnit->Wake())
        throw std::runtime_error("No live ground-target retirement fixture");
    const int components=groundUnit->NumberOfComponents();
    auto* activeGroundTarget=brain->FindSimGroundTarget(groundUnit,components,0);
    if(!activeGroundTarget)
        throw std::runtime_error("Ground target fixture has no initially selectable actor");
    // Exercise the native final request gate, not missile accuracy/eligibility.
    // Supply its attack-altitude/timer preconditions; never advance a weapon.
    const auto previousWait=brain->waitingForShot;
    const float previousTrackZ=brain->trackZ;
    auto requestAG=[&]() {
        brain->waitingForShot=0;
        brain->trackZ=aircraft->ZPos();
        const float dx=activeGroundTarget->XPos()-aircraft->XPos();
        const float dy=activeGroundTarget->YPos()-aircraft->YPos();
        const float dz=activeGroundTarget->ZPos()-aircraft->ZPos();
        brain->FireAGMissile(std::sqrt(dx*dx+dy*dy+dz*dz),0.0f);
    };
    brain->SetGroundTarget(activeGroundTarget);
    brain->ClearFlag(BaseBrain::MslFireFlag);
    requestAG();
    if(!brain->IsSetFlag(BaseBrain::MslFireFlag)) throw std::runtime_error("Live AG target final request gate failed");
    groundUnit->Sleep();
    requestAG();
    if(brain->IsSetFlag(BaseBrain::MslFireFlag) || brain->GetGroundTarget())
        throw std::runtime_error("Final AG request retained a sleeping target or its fire flag");
    brain->SetGroundTarget(activeGroundTarget);
    const auto savedShotTimer=brain->missileShotTimer;
    brain->missileShotTimer=SimLibElapsedTime+5000;
    brain->SetFlag(BaseBrain::MslFireFlag bitor BaseBrain::GunFireFlag);
    aircraft->FCC->releaseConsent=TRUE;
    brain->GroundAttackMode();
    if(brain->GetGroundTarget() || brain->IsSetFlag(BaseBrain::MslFireFlag bitor BaseBrain::GunFireFlag) || aircraft->FCC->releaseConsent)
        throw std::runtime_error("AG cooldown retained a retired target or pending release");
    brain->missileShotTimer=savedShotTimer;
    // Native reaggregation sleeps components before destroying their entities.
    // Both normal selection and the scarce-target fallback must reject them.
    if(brain->FindSimGroundTarget(groundUnit,components,0))
        throw std::runtime_error("Ground AI selected a sleeping component during retirement");
    if(brain->FindSimGroundTarget(groundUnit,1,0))
        throw std::runtime_error("Scarce-target fallback selected a sleeping component");
    if(!groundUnit->Wake() || !brain->FindSimGroundTarget(groundUnit,components,0))
        throw std::runtime_error("Awakened ground targets did not become selectable again");
    brain->SetGroundTarget(activeGroundTarget);
    requestAG();
    if(!brain->IsSetFlag(BaseBrain::MslFireFlag)) throw std::runtime_error("Reawakened AG target final request gate failed");
    brain->SetGroundTarget(NULL);
    requestAG();
    if(brain->IsSetFlag(BaseBrain::MslFireFlag)) throw std::runtime_error("Final AG request fired without a target");
    brain->waitingForShot=previousWait;
    brain->trackZ=previousTrackZ;
    brain->gndTargetHistory[0]=brain->gndTargetHistory[1]=nullptr;
    if(!groundUnit->Reaggregate(FalconLocalSession)) throw std::runtime_error("Retirement fixture reaggregation failed");
    brain->missionClass=previousMission;
}
bool RunWeaponsFireGateDiagnostic(AircraftClass* aircraft) {
    auto* brain=aircraft->DBrain();
    if(brain->curMode!=DigitalBrain::MissileEngageMode || !brain->curMissile || !brain->targetPtr) return false;
    if(QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"weapons_free")!="applied") return false;
    brain->ClearFlag(BaseBrain::MslFireFlag);
    brain->FireControl();
    if(!brain->IsSetFlag(BaseBrain::MslFireFlag)) return false;
    // The unmodified controller accepted native geometry, stores and sensors.
    // Keep the same physical instant for the hold/free comparison; do not
    // change any range, sensor result, ammunition or launch eligibility input.
    const auto target=brain->targetPtr->BaseData()->Id();
    const auto missile=brain->curMissile->Id();
    brain->ClearFlag(BaseBrain::MslFireFlag);
    if(QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"weapons_hold")!="applied")
        throw std::runtime_error("Eligible aircraft refused hold order");
    brain->FireControl();
    if(brain->IsSetFlag(BaseBrain::MslFireFlag) || brain->missileShotTimer<=SimLibElapsedTime)
        throw std::runtime_error("Hold did not suppress native missile fire request");
    if(QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"weapons_free")!="applied")
        throw std::runtime_error("Held aircraft refused free order");
    brain->FireControl();
    const bool resumed=brain->IsSetFlag(BaseBrain::MslFireFlag);
    brain->ClearFlag(BaseBrain::MslFireFlag);
    if(!resumed || !brain->curMissile || brain->curMissile->Id()!=missile || !brain->targetPtr || brain->targetPtr->BaseData()->Id()!=target)
        throw std::runtime_error("Free did not restore the same native missile fire request");
    fprintf(stderr,"[weapons-fire-gate] actor=%lu target=%lu missile=%lu accepted_before=1 held=1 accepted_after=1\n",aircraft->Id().num_,target.num_,missile.num_);
    return true;
}
void RunBvrAggregateDiagnostic(AircraftClass* aircraft) {
    auto* brain=aircraft->DBrain();
    auto* own=static_cast<FlightClass*>(aircraft->GetCampaignObject());
    VuBin<FlightClass> opponent(NewFlight(own->Type(),nullptr,nullptr));
    opponent->SetOwner(own->GetCountry());
    VuListIterator countries(AllUnitList);
    for(auto* entity=countries.GetFirst();entity;entity=countries.GetNext()) {
        auto* unit=static_cast<UnitClass*>(entity);
        if(GetTTRelations(own->GetTeam(),unit->GetTeam())>=Hostile) {opponent->SetOwner(unit->GetCountry());break;}
    }
    if(GetTTRelations(own->GetTeam(),opponent->GetTeam())<Hostile)
        throw std::runtime_error("Aggregate threat fixture has no hostile country");
    opponent->SetRoster(0);
    for(int slot=0;slot<=own->NumberOfComponents();++slot) opponent->SetNumVehicles(slot,1);
    opponent->SetPosition(aircraft->XPos()+1000,aircraft->YPos(),aircraft->ZPos());
    if(!opponent->IsAggregate() || opponent->NumberOfComponents()!=0 || opponent->GetTotalVehicles()<=own->NumberOfComponents())
        throw std::runtime_error("Aggregate threat fixture has invalid force sizes");
    auto* target=new SimObjectType(opponent.get());target->Reference();
    brain->SetTarget(target);
    if(brain->targetPtr!=target) throw std::runtime_error("Aggregate threat fixture target was rejected");
    target->localData->range=1000;target->localData->ata=0;target->localData->ataFrom=0;
    const auto savedMission=own->GetUnitMission();const float savedRange=brain->maxAAWpnRange;
    own->SetUnitMission(AMIS_INTERCEPT);brain->maxAAWpnRange=20*NM_TO_FT;
    brain->ChoiceProfile();const auto larger=brain->bvrCurrProfile;
    opponent->SetNumVehicles(own->NumberOfComponents(),0);
    brain->ChoiceProfile();const auto equal=brain->bvrCurrProfile;
    own->SetUnitMission(savedMission);brain->maxAAWpnRange=savedRange;
    brain->SetTarget(nullptr);target->Release();
    fprintf(stderr,"[bvr-aggregate] own=%d larger_profile=%d equal_profile=%d\n",own->NumberOfComponents(),int(larger),int(equal));
    if(larger!=DigitalBrain::Plevel2c || equal!=DigitalBrain::Pwall)
        throw std::runtime_error("BVR profile ignored aggregate flight numerical advantage");
}
unsigned RunBvrPursuitDiagnostic(AircraftClass* aircraft, AircraftClass* target) {
    auto* brain=aircraft->DBrain();
    auto* lock=new SimObjectType(target);lock->Reference();
    brain->SetTarget(lock);
    brain->reactiont=0;
    const float x=aircraft->XPos()+10000, y=aircraft->YPos(), z=aircraft->ZPos();
    target->SetYPR(0,0,0);target->SetDelta(1000,0,0);
    unsigned refreshes=0;float previous=-1;
    // Controlled moving-target input to the original pursuit controller.
    // No sensor lock, fire or damage outcome is being tested here.
    for(int tick=0;tick<400;++tick) {
        target->SetPosition(x+tick*20,y,z);
        CalcRelGeom(aircraft,lock,nullptr,50.0f);
        if(fabs(lock->localData->azFrom)<160*DTR)
            throw std::runtime_error("Pursuit fixture did not enter the pure branch");
        brain->BaseLineIntercept();
        if(brain->trackX!=previous) {++refreshes;previous=brain->trackX;}
        if(!std::isfinite(brain->trackX) || !std::isfinite(brain->trackY) || !std::isfinite(brain->trackZ))
            throw std::runtime_error("Pursuit produced a nonfinite track point");
        if(target->XPos()-brain->trackX>3100)
            throw std::runtime_error("Pursuit stopped refreshing a moving target after its reaction interval");
    }
    brain->SetTarget(nullptr);lock->Release();
    if(refreshes<3) throw std::runtime_error("Pursuit failed to refresh the moving target repeatedly");
    const int savedSkill=brain->SkillLevel();brain->SetSkill(0);
    if(fabs(aircraft->Pitch())>=45*DTR) throw std::runtime_error("Low-G tracking fixture is not level enough");
    brain->trackX=aircraft->XPos()-10000*aircraft->dmx[0][0];
    brain->trackY=aircraft->YPos()-10000*aircraft->dmx[0][1];
    brain->trackZ=aircraft->ZPos()-10000*aircraft->dmx[0][2];
    for(float requestedG:{0.0f,0.5f,1.0f,1.0f/0.85f,2.0f,5.0f}) {
        const float angle=brain->AutoTrack(requestedG);
        if(angle<=90 || !std::isfinite(brain->rStick) || !std::isfinite(brain->pStick))
            throw std::runtime_error("Low-G rearward AutoTrack produced nonfinite control input");
    }
    brain->SetSkill(savedSkill);
    return refreshes;
}
void RunTracerLifetimeDiagnostic(GunClass* gun) {
    if(!gun || !gun->bullet || gun->numTracers<2) throw std::runtime_error("No native tracer gun");
    auto* helicopter=static_cast<HelicopterClass*>(gun->Parent());
    const float expected=float((reinterpret_cast<unsigned char*>(gun->GetWCD())[45]&7)+1);
    const float savedStep=SimLibMajorFrameTime;
    for(float step:{0.01f,0.02f,0.04f}) {
        SimLibMajorFrameTime=step;gun->qTimer=0;gun->numFlying=1;
        for(int i=0;i<gun->numTracers;++i) gun->bullet[i]={};
        auto& bullet=gun->bullet[0];bullet.flying=1;
        bullet.x=helicopter->XPos();bullet.y=helicopter->YPos();bullet.z=helicopter->ZPos()-1000;
        bullet.xdot=100;
        float elapsed=0;
        while(gun->numFlying && elapsed<expected+1) {
            int fire=0;
            gun->Exec(&fire,helicopter->gunDmx,&helicopter->platformAngles,nullptr,FALSE);
            elapsed+=step;
        }
        fprintf(stderr,"[tracer-lifetime] step=%.3f expected=%.3f actual=%.3f\n",step,expected,elapsed);
        if(gun->numFlying || fabs(elapsed-expected)>step+0.001f)
            throw std::runtime_error("Native tracer lifetime depends on frame interval");
    }
    SimLibMajorFrameTime=savedStep;
}
unsigned VerifyRocketDispersion(MissileClass* rocket) {
    // Controlled nonzero model configuration: shipped FF6 data defaults to
    // zero. Exercise the original launch function with all relevant flags.
    auto* savedAux=rocket->auxData;const int savedFlags=rocket->flags;
    MissileAuxData configured=*savedAux;configured.rocketDispersionConeAngle=4.0f;
    struct Restore {
        MissileClass* rocket;MissileAuxData* aux;int flags;
        ~Restore() {rocket->auxData=aux;rocket->flags=flags;}
    } restore{rocket,savedAux,savedFlags};
    rocket->auxData=&configured;
    unsigned checked=0;
    for(int flags:{0,int(MissileClass::EndGame),int(MissileClass::ClosestApprch),int(MissileClass::SensorLostLock),int(MissileClass::FindingImpact),int(MissileClass::FindingImpact|MissileClass::SensorLostLock)}) {
        bool spread=false;
        for(int sample=0;sample<8;++sample) {
            rocket->flags=flags;rocket->SetLaunchData();
            const float az=rocket->psi-rocket->parent->Yaw()-rocket->initAz;
            const float el=rocket->theta-rocket->parent->Pitch()-rocket->initEl;
            if(!std::isfinite(az)||!std::isfinite(el)||fabs(az)>2.0f*DTR+1e-5f||fabs(el)>2.0f*DTR+1e-5f)
                throw std::runtime_error("Rocket dispersion exceeded configured launch bounds");
            spread=spread||fabs(az)>1e-5f||fabs(el)>1e-5f;++checked;
        }
        if(spread==bool(flags & MissileClass::FindingImpact))
            throw std::runtime_error("Rocket dispersion confused prediction and unrelated missile flags");
    }
    return checked;
}
void RunDetailedDiagnostic(const std::string& testCase) {
    if(testCase=="tanker-request" || testCase=="tanker-discovery" || testCase=="tanker-flight") {
        // Match entering detailed mode after campaign planning has already
        // created support flights, rather than subscribing before their birth.
        DoCampaignLoop(1);
        for(int tick=0;tick<360;++tick) {
            SetTime(TheCampaign.CurrentTime+5000);
            DoCampaignLoop(0);UpdateRealUnits(5000);
            TheCampaign.vuThread->Update(-1);gMainThread->Update(-1);
        }
        InitializeDetailedRuntime();
        std::set<VU_ID> expected,actual;
        { VuListIterator units(AllUnitList);
          for(auto* e=units.GetFirst();e;e=units.GetNext()) {
              auto* unit=static_cast<UnitClass*>(e);
              if(unit->IsFlight() && unit->GetSType()==STYPE_UNIT_TANKER) expected.insert(unit->Id());
          }
        }
        if(expected.empty()) throw std::runtime_error("No loaded tanker flight fixture");
        { VuListIterator tankers(SimDriver.tankerList);
          for(auto* e=tankers.GetFirst();e;e=tankers.GetNext()) actual.insert(e->Id());
        }
        fprintf(stderr,"[tanker-discovery] loaded=%zu registered=%zu\n",expected.size(),actual.size());
        if(actual!=expected) throw std::runtime_error("Loaded tanker flights missing from detailed discovery list");
        if(testCase=="tanker-request" || testCase=="tanker-flight") {
            FlightClass* flight=nullptr;
            for(const auto& id:expected) {
                auto* candidate=static_cast<FlightClass*>(vuDatabase->Find(id));
                if(candidate->IsAggregate() && candidate->ZPos()<-5000 && candidate->GetTotalVehicles()>0) {flight=candidate;break;}
            }
            if(!flight) throw std::runtime_error("No airborne native tanker flight fixture");
            VuBin<FlightClass> keepFlight(flight);
            if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Native tanker flight failed to deaggregate");
            auto* tanker=dynamic_cast<AircraftClass*>(flight->GetComponentLead());
            if(!tanker || !tanker->DBrain()->IsTanker()) throw std::runtime_error("Native tanker brain missing");
            VuBin<AircraftClass> keepTanker(tanker);
            if(!tanker->IsAwake()) tanker->Wake();
            if(SimDriver.FindTanker(tanker)!=flight) throw std::runtime_error("Native nearest tanker discovery failed");
            if(testCase=="tanker-request") {
                FlightClass* receiverFlight=nullptr;float nearest=1e30f;
                { VuListIterator units(AllUnitList);
                  for(auto* e=units.GetFirst();e;e=units.GetNext()) {
                      auto* candidate=dynamic_cast<FlightClass*>(e);
                      if(!candidate || !candidate->IsAggregate() || candidate->GetTeam()!=flight->GetTeam() || candidate->GetSType()==STYPE_UNIT_TANKER || candidate->ZPos()>-5000 || candidate->GetTotalVehicles()<2) continue;
                      if(Falcon4ClassTable[candidate->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE]==TYPE_HELICOPTER) continue;
                      const float range=float(hypot(candidate->XPos()-tanker->XPos(),candidate->YPos()-tanker->YPos()));
                      if(range<nearest) {nearest=range;receiverFlight=candidate;}
                  }
                }
                if(!receiverFlight) throw std::runtime_error("No airborne receiver flight fixture");
                VuBin<FlightClass> keepReceiverFlight(receiverFlight);
                if(!receiverFlight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Receiver flight deaggregation failed");
                std::vector<VuBin<AircraftClass>> receivers;
                { VuListIterator parts(receiverFlight->GetComponents());
                  for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                      auto* ac=dynamic_cast<AircraftClass*>(e);
                      if(!ac) throw std::runtime_error("Receiver is not an aircraft");
                      if(!ac->IsAwake()) ac->Wake();
                      ac->Sms->SetOperatorMasterSafe(true);receivers.emplace_back(ac);
                  }
                }
                if(receivers.size()<2) throw std::runtime_error("Receiver flight lacks multiple queue positions");
                auto request=[&]() {
                    auto* message=new FalconTankerMessage(tanker->Id(),FalconLocalGame);
                    message->dataBlock.type=FalconTankerMessage::RequestFuel;
                    message->dataBlock.caller=receivers.front()->Id();message->dataBlock.data1=1;
                    FalconSendMessage(message);
                    AdvanceDetailedFrame();
                };
                request();
                const std::string assigned=DetailedSnapshotJson();
                for(int repeat=0;repeat<2;++repeat) {
                    for(size_t i=0;i<receivers.size();++i) {
                        auto* ac=receivers[i].get();
                        fprintf(stderr,"[tanker-request] repeat=%d pilot=%d assigned=%lu expected=%lu need=%d status=%d position=%d expected_position=%zu\n",repeat,ac->pilotSlot,(unsigned long)ac->DBrain()->Tanker().num_,(unsigned long)tanker->Id().num_,ac->DBrain()->IsSetATC(DigitalBrain::NeedToRefuel)?1:0,int(ac->DBrain()->RefuelStatus()),tanker->TBrain()->TankingPosition(ac),i);
                        const auto status=ac->DBrain()->RefuelStatus();
                        if(ac->DBrain()->Tanker()!=tanker->Id() || !ac->DBrain()->IsSetATC(DigitalBrain::NeedToRefuel) || (status!=DigitalBrain::refWaiting && status!=DigitalBrain::refRefueling) || tanker->TBrain()->TankingPosition(ac)!=int(i)) throw std::runtime_error("Native tanker request or queue order mismatch");
                    }
                    if(repeat==0) request();
                }
                for(const auto& ac:receivers) {
                    tanker->TBrain()->RemoveFromQ(ac.get());
                    if(tanker->TBrain()->TankingPosition(ac.get())!=-1) throw std::runtime_error("Duplicate request left a duplicate receiver in queue");
                }
                std::cout<<"{\"status\":\"ok\",\"case\":\"tanker-request\",\"frames\":2,\"receivers\":"<<receivers.size()<<",\"duplicate_verified\":true,\"queue_removed\":true,\"initial_range_m\":"<<nearest*.3048<<",\"assigned_detail\":"<<assigned<<"}"<<std::endl;
                return;
            }
            const float x=tanker->XPos(),y=tanker->YPos();
            for(int frame=0;frame<500;++frame) {
                AdvanceDetailedFrame();
                if(tanker->IsDead() || tanker->OnGround() || !std::isfinite(tanker->XPos()) || !std::isfinite(tanker->YPos()) || !std::isfinite(tanker->ZPos())) throw std::runtime_error("Native tanker flight invalid");
            }
            const float travel=float(hypot(tanker->XPos()-x,tanker->YPos()-y)*.3048);
            if(travel<500) throw std::runtime_error("Native tanker failed to fly");
            std::cout<<"{\"status\":\"ok\",\"case\":\"tanker-flight\",\"frames\":500,\"travel_m\":"<<travel<<",\"native_brain\":true,\"discovered\":true,\"detail\":"<<DetailedSnapshotJson()<<"}"<<std::endl;
            return;
        }
        std::cout<<"{\"status\":\"ok\",\"case\":\"tanker-discovery\",\"loaded\":"<<expected.size()<<",\"registered\":"<<actual.size()<<"}"<<std::endl;
        return;
    }
    if(testCase=="entity-lifetime") {
        InitializeDetailedRuntime();
        VuListIterator units(AllUnitList);
        auto* entity=static_cast<FalconEntity*>(units.GetFirst());
        if(!entity) throw std::runtime_error("No entity for lifetime diagnostic");
        const int baseline=entity->RefCount();
        // Exceed the old 16-bit wrap interval while retaining the campaign
        // owner. A scoped temporary must never consume its owner's reference.
        for(unsigned i=0;i<70000;++i) {
            {
                VuBin<FalconEntity> first(entity),copy(first),assigned;
                assigned=copy;copy.reset();first=first;
                if(entity->RefCount()!=baseline+2)
                    throw std::runtime_error("Entity references unbalanced during ownership transfer");
            }
            if(entity->RefCount()!=baseline)
                throw std::runtime_error("Scoped entity references accumulate after destruction");
        }
        auto* sensorTarget=new SimObjectType(entity);sensorTarget->Reference();
        sensorTarget->Release();
        if(entity->RefCount()!=baseline)
            throw std::runtime_error("Sensor target release consumed the campaign owner");
        std::cout<<"{\"status\":\"ok\",\"case\":\"entity-lifetime\",\"cycles\":70000,\"initial_refs\":"<<baseline<<",\"final_refs\":"<<entity->RefCount()<<"}"<<std::endl;
        return;
    }
    const bool weaponOrderCase=testCase=="weapons-fire-gate";
    const bool radarTrackCase=testCase=="radar-power-track";
    const bool rocketCase=testCase=="rocket-dispersion" || testCase=="helo-target-retire" || testCase=="rocket-predictor-flight" || testCase=="rocket-ground-hit" || testCase=="rocket-ground-miss" || testCase=="rocket-salvo" || testCase=="helo-altitude" || (testCase=="helo-rockets" || testCase=="helo-rockets-only") || testCase=="rocket-predictor" || testCase=="rocket-mount";
    ff_headless::combat.trackProjectileEnds=true;
    const bool returnFlightCase=testCase=="return-flight" || testCase=="return-recovery" || testCase=="return-taxi" || testCase=="airstrip-return";
    if (!returnFlightCase && testCase != "atc-airstrip" && testCase != "atc-scheduler" && testCase != "return-route" && testCase != "waypoint-hold" && testCase != "carrier-approach" && testCase != "carrier-return" && testCase != "carrier-reentry" && !weaponOrderCase && testCase != "bvr-aggregate" && !radarTrackCase && testCase != "rocket-dispersion" && testCase != "helo-target-retire" && testCase != "rocket-predictor-flight" && testCase != "rocket-ground-hit" && testCase != "rocket-ground-miss" && testCase != "rocket-salvo" && testCase != "helo-altitude" && testCase != "rocket-mount" && testCase != "rocket-predictor" && testCase != "helo-rockets" && testCase != "helo-rockets-only" && testCase != "helo-gun-miss" && testCase != "helo-gun-hit" && testCase != "gun-lifetime" && testCase != "helo-combat" && testCase != "bvr-pursuit" && testCase != "naval-combat" && testCase != "sea-docked" && testCase != "sea-route" && testCase != "region-drain" && testCase != "countermeasures" && testCase != "hit" && testCase != "miss" && testCase != "unarmed" && testCase != "ground" && testCase != "air" && testCase != "rocket-legacy-pod" && testCase != "rocket-hold" && testCase != "ground-retarget" && testCase != "bomb-hold" && testCase != "bomb-flight" && testCase != "bomb-ground-hit" && testCase != "bomb-ground-miss" && testCase != "helo" && testCase != "helo-route" && testCase != "sea" && testCase != "feature" && testCase != "region" && testCase != "air-combat") throw std::runtime_error("Unknown detailed diagnostic case");
    const bool heloCombat=rocketCase || testCase=="helo-combat" || testCase=="helo-gun-hit" || testCase=="helo-gun-miss";
    const bool pursuitCase=testCase=="bvr-pursuit" || testCase=="bvr-aggregate";
    const bool heloCase=testCase=="helo" || testCase=="helo-route" || testCase=="gun-lifetime";
    const bool seaCase=testCase=="sea" || testCase=="sea-route" || testCase=="sea-docked";
    const bool routeCase=testCase=="helo-route" || testCase=="sea-route";
    InitializeDetailedRuntime();
    if(testCase=="atc-scheduler" || testCase=="atc-airstrip") {
        ObjectiveClass* base=nullptr;
        { VuListIterator objectives(AllObjList);
          for(auto* e=objectives.GetFirst();e;e=objectives.GetNext()) {
              auto* candidate=static_cast<ObjectiveClass*>(e);
              if(candidate->GetType()==(testCase=="atc-airstrip"?TYPE_AIRSTRIP:TYPE_AIRBASE) && candidate->IsLocal() && candidate->brain && candidate->brain->NumRunways()>0) {base=candidate;break;}
          }
        }
        if(!base) throw std::runtime_error("No local native ATC with runways");
        auto* brain=base->brain;
        const int runway=brain->GetRunwayStats()[0].rwIndexes[0];
        const VU_ID retired(0xfffffffe,0xfffffffe);
        if(vuDatabase->Find(retired)) throw std::runtime_error("Retired traffic fixture ID is occupied");
        brain->AddTraffic(retired,lTakingPosition,runway,SimLibElapsedTime);
        if(!brain->InList(retired)) throw std::runtime_error("Native ATC failed to queue traffic");
        AdvanceDetailedFrame();
        if(brain->InList(retired)) throw std::runtime_error("Detailed frame did not execute native ATC queue cleanup");
        brain->AddTraffic(retired,lTakingPosition,runway,SimLibElapsedTime);
        for(int i=0;i<250;++i) {
            AdvanceDetailedFrame();
            if(!brain->InList(retired)) throw std::runtime_error("Native ATC ran before its five-second cadence");
        }
        AdvanceDetailedFrame();
        if(brain->InList(retired)) throw std::runtime_error("Native ATC did not run again after five seconds");
        std::cout<<"{\"status\":\"ok\",\"case\":\""<<testCase<<"\",\"frames\":252,\"cleanup_cycles\":2,\"cadence_ms\":5000}"<<std::endl;
        return;
    }
    if(testCase=="carrier-approach" || testCase=="waypoint-hold" || testCase=="carrier-return" || testCase=="carrier-reentry" || testCase=="return-route" || returnFlightCase) {
        DoCampaignLoop(1);
        for(int tick=0;tick<360;++tick) {
            SetTime(TheCampaign.CurrentTime+5000);
            DoCampaignLoop(0);UpdateRealUnits(5000);
            TheCampaign.vuThread->Update(-1);gMainThread->Update(-1);
        }
        SimLibElapsedTime=TheCampaign.CurrentTime;
        FlightClass* flight=nullptr;
        {
            VuListIterator units(AllUnitList);
            for(auto* e=units.GetFirst();e;e=units.GetNext()) {
                auto* candidate=dynamic_cast<FlightClass*>(e);
                if(!candidate || !candidate->IsAggregate() || candidate->GetTotalVehicles()<1 || candidate->ZPos()>-5000) continue;
                if(Falcon4ClassTable[candidate->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE]==TYPE_HELICOPTER) continue;
                auto* base=candidate->GetUnitAirbase();auto* wp=candidate->GetCurrentUnitWP();
                if(!wp || wp->GetWPAction()==WP_TAKEOFF) continue;
                if((testCase=="carrier-approach" || testCase=="waypoint-hold" || testCase=="carrier-return" || testCase=="carrier-reentry") && (!base || !base->IsTaskForce())) continue;
                if((testCase=="carrier-approach" || testCase=="waypoint-hold" || testCase=="carrier-return" || testCase=="carrier-reentry") && hypot(candidate->XPos()-base->XPos(),candidate->YPos()-base->YPos())<20*NM_TO_FT) continue;
                auto* land=candidate->GetFirstUnitWP();
                while(land && land->GetWPAction()!=WP_LAND) land=land->GetNextWP();
                if(testCase=="return-route" && (!land || !land->GetPrevWP() || !land->GetPrevWP()->GetPrevWP())) continue;
                if(returnFlightCase && (!land || !land->GetPrevWP() || !base || !base->IsObjective() || base->GetType()!=(testCase=="airstrip-return"?TYPE_AIRSTRIP:TYPE_AIRBASE) || !static_cast<ObjectiveClass*>(base)->brain)) continue;
                flight=candidate;break;
            }
        }
        if(!flight) throw std::runtime_error("No eligible airborne flight for "+testCase);
        if(testCase=="carrier-approach" || testCase=="carrier-return" || testCase=="waypoint-hold") {
            auto* base=flight->GetUnitAirbase();
            auto* squad=static_cast<SquadronClass*>(flight->GetUnitSquadron());
            if(!base || !squad) throw std::runtime_error("Carrier return parent missing");
            VuBin<FlightClass> keepFlight(flight);
            const int losses=squad->GetTotalLosses(),pilotLosses=squad->GetPilotLosses();
            std::map<int,int> pilots;
            for(int slot=0;slot<PILOTS_PER_FLIGHT;++slot) if(flight->pilots[slot]!=NO_PILOT) pilots[slot]=flight->pilots[slot];
            for(auto* wp=flight->GetFirstUnitWP();wp;wp=wp->GetNextWP()) {
                float x,y,z;wp->GetLocation(&x,&y,&z);
                fprintf(stderr,"[carrier-route] action=%d x=%.1f y=%.1f alt=%.1f flags=%lu\n",wp->GetWPAction(),x,y,-z,wp->GetWPFlags());
            }
            if(!base->Deaggregate(FalconLocalSession)) throw std::runtime_error("Carrier deaggregation failed");
            if(!base->IsAwake()) base->Wake();
            if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Carrier return flight deaggregation failed");
            std::vector<VuBin<AircraftClass>> aircrafts;
            { VuListIterator parts(flight->GetComponents());
              for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                  auto* ac=dynamic_cast<AircraftClass*>(e);
                  if(!ac) throw std::runtime_error("Carrier return fixture is not an aircraft");
                  if(!ac->IsAwake()) ac->Wake();
                  ac->Sms->SetOperatorMasterSafe(true);
                  FalconWingmanMsg command(flight->Id(),FalconLocalGame);
                  command.dataBlock.from=ac->Id();command.dataBlock.to=AiWingman;command.dataBlock.command=FalconWingmanMsg::WMRTB;
                  ac->DBrain()->ReceiveOrders(&command);
                  if(!ac->DBrain()->IsReturningToBase()) throw std::runtime_error("Carrier return command rejected");
                  if(testCase=="carrier-approach") {
                      // Isolate the landing leg without relocating aircraft or
                      // changing route coordinates, speed, fuel or physics.
                      auto* landing=ac->curWaypoint->GetNextWP();
                      if(!landing || landing->GetWPAction()!=WP_LAND) throw std::runtime_error("Carrier approach lacks landing waypoint");
                      ac->DBrain()->SetReturnToBaseWaypoint(landing);
                  }
                  aircrafts.emplace_back(ac);
              }
            }
            if(aircrafts.empty()) throw std::runtime_error("Carrier return aircraft missing");
            if(testCase=="waypoint-hold") {
                std::map<int,float> altitudes;
                bool simple=false,complex=false;
                for(const auto& keep:aircrafts) {
                    auto* ac=keep.get();
                    if(!(ac->curWaypoint->GetWPFlags() & WPF_HOLDCURRENT) || ac->curWaypoint->GetWPAltitude()>100)
                        throw std::runtime_error("Altitude-hold fixture lacks a low stored hold waypoint");
                    altitudes[ac->pilotSlot]=ac->ZPos();
                }
                float maxError=0;
                for(int frame=0;frame<15000;++frame) {
                    AdvanceDetailedFrame();
                    for(const auto& keep:aircrafts) {
                        auto* ac=keep.get();float x,y,z;ac->DBrain()->GetTrackPoint(x,y,z);
                        simple|=ac->af->GetSimpleMode()!=0;complex|=ac->af->GetSimpleMode()==0;
                        if(!(ac->curWaypoint->GetWPFlags() & WPF_HOLDCURRENT) || fabs(z-altitudes[ac->pilotSlot])>1.0f)
                            throw std::runtime_error("Native waypoint controller changed held altitude target");
                        maxError=max(maxError,float(fabs(ac->ZPos()-altitudes[ac->pilotSlot])*.3048));
                        if(ac->IsDead() || ac->OnGround() || maxError>200) throw std::runtime_error("Altitude-hold flight departed its altitude envelope");
                    }
                }
                if(!simple || !complex) throw std::runtime_error("Altitude-hold fixture needs both waypoint controllers");
                for(const auto& keep:aircrafts) keep->curWaypoint->UnSetWPFlag(WPF_HOLDCURRENT);
                AdvanceDetailedFrame();
                for(const auto& keep:aircrafts) {
                    auto* ac=keep.get();float x,y,z;ac->DBrain()->GetTrackPoint(x,y,z);
                    ac->curWaypoint->SetWPFlag(WPF_HOLDCURRENT);
                    if(fabs(z-altitudes[ac->pilotSlot])<1000.0f) throw std::runtime_error("Released hold flag still uses held altitude");
                    altitudes[ac->pilotSlot]=ac->ZPos();
                }
                AdvanceDetailedFrame();
                for(const auto& keep:aircrafts) {
                    float x,y,z;keep->DBrain()->GetTrackPoint(x,y,z);
                    if(fabs(z-altitudes[keep->pilotSlot])>1.0f) throw std::runtime_error("Reenabled hold flag did not capture current altitude");
                }
                std::cout<<"{\"status\":\"ok\",\"case\":\"waypoint-hold\",\"frames\":15002,\"aircraft\":"<<aircrafts.size()<<",\"simple_and_complex\":true,\"flag_release_verified\":true,\"max_altitude_error_m\":"<<maxError<<"}"<<std::endl;
                return;
            }
            const std::string initial=DetailedSnapshotJson();
            std::set<int> recovered;
            int frames=0;
            // The saved RTB predecessor can be hundreds of kilometres from
            // the carrier. Allow the complete outbound and return legs; the
            // isolated landing-leg diagnostic keeps its 30-minute deadline.
            const int frameLimit=testCase=="carrier-return"?360000:90000;
            float maxRecoveryRange=0;
            for(;frames<frameLimit;++frames) {
                AdvanceDetailedFrame();
                for(const auto& keep:aircrafts) {
                    auto* ac=keep.get();const int slot=ac->pilotSlot;
                    if(recovered.count(slot)) continue;
                    const float range=float(hypot(ac->XPos()-base->XPos(),ac->YPos()-base->YPos()));
                    if(frames%250==0 || ac->IsDead() || ac->pctStrength<=0) {
                        float tx,ty,tz;ac->DBrain()->GetTrackPoint(tx,ty,tz);
                        fprintf(stderr,"\n[carrier-fuel] seconds=%.2f pilot=%d internal_lb=%.1f external_lb=%.1f bingo=%d fumes=%d flameout=%d airbase=%lu\n",(frames+1)*.02,slot,ac->af->Fuel(),ac->af->ExternalFuel(),ac->DBrain()->IsSetATC(DigitalBrain::SaidBingo)?1:0,ac->DBrain()->IsSetATC(DigitalBrain::SaidFumes)?1:0,ac->DBrain()->IsSetATC(DigitalBrain::SaidFlameout)?1:0,(unsigned long)ac->DBrain()->Airbase().num_);
                        fprintf(stderr,"\n[carrier-navigation] pilot=%d position=%.1f,%.1f target=%.1f,%.1f base=%.1f,%.1f target_base_m=%.1f simple=%d\n",slot,ac->XPos(),ac->YPos(),tx,ty,base->XPos(),base->YPos(),hypot(tx-base->XPos(),ty-base->YPos())*.3048,ac->af->GetSimpleMode());
                        fprintf(stderr,"\n[carrier-return] seconds=%.2f pilot=%d range_m=%.1f wp=%d mode=%d atc=%d ground=%d removed=%d strength=%.3f altitude_m=%.1f speed=%.1f pitch=%.3f target_alt_m=%.1f wp_alt_m=%.1f\n",(frames+1)*.02,slot,range*.3048,ac->curWaypoint?ac->curWaypoint->GetWPAction():-1,int(ac->DBrain()->GetCurrentMode()),int(ac->DBrain()->ATCStatus()),ac->OnGround()?1:0,ac->IsSetRemoveFlag()?1:0,ac->pctStrength,-ac->ZPos()*.3048,ac->GetVt()*.3048,ac->Pitch(),-tz*.3048,ac->curWaypoint?ac->curWaypoint->GetWPAltitude()*.3048:0);
                    }
                    if(ac->IsSetRemoveFlag() && pilots.count(slot) && flight->pilots[slot]==NO_PILOT && squad->GetPilotStatus(pilots[slot])==PILOT_AVAILABLE) {
                        if(range>3100 || ac->OnGround() || ac->pctStrength<1.0f) throw std::runtime_error("Carrier proximity recovery geometry or strength mismatch");
                        maxRecoveryRange=max(maxRecoveryRange,range*.3048f);recovered.insert(slot);
                    } else if(ac->IsDead() || ac->pctStrength<=0) throw std::runtime_error("Carrier return aircraft lost");
                }
                if(recovered.size()==aircrafts.size()) {++frames;break;}
            }
            if(recovered.size()!=aircrafts.size()) throw std::runtime_error(testCase=="carrier-return"?"Carrier return timed out after 120 minutes":"Carrier approach timed out after 30 minutes");
            for(int i=0;i<300;++i) AdvanceDetailedFrame();
            for(const auto& keep:aircrafts) if(vuDatabase->Find(keep->Id())) throw std::runtime_error("Carrier recovered actor remains in database");
            if(squad->GetTotalLosses()!=losses || squad->GetPilotLosses()!=pilotLosses) throw std::runtime_error("Carrier recovery increased squadron losses");
            std::cout<<"{\"status\":\"ok\",\"case\":\""<<testCase<<"\",\"frames\":"<<frames<<",\"aircraft\":"<<aircrafts.size()<<",\"recovered\":"<<recovered.size()<<",\"max_recovery_range_m\":"<<maxRecoveryRange<<",\"initial_detail\":"<<initial<<",\"final_detail\":"<<DetailedSnapshotJson()<<"}"<<std::endl;
            return;
        }
        if(returnFlightCase) {
            auto* base=static_cast<ObjectiveClass*>(flight->GetUnitAirbase());
            auto* squad=static_cast<SquadronClass*>(flight->GetUnitSquadron());
            if(!squad) throw std::runtime_error("Return flight has no parent squadron");
            const int initialLosses=squad->GetTotalLosses(),initialPilotLosses=squad->GetPilotLosses();
            std::map<int,int> assignedPilots;
            for(int slot=0;slot<PILOTS_PER_FLIGHT;++slot) if(flight->pilots[slot]!=NO_PILOT) assignedPilots[slot]=flight->pilots[slot];
            // Match regional entry: physical runway/taxiway features must exist
            // before aircraft wake, not only the aggregate ATC point geometry.
            if(base->IsAggregate() && !base->Deaggregate(FalconLocalSession)) throw std::runtime_error("Return airbase features failed to deaggregate");
            if(!base->IsAwake()) base->Wake();
            { VuListIterator features(base->GetComponents());
              for(auto* e=features.GetFirst();e;e=features.GetNext()) SyncDetailedModel(static_cast<SimBaseClass*>(e));
            }
            if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Return flight deaggregation failed");
            std::vector<VuBin<AircraftClass>> aircrafts;
            { VuListIterator parts(flight->GetComponents());
              for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                  auto* ac=dynamic_cast<AircraftClass*>(e);
                  if(!ac) throw std::runtime_error("Return flight created non-aircraft");
                  if(!ac->IsAwake()) ac->Wake();
                  ac->Sms->SetOperatorMasterSafe(true);
                  FalconWingmanMsg command(flight->Id(),FalconLocalGame);
                  command.dataBlock.from=ac->Id();command.dataBlock.to=AiWingman;
                  command.dataBlock.command=FalconWingmanMsg::WMRTB;
                  ac->DBrain()->ReceiveOrders(&command);
                  if(!ac->DBrain()->IsReturningToBase()) throw std::runtime_error("Return flight command rejected");
                  aircrafts.emplace_back(ac);
              }
            }
            if(aircrafts.empty()) throw std::runtime_error("Return flight fixture empty");
            const std::string initialDetail=DetailedSnapshotJson();
            std::string approachDetail;
            std::map<int,std::string> approachCheckpoints;
            std::ostringstream landingCheckpoints;bool landingComma=false;int displayedPilot=-1;
            std::set<int> landed,controlled,runwayTouchdowns;
            int frames=0;
            for(;frames<90000;++frames) {
                AdvanceDetailedFrame();
                for(const auto& keep:aircrafts) {
                    auto* ac=keep.get();const int pilot=ac->pilotSlot;
                    if(ac->IsSetRemoveFlag() && landed.count(pilot) && assignedPilots.count(pilot) &&
                       squad->GetPilotStatus(assignedPilots[pilot])==PILOT_AVAILABLE && flight->pilots[pilot]==NO_PILOT) continue;
                    if(ac->DBrain()->ATCStatus()!=noATC) controlled.insert(pilot);
                    if(!approachCheckpoints.count(pilot) && ac->DBrain()->ATCStatus()==lOnFinal) approachCheckpoints[pilot]=DetailedSnapshotJson();
                    if(ac->OnGround() && ac->onFlatFeature && base->brain->IsOnRunway(ac) && ac->af->gearPos>=0.99f) runwayTouchdowns.insert(pilot);
                    if(ac->OnGround() && !ac->IsDead() && ac->pctStrength>0 && ac->GetVt()<30*KNOTS_TO_FTPSEC &&
                       (ac->DBrain()->ATCStatus()==lLanded || ac->DBrain()->ATCStatus()==lTaxiOff) && runwayTouchdowns.count(pilot) && !landed.count(pilot)) {
                        if(!approachCheckpoints.count(pilot)) throw std::runtime_error("Landed aircraft has no native final approach checkpoint");
                        landed.insert(pilot);displayedPilot=pilot;approachDetail=approachCheckpoints[pilot];
                        if(landingComma) landingCheckpoints<<',';landingComma=true;
                        landingCheckpoints<<"{\"pilot_slot\":"<<pilot<<",\"frame\":"<<frames+1<<",\"detail\":"<<DetailedSnapshotJson()<<"}";
                    }
                    if(frames%250==0) fprintf(stderr,"\n[return-flight] seconds=%.2f pilot=%d atc=%d mode=%d wp=%d range_m=%.1f altitude_m=%.1f speed_mps=%.1f fuel_lb=%.1f ground=%d removed=%d\n",
                        (frames+1)*0.02,pilot,int(ac->DBrain()->ATCStatus()),int(ac->DBrain()->GetCurrentMode()),ac->curWaypoint?ac->curWaypoint->GetWPAction():-1,
                        hypot(ac->XPos()-base->XPos(),ac->YPos()-base->YPos())*0.3048,-ac->ZPos()*0.3048,ac->GetVt()*0.3048,
                        ac->af->Fuel()+ac->af->ExternalFuel(),ac->OnGround()?1:0,ac->IsSetRemoveFlag()?1:0);
                    if(ac->IsDead() || ac->pctStrength<=0) throw std::runtime_error("Return flight retired/destroyed pilot="+std::to_string(pilot)+" landed="+std::to_string(landed.count(pilot))+" removed="+std::to_string(ac->IsSetRemoveFlag())+" strength="+std::to_string(ac->pctStrength)+" atc="+std::to_string(ac->DBrain()->ATCStatus()));
                    if(ac->IsSetRemoveFlag() && !landed.count(pilot)) throw std::runtime_error("Return flight removed before verified landing");
                }
                if(landed.size()==aircrafts.size()) {++frames;break;}
            }
            if(approachDetail.empty() || landed.size()!=aircrafts.size() || controlled.size()!=aircrafts.size()) throw std::runtime_error("Return flight did not complete controlled landing within 30 minutes");
            const std::string landedDetail=DetailedSnapshotJson();
            std::string taxiDetail="{}";
            int recoveryFrames=0,taxiRecovered=0;
            if(testCase!="return-flight") {
                VuBin<FlightClass> keepFlight(flight);
                std::set<int> recovered;
                for(;recoveryFrames<90000;++recoveryFrames) {
                    AdvanceDetailedFrame();
                    for(const auto& keep:aircrafts) {
                        auto* ac=keep.get();const int pilot=ac->pilotSlot;
                        if(recovered.count(pilot)) continue;
                        if(pilot==displayedPilot && taxiDetail=="{}" && ac->DBrain()->ATCStatus()==lTaxiOff && !ac->IsSetRemoveFlag()) taxiDetail=DetailedSnapshotJson();
                        if(recoveryFrames%1500==0) {
                            float tx,ty,tz;ac->DBrain()->GetTrackPoint(tx,ty,tz);
                            fprintf(stderr,"\n[return-recovery] seconds=%.2f pilot=%d atc=%d speed=%.2f removed=%d dead=%d campaign_status=%d taxi=%d yaw=%.2f pos=%.2f,%.2f target=%.2f,%.2f distance=%.2f pitch=%.3f nws=%d stick=%.3f simple=%d throttle=%.3f rpm=%.3f brake=%d engine_stopped=%d\n",
                                (recoveryFrames+1)*0.02,pilot,int(ac->DBrain()->ATCStatus()),ac->GetVt()*0.3048,ac->IsSetRemoveFlag()?1:0,ac->IsDead()?1:0,int(flight->plane_stats[pilot]),ac->DBrain()->GetTaxiPoint(),ac->Yaw(),ac->XPos(),ac->YPos(),tx,ty,hypot(ac->XPos()-tx,ac->YPos()-ty),ac->Pitch()*RTD,ac->af->IsSet(AirframeClass::NoseSteerOn)?1:0,ac->af->rstick,ac->af->GetSimpleMode(),ac->af->throtl,ac->af->rpm,ac->af->IsSet(AirframeClass::WheelBrakes)?1:0,ac->af->IsSet(AirframeClass::EngineStopped)?1:0);
                        }
                        if(ac->IsSetRemoveFlag() && assignedPilots.count(pilot) && flight->pilots[pilot]==NO_PILOT &&
                           squad->GetPilotStatus(assignedPilots[pilot])==PILOT_AVAILABLE &&
                           (flight->plane_stats[pilot]==AIRCRAFT_RTB || flight->plane_stats[pilot]==AIRCRAFT_NOT_ASSIGNED)) {
                            recovered.insert(pilot);
                            if(ac->DBrain()->ATCStatus()==lTaxiOff) ++taxiRecovered;
                        }
                        else if(ac->IsDead() || ac->pctStrength<=0 || flight->plane_stats[pilot]==AIRCRAFT_DEAD)
                            throw std::runtime_error("Landed aircraft became a campaign loss during recovery");
                    }
                    if(recovered.size()==aircrafts.size()) {++recoveryFrames;break;}
                }
                if(recovered.size()!=aircrafts.size()) throw std::runtime_error("Landed aircraft did not finish native recovery within 30 minutes");
                if((testCase=="return-taxi" || testCase=="airstrip-return") && (taxiDetail=="{}" || taxiRecovered!=int(aircrafts.size()))) throw std::runtime_error("Aircraft recovered without completing native runway exit/taxi");
                for(int cleanup=0;cleanup<300;++cleanup) AdvanceDetailedFrame();
                for(const auto& keep:aircrafts) {
                    auto* ac=keep.get();
                    if(vuDatabase->Find(ac->Id()) || base->brain->InList(ac->Id()))
                        throw std::runtime_error("Recovered aircraft remains in native database or ATC queue");
                    if(squad->GetPilotStatus(assignedPilots.at(ac->pilotSlot))!=PILOT_AVAILABLE || flight->pilots[ac->pilotSlot]!=NO_PILOT)
                        throw std::runtime_error("Recovered pilot was not released to squadron");
                }
                if(squad->GetTotalLosses()!=initialLosses || squad->GetPilotLosses()!=initialPilotLosses) {
                    throw std::runtime_error("Recovery increased squadron aircraft or pilot losses");
                }
            }
            std::cout<<"{\"status\":\"ok\",\"case\":\""<<testCase<<"\",\"recovery_frames\":"<<recoveryFrames<<",\"taxi_recovered\":"<<taxiRecovered<<",\"displayed_pilot\":"<<displayedPilot<<",\"landing_checkpoints\":["<<landingCheckpoints.str()<<"],\"frames\":"<<frames<<",\"aircraft\":"<<aircrafts.size()<<",\"landed\":"<<landed.size()<<",\"controlled\":"<<controlled.size()<<",\"runway_touchdowns\":"<<runwayTouchdowns.size()<<",\"initial_detail\":"<<initialDetail<<",\"approach_detail\":"<<approachDetail<<",\"landed_detail\":"<<landedDetail<<",\"taxi_detail\":"<<taxiDetail<<"}"<<std::endl;
            return;
        }
        if(testCase=="return-route") {
            auto* land=flight->GetFirstUnitWP();
            while(land && land->GetWPAction()!=WP_LAND) land=land->GetNextWP();
            auto* approach=land->GetPrevWP();
            std::map<int,VU_ID> previousIds;
            int checked=0;
            for(int pass=0;pass<4;++pass) {
                if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Return route deaggregation failed");
                {
                    VuListIterator parts(flight->GetComponents());
                    for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                        auto* ac=dynamic_cast<AircraftClass*>(e);
                        if(!ac) throw std::runtime_error("Return route fixture is not an aircraft");
                        if(!ac->IsAwake()) ac->Wake();
                        if(pass && (!previousIds.count(ac->pilotSlot) || previousIds[ac->pilotSlot]==ac->Id()))
                            throw std::runtime_error("Return route pilot was not recreated");
                        previousIds[ac->pilotSlot]=ac->Id();
                        if(pass==0) {
                            FalconWingmanMsg command(flight->Id(),FalconLocalGame);
                            command.dataBlock.from=ac->Id();command.dataBlock.to=AiWingman;
                            command.dataBlock.command=FalconWingmanMsg::WMRTB;
                            ac->DBrain()->ReceiveOrders(&command);
                        }
                        if(pass<3) {
                            if(!ac->DBrain()->IsReturningToBase() || !ac->curWaypoint)
                                throw std::runtime_error("Valid return intent lost across route edit");
                            float x,y,z,ex,ey,ez;
                            ac->curWaypoint->GetLocation(&x,&y,&z);approach->GetLocation(&ex,&ey,&ez);
                            if(x!=ex || y!=ey || z!=ez || ac->curWaypoint->GetWPAction()!=approach->GetWPAction())
                                throw std::runtime_error("Restored return waypoint disagrees with revised route");
                        } else if(ac->DBrain()->IsReturningToBase())
                            throw std::runtime_error("Removed landing route retained stale return intent");
                        ++checked;
                    }
                }
                if(!flight->Reaggregate(FalconLocalSession)) throw std::runtime_error("Return route reaggregation failed");
                if(pass==0) {
                    auto* inserted=new WayPointClass();
                    inserted->CloneWP(approach);
                    float x,y,z;approach->GetLocation(&x,&y,&z);
                    inserted->SetLocation(x+10000,y+10000,z);
                    approach->GetPrevWP()->InsertWP(inserted);
                } else if(pass==1) {
                    float x,y,z;approach->GetLocation(&x,&y,&z);
                    approach->SetLocation(x+20000,y+20000,z);
                } else if(pass==2) {
                    for(auto* wp=flight->GetFirstUnitWP();wp;wp=wp->GetNextWP())
                        if(wp->GetWPAction()==WP_LAND) wp->SetWPAction(WP_NOTHING);
                }
            }
            std::cout<<"{\"status\":\"ok\",\"case\":\"return-route\",\"passes\":4,\"actors_checked\":"<<checked<<",\"inserted_waypoint\":true,\"replanned_approach\":true,\"removed_landing\":true}"<<std::endl;
            return;
        }
        const int count=flight->GetTotalVehicles();
        float maxError=0,maxFormationError=0,minTravel=1e9f,maxStep=0;
        extern float VFormRight[4],VFormAhead[4];
        std::map<int,VU_ID> ids;
        for(int pass=0;pass<2;++pass) {
            float x,y,z;
            flight->GetRealPosition(&x,&y,&z);
            if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Carrier flight refused deaggregation");
            int seen=0;
            std::vector<VuBin<AircraftClass>> aircrafts;
            std::vector<std::array<float,3>> origins,previous;
            {
                VuListIterator parts(flight->GetComponents());
                for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                    auto* aircraft=dynamic_cast<AircraftClass*>(e);
                    if(!aircraft) throw std::runtime_error("Carrier flight created non-aircraft");
                    if(!aircraft->IsAwake()) aircraft->Wake();
                    aircraft->Sms->SetOperatorMasterSafe(true);
                    if(pass==0) {
                        // An invalid return route must not latch RTB or clear
                        // the existing navigation point. Restore the fixture
                        // immediately; no physical frame runs without a route.
                        auto* route=aircraft->waypoint;
                        auto* current=aircraft->curWaypoint;
                        aircraft->waypoint=nullptr;
                        FalconWingmanMsg command(flight->Id(),FalconLocalGame);
                        command.dataBlock.from=aircraft->Id();command.dataBlock.to=AiWingman;
                        command.dataBlock.command=FalconWingmanMsg::WMRTB;
                        try { aircraft->DBrain()->ReceiveOrders(&command); }
                        catch(...) { aircraft->waypoint=route;throw; }
                        aircraft->waypoint=route;
                        if(aircraft->DBrain()->IsReturningToBase() || aircraft->curWaypoint!=current)
                            throw std::runtime_error("Invalid native return order changed flight intent");
                    }
                    aircrafts.emplace_back(aircraft);
                    origins.push_back({aircraft->XPos(),aircraft->YPos(),aircraft->ZPos()});
                    const float error=float(hypot(aircraft->XPos()-x,aircraft->YPos()-y)*0.3048);
                    fprintf(stderr,"[carrier-reentry] pass=%d pilot=%d displacement_m=%.3f\n",pass,aircraft->pilotSlot,error);
                    maxError=max(maxError,error);
                    const int slot=aircraft->pilotSlot;
                    if(slot<0 || slot>=4) throw std::runtime_error("Carrier fixture has an unsupported formation slot");
                    const float expected=float(hypot(VFormRight[slot],VFormAhead[slot])*0.3048);
                    maxFormationError=max(maxFormationError,fabs(error-expected));
                    if(!std::isfinite(error) || fabs(error-expected)>1.0f) throw std::runtime_error("Carrier aircraft placement disagrees with native formation offset");
                    if(pass && (!ids.count(slot) || ids[slot]==aircraft->Id())) throw std::runtime_error("Carrier aircraft pilot identity was not preserved on recreation");
                    ids[aircraft->pilotSlot]=aircraft->Id();++seen;
                }
            }
            previous=origins;
            for(int frame=0;frame<1500;++frame) {
                AdvanceDetailedFrame();
                for(size_t i=0;i<aircrafts.size();++i) {
                    auto* ac=aircrafts[i].get();
                    const float x=ac->XPos(),y=ac->YPos(),z=ac->ZPos();
                    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(ac->GetVt()))
                        throw std::runtime_error("Carrier flight produced non-finite physical state");
                    const float step=float(hypot(x-previous[i][0],y-previous[i][1])*0.3048);
                    maxStep=max(maxStep,step);
                    if(step>100 || ac->IsDead() || ac->OnGround()) throw std::runtime_error("Carrier flight teleported or stopped airborne flight");
                    previous[i]={x,y,z};
                }
            }
            for(size_t i=0;i<aircrafts.size();++i) {
                auto* ac=aircrafts[i].get();
                const float travel=float(hypot(ac->XPos()-origins[i][0],ac->YPos()-origins[i][1])*0.3048);
                fprintf(stderr,"[carrier-flight] pass=%d pilot=%d travel_m=%.3f speed_mps=%.3f\n",pass,ac->pilotSlot,travel,ac->GetVt()*0.3048);
                minTravel=min(minTravel,travel);
                if(travel<500) throw std::runtime_error("Carrier aircraft failed to fly after entry");
            }
            if(seen!=count || !flight->Reaggregate(FalconLocalSession)) throw std::runtime_error("Carrier flight roster/reaggregation mismatch");
        }
        std::cout<<"{\"status\":\"ok\",\"case\":\"carrier-reentry\",\"aircraft\":"<<count<<",\"passes\":2,\"frames_per_pass\":1500,\"min_travel_m\":"<<minTravel<<",\"max_step_m\":"<<maxStep<<",\"max_formation_error_m\":"<<maxFormationError<<",\"max_displacement_m\":"<<maxError<<"}"<<std::endl;
        return;
    }
    if (weaponOrderCase || radarTrackCase || heloCombat || pursuitCase || testCase == "region-drain" || testCase == "countermeasures" || (testCase == "air" || ((testCase == "rocket-legacy-pod" || testCase == "rocket-hold") || testCase == "ground-retarget") || (testCase == "bomb-hold" || (testCase == "bomb-flight" || testCase == "bomb-ground-hit" || testCase == "bomb-ground-miss"))) || heloCase || testCase == "region" || testCase == "air-combat") {
        DoCampaignLoop(1);
        for (int tick = 0; tick < 360; ++tick) {
            SetTime(TheCampaign.CurrentTime + 5000);
            DoCampaignLoop(0); UpdateRealUnits(5000);
            TheCampaign.vuThread->Update(-1); gMainThread->Update(-1);
        }
        SimLibElapsedTime = TheCampaign.CurrentTime;
    }
    if(testCase=="countermeasures") { RunCountermeasureDiagnostic(); return; }
    VuListIterator it(AllUnitList);
    auto campaignParent = static_cast<Unit>(it.GetFirst());
    if (!campaignParent) throw std::runtime_error("No campaign unit for native diagnostic");
    if (weaponOrderCase || radarTrackCase || heloCombat || testCase == "naval-combat" || testCase == "region-drain" || testCase == "region" || testCase == "air-combat") {
        const bool airCombat=weaponOrderCase || radarTrackCase || testCase=="air-combat" || testCase=="region-drain";
        const bool drainTest=testCase=="region-drain";
        const bool navalCombat=testCase=="naval-combat";
        VuBin<SimBaseClass> exitMissile;
        auto armedForAirCombat=[](UnitClass* unit) {
            if(!unit->IsFlight()) return false;
            auto* flight=static_cast<FlightClass*>(unit);
            for(int a=0;a<flight->GetLoadouts();++a) for(int hp=0;hp<HARDPOINT_MAX;++hp) {
                const auto& loadout=flight->GetLoadout()[a];
                const int weapon=loadout.WeaponID[hp];
                if(weapon<=0 || weapon>=NumWeaponTypes || !loadout.WeaponCount[hp]) continue;
                const int index=WeaponDataTable[weapon].Index;
                if(index<0 || index>=NumEntities) continue;
                const auto* info=Falcon4ClassTable[index].vuClassData.classInfo_;
                if(info[VU_TYPE]==TYPE_MISSILE && info[VU_STYPE]==STYPE_MISSILE_AIR_AIR) return true;
            }
            return false;
        };
        Unit a=nullptr,b=nullptr; double best=1e30;
        VuListIterator first(AllUnitList),second(AllUnitList);
        bool preferMi8=false;
        if(rocketCase) for(auto* e=first.GetFirst();e;e=first.GetNext()) {
            auto* unit=static_cast<UnitClass*>(e);
            if(unit->IsFlight() && !unit->Inactive() && !unit->IsDead() && unit->GetTotalVehicles()>0 &&
               std::string(GetVehicleName(unit->GetVehicleID(0)))=="Mi-8") {preferMi8=true;break;}
        }
        for(int scan=0;scan<37;++scan) {
        best=1e30;a=b=nullptr;
        for(auto* e=first.GetFirst();e;e=first.GetNext()) {
            if(heloCombat && (!static_cast<UnitClass*>(e)->IsFlight() || Falcon4ClassTable[static_cast<UnitClass*>(e)->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE]!=TYPE_HELICOPTER)) continue;
            if(heloCombat) {
                auto* flight=static_cast<FlightClass*>(e);
                fprintf(stderr,"[helo-roster] id=%lu vehicle=%s mission=%d count=%d\n",flight->Id().num_,GetVehicleName(flight->GetVehicleID(0)),flight->GetUnitMission(),flight->GetTotalVehicles());
            }
            if(rocketCase && preferMi8 && std::string(GetVehicleName(static_cast<UnitClass*>(e)->GetVehicleID(0)))!="Mi-8") continue;
            if(airCombat && !armedForAirCombat(static_cast<UnitClass*>(e))) continue;
            auto* u=static_cast<UnitClass*>(e); if((airCombat ? (!u->IsFlight()||u->ZPos()>-5000||Falcon4ClassTable[u->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE]==TYPE_HELICOPTER) : (heloCombat?!u->IsFlight():navalCombat?!u->IsTaskForce():!u->IsBattalion()))||u->Inactive()||u->IsDead()||u->GetTotalVehicles()<1) continue;
            for(auto* f=second.GetFirst();f;f=second.GetNext()) {
                if(airCombat && !armedForAirCombat(static_cast<UnitClass*>(f))) continue;
                auto* v=static_cast<UnitClass*>(f); if((airCombat ? (!v->IsFlight()||v->ZPos()>-5000||Falcon4ClassTable[v->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE]==TYPE_HELICOPTER) : (navalCombat?!v->IsTaskForce():!v->IsBattalion()))||v->Inactive()||v->IsDead()||v->GetTotalVehicles()<1||GetTTRelations(u->GetTeam(),v->GetTeam())<Hostile) continue;
                double dx=u->XPos()-v->XPos(),dy=u->YPos()-v->YPos(),d=dx*dx+dy*dy;
                if(d<best) {best=d;a=u;b=v;}
            }
        }
        if(!airCombat || best<30.0*30.0*3280.839895*3280.839895) break;
        if(scan==36) throw std::runtime_error("No opposing airborne flights within 30 km");
        for(int tick=0;tick<60;++tick) {
            SetTime(TheCampaign.CurrentTime+5000);DoCampaignLoop(0);UpdateRealUnits(5000);
            TheCampaign.vuThread->Update(-1);gMainThread->Update(-1);
        }
        SimLibElapsedTime=TheCampaign.CurrentTime;
        }
        if(!a||!b) throw std::runtime_error("No opposing units");
        if(navalCombat) {
            // Controlled starting separation only; native AI must detect,
            // select weapons and fire. No target lock or damage is injected.
            GridIndex ax,ay;a->GetLocation(&ax,&ay);
            b->SetLocation(ax+3,ay);
            for(auto* unit:{a,b}) { unit->DisposeWayPoints(); unit->SetCurrentWaypoint(0); }
            best=9.0*3280.839895*3280.839895;
        }
        if(heloCombat) {
            GridIndex hx,hy;a->GetLocation(&hx,&hy);
            if(rocketCase) {
                int attackClass=0;
                for(int index=1;index<NumEntities;++index) {
                    const auto& type=Falcon4ClassTable[index];
                    if(type.dataType!=DTYPE_UNIT || !type.dataPtr || type.vuClassData.classInfo_[VU_DOMAIN]!=DOMAIN_AIR || type.vuClassData.classInfo_[VU_TYPE]!=TYPE_FLIGHT || type.vuClassData.classInfo_[VU_STYPE]!=STYPE_UNIT_ATTACK_HELO) continue;
                    auto* unit=static_cast<UnitClassDataType*>(type.dataPtr);
                    auto* vehicle=GetVehicleClassData(unit->VehicleType[0]);
                    if(!vehicle) continue;
                    for(int hp=1;hp<HARDPOINT_MAX && !attackClass;++hp) if(vehicle->Weapons[hp]==255) {
                        for(int entry=0;entry<MAX_WEAPONS_IN_LIST;++entry) {
                            int weapon=GetListEntryWeapon(vehicle->Weapon[hp],entry);if(!weapon) break;
                            for(int rocket=0;rocket<NumRocketTypes;++rocket) if(RocketDataTable[rocket].weaponId==weapon && RocketDataTable[rocket].nweaponId>0) attackClass=index;
                        }
                    }
                    if(attackClass) break;
                }
                if(!attackClass) throw std::runtime_error("Loaded data has no attack-helicopter flight class");
                auto* attack=NewFlight(attackClass+VU_LAST_ENTITY_TYPE,nullptr,nullptr);
                attack->SetOwner(a->GetCountry());attack->SetLocation(hx,hy);attack->SetAltitude(1000);
                attack->SetRoster(0);attack->SetNumVehicles(0,1);
                attack->plane_stats[0]=AIRCRAFT_AVAILABLE;attack->pilots[0]=2;
                vuDatabase->Insert(attack);a=attack;
                fprintf(stderr,"[helo-attack] diagnostic native flight class=%d vehicle=%s\n",attackClass,GetVehicleName(attack->GetVehicleID(0)));
                auto* vehicle=GetVehicleClassData(attack->GetVehicleID(0));
                for(int hp=0;hp<HARDPOINT_MAX;++hp) if(vehicle->Weapon[hp]) {
                    fprintf(stderr,"[helo-hardpoint] hp=%d weapon=%d count=%d\n",hp,vehicle->Weapon[hp],int(vehicle->Weapons[hp]));
                    if(vehicle->Weapons[hp]==255) for(int entry=0;entry<MAX_WEAPONS_IN_LIST;++entry) {
                        const int weapon=GetListEntryWeapon(vehicle->Weapon[hp],entry);if(!weapon) break;
                        fprintf(stderr,"[helo-compatible] weapon=%d name=%s count=%d hit=%d guidance=%d\n",weapon,WeaponDataTable[weapon].Name,GetListEntryWeapons(vehicle->Weapon[hp],entry),WeaponDataTable[weapon].HitChance[b->GetMovementType()],WeaponDataTable[weapon].GuidanceFlags);
                    }
                }
            }
            const GridIndex tx=hx+(rocketCase?0:2),ty=hy+(rocketCase?1:0);
            b->SetLocation(tx,ty);b->DisposeWayPoints();
            a->SetUnitMission(AMIS_CAS);a->SetTarget(b);a->SetFinal(1);a->SetAborted(0);
            auto* flight=static_cast<FlightClass*>(a);
            if(rocketCase) {
                const int loaded=flight->LoadWeapons(flight->GetUnitSquadron(),DefaultDamageMods,b->GetMovementType(),98,0,WEAP_DUMB_ONLY);
                fprintf(stderr,"[helo-loadout] native ground-attack weapons loaded=%d\n",loaded);
                // Explicit diagnostic payload selected from the original
                // hardpoint compatibility menu; automatic campaign scoring
                // assigns no damage score to the empty launcher container.
                auto* vehicle=GetVehicleClassData(flight->GetVehicleID(0));
                auto* payload=new LoadoutStruct;
                payload->WeaponID[0]=vehicle->Weapon[0];payload->WeaponCount[0]=vehicle->Weapons[0];
                bool launcherLoaded=false;
                for(int hp=1;hp<HARDPOINT_MAX && !launcherLoaded;++hp) if(vehicle->Weapons[hp]==255) {
                    for(int entry=0;entry<MAX_WEAPONS_IN_LIST;++entry) {
                        const int weapon=GetListEntryWeapon(vehicle->Weapon[hp],entry);if(!weapon) break;
                        bool mapped=false;
                        for(int rocket=0;rocket<NumRocketTypes;++rocket) if(RocketDataTable[rocket].weaponId==weapon && RocketDataTable[rocket].nweaponId>0) mapped=true;
                        if(mapped) {
                            payload->WeaponID[hp]=weapon;payload->WeaponCount[hp]=1;launcherLoaded=true;break;
                        }
                    }
                }
                if(!launcherLoaded) {delete payload;throw std::runtime_error("No compatible native rocket launcher");}
                flight->SetLoadout(payload,1);
            }
            // This is a scheduled CAS fixture, not an AWACS diversion. An
            // assigned target can undivert a new flight to its empty old mission.
            flight->ClearEvalFlag(0xff);flight->SetUnitMissionTarget(b->Id());flight->ClearAssignedTarget();
            a->DisposeWayPoints();
            a->AddUnitWP(hx,hy,1000,100,TheCampaign.CurrentTime,0,WP_NOTHING);
            a->AddUnitWP(tx,ty,1000,100,TheCampaign.CurrentTime+5*CampaignMinutes,5*CampaignMinutes,WP_GNDSTRIKE);
            a->SetCurrentWaypoint(2);a->GetCurrentUnitWP()->SetWPTarget(b->Id());
            a->GetCurrentUnitWP()->SetWPFlag(WPF_TARGET);
        }
        const double x=(a->YPos()+b->YPos())/(2*3280.839895),y=(a->XPos()+b->XPos())/(2*3280.839895);
        fprintf(stderr,"[detailed] region %.3f %.3f distance %.3f km\n",x,y,sqrt(best)/3280.839895);
        if(airCombat) {
            // Diagnostic orders only: native sensors, AI, stores and dynamics
            // still decide whether and when to shoot. No prescribed aircraft motion.
            for(auto* unit:{a,b}) {
                auto* target=unit==a?b:a;
                unit->SetUnitMission(AMIS_INTERCEPT);unit->SetTarget(target);
                unit->SetFinal(1);unit->SetAborted(0);
                static_cast<FlightClass*>(unit)->ClearEvalFlag(0xff);
                static_cast<FlightClass*>(unit)->SetUnitMissionTarget(target->Id());
                static_cast<FlightClass*>(unit)->ClearAssignedTarget();
                GridIndex ux,uy,tx,ty;unit->GetLocation(&ux,&uy);target->GetLocation(&tx,&ty);
                const int altitude=static_cast<int>(-unit->ZPos());
                unit->DisposeWayPoints();
                unit->AddUnitWP(ux,uy,altitude,450,TheCampaign.CurrentTime,0,WP_NOTHING);
                unit->AddUnitWP(tx,ty,altitude,450,TheCampaign.CurrentTime+10*CampaignMinutes,5*CampaignMinutes,WP_INTERCEPT);
                unit->AddUnitWP(ux,uy,altitude,450,TheCampaign.CurrentTime+30*CampaignMinutes,0,WP_NOTHING);
                unit->SetCurrentWaypoint(2);
                unit->GetCurrentUnitWP()->SetWPTarget(target->Id());
                unit->GetCurrentUnitWP()->SetWPFlag(WPF_TARGET);
            }
        }
        if(rocketCase) fprintf(stderr,"[rocket-mission] before-region id=%lu mission=%d db_same=%d\n",a->Id().num_,a->GetUnitMission(),vuDatabase->Find(a->Id())==a);
        SetDetailedRegion(true,x,y,airCombat?20:10);
        if(rocketCase) fprintf(stderr,"[rocket-mission] after-region id=%lu mission=%d db_same=%d\n",a->Id().num_,a->GetUnitMission(),vuDatabase->Find(a->Id())==a);
        unsigned helicopterRocketRounds=0;
        if(heloCombat) std::cerr<<"[helo-initial] "<<DetailedSnapshotJson()<<std::endl;
        if(testCase=="helo-rockets-only") {
            // Controlled empty-gun load state separates rocket delivery from
            // the faster cannon killing the same target before rocket arrival.
            VuListIterator shooters(SimDriver.objectList);
            for(auto* e=shooters.GetFirst();e;e=shooters.GetNext()) {
                auto* h=dynamic_cast<HelicopterClass*>(e);
                if(!h || h->GetCampaignObject()!=a) continue;
                for(int hp=0;hp<h->Sms->NumHardpoints();++hp) {
                    auto* gun=h->Sms->hardPoint[hp]->GetGun();
                    if(gun) {gun->numRoundsRemaining=0;h->Sms->hardPoint[hp]->weaponCount=0;}
                }
            }
        }
        if(rocketCase) {
            AdvanceDetailedFrame();
            fprintf(stderr,"[rocket-mission] after-frame id=%lu mission=%d db_same=%d\n",a->Id().num_,a->GetUnitMission(),vuDatabase->Find(a->Id())==a);
            if(a->GetUnitMission()!=AMIS_CAS)
                throw std::runtime_error("Scheduled helicopter CAS reverted during native target selection");
            VuListIterator helos(SimDriver.objectList);
            for(auto* e=helos.GetFirst();e;e=helos.GetNext()) if(auto* h=dynamic_cast<HelicopterClass*>(e)) {
                if(h->GetCampaignObject()==a && h->targetPtr) {
                    auto* target=h->targetPtr->BaseData();
                    const float px=target->XPos()-3000,py=target->YPos(),pz=target->ZPos()-500;
                    h->hf->Init(px,py,pz);h->SetPosition(px,py,pz);h->SetYPR(0,0,0);h->SetDelta(0,0,0);CalcTransformMatrix(h);
                    CalcRelGeom(h,h->targetPtr,nullptr,50);CalcRelGeom(h,h->targetList,nullptr,50);
                    if(testCase=="helo-target-retire") {
                        h->hBrain->GunsEngage();
                        if(!h->hBrain->rocketAimValid)
                            throw std::runtime_error("Target retirement fixture has no native rocket solution");
                        auto* tracked=h->hBrain->targetPtr;tracked->Reference();
                        h->hBrain->ClearTarget();
                        h->SetTarget(NULL);
                        const auto clearedAimDetail=DetailedSnapshotJson();
                        const bool aimReset=!h->hBrain->rocketAimValid && h->hBrain->rocketAimNextUpdate==0;
                        h->SetTarget(tracked);h->hBrain->SetTarget(tracked);tracked->Release();
                        if(!aimReset) throw std::runtime_error("Cleared helicopter target retained its rocket solution");
                        h->hBrain->UpdateRocketAim();
                        if(!h->hBrain->rocketAimValid)
                            throw std::runtime_error("Reacquired helicopter target did not recompute rocket aim");
                        if(!h->Sms->FindWeaponClass(wcRocketWpn)) throw std::runtime_error("Retirement fixture has no rocket pod");
                        auto* pendingPod=dynamic_cast<BombClass*>(h->Sms->GetCurrentWeapon());
                        if(!pendingPod || !pendingPod->IsLauncher()) throw std::runtime_error("Retirement fixture selected non-pod");
                        const int onboard=pendingPod->LauGetRoundsOnboard();
                        h->FCC->SetTarget(h->targetPtr);h->Sms->SetAGBPair(false);
                        h->Sms->SetMasterArm(SMSBaseClass::Arm);
                        h->Sms->LaunchRocket();
                        if(!pendingPod->LauIsFiring() || !pendingPod->targetPtr) throw std::runtime_error("Retirement fixture did not queue targeted rockets");
                        if(QueueDetailedAction(h->Id().creator_,h->Id().num_,"master_safe")!="applied" ||
                           pendingPod->LauIsFiring() || pendingPod->LauGetRoundsOnboard()!=onboard)
                            throw std::runtime_error("Helicopter user Safe did not immediately cancel queued rockets");
                        h->hBrain->GunsEngage();h->Sms->SetMasterArm(SMSBaseClass::Arm);
                        if(!h->Sms->IsOperatorMasterSafe() || h->Sms->MasterArm()!=SMSBaseClass::Safe)
                            throw std::runtime_error("Helicopter AI overrode user Safe");
                        if(QueueDetailedAction(h->Id().creator_,h->Id().num_,"weapons_free")!="aircraft_unavailable")
                            throw std::runtime_error("Helicopter accepted an aircraft-only order");
                        const auto masterSafeDetail=DetailedSnapshotJson();
                        if(QueueDetailedAction(h->Id().creator_,h->Id().num_,"master_arm")!="applied" ||
                           h->Sms->IsOperatorMasterSafe() || h->Sms->MasterArm()!=SMSBaseClass::Arm || pendingPod->LauIsFiring())
                            throw std::runtime_error("Helicopter Arm failed or revived a cancelled salvo");
                        h->Sms->FindWeaponClass(wcRocketWpn);h->FCC->SetTarget(h->targetPtr);
                        h->Sms->LaunchRocket();
                        if(!pendingPod->LauIsFiring()) throw std::runtime_error("Helicopter could not queue fresh rockets after Arm");
                        VuBin<SimBaseClass> retired(static_cast<SimBaseClass*>(target));
                        retired->Sleep();
                        h->Sms->Exec();
                        if(pendingPod->LauIsFiring() || pendingPod->targetPtr || pendingPod->LauGetRoundsOnboard()!=onboard)
                            throw std::runtime_error("Retired target retained a queued rocket salvo or consumed ammunition");
                        AdvanceDetailedFrame();
                        if((h->targetPtr && h->targetPtr->BaseData()==retired.get()) ||
                           (h->hBrain->targetPtr && h->hBrain->targetPtr->BaseData()==retired.get()))
                            throw std::runtime_error("Helicopter retained a sleeping target until periodic refresh");
                        if(h->hBrain->rocketAimValid && h->hBrain->rocketAimTarget==retired->Id())
                            throw std::runtime_error("Sleeping target retained a valid rocket solution");
                        if(QueueDetailedAction(h->Id().creator_,h->Id().num_,"master_safe")!="applied")
                            throw std::runtime_error("Helicopter Safe command failed before sleep");
                        h->Sleep();
                        if(QueueDetailedAction(h->Id().creator_,h->Id().num_,"master_arm")!="aircraft_unavailable")
                            throw std::runtime_error("Sleeping helicopter accepted user Arm");
                        h->Sms->SetOperatorMasterSafe(false);
                        h->Wake();h->Sms->SetMasterArm(SMSBaseClass::Arm);
                        AdvanceDetailedFrame();
                        if(!h->Sms->IsOperatorMasterSafe() || h->Sms->MasterArm()!=SMSBaseClass::Safe)
                            throw std::runtime_error("Helicopter wake or AI lost restored user Safe");
                        std::cout<<"{\"status\":\"ok\",\"case\":\"helo-target-retire\",\"master_control\":true,\"master_safe_restored\":true,\"master_safe_detail\":"<<masterSafeDetail<<",\"target_dropped\":true,\"aim_reset\":true,\"aim_recomputed\":true,\"frames\":2,\"helicopter_id\":\""<<h->Id().creator_<<":"<<h->Id().num_<<"\",\"cleared_aim_detail\":"<<clearedAimDetail<<"}"<<std::endl;
                        SetDetailedRegion(false,x,y,10);
                        return;
                    }
                    if(testCase=="helo-altitude") {
                        if(!target->OnGround()) throw std::runtime_error("Combat altitude fixture requires a ground target");
                        const float targetX=target->XPos()+target->XDelta()*SimLibMajorFrameTime;
                        const float targetY=target->YPos()+target->YDelta()*SimLibMajorFrameTime;
                        const float targetZ=target->ZPos()+target->ZDelta()*SimLibMajorFrameTime;
                        const float groundZ=OTWDriver.GetGroundLevel(h->XPos()+h->XDelta(),h->YPos()+h->YDelta());
                        const float clearance=200.0f+0.075f*std::hypot(targetX-h->XPos(),targetY-h->YPos());
                        const float expected=min(3500.0f,max(300.0f,groundZ-targetZ+clearance));
                        h->hBrain->hasAltitudeCommand=false;
                        h->hBrain->GunsEngage();
                        const float actual=h->hBrain->commandedAltitudeAGL;
                        fprintf(stderr,"[helo-altitude] terrain_z=%.2f expected_agl=%.2f commanded_agl=%.2f\n",groundZ,expected,actual);
                        if(!h->hBrain->hasAltitudeCommand || !std::isfinite(actual) || fabs(expected-actual)>0.1f)
                            throw std::runtime_error("Helicopter combat passed MSL altitude to the AGL controller");
                        // Turn away: GunsEngage must enter AutoTrack and retain the
                        // target-relative world altitude instead of the route altitude.
                        h->SetYPR(float(PI),0,0);CalcTransformMatrix(h);
                        h->hBrain->hasAltitudeCommand=false;
                        h->hBrain->GunsEngage();
                        const float trackingExpected=groundZ-target->ZPos()+500.0f;
                        const float trackingActual=h->hBrain->commandedAltitudeAGL;
                        fprintf(stderr,"[helo-altitude-track] expected_agl=%.2f commanded_agl=%.2f route_agl=%.2f\n",trackingExpected,trackingActual,h->GetWPalt());
                        if(!h->hBrain->hasAltitudeCommand || !std::isfinite(trackingActual) || fabs(trackingExpected-trackingActual)>0.1f)
                            throw std::runtime_error("Helicopter AutoTrack ignored its target altitude");
                        SetDetailedRegion(false,x,y,10);
                        std::cout<<"{\"status\":\"ok\",\"case\":\"helo-altitude\",\"commanded_agl_ft\":"<<actual<<",\"expected_agl_ft\":"<<expected
                                 <<",\"tracking_agl_ft\":"<<trackingActual<<",\"tracking_expected_agl_ft\":"<<trackingExpected<<"}"<<std::endl;
                        return;
                    }
                }
                for(int hp=0;hp<h->Sms->NumHardpoints();++hp) {
                    auto* station=h->Sms->hardPoint[hp];auto* weapon=station->weaponPointer.get();
                    if((testCase=="rocket-predictor-flight" || testCase=="rocket-ground-hit" || testCase=="rocket-ground-miss") && h->GetCampaignObject()==a && weapon && weapon->IsLauncher()) {
                        if(!h->targetPtr) throw std::runtime_error("Rocket ground fixture has no victim");
                        VuBin<SimBaseClass> victim(static_cast<SimBaseClass*>(h->targetPtr->BaseData()));
                        const float initial=victim->Strength();
                        const float px=victim->XPos()+(testCase=="rocket-ground-miss"?2000.0f:0.0f);
                        const float py=victim->YPos();
                        const bool shallow=testCase=="rocket-predictor-flight";
                        h->SetPosition(px,py,OTWDriver.GetGroundLevel(px,py)-(shallow?500.0f:5000.0f));
                        h->SetYPR(0,shallow?-0.14f:-float(PI*0.5),0);h->SetDelta(0,0,0);CalcTransformMatrix(h);
                        VuBin<MissileClass> rocket(static_cast<MissileClass*>(InitAMissile(h,static_cast<BombClass*>(weapon)->LauGetWeaponId(),0)));
                        rocket->SetPosition(h->XPos(),h->YPos(),h->ZPos());
                        rocket->SetLaunchRotation(0,0);
                        float predictedX=0,predictedY=0,predictedZ=0,predictedTime=0;
                        const auto predictionClock=SimLibElapsedTime;
                        const auto predictionEnds=ff_headless::combat.missileEnds;
                        if(!rocket->PredictRocketGroundImpact(&predictedX,&predictedY,&predictedZ,&predictedTime))
                            throw std::runtime_error("Native rocket predictor found no ground solution");
                        if(SimLibElapsedTime!=predictionClock || ff_headless::combat.missileEnds!=predictionEnds || victim->Strength()!=initial || rocket->GetRuntime()!=0)
                            throw std::runtime_error("Rocket prediction changed live simulation state");
                        // No target pointer: only native flight, ground impact and area damage.
                        rocket->Start(nullptr);vuDatabase->Insert(rocket.get());rocket->Wake();
                        const auto key=std::make_pair((unsigned long)rocket->Id().creator_,(unsigned long)rocket->Id().num_);
                        for(int tick=0;tick<6000 && !rocket->IsExploding();++tick) {
                            SimLibElapsedTime+=20;SimLibFrameElapsed=float(SimLibElapsedTime);++SimLibFrameCount;
                            rocket->Exec();DispatchDetailedMessages();
                            if(!std::isfinite(rocket->XPos()) || !std::isfinite(rocket->YPos()) || !std::isfinite(rocket->ZPos()))
                                throw std::runtime_error("Rocket ground flight produced non-finite position");
                        }
                        const float after=victim->Strength();
                        const float predictionError=std::hypot(predictedX-rocket->XPos(),predictedY-rocket->YPos());
                        fprintf(stderr,"[rocket-prediction] horizontal_error=%.3f time_error=%.3f predicted_time=%.3f\n",predictionError,predictedTime-rocket->GetRuntime(),predictedTime);
                        if(predictionError>50.0f || fabs(predictedTime-rocket->GetRuntime())>0.04f)
                            throw std::runtime_error("Native rocket prediction disagrees with actual ground flight");
                        fprintf(stderr,"[rocket-ground] end=%d time=%.2f delta_xy=%.2f,%.2f strength=%.3f->%.3f radius=%.1f\n",rocket->done,rocket->GetRuntime(),rocket->XPos()-victim->XPos(),rocket->YPos()-victim->YPos(),initial,after,sqrt(rocket->lethalRadiusSqrd));
                        if(!rocket->IsExploding() || ff_headless::combat.projectileEnds[key]!=1)
                            throw std::runtime_error("Rocket ground flight did not terminate exactly once");
                        if((after<initial)!=(testCase=="rocket-ground-hit"))
                            throw std::runtime_error("Native rocket ground proximity damage disagrees with hit/miss placement");
                        if(testCase=="rocket-ground-hit" && !ff_headless::combat.projectileDamage[key])
                            throw std::runtime_error("Rocket ground damage was not attributed to this projectile");
                        std::cout<<"{\"status\":\"ok\",\"case\":\""<<testCase<<"\",\"before\":"<<initial<<",\"after\":"<<after<<",\"end_code\":"<<rocket->done<<",\"seconds\":"<<rocket->GetRuntime()<<",\"terminal_events\":1}"<<std::endl;
                        SetDetailedRegion(false,x,y,10);
                        return;
                    }
                    if(testCase=="rocket-salvo" && h->GetCampaignObject()==a && weapon && weapon->IsLauncher()) {
                        auto* launcher=static_cast<BombClass*>(weapon);
                        if(!h->targetPtr) throw std::runtime_error("Rocket salvo fixture has no target");
                        const VU_ID expectedTarget=h->targetPtr->BaseData()->Id();
                        const int rounds=launcher->LauGetRoundsRemaining();
                        h->Sms->SetCurrentWeapon(hp,weapon);
                        h->FCC->SetTarget(h->targetPtr);
                        h->Sms->SetMasterArm(SMSBaseClass::Arm);
                        h->Sms->LaunchRocket();
                        // A completed selection change must not retarget the queued salvo.
                        h->FCC->SetTarget(nullptr);
                        std::set<VU_ID> checked;
                        for(int tick=0;tick<3000 && int(checked.size())<rounds;++tick) {
                            SimLibElapsedTime+=20;
                            h->Sms->Exec();gMainThread->Update(-1);
                            VuListIterator launched(SimDriver.objectList);
                            for(auto* e=launched.GetFirst();e;e=launched.GetNext()) {
                                auto* m=dynamic_cast<MissileClass*>(e);
                                if(!m || m->Parent()!=h || !checked.insert(m->Id()).second) continue;
                                fprintf(stderr,"[rocket-salvo] projectile=%lu target=%lu expected=%lu\n",m->Id().num_,m->targetPtr?m->targetPtr->BaseData()->Id().num_:0,expectedTarget.num_);
                                if(!m->targetPtr || m->targetPtr->BaseData()->Id()!=expectedTarget)
                                    throw std::runtime_error("Queued rocket lost its committed salvo target");
                            }
                        }
                        if(rounds<=0 || int(checked.size())!=rounds || launcher->LauIsFiring())
                            throw std::runtime_error("Rocket salvo did not exhaust its queued inventory");
                        if(launcher->targetPtr) throw std::runtime_error("Completed launcher retained its salvo target");
                        SetDetailedRegion(false,x,y,10);
                        std::cout<<"{\"status\":\"ok\",\"case\":\"rocket-salvo\",\"rounds\":"<<rounds<<",\"targets_checked\":"<<checked.size()<<"}"<<std::endl;
                        return;
                    }
                    if(testCase=="rocket-dispersion" && h->GetCampaignObject()==a && weapon && weapon->IsLauncher()) {
                        VuBin<MissileClass> rocket(static_cast<MissileClass*>(InitAMissile(h,static_cast<BombClass*>(weapon)->LauGetWeaponId(),0)));
                        rocket->Start(nullptr);
                        const unsigned checked=VerifyRocketDispersion(rocket.get());
                        SetDetailedRegion(false,x,y,10);
                        std::cout<<"{\"status\":\"ok\",\"case\":\"rocket-dispersion\",\"samples\":"<<checked<<",\"configured_cone_deg\":4}"<<std::endl;
                        return;
                    }
                    if(testCase=="rocket-mount" && h->GetCampaignObject()==a && weapon && weapon->IsLauncher()) {
                        // Controlled mount orientation, through the ordinary SMS salvo path.
                        // Do not steer the projectile or modify damage/dispersion data.
                        const int slot=min(station->NumPoints()-1,weapon->GetRackSlot());
                        const float az=0.2f,el=-0.15f;
                        station->SetSubRotation(slot,az,el);
                        h->Sms->SetCurrentWeapon(hp,weapon);
                        h->Sms->SetMasterArm(SMSBaseClass::Arm);
                        h->Sms->LaunchRocket();
                        h->Sms->Exec();
                        gMainThread->Update(-1);
                        unsigned checked=0;
                        VuListIterator launched(SimDriver.objectList);
                        for(auto* e=launched.GetFirst();e;e=launched.GetNext()) {
                            auto* m=dynamic_cast<MissileClass*>(e);
                            if(!m || m->Parent()!=h) continue;
                            auto* pose=static_cast<SimBaseClass*>(m);
                            const float yawError=pose->Yaw()-h->Yaw()-az;
                            const float pitchError=pose->Pitch()-h->Pitch()-el;
                            const float tolerance=fabs(m->GetRocketDispersionConeAngle())*DTR*0.5f+0.001f;
                            fprintf(stderr,"[rocket-mount] yaw_error=%.4f pitch_error=%.4f tolerance=%.4f\n",yawError,pitchError,tolerance);
                            if(!std::isfinite(yawError) || !std::isfinite(pitchError) || fabs(yawError)>tolerance || fabs(pitchError)>tolerance)
                                throw std::runtime_error("Native rocket ignored launcher mount rotation");
                            ++checked;
                        }
                        if(checked!=1) throw std::runtime_error("Expected one native rocket on the first salvo tick");
                        SetDetailedRegion(false,x,y,10);
                        std::cout<<"{\"status\":\"ok\",\"case\":\"rocket-mount\",\"projectiles_checked\":"<<checked<<"}"<<std::endl;
                        return;
                    }
                    if(testCase=="rocket-predictor" && h->GetCampaignObject()==a && weapon && weapon->IsLauncher()) {
                        VuBin<MissileClass> probe(static_cast<MissileClass*>(InitAMissile(h,static_cast<BombClass*>(weapon)->LauGetWeaponId(),0)));
                        const float savedYaw=h->Yaw(),savedPitch=h->Pitch(),savedRoll=h->Roll();
                        for(float heading:{0.0f,float(PI*0.5)}) {
                            h->SetYPR(heading,-0.25f,0);
                            float ix=0,iy=0,iz=0,time=0;
                            if(!probe->FindRocketGroundImpact(&ix,&iy,&iz,&time)) throw std::runtime_error("Rocket predictor has no downward solution");
                            const float dx=ix-h->XPos(),dy=iy-h->YPos();
                            const float impactTerrain=OTWDriver.GetGroundLevel(ix,iy);
                            fprintf(stderr,"[rocket-predictor-terrain] impact_z=%.1f terrain_z=%.1f time=%.3f\n",iz,impactTerrain,time);
                            if(!std::isfinite(time) || time<=0 || fabs(iz-impactTerrain)>0.1f)
                                throw std::runtime_error("Rocket predictor returned no terrain intersection or flight time");
                            fprintf(stderr,"[rocket-predictor] heading=%.3f dx=%.1f dy=%.1f\n",heading,dx,dy);
                            if(!std::isfinite(ix) || !std::isfinite(iy) || !std::isfinite(iz) ||
                                (heading==0 ? (dx<=0 || fabs(dy)>1) : (dy<=0 || fabs(dx)>1)))
                                throw std::runtime_error("Rocket impact prediction does not follow native heading axes");
                        }
                        h->SetYPR(savedYaw,savedPitch,savedRoll);
                        SetDetailedRegion(false,x,y,10);
                        std::cout<<"{\"status\":\"ok\",\"case\":\"rocket-predictor\",\"directions_checked\":2}"<<std::endl;
                        return;
                    }
                    if(weapon) {
                        if(h->GetCampaignObject()==a && weapon->IsLauncher()) helicopterRocketRounds+=static_cast<BombClass*>(weapon)->LauGetRoundsRemaining();
                        auto& wcd=WeaponDataTable[station->weaponId];
                        auto& ct=Falcon4ClassTable[wcd.Index];
                        fprintf(stderr,"[helo-store] actor=%lu hp=%d class=%d usable=%d launcher=%d rocket=%d rounds=%d ct=%d swd=%d wcd_swd=%d wcd_class=%d\n",h->Id().num_,hp,int(station->GetWeaponClass()),weapon->IsUseable(),weapon->IsLauncher(),weapon->IsLauncher()?static_cast<BombClass*>(weapon)->LauGetWeaponId():0,weapon->IsLauncher()?static_cast<BombClass*>(weapon)->LauGetRoundsRemaining():0,wcd.Index,ct.vehicleDataIndex,wcd.SimweapIndex,(wcd.SimweapIndex>=0 && wcd.SimweapIndex<NumSimWeaponEntries)?SimWeaponDataTable[wcd.SimweapIndex].weaponClass:-1);
                        for(int index=0;index<NumRocketTypes;++index) if(RocketDataTable[index].weaponId==station->weaponId)
                            fprintf(stderr,"[helo-rocket-map] launcher=%d projectile=%d rounds=%d\n",station->weaponId,RocketDataTable[index].nweaponId,RocketDataTable[index].weaponCount);
                    }
                }
            }
        }
        if(testCase=="helo-gun-hit" || testCase=="helo-gun-miss") {
            AdvanceDetailedFrame();
            HelicopterClass* helicopter=nullptr;
            VuListIterator candidates(SimDriver.objectList);
            for(auto* e=candidates.GetFirst();e;e=candidates.GetNext()) {
                auto* h=dynamic_cast<HelicopterClass*>(e);
                if(h && h->Guns && h->Guns->bullet && h->targetPtr && h->targetPtr->BaseData()->IsSim()) {helicopter=h;break;}
            }
            if(!helicopter) throw std::runtime_error("No helicopter collision fixture");
            VuBin<SimBaseClass> target(static_cast<SimBaseClass*>(helicopter->targetPtr->BaseData()));
            const float before=target->Strength();
            helicopter->SetPosition(target->XPos()-300,target->YPos(),target->ZPos()-100);
            CalcRelGeom(helicopter,helicopter->targetList,nullptr,1.0f/SimLibMajorFrameTime);
            helicopter->SetTarget(nullptr);helicopter->fireGun=0;
            auto* gun=helicopter->Guns;
            // Controlled in-flight projectile input, not forced damage or AI aim.
            // The selected target is cleared: native collision candidates must
            // still deliver a bullet hit through HelicopterClass::DoWeapons.
            for(int i=0;i<gun->numTracers;++i) gun->bullet[i]={};
            gun->numFlying=0;
            const int initialAmmo=gun->numRoundsRemaining;
            for(auto armState : {SMSBaseClass::Safe,SMSBaseClass::Sim}) {
                helicopter->Sms->SetMasterArm(armState);helicopter->fireGun=1;
                for(int frame=0;frame<50;++frame) {SimLibElapsedTime+=20;++SimLibFrameCount;helicopter->DoWeapons();}
                if(gun->numRoundsRemaining!=initialAmmo || gun->numFlying)
                    throw std::runtime_error("Helicopter gun fired with Master Arm Safe/Sim");
            }
            helicopter->Sms->SetOperatorMasterSafe(true);
            helicopter->Sms->SetMasterArm(SMSBaseClass::Arm);
            helicopter->DoWeapons();
            if(helicopter->Sms->MasterArm()!=SMSBaseClass::Safe || gun->numRoundsRemaining!=initialAmmo || gun->numFlying)
                throw std::runtime_error("Helicopter gun bypassed operator master safe");
            helicopter->Sms->SetOperatorMasterSafe(false);
            helicopter->fireGun=0;
            // Keep Safe for the in-flight collision check: changing the switch
            // must stop new fire without erasing bullets already in flight.
            helicopter->Sms->SetMasterArm(SMSBaseClass::Safe);
            auto& bullet=gun->bullet[1];bullet.flying=1;
            bullet.x=target->XPos()-20;bullet.y=target->YPos()+(testCase=="helo-gun-miss"?1000:0);bullet.z=target->ZPos()-1;
            bullet.xdot=gun->initBulletVelocity;gun->numFlying=1;
            helicopter->DoWeapons();gMainThread->Update(-1);
            const float after=target->Strength();
            Tpoint drawn;target->drawPointer->GetPosition(&drawn);
            fprintf(stderr,"[helo-collision] before=%.2f after=%.2f damage=%llu bullet_live=%d drawn_delta=%.1f,%.1f,%.1f\n",before,after,ff_headless::combat.detailedDamageMessages,gun->bullet[1].flying,drawn.x-target->XPos(),drawn.y-target->YPos(),drawn.z-target->ZPos());
            if(testCase=="helo-gun-hit" && (after>=before || !ff_headless::combat.helicopterDamageDealt))
                throw std::runtime_error("Unselected native ground target received no helicopter bullet damage");
            if(testCase=="helo-gun-miss" && (after!=before || ff_headless::combat.helicopterDamageDealt))
                throw std::runtime_error("Missed helicopter bullet caused unintended damage");
            const auto bulletDamageEvents=ff_headless::combat.helicopterDamageDealt;
            if(testCase=="helo-gun-hit") {
                const float savedRadius=gun->lethalRadiusSqrd;
                gun->lethalRadiusSqrd=0;
                gun->SendDamageMessage(target.get(),1.0f,FalconDamageType::ProximityDamage);
                DispatchDetailedMessages();
                if(target->Strength()!=after) throw std::runtime_error("Zero-radius weapon damaged a distant target");
                gun->SendDamageMessage(target.get(),0.0f,FalconDamageType::BulletDamage);
                DispatchDetailedMessages();
                if(!std::isfinite(target->Strength()) || target->Strength()>=after)
                    throw std::runtime_error("Zero-radius direct hit did not produce finite native damage");
                gun->lethalRadiusSqrd=savedRadius;
            }
            std::cout<<"{\"status\":\"ok\",\"case\":"<<ff::headless::JsonString(testCase)<<",\"before\":"<<before<<",\"after\":"<<after<<",\"damage_events\":"<<bulletDamageEvents<<",\"selected_target\":false,\"safe_sim_blocked\":true,\"operator_safe_blocked\":true}"<<std::endl;
            SetDetailedRegion(false,x,y,10);
            return;
        }
        if(airCombat) {
            fprintf(stderr,"[detailed] opposing flights missions=%d,%d aggregate=%d,%d ids=%lu,%lu wp=%d,%d\n",int(a->GetUnitMission()),int(b->GetUnitMission()),a->IsAggregate(),b->IsAggregate(),a->Id().num_,b->Id().num_,a->GetCurrentUnitWP()?a->GetCurrentUnitWP()->GetWPAction():-1,b->GetCurrentUnitWP()?b->GetCurrentUnitWP()->GetWPAction():-1);
            for(auto* unit:{a,b}) {
                fprintf(stderr,"[detailed] selected parent=%lu awake=%d vehicles=%d slot0=%d class0=%d domain=%d type=%d\n",unit->Id().num_,unit->IsAwake(),unit->GetTotalVehicles(),unit->GetNumVehicles(0),unit->GetVehicleID(0),int(Falcon4ClassTable[unit->GetVehicleID(0)].vuClassData.classInfo_[VU_DOMAIN]),int(Falcon4ClassTable[unit->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE]));
                int aircraftCount=0;
                VuListIterator parts(unit->GetComponents());
                for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                    auto* actor=static_cast<SimBaseClass*>(e);
                    if(actor->IsAirplane() && actor->IsAwake()) ++aircraftCount;
                    fprintf(stderr,"[detailed] selected component=%lu type=%d awake=%d aircraft=%d helo=%d\n",actor->Id().num_,actor->Type(),actor->IsAwake(),actor->IsAirplane(),actor->IsHelicopter());
                }
                if(aircraftCount!=unit->GetTotalVehicles()) throw std::runtime_error("Selected flight did not instantiate every aircraft roster slot");
            }
            VuListIterator actors(SimDriver.objectList);
            for(auto* e=actors.GetFirst();e;e=actors.GetNext()) {
                auto* actor=static_cast<SimBaseClass*>(e);if(!actor->IsAirplane()) continue;
                auto* ac=static_cast<AircraftClass*>(actor);
                if(ac->curWaypoint) {
                    float wx=0,wy=0,wz=0;ac->curWaypoint->GetLocation(&wx,&wy,&wz);
                    fprintf(stderr,"[air-route] id=%lu position=%.0f,%.0f,%.0f waypoint=%.0f,%.0f,%.0f campaign_target=%lu\n",ac->Id().num_,ac->XPos(),ac->YPos(),ac->ZPos(),wx,wy,wz,static_cast<UnitClass*>(ac->GetCampaignObject())->GetTargetId().num_);
                }
                fprintf(stderr,"[detailed] aircraft id=%lu digital=%d brain=%d hardpoints=%d team=%d wp=%d mission=%d parent=%lu\n",ac->Id().num_,ac->IsDigital(),ac->DBrain()!=nullptr,ac->Sms->NumHardpoints(),int(ac->GetTeam()),ac->curWaypoint?ac->curWaypoint->GetWPAction():-1,int(ac->DBrain()->MissionType()),ac->GetCampaignObject()->Id().num_);
            }
        }
        std::map<VU_ID,std::pair<VuBin<UnitClass>,int>> initialUnits;
        unsigned aircraftFrames=0,navalTargetFrames=0,navalGuidedFrames=0,heloTargetFrames=0;
        bool visualRangeChecked=false;
        std::set<VU_ID> navalMissiles;
        std::set<VU_ID> helicopterProjectiles;
        std::map<VU_ID,std::array<float,3>> helicopterProjectileTargets;
        std::set<int> navalFiringTeams;
        unsigned navalRadarFrames=0;
        std::string navalFlightSnapshot,navalDamageSnapshot;
        std::string airBvrSnapshot;
        std::string helicopterFireSnapshot;
        std::string helicopterDamageSnapshot;
        std::string helicopterPreemptedSnapshot;
        VuListIterator initialList(AllUnitList);
        for(auto* e=initialList.GetFirst();e;e=initialList.GetNext()) {
            auto* u=static_cast<UnitClass*>(e);
            if(!u->IsAggregate()) initialUnits.emplace(u->Id(),std::make_pair(VuBin<UnitClass>(u),u->GetTotalVehicles()));
        }
        for(int tick=0;tick<15000;++tick) {
            AdvanceDetailedFrame();
            if(heloCombat) {
                VuListIterator projectiles(SimDriver.objectList);
                for(auto* e=projectiles.GetFirst();e;e=projectiles.GetNext()) {
                    auto* missile=dynamic_cast<MissileClass*>(e);
                    if(missile && !helicopterProjectiles.empty() && missile->Id()==*helicopterProjectiles.begin() && tick%25==0 && missile->GetRuntime()<6.0f) {
                        fprintf(stderr,"[rocket-trajectory] t=%.2f pitch=%.3f flight_pitch=%.3f speed=%.1f agl=%.1f\n",missile->GetRuntime(),static_cast<SimBaseClass*>(missile)->Pitch(),atan2(-missile->ZDelta(),std::hypot(missile->XDelta(),missile->YDelta())),missile->GetVt(),OTWDriver.GetGroundLevel(missile->XPos(),missile->YPos())-missile->ZPos());
                    }
                    if(missile && missile->Parent() && missile->Parent()->IsHelicopter() && helicopterProjectiles.insert(missile->Id()).second) {
                        auto* target=missile->targetPtr?missile->targetPtr->BaseData():nullptr;
                        if(target) helicopterProjectileTargets[missile->Id()]={target->XPos(),target->YPos(),target->ZPos()};
                        fprintf(stderr,"[rocket-launch] id=%lu target=%lu pitch=%.3f yaw=%.3f speed=%.1f target_delta=%.1f,%.1f,%.1f\n",missile->Id().num_,target?target->Id().num_:0,static_cast<SimBaseClass*>(missile)->Pitch(),static_cast<SimBaseClass*>(missile)->Yaw(),missile->GetVt(),target?target->XPos()-missile->XPos():0,target?target->YPos()-missile->YPos():0,target?target->ZPos()-missile->ZPos():0);
                    }
                }
                // Rocket observation must wait for an actual projectile: guns
                // may fire earlier while the helicopter aligns its fixed pods.
                if(helicopterFireSnapshot.empty() && ff_headless::combat.helicopterWeaponsFired &&
                   (!rocketCase || !helicopterProjectiles.empty()))
                    helicopterFireSnapshot=DetailedSnapshotJson();
                if(helicopterDamageSnapshot.empty()) {
                    bool damageObserved=!rocketCase && ff_headless::combat.helicopterDamageDealt>0;
                    if(rocketCase) for(const auto& id:helicopterProjectiles) {
                        const auto found=ff_headless::combat.projectileDamage.find({(unsigned long)id.creator_,(unsigned long)id.num_});
                        if(found!=ff_headless::combat.projectileDamage.end() && found->second>0) {damageObserved=true;break;}
                    }
                    if(damageObserved) helicopterDamageSnapshot=DetailedSnapshotJson();
                }
                if(rocketCase && helicopterPreemptedSnapshot.empty()) for(const auto& id:helicopterProjectiles) {
                    const auto found=ff_headless::combat.projectileImpactAudit.find({id.creator_,id.num_});
                    if(found==ff_headless::combat.projectileImpactAudit.end()) continue;
                    const auto& impact=found->second;
                    if(impact.observed && impact.groundImpact && impact.targetKnown && impact.targetRetired &&
                       impact.targetStrength<=0 && impact.targetRangeSquared<impact.lethalRadiusSquared &&
                       !impact.liveNearbyObjects && !impact.liveDamageRequests) {
                        helicopterPreemptedSnapshot=DetailedSnapshotJson();break;
                    }
                }
                if(tick%250==0) {
                    unsigned awakeTargets=0,aliveTargets=0;
                    if(b->GetComponents()) {
                        VuListIterator parts(b->GetComponents());
                        for(auto* e=parts.GetFirst();e;e=parts.GetNext()) {
                            auto* part=static_cast<SimBaseClass*>(e);
                            if(part->IsAwake()) ++awakeTargets;
                            if(!part->IsDead() && !part->IsExploding() && part->Strength()>0) ++aliveTargets;
                        }
                    }
                    fprintf(stderr,"[helo-engagement-state] t=%.2f attacker_count=%d attacker_aggregate=%d target_count=%d target_aggregate=%d target_awake=%u target_alive=%u campaign_target=%lu\n",tick*.02,a->GetTotalVehicles(),a->IsAggregate(),b->GetTotalVehicles(),b->IsAggregate(),awakeTargets,aliveTargets,a->GetTarget()?a->GetTarget()->Id().num_:0);
                }
                VuListIterator helos(SimDriver.objectList);
                for(auto* e=helos.GetFirst();e;e=helos.GetNext()) {
                    auto* helo=dynamic_cast<HelicopterClass*>(e);if(!helo) continue;
                    if(tick%250==0 && helo->GetCampaignObject()==a)
                        fprintf(stderr,"[helo-attack-state] t=%.2f id=%lu strength=%.2f x=%.1f y=%.1f z=%.1f aim=%d error=%.1f tolerance=%.1f\n",tick*.02,helo->Id().num_,helo->Strength(),helo->XPos(),helo->YPos(),helo->ZPos(),helo->hBrain->rocketAimValid,helo->hBrain->rocketAimError,helo->hBrain->rocketAimTolerance);
                    if(helo->targetPtr && helo->targetPtr->BaseData()->IsSim()) ++heloTargetFrames;
                    if(tick%250==0) fprintf(stderr,"[helo-combat] actor=%lu target=%lu sim=%d mode=%d station=%d weapon=%d\n",helo->Id().num_,helo->targetPtr?helo->targetPtr->BaseData()->Id().num_:0,helo->targetPtr?helo->targetPtr->BaseData()->IsSim():0,int(helo->hBrain->curMode),helo->Sms->CurHardpoint(),helo->Sms->GetCurrentWeapon()?helo->Sms->GetCurrentWeapon()->Type():0);
                }
            }
            if(airCombat && tick%50==0) {
                VuListIterator weapons(SimDriver.objectList);
                for(auto* e=weapons.GetFirst();e;e=weapons.GetNext()) {
                    auto* missile=dynamic_cast<MissileClass*>(e);if(!missile) continue;
                    auto* seeker=missile->sensorArray?missile->sensorArray[0]:nullptr;
                    fprintf(stderr,"[air-flight] id=%lu weapon=%s parent=%lu t=%.2f speed=%.1f altitude=%.1f target=%lu range=%.1f seeker=%d lock=%lu end=%d\n",
                        missile->Id().num_,missile->GetWCD()->Name,missile->Parent()?missile->Parent()->Id().num_:0,missile->GetRuntime(),missile->GetVt(),-missile->ZPos(),
                        missile->targetPtr?missile->targetPtr->BaseData()->Id().num_:0,missile->targetPtr?missile->targetPtr->localData->range:0,
                        seeker?int(seeker->Type()):-1,seeker&&seeker->CurrentTarget()?seeker->CurrentTarget()->BaseData()->Id().num_:0,missile->done);
                }
            }
            if(navalCombat) {
                VuListIterator ships(SimDriver.objectList);
                for(auto* e=ships.GetFirst();e;e=ships.GetNext()) {
                    if(static_cast<SimBaseClass*>(e)->IsMissile()) {
                        auto* missile=static_cast<MissileClass*>(e);
                        if(missile->Parent() && missile->Parent()->GetDomain()==DOMAIN_SEA) {
                            navalMissiles.insert(e->Id());
                            navalFiringTeams.insert(missile->Parent()->GetTeam());
                            auto* seeker=missile->sensorArray?missile->sensorArray[0]:nullptr;
                            if(missile->targetPtr && seeker && seeker->CurrentTarget()) {
                                ++navalGuidedFrames;
                                if(!visualRangeChecked && seeker->Type()==SensorClass::Visual) {
                                    auto* visual=static_cast<VisualClass*>(seeker);
                                    auto* probe=new SimObjectType(missile->targetPtr->BaseData());
                                    probe->Reference();
                                    const float range=visual->GetTypeData()->nominalRange;
                                    probe->localData->range=range*.99f;
                                    const bool nearVisible=visual->GetSignature(probe)>0;
                                    probe->localData->range=range*1.01f;
                                    const bool farVisible=visual->GetSignature(probe)>0;
                                    probe->Release();
                                    if(!(range>0) || !nearVisible || farVisible)
                                        throw std::runtime_error("Surface visual signature violates nominal range boundary");
                                    visualRangeChecked=true;
                                }
                            }
                        }
                        if(tick%50==0) {
                            auto* tracked=missile->targetPtr;
                            auto* seeker=missile->sensorArray?missile->sensorArray[0]:nullptr;
                            fprintf(stderr,"[naval-flight] id=%lu t=%.2f alt=%.1f speed=%.1f pitch=%.3f target=%lu range=%.1f seeker=%d lock=%lu\n",
                                missile->Id().num_,missile->GetRuntime(),-missile->ZPos(),missile->GetVt(),static_cast<SimBaseClass*>(missile)->Pitch(),
                                tracked?tracked->BaseData()->Id().num_:0,tracked?tracked->localData->range:0,
                                seeker?int(seeker->Type()):-1,seeker && seeker->CurrentTarget()?seeker->CurrentTarget()->BaseData()->Id().num_:0);
                        }
                    }
                    auto* ship=dynamic_cast<GroundClass*>(e);
                    if(ship && ship->isShip && ship->targetPtr) ++navalTargetFrames;
                    if(ship && ship->isShip) {
                        auto* radar=FindSensor(ship,SensorClass::Radar);
                        if(radar && radar->CurrentTarget() && radar->CurrentTarget()->BaseData()->GetDomain()==DOMAIN_SEA)
                            ++navalRadarFrames;
                    }
                }
            }
            if(navalCombat) {
                if(navalFlightSnapshot.empty() && navalMissiles.size()>=2 && tick>=250)
                    navalFlightSnapshot=DetailedSnapshotJson();
                if(navalDamageSnapshot.empty() && ff_headless::combat.detailedDamageMessages)
                    navalDamageSnapshot=DetailedSnapshotJson();
            }
            if(airCombat && airBvrSnapshot.empty() && tick%250==0) {
                bool bvr=false,tracked=false,unconfirmed=false;
                VuListIterator observed(SimDriver.objectList);
                for(auto* object=observed.GetFirst();object;object=observed.GetNext()) {
                    auto* aircraft=dynamic_cast<AircraftClass*>(object);if(!aircraft) continue;
                    const auto tactic=aircraft->DBrain()->GetBvrTactic();
                    if(aircraft->DBrain()->GetCurrentMode()==DigitalBrain::BVREngageMode && (tactic==DigitalBrain::BvrPump || tactic==DigitalBrain::BvrFlyFormation)) bvr=true;
                    auto* radar=dynamic_cast<RadarClass*>(FindSensor(aircraft,SensorClass::Radar));
                    if(!radar || !radar->CurrentTarget()) continue;
                    bool confirmed=false;
                    for(auto* contact=aircraft->targetList;contact;contact=contact->next)
                        if(contact->BaseData()==radar->CurrentTarget()->BaseData() && radar->IsOn() && radar->IsEmitting() && contact->localData->sensorState[SensorClass::Radar]>=SensorClass::SensorTrack) confirmed=true;
                    if(confirmed) tracked=true;else unconfirmed=true;
                }
                if(bvr && tracked && unconfirmed) airBvrSnapshot=DetailedSnapshotJson();
            }
            if(weaponOrderCase) {
                VuListIterator candidates(SimDriver.objectList);
                for(auto* entity=candidates.GetFirst();entity;entity=candidates.GetNext()) {
                    auto* aircraft=dynamic_cast<AircraftClass*>(entity);
                    if(!aircraft || (aircraft->GetCampaignObject()!=a && aircraft->GetCampaignObject()!=b)) continue;
                    if(RunWeaponsFireGateDiagnostic(aircraft)) {
                        VuBin<AircraftClass> heldAircraft(aircraft);
                        std::set<VU_ID> priorMissiles;
                        auto launchedMissile=[&]() {
                            VU_ID found=FalconNullId;
                            VuListIterator objects(SimDriver.objectList);
                            for(auto* object=objects.GetFirst();object;object=objects.GetNext()) {
                                auto* missile=dynamic_cast<MissileClass*>(object);
                                if(missile && missile->Parent()==aircraft && !priorMissiles.count(missile->Id())) found=missile->Id();
                            }
                            return found;
                        };
                        VuListIterator existing(SimDriver.objectList);
                        for(auto* object=existing.GetFirst();object;object=existing.GetNext())
                            if(static_cast<SimBaseClass*>(object)->IsMissile()) priorMissiles.insert(object->Id());
                        if(QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"weapons_hold")!="applied")
                            throw std::runtime_error("Eligible aircraft refused physical hold test");
                        for(int frame=0;frame<250;++frame) {
                            AdvanceDetailedFrame();
                            if(!aircraft->DBrain()->IsOperatorWeaponsHold() || aircraft->FCC->releaseConsent || aircraft->fireGun)
                                throw std::runtime_error("Operator hold lost or native weapon release permitted");
                            if(launchedMissile()!=FalconNullId) {
                                fprintf(stderr,"[weapons-hold-failure] frame=%d order=%d mode=%d timer=%lu now=%lu\n",frame,aircraft->DBrain()->GetWeaponsAction(),int(aircraft->DBrain()->GetCurrentMode()),aircraft->DBrain()->GetMissileShotTime(),SimLibElapsedTime);
                                throw std::runtime_error("Held aircraft launched a new native missile");
                            }
                        }
                        if(QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"weapons_free")!="applied")
                            throw std::runtime_error("Held aircraft refused physical free test");
                        if(aircraft->DBrain()->IsOperatorWeaponsHold())
                            throw std::runtime_error("Explicit free did not clear operator hold");
                        VU_ID resumed=FalconNullId;unsigned resumeFrames=0;
                        // Hold also commands rejoin: allow a full turn/reacquisition,
                        // not just the immediate firing opportunity before the hold.
                        for(;resumeFrames<6000;++resumeFrames) {
                            AdvanceDetailedFrame();resumed=launchedMissile();
                            if(resumeFrames%250==0) {
                                auto* target=aircraft->targetPtr;
                                fprintf(stderr,"[weapons-fcc] mode=%d release=%d postdrop=%d weapon_class=%d\n",int(aircraft->FCC->GetMasterMode()),int(aircraft->FCC->releaseConsent),int(aircraft->FCC->postDrop),int(aircraft->Sms->curWeaponClass));
                                fprintf(stderr,"[weapons-resume] frame=%u actor=%lu mode=%d order=%d timer=%lu now=%lu fcc_range=%d min=%.1f max=%.1f target=%lu range=%.1f radar=%d\n",resumeFrames,aircraft->Id().num_,int(aircraft->DBrain()->GetCurrentMode()),aircraft->DBrain()->GetWeaponsAction(),aircraft->DBrain()->GetMissileShotTime(),SimLibElapsedTime,aircraft->FCC->inRange,aircraft->FCC->missileRMin,aircraft->FCC->missileRMax,target?target->BaseData()->Id().num_:0,target?target->localData->range:0,target?target->localData->sensorState[SensorClass::Radar]:-1);
                            }
                            if(resumed!=FalconNullId) break;
                        }
                        if(resumed==FalconNullId) throw std::runtime_error("Freed aircraft did not launch through native AI and SMS");
                        SetDetailedRegion(false,x,y,20);
                        std::cout<<"{\"status\":\"ok\",\"case\":\"weapons-fire-gate\",\"native_gate_accepted_before\":true,\"held\":true,\"native_gate_accepted_after\":true,\"hold_frames\":250,\"resume_frames\":"<<resumeFrames+1<<",\"resumed_missile\":\""<<resumed.creator_<<":"<<resumed.num_<<"\"}"<<std::endl;
                        return;
                    }
                }
            }
            if(radarTrackCase) {
                VuListIterator candidates(SimDriver.objectList);
                for(auto* e=candidates.GetFirst();e;e=candidates.GetNext()) {
                    auto* aircraft=dynamic_cast<AircraftClass*>(e);
                    if(!aircraft || (aircraft->GetCampaignObject()!=a && aircraft->GetCampaignObject()!=b)) continue;
                    auto* radar=dynamic_cast<RadarClass*>(FindSensor(aircraft,SensorClass::Radar));
                    if(!radar || !radar->IsOn() || !radar->IsEmitting() || !radar->CurrentTarget()) continue;
                    bool currentObservation=false;
                    for(auto* contact=aircraft->targetList;contact;contact=contact->next)
                        if(contact->BaseData()==radar->CurrentTarget()->BaseData() &&
                           contact->localData->sensorState[SensorClass::Radar]>=SensorClass::SensorTrack &&
                           contact->localData->sensorLoopCount[SensorClass::Radar]==SimLibElapsedTime) currentObservation=true;
                    if(!currentObservation) continue;
                    VuBin<AircraftClass> held(aircraft);
                    const auto tracked=radar->CurrentTarget()->BaseData()->Id();
                    const auto result=QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"radar_off");
                    if(result!="applied") throw std::runtime_error("Tracking radar did not accept power-off");
                    for(int frame=0;frame<=250;++frame) {
                        if(radar->IsOn() || radar->IsEmitting() || radar->CurrentTarget() || aircraft->RdrRng()!=0) {
                            fprintf(stderr,"[radar-power-track] frame=%d power=%d emitting=%d target=%d range=%.1f\n",frame,radar->IsOn(),radar->IsEmitting(),radar->CurrentTarget()!=nullptr,aircraft->RdrRng());
                            throw std::runtime_error("Powered-off radar retained or reacquired a tracking target");
                        }
                        if(frame<250) AdvanceDetailedFrame();
                    }
                    if(QueueDetailedAction(aircraft->Id().creator_,aircraft->Id().num_,"radar_on")!="applied")
                        throw std::runtime_error("Tracking radar did not accept power-on");
                    const auto powerOnTime=SimLibElapsedTime;
                    unsigned reacquireFrames=0;
                    for(;reacquireFrames<1500;++reacquireFrames) {
                        AdvanceDetailedFrame();
                        bool freshTrack=false;
                        if(radar->IsOn() && radar->IsEmitting()) {
                            // RadarDigi copies track state to lockedTarget, but
                            // observation times belong to the scanned list node.
                            for(auto* contact=aircraft->targetList;contact;contact=contact->next)
                                if(contact->BaseData()->Id()==tracked &&
                                   contact->localData->sensorState[SensorClass::Radar]>=SensorClass::SensorTrack &&
                                   contact->localData->sensorLoopCount[SensorClass::Radar]>powerOnTime) freshTrack=true;
                        }
                        if(freshTrack) break;
                        if(reacquireFrames%250==0) {
                            fprintf(stderr,"[radar-reacquire] frame=%u actor=%lu alive=%d awake=%d power=%d emission=%d target=%lu mode=%d\n",reacquireFrames,aircraft->Id().num_,!aircraft->IsDead(),aircraft->IsAwake(),radar->IsOn(),radar->IsEmitting(),radar->CurrentTarget()?radar->CurrentTarget()->BaseData()->Id().num_:0,int(aircraft->DBrain()->GetCurrentMode()));
                            for(auto* contact=aircraft->targetList;contact;contact=contact->next)
                                fprintf(stderr,"[radar-contact] id=%lu range=%.0f ata=%.1f state=%d observed=%lu power_on=%lu\n",contact->BaseData()->Id().num_,contact->localData->range,contact->localData->ata*RTD,contact->localData->sensorState[SensorClass::Radar],contact->localData->sensorLoopCount[SensorClass::Radar],powerOnTime);
                        }
                    }
                    if(reacquireFrames==1500) throw std::runtime_error("Powered-on radar did not freshly observe its original tracked target");
                    if(!radar->CurrentTarget()) throw std::runtime_error("No desired radar target available for release assertion");
                    radar->SetDesiredTarget(nullptr);
                    if(radar->CurrentTarget() || !radar->IsOn() || !radar->IsEmitting())
                        throw std::runtime_error("Powered-on radar did not release its desired target without shutting down");
                    SetDetailedRegion(false,x,y,20);
                    std::cout<<"{\"status\":\"ok\",\"case\":\"radar-power-track\",\"tracked_target\":\""<<tracked.creator_<<":"<<tracked.num_<<"\",\"off_frames\":250,\"reacquire_frames\":"<<reacquireFrames+1<<",\"same_target_reobserved\":true,\"fresh_observation\":true,\"desired_target_released\":true,\"track_cleared\":true}"<<std::endl;
                    return;
                }
            }
            if(drainTest && ff_headless::combat.detailedDamageMessages) {
                VuListIterator projectiles(SimDriver.objectList);
                for(auto* e=projectiles.GetFirst();e;e=projectiles.GetNext()) {
                    auto* actor=static_cast<SimBaseClass*>(e);
                    if(actor->IsMissile() && !actor->IsDead() && !actor->IsExploding() && !actor->IsSetRemoveFlag()) {
                        exitMissile=VuBin<SimBaseClass>(actor);break;
                    }
                }
                if(exitMissile.get()) break;
            }
            if(airCombat) {
                VuListIterator actors(SimDriver.objectList);
                for(auto* e=actors.GetFirst();e;e=actors.GetNext())
                    if(static_cast<SimBaseClass*>(e)->IsAirplane()) ++aircraftFrames;
            }
            if((tick+1)%250==0) {
                DoCampaignLoop(0);UpdateRealUnits(5000);
                TheCampaign.vuThread->Update(-1);gMainThread->Update(-1);ReconcileDetailedRegion();
                fprintf(stderr,"[detailed] region t=%.1f fires=%llu damage=%llu aircraft_frames=%u\n",(tick+1)*.02,ff_headless::combat.weaponsFired,ff_headless::combat.detailedDamageMessages,aircraftFrames);
                if(airCombat) for(auto* flight:{a,b}) {
                    fprintf(stderr,"[air-boundary] t=%.1f parent=%lu aggregate=%d xy=%.3f,%.3f center=%.3f,%.3f components=",(tick+1)*.02,flight->Id().num_,flight->IsAggregate(),flight->YPos()/3280.839895,flight->XPos()/3280.839895,x,y);
                    if(flight->GetComponents()) {
                        VuListIterator parts(flight->GetComponents());
                        for(auto* part=parts.GetFirst();part;part=parts.GetNext()) fprintf(stderr,"%lu,",part->Id().num_);
                    }
                    fprintf(stderr,"\n");
                }
                if(airCombat && tick<3000) {
                    VuListIterator actors(SimDriver.objectList);
                    for(auto* e=actors.GetFirst();e;e=actors.GetNext()) {
                        auto* actor=static_cast<SimBaseClass*>(e);if(!actor->IsAirplane()) continue;
                        auto* ac=static_cast<AircraftClass*>(actor);int targets=0;
                        for(auto* t=ac->targetList;t;t=t->next) ++targets;
                        if(ac->GetCampaignObject()==a || ac->GetCampaignObject()==b) {
                            const auto* td=ac->targetPtr?ac->targetPtr->localData:nullptr;
                            fprintf(stderr,"[detailed] engagement id=%lu target=%lu range=%.0f ata=%.1f masterarm=%d station=%d weapon=%d release=%d\n",ac->Id().num_,ac->targetPtr?ac->targetPtr->BaseData()->Id().num_:0,td?td->range:0,td?td->ata*RTD:0,int(ac->Sms->MasterArm()),ac->Sms->CurHardpoint(),ac->Sms->GetCurrentWeapon()?ac->Sms->GetCurrentWeapon()->Type():0,int(ac->FCC->releaseConsent));
                        }
                        fprintf(stderr,"[detailed] ai id=%lu mode=%d ap=%d targets=%d selected=%d pilot=%d fuel=%.1f ground=%d wp=%d next=%lu now=%lu xyz=%.0f,%.0f,%.0f\n",ac->Id().num_,int(ac->DBrain()->GetCurrentMode()),int(ac->AutopilotType()),targets,ac->targetPtr!=nullptr,ac->HasPilot(),ac->af->Fuel(),ac->OnGround(),ac->curWaypoint?ac->curWaypoint->GetWPAction():-1,ac->nextTargetUpdate,SimLibElapsedTime,ac->XPos(),ac->YPos(),ac->ZPos());
                    }
                }
            }
        }
        fprintf(stderr,"[detailed] naval weapon events=%llu target_frames=%u\n",ff_headless::combat.navalWeaponsFired,navalTargetFrames);
        if(navalCombat) {
            fprintf(stderr,"[detailed] unique naval missiles=%zu firing_events=%llu end_events=%llu\n",navalMissiles.size(),ff_headless::combat.navalWeaponsFired,ff_headless::combat.missileEnds);
            for(int code=0;code<12;++code) if(ff_headless::combat.missileEndCodes[code])
                fprintf(stderr,"[detailed] naval missile end code=%d count=%llu\n",code,ff_headless::combat.missileEndCodes[code]);
            VuListIterator ships(SimDriver.objectList);
            for(auto* e=ships.GetFirst();e;e=ships.GetNext()) {
                auto* ship=dynamic_cast<GroundClass*>(e);if(!ship || !ship->isShip) continue;
                auto* ai=ship->gai;auto* target=ai->GetGroundTargetPtr();
                fprintf(stderr,"[detailed] naval actor=%lu commander=%d ground_target=%lu last_process=%lu now=%lu\n",ship->Id().num_,ai==ai->battalionCommand,target?target->BaseData()->Id().num_:0,ship->lastProcess,SimLibElapsedTime);
            }
            for(auto* unit:{a,b}) {
                auto* other=unit==a?b:a;float range=0;
                int detected=Detected(unit,other,&range);
                fprintf(stderr,"[detailed] naval detection parent=%lu target=%lu flags=%d range=%.3f hit=%d own_move=%d other_move=%d selected=%lu\n",
                    unit->Id().num_,other->Id().num_,detected,range,unit->GetAproxHitChance(other->GetMovementType(),int(range/2)),
                    int(unit->GetMovementType()),int(other->GetMovementType()),unit->GetTarget()?unit->GetTarget()->Id().num_:0);
            }
            std::cerr<<"[naval-snapshot] "<<DetailedSnapshotJson()<<std::endl;
        }
        if(heloCombat) std::cerr<<"[helo-snapshot] "<<DetailedSnapshotJson()<<std::endl;
        if(heloCombat && !heloTargetFrames) throw std::runtime_error("Helicopter did not acquire a native component target");
        if(rocketCase && helicopterProjectiles.empty()) throw std::runtime_error("Native helicopter AI launched no rocket or missile");
        if(rocketCase && helicopterProjectiles.size()!=helicopterRocketRounds) throw std::runtime_error("Helicopter rocket projectiles did not match the initial launcher inventory");
        if(heloCombat && !ff_headless::combat.helicopterWeaponsFired) throw std::runtime_error("Native helicopter AI acquired a component target but did not fire a weapon");
        if(heloCombat && !ff_headless::combat.helicopterDamageDealt) throw std::runtime_error("Native helicopter AI fired without damaging any target");
        if(navalCombat && !ff_headless::combat.navalWeaponsFired) throw std::runtime_error("Native naval AI did not fire a weapon");
        if(navalCombat && (!navalGuidedFrames || !visualRangeChecked)) throw std::runtime_error("Naval visual guidance was not exercised");
        if(weaponOrderCase) throw std::runtime_error("No native firing-eligible aircraft for weapons order comparison");
        if(radarTrackCase) throw std::runtime_error("No native radar track acquired during diagnostic");
        if(navalCombat && (navalFiringTeams.size()!=2 || !navalRadarFrames)) throw std::runtime_error("Naval radar acquisition and bilateral firing were not exercised");
        if(!ff_headless::combat.weaponsFired || !ff_headless::combat.detailedDamageMessages) {
            for(int code=0;code<12;++code) if(ff_headless::combat.missileEndCodes[code])
                fprintf(stderr,"[detailed] missile end code=%d count=%llu\n",code,ff_headless::combat.missileEndCodes[code]);
            std::cerr<<"[failed-combat-snapshot] "<<DetailedSnapshotJson()<<std::endl;
            throw std::runtime_error(!ff_headless::combat.weaponsFired ? "Regional AI produced no native weapons" : "Regional AI fired weapons but produced no native damage");
        }
        if(airCombat && !aircraftFrames) throw std::runtime_error("No native aircraft executed in mixed combat region");
        if(airCombat && !ff_headless::combat.aircraftWeaponsFired) throw std::runtime_error("Native aircraft AI did not fire a weapon");
        if(drainTest && !exitMissile.get()) throw std::runtime_error("No live AI-fired missile at regional exit");
        if(navalCombat && (navalFlightSnapshot.empty() || navalDamageSnapshot.empty())) throw std::runtime_error("Missing naval rendering checkpoints");
        const auto snapshot=DetailedSnapshotJson();
        const auto endsBefore=ff_headless::combat.missileEnds;
        SetDetailedRegion(false,x,y,airCombat?20:10);
        const auto exitSnapshot=DetailedSnapshotJson();
        if(drainTest && (!DetailedRegionActive() || !exitMissile->IsAwake() || exitMissile->IsSetRemoveFlag()))
            throw std::runtime_error("Regional exit removed a live missile before native termination");
        int drainFrames=0;
        unsigned pilotFrames=0;
        // High-altitude ejected pilots can need over twenty minutes to land.
        // Their lifecycle continues after combat parents reaggregate.
        while(DetailedRegionActive() && drainFrames<90000) {
            AdvanceDetailedFrame(); ++drainFrames;
            VuListIterator pilots(SimDriver.objectList);
            for(auto* e=pilots.GetFirst();e;e=pilots.GetNext()) {
                auto* actor=static_cast<SimBaseClass*>(e);
                if(actor->IsEject() && actor->IsAwake() && !actor->IsDead()) {
                    ++pilotFrames;
                    if(!std::isfinite(actor->XPos()) || !std::isfinite(actor->YPos()) || !std::isfinite(actor->ZPos()))
                        throw std::runtime_error("Non-finite ejected pilot while draining");
                }
            }
            if(drainFrames%250==0) {
                DoCampaignLoop(0);UpdateRealUnits(5000);
                TheCampaign.vuThread->Update(-1);gMainThread->Update(-1);
            }
        }
        if(DetailedRegionActive()) throw std::runtime_error("Regional weapons/pilots failed to drain in thirty minutes");
        if(drainTest && (!drainFrames || ff_headless::combat.missileEnds<=endsBefore ||
            (!exitMissile->IsDead() && !exitMissile->IsExploding() && !exitMissile->IsSetRemoveFlag())))
            throw std::runtime_error("Exit missile did not terminate through native processing");
        const auto finalSnapshot=DetailedSnapshotJson();
        const auto damageAfter=ff_headless::combat.detailedDamageMessages;
        // Repeated boundary reconciliation must not apply combat losses again.
        std::map<VU_ID,int> finalCounts;
        for(const auto& pair:initialUnits) finalCounts[pair.first]=pair.second.first->GetTotalVehicles();
        for(int repeat=0;repeat<3;++repeat) { ReconcileDetailedRegion(); DispatchDetailedDamage(); }
        if(ff_headless::combat.detailedDamageMessages!=damageAfter) throw std::runtime_error("Damage replay after drain");
        int losses=0,damagedUnits=0;
        for(const auto& pair:initialUnits) {
            auto* u=pair.second.first.get();
            if(!u->IsAggregate()) throw std::runtime_error("Native unit not reaggregated on exit");
            if(u->GetTotalVehicles()!=finalCounts[pair.first]) throw std::runtime_error("Campaign losses replayed on reconciliation");
            const int lost=pair.second.second-u->GetTotalVehicles();
            if(lost>0) {losses+=lost; ++damagedUnits;}
        }
        if(!losses) throw std::runtime_error("Regional combat losses did not persist in campaign units");
        unsigned helicopterEnds=0;
        unsigned helicopterProjectileDamage=0;
        std::ostringstream impactAudits;
        impactAudits<<"[";bool firstImpact=true;
        for(const auto& projectile:helicopterProjectiles) {
            const auto& audit=ff_headless::combat.projectileImpactAudit[{projectile.creator_,projectile.num_}];
            if(!firstImpact) impactAudits<<",";firstImpact=false;
            impactAudits<<"{\"id\":\""<<projectile.creator_<<":"<<projectile.num_<<"\",\"observed\":"<<(audit.observed?"true":"false")
                <<",\"ground_impact\":"<<(audit.groundImpact?"true":"false")
                <<",\"time_ms\":"<<audit.time<<",\"target_id\":\""<<audit.targetCreator<<":"<<audit.targetNumber<<"\""
                <<",\"target_known\":"<<(audit.targetKnown?"true":"false")<<",\"target_retired\":"<<(audit.targetRetired?"true":"false")
                <<",\"target_strength\":"<<audit.targetStrength<<",\"target_range_squared_ft\":"<<audit.targetRangeSquared
                <<",\"lethal_radius_squared_ft\":"<<audit.lethalRadiusSquared
                <<",\"live_damage_requests\":"<<audit.liveDamageRequests<<",\"retired_damage_requests\":"<<audit.retiredDamageRequests
                <<",\"live_nearby_objects\":"<<audit.liveNearbyObjects
                <<",\"damage_events\":"<<ff_headless::combat.projectileDamage[{projectile.creator_,projectile.num_}]<<"}";
            const auto count=ff_headless::combat.projectileEnds[{projectile.creator_,projectile.num_}];
            fprintf(stderr,"[helicopter-projectile-end] id=%lu:%lu events=%u\n",static_cast<unsigned long>(projectile.creator_),projectile.num_,count);
            helicopterEnds+=count;
            helicopterProjectileDamage+=ff_headless::combat.projectileDamage[{projectile.creator_,projectile.num_}];
            if(helicopterProjectileTargets.count(projectile)) {
                const auto& target=helicopterProjectileTargets[projectile];
                const auto& end=ff_headless::combat.projectileEndState[{projectile.creator_,projectile.num_}];
                fprintf(stderr,"[rocket-end] id=%lu code=%.0f delta_from_launch_target=%.1f,%.1f,%.1f\n",projectile.num_,end[3],end[0]-target[0],end[1]-target[1],end[2]-target[2]);
            }
            if(count!=1) throw std::runtime_error("Helicopter projectile did not produce exactly one terminal event");
        }
        impactAudits<<"]";
        std::cout<<"{\"status\":\"ok\",\"case\":"<<ff::headless::JsonString(testCase)<<",\"campaign_losses\":"<<losses<<",\"damaged_units\":"<<damagedUnits
                 <<",\"naval_missiles\":"<<navalMissiles.size()<<",\"naval_guided_frames\":"<<navalGuidedFrames<<",\"visual_range_checked\":"<<(visualRangeChecked?"true":"false")
                 <<",\"naval_firing_teams\":"<<navalFiringTeams.size()<<",\"naval_radar_frames\":"<<navalRadarFrames
                 <<",\"helicopter_target_frames\":"<<heloTargetFrames<<",\"helicopter_weapon_events\":"<<ff_headless::combat.helicopterWeaponsFired
                 <<",\"helicopter_damage_dealt\":"<<ff_headless::combat.helicopterDamageDealt
                 <<",\"helicopter_projectiles\":"<<helicopterProjectiles.size()
                 <<",\"helicopter_projectile_ends\":"<<helicopterEnds
                 <<",\"helicopter_projectile_damage\":"<<helicopterProjectileDamage
                 <<",\"helicopter_impact_audit\":"<<impactAudits.str()
                 <<",\"helicopter_rocket_rounds\":"<<helicopterRocketRounds
                 <<",\"rocket_only_empty_gun_fixture\":"<<(testCase=="helo-rockets-only"?"true":"false")
                 <<",\"helicopter_fire_detail\":"<<(helicopterFireSnapshot.empty()?"null":helicopterFireSnapshot)
                 <<",\"helicopter_damage_detail\":"<<(helicopterDamageSnapshot.empty()?"null":helicopterDamageSnapshot)
                 <<",\"helicopter_preempted_detail\":"<<(helicopterPreemptedSnapshot.empty()?"null":helicopterPreemptedSnapshot)
                 <<",\"naval_region\":{\"x\":"<<x<<",\"y\":"<<y<<",\"radius_km\":"<<(airCombat?20:10)<<",\"active\":true}"
                 <<",\"air_bvr_detail\":"<<(airBvrSnapshot.empty()?"null":airBvrSnapshot)
                 <<",\"naval_flight_detail\":"<<(navalFlightSnapshot.empty()?"null":navalFlightSnapshot)
                 <<",\"naval_damage_detail\":"<<(navalDamageSnapshot.empty()?"null":navalDamageSnapshot)
                 <<",\"aircraft_frames\":"<<aircraftFrames<<",\"pilot_frames\":"<<pilotFrames<<",\"drain_frames\":"<<drainFrames<<",\"reaggregated\":true,\"exit_detail\":"<<exitSnapshot<<",\"final_detail\":"<<finalSnapshot<<",\"detail\":"<<snapshot<<"}"<<std::endl;
        return;
    }
    if (pursuitCase || testCase == "ground" || (testCase == "air" || ((testCase == "rocket-legacy-pod" || testCase == "rocket-hold") || testCase == "ground-retarget") || (testCase == "bomb-hold" || (testCase == "bomb-flight" || testCase == "bomb-ground-hit" || testCase == "bomb-ground-miss"))) || heloCase || seaCase) {
        Unit ground = nullptr;
        for (auto* e = it.GetFirst(); e; e = it.GetNext()) {
            auto* u = static_cast<Unit>(e);
            if(testCase=="rocket-hold" || testCase=="rocket-legacy-pod") {
                if(!u->IsFlight()) continue;
                auto* flight=static_cast<FlightClass*>(u);
                bool suitable=flight->GetLoadouts()>0;
                for(int slot=0;slot<flight->GetLoadouts();++slot) {
                    bool rockets=false;
                    const auto& loadout=flight->GetLoadout()[slot];
                    for(int hp=0;hp<HARDPOINT_MAX;++hp) {
                        int weapon=loadout.WeaponID[hp];
                        if(weapon<=0 || weapon>=NumWeaponTypes || !loadout.WeaponCount[hp]) continue;
                        int index=WeaponDataTable[weapon].Index;
                        if(index>=0 && index<NumEntities) {
                            const auto& entry=Falcon4ClassTable[index];
                            if(testCase=="rocket-legacy-pod") {
                                if(entry.vuClassData.classInfo_[VU_TYPE]==TYPE_ROCKET && SimWeaponDataTable[entry.vehicleDataIndex].weaponClass==wcRocketWpn) rockets=true;
                            } else if(entry.vuClassData.classInfo_[VU_TYPE]==TYPE_LAUNCHER && entry.vuClassData.classInfo_[VU_STYPE]==STYPE_ROCKET) rockets=true;
                        }
                    }
                    if(!rockets) suitable=false;
                }
                if(!suitable) continue;
            }
            if(testCase=="bomb-hold" || testCase=="bomb-flight" || testCase=="bomb-ground-hit" || testCase=="bomb-ground-miss") {
                if(!u->IsFlight()) continue;
                auto* flight=static_cast<FlightClass*>(u);
                bool suitable=flight->GetLoadouts()>0;
                for(int slot=0;slot<flight->GetLoadouts();++slot) {
                    int firstBomb=0,count=0;
                    const auto& loadout=flight->GetLoadout()[slot];
                    for(int hp=0;hp<HARDPOINT_MAX;++hp) {
                        const int weapon=loadout.WeaponID[hp];
                        if(weapon<=0 || weapon>=NumWeaponTypes || !loadout.WeaponCount[hp]) continue;
                        const int index=WeaponDataTable[weapon].Index;
                        if(index<0 || index>=NumEntities) continue;
                        const auto* info=Falcon4ClassTable[index].vuClassData.classInfo_;
                        if(info[VU_TYPE]!=TYPE_BOMB) continue;
                        if(!firstBomb) firstBomb=weapon;
                        if(weapon==firstBomb && (info[VU_STYPE]==STYPE_BOMB || info[VU_STYPE]==STYPE_BOMB_IRON)) count+=loadout.WeaponCount[hp];
                    }
                    if(count<4) suitable=false;
                }
                if(!suitable) continue;
            }
            if(testCase=="gun-lifetime") {
                auto* vehicle=GetVehicleClassData(u->GetVehicleID(0));
                // Reuse the loaded UH-60L tracer gun used by helo-combat.
                if(!vehicle || std::string(vehicle->Name)!="UH-60L") continue;
            }
            if ((testCase == "ground" ? u->IsBattalion() : seaCase ? u->IsTaskForce() : (u->IsFlight() && (testCase=="gun-lifetime" || u->ZPos() < -5000) && (Falcon4ClassTable[u->GetVehicleID(0)].vuClassData.classInfo_[VU_TYPE] == TYPE_HELICOPTER) == heloCase)) && u->GetTotalVehicles() > (pursuitCase?1:0) && (testCase!="bvr-aggregate" || u->GetTotalVehicles()==2)) { ground = u; break; }
        }
        if (!ground) throw std::runtime_error(testCase=="rocket-legacy-pod" ? "No native flight with legacy rocket pods in this save" : "No native battalion");
        if(routeCase) {
            GridIndex x,y;ground->GetLocation(&x,&y);
            // A native waypoint order defines the fixture. Original helicopter
            // flight AI or naval ground-AI branches supply all motion.
            ground->DisposeWayPoints();
            const int altitude=seaCase?0:static_cast<int>(-OTWDriver.GetGroundLevel(ground->XPos(),ground->YPos())+1000);
            ground->AddUnitWP(x,y,altitude,100,TheCampaign.CurrentTime,0,WP_NOTHING);
            ground->AddUnitWP(x+5,y,altitude,100,TheCampaign.CurrentTime+5*CampaignMinutes,0,WP_NOTHING);
            ground->SetCurrentWaypoint(2);
        }
        const int initial = ground->GetTotalVehicles();
        if (!ground->Deaggregate(FalconLocalSession)) throw std::runtime_error("Native deaggregation refused");
        if (!ground->Wake()) throw std::runtime_error("Native battalion did not wake");
        int count = 0;
        float routeX=0,routeY=0,routeZ=0;
        if(routeCase) ground->GetCurrentUnitWP()->GetLocation(&routeX,&routeY,&routeZ);
        std::map<SimBaseClass*,std::array<float,3>> starts;
        {
        VuListIterator check(ground->GetComponents());
        for (auto* e = check.GetFirst(); e; e = check.GetNext()) {
            auto* v = static_cast<SimBaseClass*>(e);
            if (!v->IsAwake() || !v->drawPointer || (testCase == "ground" && (!static_cast<GroundClass*>(v)->gai || !static_cast<GroundClass*>(v)->Sms)))
                throw std::runtime_error("Incomplete native ground actor");
            if(dynamic_cast<GroundClass*>(v)) {
                const float tableHealth=GetVehicleClassData(v->Type()-VU_LAST_ENTITY_TYPE)->HitPoints;
                if(tableHealth<=0 || v->Strength()!=tableHealth || v->MaxStrength()!=tableHealth || v->pctStrength!=1.0f)
                    throw std::runtime_error("Fresh ground/naval health differs from original vehicle hit points");
            }
            if ((testCase == "air" || ((testCase == "rocket-legacy-pod" || testCase == "rocket-hold") || testCase == "ground-retarget") || (testCase == "bomb-hold" || (testCase == "bomb-flight" || testCase == "bomb-ground-hit" || testCase == "bomb-ground-miss")))) {
                auto* ac = static_cast<AircraftClass*>(v);
                if (!ac->af || !ac->DBrain() || !ac->Sms || !ac->FCC || !ac->IsDigital())
                    throw std::runtime_error("Incomplete native aircraft");
            }
            if (heloCase) {
                auto* h = static_cast<HelicopterClass*>(v);
                if (!h->hf || !h->Sms || !h->FCC || !h->hBrain) throw std::runtime_error("Incomplete native helicopter");
                if(testCase=="helo-route") {
                    float x,y,z;h->curWaypoint->GetLocation(&x,&y,&z);
                    if(x!=routeX || y!=routeY || z!=routeZ) throw std::runtime_error("Helicopter initialization skipped the commanded waypoint");
                }
            }
            if(seaCase) {
                auto* ship=dynamic_cast<GroundClass*>(v);
                if(!ship || !ship->gai || !ship->Sms || v->GetDomain()!=DOMAIN_SEA)
                    throw std::runtime_error("Incomplete native ship AI/stores");
                if(testCase=="sea-docked" && (!(ship->gai->moveFlags & GNDAI_MOVE_FIXED_POSITIONS) || ship->gai->moveState!=GNDAI_MOVE_HALTED))
                    throw std::runtime_error("Docked fixture requires native port fixed positions");
                fprintf(stderr,"[detailed] ship initialized movement=%d fixed=%d\n",int(ship->gai->moveState),int((ship->gai->moveFlags & GNDAI_MOVE_FIXED_POSITIONS)!=0));
            }
            starts[v] = {v->XPos(),v->YPos(),v->ZPos()};
            ++count;
        }
        } // Release the collection lock before diagnostics can reaggregate it.
        if (count != initial) throw std::runtime_error("Deaggregation count mismatch");
        fprintf(stderr, "[detailed] deaggregated and woke %d vehicles\n", count);
        if(pursuitCase) {
            auto actors=starts.begin();auto* aircraft=static_cast<AircraftClass*>(actors++->first);
            auto* target=static_cast<AircraftClass*>(actors->first);
            // Pointer-map order varies between processes. These controller
            // fixtures need the flight lead; a held wingman may legitimately
            // refuse a new target while requesting engagement permission.
            if(aircraft->DBrain()->isWing) std::swap(aircraft,target);
            if(aircraft->DBrain()->isWing || !target->DBrain()->isWing)
                throw std::runtime_error("Pursuit fixture requires a native lead and wingman");
            fprintf(stderr,"[bvr-fixture-role] lead=%lu wingman=%lu\n",aircraft->Id().num_,target->Id().num_);
            if(testCase=="bvr-aggregate") RunBvrAggregateDiagnostic(aircraft);
            const unsigned refreshes=testCase=="bvr-aggregate"?0:RunBvrPursuitDiagnostic(aircraft,target);
            if(!ground->Reaggregate(FalconLocalSession) || ground->GetTotalVehicles()!=initial)
                throw std::runtime_error("Pursuit fixture did not reaggregate intact");
            if(testCase=="bvr-aggregate")
                std::cout<<"{\"status\":\"ok\",\"case\":\"bvr-aggregate\",\"own_aircraft\":2,\"larger_opponent\":3,\"equal_opponent\":2,\"profiles_checked\":2,\"reaggregated\":true}"<<std::endl;
            else
                std::cout<<"{\"status\":\"ok\",\"case\":\"bvr-pursuit\",\"refreshes\":"<<refreshes<<",\"seconds\":8,\"reaggregated\":true}"<<std::endl;
            return;
        }
        if(testCase=="bomb-ground-hit" || testCase=="bomb-ground-miss") { RunBombDamageDiagnostic(static_cast<AircraftClass*>(starts.begin()->first),testCase=="bomb-ground-hit");return; }
        if(testCase=="rocket-legacy-pod") { RunLegacyRocketDiagnostic(static_cast<AircraftClass*>(starts.begin()->first));return; }
        if(testCase=="rocket-hold") { RunRocketHoldDiagnostic(static_cast<AircraftClass*>(starts.begin()->first));return; }
        if(testCase=="bomb-hold" || testCase=="bomb-flight") { RunBombHoldDiagnostic(static_cast<AircraftClass*>(starts.begin()->first),testCase=="bomb-flight");return; }
        if(testCase=="ground-retarget") {
            RunGroundRetargetDiagnostic(static_cast<AircraftClass*>(starts.begin()->first));
            if(!ground->Reaggregate(FalconLocalSession)) throw std::runtime_error("Retarget fixture failed to reaggregate");
            std::cout<<"{\"status\":\"ok\",\"case\":\"ground-retarget\",\"cycles\":128,\"retained_refs\":1,\"reaggregated\":true,\"actor_id\":\""<<groundRetargetActor<<"\",\"assigned_detail\":"<<groundRetargetAssignedSnapshot<<",\"cleared_detail\":"<<groundRetargetClearedSnapshot<<"}"<<std::endl;
            return;
        }
        unsigned courseReversals=0;
        if(testCase=="gun-lifetime") {
            RunTracerLifetimeDiagnostic(static_cast<HelicopterClass*>(starts.begin()->first)->Guns);
            if(!ground->Reaggregate(FalconLocalSession) || ground->GetTotalVehicles()!=initial)
                throw std::runtime_error("Tracer fixture failed to reaggregate");
            std::cout<<"{\"status\":\"ok\",\"case\":\"gun-lifetime\",\"frame_intervals\":3,\"reaggregated\":true}"<<std::endl;
            return;
        }
        std::map<SimBaseClass*,bool> reversed;
        for (int tick = 0; tick < (testCase=="sea-route" || testCase=="sea-docked"?30000:routeCase?3000:500); ++tick) {
            AdvanceDetailedFrame();
            if(testCase=="sea-route") for(const auto& pair:starts) {
                auto* actor=pair.first;
                const double forward=(routeX-pair.second[0])*actor->XDelta()+(routeY-pair.second[1])*actor->YDelta();
                if(forward < -100 && !reversed[actor]) { reversed[actor]=true; ++courseReversals; }
                if(!std::isfinite(actor->XPos()) || !std::isfinite(actor->YPos()) || !std::isfinite(actor->ZPos()))
                    throw std::runtime_error("Non-finite ship state along route");
            }
        }
        if(testCase=="sea-route") for(const auto& pair:starts) {
            auto* actor=static_cast<GroundClass*>(pair.first);
            fprintf(stderr,"[detailed] ship endpoint delta=%.1f,%.1f remaining=%.1f velocity=%.1f,%.1f reversed=%d waypoint=%d move_state=%d maxvel=%.1f\n",
                actor->XPos()-pair.second[0],actor->YPos()-pair.second[1],std::hypot(routeX-actor->XPos(),routeY-actor->YPos()),
                actor->XDelta(),actor->YDelta(),int(reversed[actor]),actor->curWaypoint==actor->waypoint,int(actor->gai->moveState),actor->gai->maxvel);
        }
        if(testCase=="sea-route" && courseReversals!=static_cast<unsigned>(initial))
            throw std::runtime_error("Not every ship turned back after reaching its route endpoint");
        int movedCount = 0, horizontalMoved = 0;
        for (const auto& pair : starts) {
            auto* v = pair.first;
            if (!std::isfinite(v->XPos()) || !std::isfinite(v->YPos()) || !std::isfinite(v->ZPos())) throw std::runtime_error("Non-finite native actor position");
            const float dx = v->XPos()-pair.second[0], dy = v->YPos()-pair.second[1];
            const float dz=v->ZPos()-pair.second[2];
            movedCount += dx*dx+dy*dy+dz*dz > 1;
            horizontalMoved += dx*dx+dy*dy > 100*100;
            if(testCase=="sea-docked" && dx*dx+dy*dy>1) throw std::runtime_error("Docked ship moved from fixed berth");
            if(routeCase) {
                const float before=std::hypot(routeX-pair.second[0],routeY-pair.second[1]);
                const float after=std::hypot(routeX-v->XPos(),routeY-v->YPos());
                if(before-after<100) throw std::runtime_error("Native unit failed to approach its commanded waypoint");
            }
            if(seaCase) fprintf(stderr,"[detailed] ship delta=%.1f,%.1f,%.1f speed=%.1f\n",dx,dy,dz,v->GetVt());
            if(heloCase) {
                auto* h=static_cast<HelicopterClass*>(v);
                fprintf(stderr,"[detailed] helo ground=%d delta=%.1f,%.1f,%.1f controls=%.3f,%.3f,%.3f mode=%d\n",h->OnGround(),dx,dy,h->ZPos()-pair.second[2],h->hBrain->pStick,h->hBrain->rStick,h->hBrain->throtl,int(h->hBrain->curMode));
            }
            if (testCase == "ground" && static_cast<GroundClass*>(v)->lastThought <= SimLibElapsedTime-10000)
                throw std::runtime_error("Ground AI did not think");
        }
        if(routeCase && horizontalMoved!=initial) throw std::runtime_error("Native waypoint flight/sailing did not move horizontally");
        if ((testCase == "air" || ((testCase == "rocket-legacy-pod" || testCase == "rocket-hold") || testCase == "ground-retarget") || (testCase == "bomb-hold" || (testCase == "bomb-flight" || testCase == "bomb-ground-hit" || testCase == "bomb-ground-miss"))) && movedCount != initial) throw std::runtime_error("Native aircraft did not fly");
        const auto routeSnapshot=DetailedSnapshotJson();
        if (!ground->Reaggregate(FalconLocalSession) || !ground->IsAggregate() || ground->GetTotalVehicles() != initial) throw std::runtime_error("Reaggregation failed to conserve actors");
        std::cout << "{\"status\":\"ok\",\"case\":" << ff::headless::JsonString(testCase) << ",\"moved\":" << movedCount << ",\"before\":" << initial << ",\"after\":" << ground->GetTotalVehicles() << ",\"course_reversals\":" << courseReversals << ",\"detail\":" << routeSnapshot << "}" << std::endl;
        return;
    }
    if (testCase == "feature") {
        VuListIterator objectives(AllObjList);
        auto* objective = static_cast<ObjectiveClass*>(objectives.GetFirst());
        if (!objective || !objective->Deaggregate(FalconLocalSession)) throw std::runtime_error("Native objective deaggregation failed");
        int count=0;
        {
        VuListIterator features(objective->GetComponents());
        for (auto* e=features.GetFirst();e;e=features.GetNext()) {
            auto* f=static_cast<SimFeatureClass*>(e);
            if (!f->IsAwake() || !f->drawPointer) throw std::runtime_error("Native feature incomplete");
            f->Exec(); ++count;
        }
        }
        if (!count || !objective->Reaggregate(FalconLocalSession) || !objective->IsAggregate()) throw std::runtime_error("Native objective reaggregation failed");
        std::cout << "{\"status\":\"ok\",\"case\":\"feature\",\"features\":" << count << "}" << std::endl;
        return;
    }
    // No aggregate-target fallback is allowed to convert a missed shot into a hit.
    campaignParent->SetAggregate(0);
    int vehicleType = -1;
    for (int c = 1; c < NumEntities; ++c) {
        if (Falcon4ClassTable[c].dataType != DTYPE_VEHICLE) continue;
        auto* vehicle = static_cast<VehicleClassDataType*>(Falcon4ClassTable[c].dataPtr);
        if (vehicle && strstr(vehicle->Name, "F-16")) { vehicleType = c + VU_LAST_ENTITY_TYPE; break; }
    }
    if (vehicleType < 0) throw std::runtime_error("Diagnostic requires native F-16 vehicle data");
    VuBin<SimVehicleClass> parent(new SimVehicleClass(vehicleType));
    VuBin<SimVehicleClass> target(new SimVehicleClass(vehicleType));
    parent->SetCountry(2); target->SetCountry(6);
    parent->SetCampaignObject(campaignParent);
    target->SetCampaignObject(campaignParent);
    // Prescribed target motion is a diagnostic fixture, not tactical aircraft AI.
    parent->SetPosition(1500000, 1500000, -20000);
    target->SetPosition(1500000 + (testCase == "unarmed" ? 100 : 10000), 1500000 + (testCase == "miss" ? 20000 : 0), -20000);
    parent->SetDelta(800, 0, 0); target->SetDelta(800, 0, 0);
    parent->SetYPR(0, 0, 0); target->SetYPR(0, 0, 0);
    CalcTransformMatrix(parent.get()); CalcTransformMatrix(target.get());
    parent->SetPowerOutput(1); target->SetPowerOutput(1);
    vuDatabase->Insert(parent.get()); vuDatabase->Insert(target.get());
    SimDriver.AddToObjectList(parent.get()); SimDriver.AddToObjectList(target.get());
    for (int c = 1; c < NumEntities; ++c) {
        if (Falcon4ClassTable[c].dataType != DTYPE_WEAPON) continue;
        auto* weapon = static_cast<WeaponClassDataType*>(Falcon4ClassTable[c].dataPtr);
        if (weapon && strstr(weapon->Name, "AIM-9")) {
            const int i = static_cast<int>(weapon - WeaponDataTable);
            auto missile = static_cast<MissileClass*>(InitAMissile(parent.get(), i, 0));
            VuBin<MissileClass> keepMissile(missile);
            fprintf(stderr, "[detailed] constructed original %s id=%lu\n", WeaponDataTable[i].Name, missile->Id().num_);
            auto* lock = new SimObjectType(target.get()); lock->Reference();
            missile->SetPosition(parent->XPos(), parent->YPos(), parent->ZPos());
            missile->SetLaunchRotation(0, 0);
            missile->SetTarget(lock);
            missile->Start(lock);
            lock->Release();
            vuDatabase->Insert(missile);
            const float initialStrength = target->Strength();
            const auto initialSeeker=missile->sensorArray[0]->Type();
            const auto startTime = SimLibElapsedTime;
            float maximumSpeed = 0; unsigned trackedFrames = 0, coastFrames = 0;
            for (int frame = 0; frame < 6000 && !missile->IsExploding(); ++frame) {
                SimLibElapsedTime = startTime + (frame + 1) * 20;
                SimLibFrameElapsed = static_cast<float>(SimLibElapsedTime);
                ++SimLibFrameCount;
                parent->SetPosition(parent->XPos() + 16, parent->YPos(), parent->ZPos());
                target->SetPosition(target->XPos() + 16, target->YPos(), target->ZPos());
                missile->Exec();
                if(missile->sensorArray[0]->Type()!=initialSeeker)
                    throw std::runtime_error("AIM-9 unexpectedly changed seeker type in flight");
                maximumSpeed = max(maximumSpeed, missile->GetVt());
                trackedFrames += missile->targetPtr != nullptr;
                coastFrames += missile->PowerOutput() < 0.01f;
                DispatchDetailedMessages();
                if (!std::isfinite(missile->XPos()) || !std::isfinite(missile->YPos()) || !std::isfinite(missile->ZPos()))
                    throw std::runtime_error("Native missile produced non-finite position");
                if (frame % 100 == 0) fprintf(stderr, "[detailed] t=%.2f speed=%.1f target=%d end=%d strength=%.1f\n", missile->GetRuntime(), missile->GetVt(), missile->targetPtr != nullptr, missile->done, target->Strength());
            }
            fprintf(stderr, "[detailed] end=%d runtime=%.2f strength=%.1f -> %.1f damage=%llu ends=%llu\n", missile->done, missile->GetRuntime(), initialStrength, target->Strength(), ff_headless::combat.detailedDamageMessages, ff_headless::combat.missileEnds);
            const bool hit = target->Strength() < initialStrength;
            // Pump both public scheduling entry points: the original VU queue
            // must deliver the single terminal event exactly once.
            for(int repeat=0;repeat<3;++repeat) { gMainThread->Update(-1); DispatchDetailedMessages(); }
            if(ff_headless::combat.missileEnds!=1) throw std::runtime_error("Missile terminal event was lost or delivered more than once");
            if (!missile->IsExploding() || !ff_headless::combat.missileEnds || (testCase == "hit" && !hit) || (testCase != "hit" && hit))
                throw std::runtime_error("Native detailed diagnostic outcome failed: " + testCase);
            std::cout << "{\"status\":\"ok\",\"case\":" << ff::headless::JsonString(testCase)
                << ",\"weapon\":" << ff::headless::JsonString(WeaponDataTable[i].Name)
                << ",\"native_seeker\":true,\"native_dynamics\":true,\"native_damage\":true"
                << ",\"seconds\":" << missile->GetRuntime() << ",\"end_code\":" << missile->done
                << ",\"terminal_events\":" << ff_headless::combat.missileEnds
                << ",\"seeker_type\":" << int(initialSeeker)
                << ",\"maximum_speed_fps\":" << maximumSpeed << ",\"tracked_frames\":" << trackedFrames
                << ",\"coast_frames\":" << coastFrames << ",\"initial_strength\":" << initialStrength
                << ",\"final_strength\":" << target->Strength() << ",\"damage_messages\":" << ff_headless::combat.detailedDamageMessages << "}" << std::endl;
            campaignParent->SetAggregate(1);
            SimDriver.RemoveFromObjectList(parent.get()); SimDriver.RemoveFromObjectList(target.get());
            vuDatabase->Remove(missile); vuDatabase->Remove(target.get()); vuDatabase->Remove(parent.get());
            DispatchDetailedMessages();
            return;
        }
    }
    throw std::runtime_error("No AIM-9 weapon in loaded class table");
}



