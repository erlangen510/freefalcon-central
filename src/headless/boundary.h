#pragma once
#include <stdexcept>
#include <string>
#include <map>
#include <utility>
#include <array>

namespace ff_headless {
struct ProjectileImpactAudit {
    bool observed=false,groundImpact=false,targetKnown=false,targetRetired=false;
    float targetRangeSquared=0,lethalRadiusSquared=0,targetStrength=0;
    unsigned long targetCreator=0,targetNumber=0,time=0;
    unsigned liveDamageRequests=0,retiredDamageRequests=0,liveNearbyObjects=0;
};
struct CombatCounters {
    unsigned long long engagements = 0;
    unsigned long long losses = 0;
    unsigned long long missileEnds = 0, detailedDamageMessages = 0, weaponsFired = 0;
    unsigned long long aircraftWeaponsFired = 0, aircraftDamageMessages = 0;
    unsigned long long navalWeaponsFired = 0;
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
