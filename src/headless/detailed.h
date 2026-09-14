#pragma once
#include <string>
#include <cstdint>
void RunDetailedDiagnostic(const std::string& testCase);

void InitializeDetailedRuntime();
void AdvanceDetailedFrame();

bool DetailedRegionActive();
void SetDetailedRegion(bool active,double x,double y,double radius);
void ReconcileDetailedRegion();
std::string DetailedSnapshotJson();

void FinishDetailedDrain();

std::string DetailedTerrainJson(double x,double y,double radius);

class SimBaseClass;
void RecordDetailedTrajectory(SimBaseClass* actor);
class FalconMissileEndMessage;
class FalconDamageMessage;
void QueueDetailedCampaignDamage(FalconMissileEndMessage*, FalconDamageMessage*);
unsigned PendingDetailedDamage();
void DispatchDetailedDamage();
std::string QueueDetailedAction(uint32_t creator, uint32_t number, const std::string& action);
class AircraftClass;
void RememberDetailedAircraftState(AircraftClass* aircraft);
void RestoreDetailedAircraftState(AircraftClass* aircraft);
