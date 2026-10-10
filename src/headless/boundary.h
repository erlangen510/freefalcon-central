#pragma once
#include <stdexcept>
#include <string>
#include <map>
#include <utility>
#include <array>
#include <deque>

namespace ff_headless {
// Value-only bounded journal: never retains legacy entity pointers.
struct CombatEvent {
    unsigned long long sequence=0;
    unsigned long time=0, shooterCreator=0,shooterNumber=0,targetCreator=0,targetNumber=0;
    unsigned long weaponCreator=0,weaponNumber=0;
    unsigned weaponType=0;
    int fireCategory=-1;
    bool damage=false;
    float before=0,after=0;
};
struct ProjectileImpactAudit {
    bool observed=false,groundImpact=false,targetKnown=false,targetRetired=false;
    float targetRangeSquared=0,lethalRadiusSquared=0,targetStrength=0;
    unsigned long targetCreator=0,targetNumber=0,time=0;
    unsigned liveDamageRequests=0,retiredDamageRequests=0,liveNearbyObjects=0;
};
struct CombatCounters {
    struct RecoveryEvent {
        unsigned long long sequence=0;
        unsigned long time=0,actorCreator=0,actorNumber=0,flightCreator=0,flightNumber=0;
        int slot=-1,atc=0;
        bool onGround=false;
    };
    unsigned long long recoverySequence=0;
    std::deque<RecoveryEvent> recoveries;
    void recordRecovery(RecoveryEvent event) {
        event.sequence=++recoverySequence;
        recoveries.push_back(event);
        if(recoveries.size()>256) recoveries.pop_front();
    }
    unsigned long long eventSequence=0;
    std::deque<CombatEvent> events;
    void record(CombatEvent event) {
        event.sequence=++eventSequence;
        events.push_back(event);
        if(events.size()>512) events.pop_front();
    }
    unsigned long long engagements = 0;
    unsigned long long losses = 0;
    unsigned long long missileEnds = 0, detailedDamageMessages = 0, weaponsFired = 0;
    unsigned long long aircraftWeaponsFired = 0, aircraftDamageMessages = 0;
    unsigned long long navalWeaponsFired = 0;
    unsigned long long navalGunEvents = 0;
    unsigned long long helicopterWeaponsFired = 0;
    unsigned long long helicopterDamageDealt = 0;
    bool trackProjectileEnds = false;
    std::map<std::pair<unsigned long,unsigned long>,unsigned> projectileEnds;
    std::map<std::pair<unsigned long,unsigned long>,unsigned> projectileDamage;
    std::map<std::array<unsigned long,4>,unsigned> projectileVictimDamage;
    std::map<std::pair<unsigned long,unsigned long>,std::array<float,4>> projectileEndState;
    std::map<std::pair<unsigned long,unsigned long>,ProjectileImpactAudit> projectileImpactAudit;
    int lastMissileEnd = -1;
    unsigned long long missileEndCodes[12] = {};
};
extern CombatCounters combat;
// An offline campaign host must never silently substitute detailed simulation.
[[noreturn]] inline void unsupported(const char* operation) {
    throw std::runtime_error(std::string("Unsupported headless operation: ") + operation);
}
}
