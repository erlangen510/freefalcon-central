#include "stdhdr.h"
#include "detailed.h"
#include "aircrft.h"
#include "bomb.h"
#include "unit.h"
#include "camplist.h"
#include "classtbl.h"
#include "vehicle.h"
#include "simdrive.h"
#include "boundary.h"
#include <cmath>
#include <array>
#include <map>
#include <iostream>
#include <stdexcept>
#include <vector>

void RunCountermeasureDiagnostic() {
    VuBin<UnitClass> flight;
    AircraftClass* aircraft=nullptr;
    VuListIterator units(AllUnitList);
    for(auto* entity=units.GetFirst();entity;entity=units.GetNext()) {
        auto* unit=static_cast<UnitClass*>(entity);
        if(!unit->IsFlight() || !unit->IsAggregate() || unit->ZPos()>-5000 || unit->GetTotalVehicles()<1) continue;
        const auto* cls=&Falcon4ClassTable[unit->GetVehicleID(0)];
        if(cls->vuClassData.classInfo_[VU_TYPE]==TYPE_HELICOPTER || !cls->dataPtr) continue;
        if(!(static_cast<VehicleClassDataType*>(cls->dataPtr)->Flags & 0x40000000)) continue;
        if(!unit->Deaggregate(FalconLocalSession)) continue;
        unit->Wake();
        VuListIterator parts(unit->GetComponents());
        for(auto* part=parts.GetFirst();part;part=parts.GetNext()) {
            auto* actor=static_cast<AircraftClass*>(part);
            if(actor->IsAwake() && !actor->OnGround() && actor->counterMeasureStation[CHAFF_STATION].weaponCount>0 && actor->counterMeasureStation[FLARE_STATION].weaponCount>0) {
                aircraft=actor;flight.reset(unit);break;
            }
        }
        if(aircraft) break;
        unit->Reaggregate(FalconLocalSession);
    }
    if(!aircraft) throw std::runtime_error("No airborne native aircraft with countermeasures");
    const int vehicles=flight->GetTotalVehicles();
    const int chaffBefore=aircraft->counterMeasureStation[CHAFF_STATION].weaponCount;
    const int flareBefore=aircraft->counterMeasureStation[FLARE_STATION].weaponCount;
    // Exercise the native command-processing path, including aircraft/fault gates.
    aircraft->dropChaffCmd=TRUE;AdvanceDetailedFrame();
    aircraft->dropFlareCmd=TRUE;AdvanceDetailedFrame();
    const int chaffUsed=chaffBefore-aircraft->counterMeasureStation[CHAFF_STATION].weaponCount;
    const int flareUsed=flareBefore-aircraft->counterMeasureStation[FLARE_STATION].weaponCount;
    if(chaffUsed<=0 || flareUsed<=0) throw std::runtime_error("Native countermeasure commands did not consume stores");
    std::vector<VuBin<BombClass>> decoys;
    int chaff=0,flare=0;
    VuListIterator objects(SimDriver.objectList);
    for(auto* entity=objects.GetFirst();entity;entity=objects.GetNext()) {
        auto* actor=static_cast<SimBaseClass*>(entity);if(!actor->IsBomb()) continue;
        auto* bomb=static_cast<BombClass*>(actor);if(bomb->Parent()!=aircraft) continue;
        if(bomb->IsSetBombFlag(BombClass::IsChaff)) ++chaff;
        else if(bomb->IsSetBombFlag(BombClass::IsFlare)) ++flare;
        else continue;
        decoys.emplace_back(bomb);
    }
    if(chaff!=chaffUsed || flare!=flareUsed) throw std::runtime_error("Countermeasure objects and consumed stores disagree");
    const auto snapshot=DetailedSnapshotJson();
    std::vector<std::array<float,3>> starts;
    for(const auto& decoy:decoys) starts.push_back({decoy->XPos(),decoy->YPos(),decoy->ZPos()});
    for(int tick=0;tick<100;++tick) AdvanceDetailedFrame();
    int moved=0;
    for(size_t i=0;i<decoys.size();++i) {
        const auto* decoy=decoys[i].get();
        const float dx=decoy->XPos()-starts[i][0],dy=decoy->YPos()-starts[i][1],dz=decoy->ZPos()-starts[i][2];
        if(!std::isfinite(dx+dy+dz)) throw std::runtime_error("Non-finite native countermeasure trajectory");
        if(dx*dx+dy*dy+dz*dz>1) ++moved;
    }
    if(moved!=decoys.size()) throw std::runtime_error("Native decoys did not move");
    for(int tick=0;tick<1000;++tick) AdvanceDetailedFrame();
    for(const auto& decoy:decoys)
        if(!decoy->IsDead() && !decoy->IsSetRemoveFlag()) throw std::runtime_error("Native decoy failed to expire within 22 seconds");
    for(int cycle=0;cycle<500 && (aircraft->counterMeasureStation[CHAFF_STATION].weaponCount>0 || aircraft->counterMeasureStation[FLARE_STATION].weaponCount>0);++cycle) {
        aircraft->dropChaffCmd=TRUE;aircraft->dropFlareCmd=TRUE;
        AdvanceDetailedFrame();AdvanceDetailedFrame();
    }
    if(aircraft->counterMeasureStation[CHAFF_STATION].weaponCount!=0 || aircraft->counterMeasureStation[FLARE_STATION].weaponCount!=0)
        throw std::runtime_error("Native countermeasure stores did not exhaust");
    auto countDecoys=[&]() {
        int count=0;VuListIterator list(SimDriver.objectList);
        for(auto* e=list.GetFirst();e;e=list.GetNext()) {
            auto* actor=static_cast<SimBaseClass*>(e);
            if(actor->IsBomb() && static_cast<BombClass*>(actor)->Parent()==aircraft && !actor->IsDead()) ++count;
        }
        return count;
    };
    const int beforeEmpty=countDecoys();
    aircraft->dropChaffCmd=TRUE;aircraft->dropFlareCmd=TRUE;
    AdvanceDetailedFrame();AdvanceDetailedFrame();
    if(countDecoys()!=beforeEmpty || aircraft->counterMeasureStation[CHAFF_STATION].weaponCount!=0 || aircraft->counterMeasureStation[FLARE_STATION].weaponCount!=0)
        throw std::runtime_error("Empty countermeasure command created a decoy or underflowed stores");
    for(int tick=0;tick<1100;++tick) AdvanceDetailedFrame();
    if(countDecoys()!=0) throw std::runtime_error("Exhausted countermeasure burst did not expire");
    if(ff_headless::combat.detailedDamageMessages) throw std::runtime_error("Countermeasure-only fixture generated damage");
    const int pilot=aircraft->pilotSlot;
    std::map<int,std::pair<int,int>> inventories;
    VuListIterator beforeExit(flight->GetComponents());
    for(auto* e=beforeExit.GetFirst();e;e=beforeExit.GetNext()) {
        auto* actor=static_cast<AircraftClass*>(e);
        inventories[actor->pilotSlot]={actor->counterMeasureStation[CHAFF_STATION].weaponCount,actor->counterMeasureStation[FLARE_STATION].weaponCount};
    }
    if(!flight->Reaggregate(FalconLocalSession) || flight->GetTotalVehicles()!=vehicles) throw std::runtime_error("Countermeasure flight failed to reaggregate");
    if(!flight->Deaggregate(FalconLocalSession)) throw std::runtime_error("Countermeasure flight failed to re-enter detailed simulation");
    flight->Wake();
    bool restored=false;VuListIterator survivors(flight->GetComponents());
    for(auto* e=survivors.GetFirst();e;e=survivors.GetNext()) {
        auto* actor=static_cast<AircraftClass*>(e);
        const auto expected=inventories.find(actor->pilotSlot);
        if(expected==inventories.end() || expected->second!=std::make_pair(actor->counterMeasureStation[CHAFF_STATION].weaponCount,actor->counterMeasureStation[FLARE_STATION].weaponCount))
            throw std::runtime_error("Countermeasure inventory changed or moved to another pilot on re-entry");
        if(actor->pilotSlot!=pilot) continue;
        if(actor->counterMeasureStation[CHAFF_STATION].weaponCount!=0 || actor->counterMeasureStation[FLARE_STATION].weaponCount!=0)
            throw std::runtime_error("Detailed region re-entry replenished expended countermeasures");
        restored=true;
    }
    if(!restored || !flight->Reaggregate(FalconLocalSession)) throw std::runtime_error("Countermeasure inventory round-trip lost its pilot");
    std::cout<<"{\"status\":\"ok\",\"case\":\"countermeasures\",\"chaff\":"<<chaff<<",\"flare\":"<<flare<<",\"moved\":"<<moved<<",\"expired\":true,\"empty_stores_inert\":true,\"inventory_persisted\":true,\"chaff_inventory\":"<<chaffBefore<<",\"flare_inventory\":"<<flareBefore<<",\"reaggregated\":true,\"detail\":"<<snapshot<<"}"<<std::endl;
}
