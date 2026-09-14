// Native driver constructor and list routines from sim/simdrive/simdrive.cpp.
#include "stdhdr.h"
#include "simdrive.h"
#include "simfiltr.h"
#include "camplist.h"
#include "boundary.h"
#include "detailed.h"
#include <cmath>
#include "msginc/missileendmsg.h"
#include "msginc/damagemsg.h"
#include "atcbrain.h"
#include "team.h"
#include "fakerand.h"
#include <vector>
namespace {
struct DeferredDamage { float due; FalconMissileEndMessage* end; FalconDamageMessage* damage; };
std::vector<DeferredDamage> deferredDamage;
}
void QueueDetailedCampaignDamage(FalconMissileEndMessage* end, FalconDamageMessage* damage) {
    // Original SfxClass MESSAGE_TIMER constructor and RunSecondarySfx contract.
    // This is the existing aggregate-to-sim shell model, not a new projectile.
    deferredDamage.push_back({SIM_ELAPSED_SEC+PRANDFloatPos()*25.0f,end,damage});
}
unsigned PendingDetailedDamage() { return static_cast<unsigned>(deferredDamage.size()); }
void DispatchDetailedDamage() {
    for(auto it=deferredDamage.begin();it!=deferredDamage.end();) {
        if(SIM_ELAPSED_SEC<it->due) { ++it; continue; }
        if(it->end) FalconSendMessage(it->end,FALSE);
        if(it->damage) FalconSendMessage(it->damage,FALSE);
        it=deferredDamage.erase(it);
    }
}
SimulationDriver SimDriver;
float SimLibLastMajorFrameTime = 0.02f;


SimulationDriver::SimulationDriver(void)
{
    SimLibFrameElapsed = static_cast<float>(vuxGameTime);
    SimLibElapsedTime = static_cast<SIM_ULONG>(SimLibFrameElapsed);
    UPDATE_SIM_ELAPSED_SECONDS; // COBRA - RED - Scale Elapsed Seconds

    SimLibFrameCount = 0;
    objectList = NULL;
    combinedList = NULL;
    combinedFeatureList = NULL;
    ObjsWithNoCampaignParentList = NULL;
    featureList = NULL;
    campUnitList = NULL;
    campObjList = NULL;
    motionOn = TRUE;
    playerEntity = NULL;
    doEvent = FALSE;
    doFile = FALSE;
    doExit = FALSE;
    doGraphicsExit = FALSE;
    facList = NULL;
    tankerList = NULL;
    atcList = NULL;
    nextATCTime = 0;
    avtrOn = FALSE;
    lastRealTime = 0;
    inCycle = 0; // MLR 1/2/2005 -
}

SimulationDriver::~SimulationDriver(void)
{
}

void SimulationDriver::AddToObjectList(VuEntity* theObject)
{
    objectList->ForcedInsert(theObject);
    combinedList->ForcedInsert(theObject);
}

void SimulationDriver::AddToFeatureList(VuEntity* theObject)
{
    featureList->ForcedInsert(theObject);
    combinedFeatureList->ForcedInsert(theObject);
}

void SimulationDriver::AddToCampUnitList(VuEntity* theObject)
{
    campUnitList->ForcedInsert(theObject);
    combinedList->ForcedInsert(theObject);
    // MonoPrint ("Adding %d\n", theObject->Id().num_);
}

void SimulationDriver::AddToCampFeatList(VuEntity* theObject)
{
    campObjList->ForcedInsert(theObject);
    combinedFeatureList->ForcedInsert(theObject);
}

void SimulationDriver::AddToCombUnitList(VuEntity* theObject)
{
    combinedList->ForcedInsert(theObject);
}

void SimulationDriver::AddToCombFeatList(VuEntity* theObject)
{
    combinedFeatureList->ForcedInsert(theObject);
}

void SimulationDriver::RemoveFromFeatureList(VuEntity* theObject)
{
    featureList->Remove(theObject);
    combinedFeatureList->Remove(theObject);
}

void SimulationDriver::RemoveFromObjectList(VuEntity* theObject)
{
    objectList->Remove(theObject);
    combinedList->Remove(theObject);
}

AircraftClass* SimulationDriver::GetPlayerAircraft() const
{
    return (playerEntity and playerEntity->IsAirplane()) ?
               ((AircraftClass*)(playerEntity)) :
               NULL;
}

void SimulationDriver::SetPlayerEntity(SimMoverClass* entity) { if (entity) ff_headless::unsupported("human pilot"); playerEntity = nullptr; }

void InitializeDetailedLists() {
 UnitFilter unitFilter(0, 1, 0, 0); ObjFilter objectiveFilter(0);
 SimAirfieldFilter airbaseFilter; SimDynamicTacanFilter dynamicTacanFilter;
 SimDriver.campUnitList = new FalconPrivateOrderedList(&unitFilter); SimDriver.campUnitList->Register();
 SimDriver.campObjList = new FalconPrivateOrderedList(&objectiveFilter); SimDriver.campObjList->Register();
 SimDriver.atcList = new VuFilteredList(&airbaseFilter); SimDriver.atcList->Register();
 // Detailed lists are created after the campaign is loaded; Register only
 // subscribes to future database changes, so seed existing native airbases.
 { VuListIterator objectives(AllObjList);
   for(auto* e=objectives.GetFirst();e;e=objectives.GetNext()) SimDriver.atcList->Insert(e);
 }
 SimDriver.tankerList = new VuFilteredList(&dynamicTacanFilter); SimDriver.tankerList->Register();
 // Like ATC, tanker discovery must include flights created before regional
 // detailed mode starts. Insert applies the original tanker-only filter.
 { VuListIterator units(AllUnitList);
   for(auto* e=units.GetFirst();e;e=units.GetNext()) SimDriver.tankerList->Insert(e);
 }
 AllSimFilter all;
 SimDriver.facList = new FalconPrivateList(&all); SimDriver.facList->Register();
 SimDriver.ObjsWithNoCampaignParentList = new FalconPrivateList(&all); SimDriver.ObjsWithNoCampaignParentList->Register();
 SimObjectFilter objects; SimFeatureFilter features; CombinedSimFilter combined;
 SimDriver.objectList = new FalconPrivateOrderedList(&objects); SimDriver.objectList->Register();
 SimDriver.featureList = new FalconPrivateOrderedList(&features); SimDriver.featureList->Register();
 SimDriver.combinedList = new FalconPrivateOrderedList(&combined); SimDriver.combinedList->Register();
 SimDriver.combinedFeatureList = new FalconPrivateOrderedList(&combined); SimDriver.combinedFeatureList->Register();
}

#include "airframe.h"

// InitVU already creates gMainThread with a SimThread FalconMessageFilter.
// A second SimThread queue delivers every event twice, including weapon-fire
// side effects. Pump the original queue at detailed frame cadence instead.
void DispatchDetailedMessages() { gMainThread->Update(-1); }

#include "fcc.h"
#include "guns.h"







#include "playerop.h"
#include "unit.h"

void SimulationDriver::WakeCampaignBase(int isUnit, CampBaseClass* baseEntity,
                                        TailInsertList* comps)
{
    SimBaseClass* theObject = NULL;
    int vehicles = 0, last_to_add = 0, woken = 0;

    ShiAssert(baseEntity);

    if (isUnit)
    {
        vehicles = ((Unit)baseEntity)->GetTotalVehicles();

        if (vehicles > 4)
        {
            // edg: a better detail setting algorithm.  Minimum vehicles is 3.
            // use a quadratic function on slider percentage.
            // so, if the battalion is 48 units the slider settings will give:
            // 0 = 3
            // 1 = 4
            // 2 = 10
            //  3 = 20
            //  4 = 33
            //  5 = 48
            int num = PlayerOptions.ObjectDeaggLevel();
            num *= num;
            last_to_add = 2 + vehicles * num / 10000;
        }
        else
        {
            last_to_add = vehicles;
        }
    }

    // Add our list of objects.
    VuListIterator cit(comps);

    for (theObject = static_cast<SimBaseClass*>(cit.GetFirst());
         theObject not_eq NULL;
         theObject = static_cast<SimBaseClass*>(cit.GetNext()))
    {
        // KCK: Decide not to wake some ground vehicles/features -
        if (isUnit)
        {
            // Wake vehicles by percentage
            if ((woken <= last_to_add) or
                (theObject->GetSlot() ==
                 ((Unit)baseEntity)->class_data->RadarVehicle))
            {
                WakeObject(theObject);
                woken++;
            }
        }
        else
        {
            // Wake features by detail level
            if (theObject->displayPriority <=
                PlayerOptions.BuildingDeaggLevel())
            {
                WakeObject(theObject);
                woken++;
            }
        }

        theObject->SetCampaignObject(baseEntity);
    }
}
void SimulationDriver::SleepCampaignFlight(TailInsertList* flightList)
{
    SimBaseClass* theObject;
    VuListIterator flit(flightList);

    // Put all objects in this flight to sleep
    theObject = (SimBaseClass*)flit.GetFirst();
    ShiAssert(theObject == NULL or
              FALSE == F4IsBadReadPtr(theObject, sizeof *theObject));

    //while (theObject) // JB 010306 CTD
    while (theObject and
           not F4IsBadReadPtr(theObject, sizeof(SimBaseClass))) // JB 010306 CTD
    {
        theObject->Sleep();
        theObject = (SimBaseClass*)flit.GetNext();
    }
}
void SimulationDriver::SleepObject(SimBaseClass* theObject)
{
    if (not theObject or not theObject->IsAwake())
        return;

    theObject->Sleep();
}
void SimulationDriver::WakeObject(SimBaseClass* object) { if (!object || object->IsAwake()) return; if (object->IsSetFalcFlag(FEC_PLAYERONLY)) ff_headless::unsupported("Human-only sim entity"); object->Wake(); }

#include "simio.h"
extern int g_nIdleCutoffPad;
SIMLIB_IO_CLASS IO;
SIMLIB_IO_CLASS::SIMLIB_IO_CLASS()
{
    for (int i = 0; i < AXIS_MAX; i++)
    {
        analog[i].engrValue = 0.F;
        analog[i].isUsed = false;
        analog[i].center = 0;
        analog[i].cutoff = 15000;
        analog[i].ioVal = 0;
        analog[i].isReversed = false;
        analog[i].smoothingFactor = 0; // Retro 19Feb2004
    }

    mouseWheelPresent = false;
    idleCutoffPad = g_nIdleCutoffPad;
}
SIM_INT SIMLIB_IO_CLASS::GetAxisValue(GameAxis_t id)
{
    return (analog[id].ioVal);
}
#include "flight.h"
#include "team.h"
#include "radar.h"
#include "laserpod.h"
#include "harmpod.h"
#include "object.h"
#include "missile.h"
#include "misldisp.h"
#include "otwdrive.h"
extern SensorClass* FindLaserPod(SimMoverClass*);
FlightClass* SimulationDriver::FindTanker(SimBaseClass* center)
{
    FlightClass* tankerFlight;

    tankerFlight = (FlightClass*)FindNearest(center, tankerList);

    if (tankerFlight)
    {
        return tankerFlight;
    }
    else
    {
        return NULL;
    }
}
VU_ID FindAircraftTarget(AircraftClass* theAC)
{
    VU_ID tgtId = FalconNullId;
    MissileClass* theMissile = NULL;
    SensorClass* mslDisplay = NULL;
    SensorClass* tPodDisplay = tPodDisplay = FindLaserPod(theAC);
    ;
    RadarClass* theRadar = (RadarClass*)FindSensor(theAC, SensorClass::Radar);

    // 2000-11-15 ADDED BY S.G. SO PADLOCKED OBJECT CAN BE TARGETED FIRST
    if (OTWDriver.mpPadlockPriorityObject)
    {
        if (not OTWDriver.mpPadlockPriorityObject->IsMissile())
        {
            tgtId = OTWDriver.mpPadlockPriorityObject->Id();

            if (tgtId not_eq FalconNullId)
            {
                return (tgtId);
            }
        }
    }

    // END OF ADDED SECTION

    if (theAC->Sms)
        theMissile = (MissileClass*)theAC->Sms->GetCurrentWeapon();

    if (theMissile and theMissile->IsMissile())
    {
        mslDisplay = (SensorClass*)theMissile->display;
    }

    if (tPodDisplay and tPodDisplay->IsSOI())
    {
        tgtId = tPodDisplay->TargetUnderCursor();

        if (tgtId == FalconNullId)
        {
            if (tPodDisplay->CurrentTarget())
                tgtId = tPodDisplay->CurrentTarget()->BaseData()->Id();
        }
    }

    //Cobra need to separate out HTS so it still targets after all 88's fired
    HarmTargetingPod* theHtS = (HarmTargetingPod*)FindSensor(
        SimDriver.GetPlayerAircraft(), SensorClass::HTS);

    if (not mslDisplay and theHtS)
    {
        tgtId = theHtS->FindIDUnderCursor();

        if (tgtId == FalconNullId and theHtS->CurrentTarget())
        {
            tgtId = theHtS->CurrentTarget()->BaseData()->Id();
        }
    }

    if (tgtId == FalconNullId and mslDisplay and mslDisplay->IsSOI())
    {
        if (mslDisplay->Type() == SensorClass::HTS)
        {
            tgtId = ((HarmTargetingPod*)mslDisplay)->FindIDUnderCursor();

            if (tgtId == FalconNullId)
            {
                if (mslDisplay->CurrentTarget())
                    tgtId = mslDisplay->CurrentTarget()->BaseData()->Id();
            }
        }
        else
        {
            if (((MissileDisplayClass*)mslDisplay)->DisplayType() ==
                    MissileDisplayClass::AGM65_IR or
                ((MissileDisplayClass*)mslDisplay)->DisplayType() ==
                    MissileDisplayClass::AGM65_TV)
            {
                if (theMissile->targetPtr)
                    tgtId = theMissile->targetPtr->BaseData()->Id();
            }
        }
    }

    if (tgtId == FalconNullId and theRadar)
    {
        tgtId = theRadar->TargetUnderCursor();

        if (tgtId == FalconNullId)
        {
            if (theRadar->CurrentTarget())
                tgtId = theRadar->CurrentTarget()->BaseData()->Id();
        }
    }

    return tgtId;
}
SimBaseClass* SimulationDriver::FindNearest(SimBaseClass* center,
                                            VuLinkedList* sourceList)
{
    SimBaseClass* curObj;
    SimBaseClass* retval = NULL;
    float tmpRng, rngSqr = FLT_MAX;
    VuListIterator findWalker(sourceList);
    float myX, myY, myZ;

    if (center)
    {
        curObj = (SimBaseClass*)findWalker.GetFirst();
        myX = center->XPos();
        myY = center->YPos();
        myZ = center->ZPos();

        while (curObj)
        {
            tmpRng = (curObj->XPos() - myX) * (curObj->XPos() - myX) +
                     (curObj->YPos() - myY) * (curObj->YPos() - myY) +
                     (curObj->ZPos() - myZ) * (curObj->ZPos() - myZ);

            if (tmpRng < rngSqr and TeamInfo[curObj->GetTeam()]->TStance(
                                        center->GetTeam()) < Hostile)
            {
                rngSqr = tmpRng;
                retval = curObj;
            }

            curObj = (SimBaseClass*)findWalker.GetNext();
        }
    }

    return retval;
}
#include "ui/include/tac_class.h"
tactical_mission* current_tactical_mission = nullptr;
tactical_type tactical_mission::get_type(void)
{
    return (tactical_type)TheCampaign.TE_type;
}
#include <vector>
#include <set>
extern void SyncDetailedModel(SimBaseClass*);
// Original SimulationDriver::Cycle model scheduling, with a fixed host clock.
// No player input, renderer cycle, remote ownership, or graphical frame pacing.
void SimulationDriver::Cycle() {
    struct CycleScope { bool& active; CycleScope(bool& value):active(value){active=true;} ~CycleScope(){active=false;} } scope(inCycle);
    std::set<SimBaseClass*> executed;
    for (int pass=0;pass<2;++pass) {
        std::vector<VuBin<SimBaseClass>> entities;
        VuListIterator it(objectList);
        for (auto* e=it.GetFirst();e;e=it.GetNext()) entities.emplace_back(static_cast<SimBaseClass*>(e));
        for (const auto& keep:entities) {
            auto* e=keep.get();
            if (e->IsSetRemoveFlag()) { vuDatabase->Remove(e); continue; }
            if (!e->IsAwake()) { RemoveFromObjectList(e); continue; }
            if (!executed.insert(e).second) continue;
            if (!e->IsLocal()) ff_headless::unsupported("Remote detailed entity execution");
            e->UnSetFELocalFlag(FELF_ADDED_DURING_SIMDRIVER_CYCLE);
            if (!e->EntityDriver()) ff_headless::unsupported("Detailed actor missing native VU driver");
            SyncDetailedModel(e);
            RecordDetailedTrajectory(e);
            try { e->EntityDriver()->Exec(SimLibElapsedTime); }
            catch(const std::exception& error) {
                throw std::runtime_error("Native actor type="+std::to_string(e->Type())+" id="+std::to_string(e->Id().num_)
                    +" time="+std::to_string(SimLibElapsedTime)+" position="+std::to_string(e->XPos())+","+std::to_string(e->YPos())+","+std::to_string(e->ZPos())+": "+error.what());
            }
            for(float value:{e->XPos(),e->YPos(),e->ZPos(),e->Yaw(),e->Pitch(),e->Roll(),e->XDelta(),e->YDelta(),e->ZDelta()})
                if(!std::isfinite(value)) ff_headless::unsupported("Non-finite native physical state");
            RecordDetailedTrajectory(e);
            SyncDetailedModel(e);
        }
    }
    UpdateATC();
}

int gRebuildBubbleNow = 0;

SimBaseClass* SimulationDriver::FindNearestTraffic(AircraftClass* aircraft,
                                                   ObjectiveClass* self,
                                                   float* altitude)
{
    SimBaseClass* retval = NULL;
    SimBaseClass* theObject = NULL;
    VuListIterator updateWalker(objectList);
    float myX = 0.0F, myY = 0.0F, myAlt = 0.0F;
    float tmpRange = 0.0F, trafficRange = 0.0F, tmpTrafficAlt = 0.0F,
          trafficAlt = 0.0F;
    Team myTeam = 0;
    int trafficCheckRange = 10; // Range to check for traffic
    int trafficCheckAlt = 2000; // Relative altitude to check for traffic
    int priTrafficDist = 5; // Priority traffic distance (not fully implimented)


    if (not playerEntity)
        return NULL;

    myX = aircraft->XPos(); // My X position
    myY = aircraft->YPos(); // My Y position
    myAlt = -aircraft->ZPos(); // My Altitude
    myTeam = aircraft->GetTeam(); // Team info


    theObject = (SimBaseClass*)updateWalker.GetFirst();

    while (theObject)
    {
        // check Flight callsign so doesn't call out your flight
        // check it is an airplane and not dead
        // check that it is not on the ground
        // checks that it is not hostile
        if (aircraft->GetCallsignIdx() not_eq theObject->GetCallsignIdx() and
            theObject->IsAirplane() and not theObject->IsDead() and
            not theObject->OnGround() and
            GetTTRelations((Team)theObject->GetTeam(), myTeam) <= Neutral)
        {
            if (retval == NULL)
            {
                // Range to traffic
                trafficRange =
                    (theObject->XPos() - myX) * (theObject->XPos() - myX) +
                    (theObject->YPos() - myY) * (theObject->YPos() - myY);

                // Altitude of traffic
                trafficAlt = -theObject->ZPos();

                // Check to see if traffic inside trafficCheckRange
                if (SimToGrid(sqrt(trafficRange)) <=
                    trafficCheckRange) //SimToGrid and sqrt convert to NM
                {
                    // Check to see if altitude of traffic falls within trafficCheckAlt limits
                    if (abs(trafficAlt - myAlt) <= trafficCheckAlt)
                    {
                        FindTrafficConflict(theObject, aircraft,
                                            self); // Check for conflict

                        if (self->brain->trafficCheck ==
                            conflictTraffic) // Traffic is a conflict
                        {
                            retval = theObject; // Set retval to current traffic
                            self->brain->trafficCheck =
                                newTraffic; // This is new traffic
                        }
                    }
                }
            }
            // Arrive here if retval is already set to traffic that is found.  Here we look to see
            // if this traffic is closer than the traffic already set
            else
            {
                // temp Range to traffic
                tmpRange =
                    (theObject->XPos() - myX) * (theObject->XPos() - myX) +
                    (theObject->YPos() - myY) * (theObject->YPos() - myY);

                // temp Altitude of traffic
                tmpTrafficAlt = -theObject->ZPos();

                // if traffic is inside the priority traffic range set by priTrafficDist, find the
                // aircraft closest to my altitude even if it's farther away
                if (abs(tmpTrafficAlt - myAlt) < abs(trafficAlt - myAlt) and
                    SimToGrid(sqrt(tmpRange)) <= priTrafficDist)
                {
                    FindTrafficConflict(theObject, aircraft,
                                        self); // Check for conflict

                    // a conflict was found and set in FindTrafficConflict function
                    if (self->brain->trafficCheck == conflictTraffic)
                    {
                        // this return value to this traffic because it is more of a threat
                        trafficRange = tmpRange; // setup traffic range
                        retval = theObject;
                        self->brain->trafficCheck = priorityTraffic;
                    }
                }
                else
                {
                    if (tmpRange < trafficRange)
                    {
                        FindTrafficConflict(theObject, aircraft, self);

                        if (self->brain->trafficCheck == conflictTraffic)
                        {
                            trafficRange = tmpRange;
                            retval = theObject;
                            self->brain->trafficCheck = newTraffic;
                        }
                    }
                }
            }
        }

        theObject = (SimBaseClass*)updateWalker.GetNext();
    }

    if (retval)
    {
        *altitude = -retval->ZPos();

        if (retval == self->brain->pLastTraffic)
        {
            self->brain->trafficCheck = oldTraffic;
        }

        self->brain->pLastTraffic = retval; //Store last traffic called out
        self->brain->trafficAltitude = *altitude; // Store traffic altitude
        self->brain->trafficRange = trafficRange; // Store traffic range
    }
    else
        self->brain->pLastTraffic = NULL;

    return (retval);
}
void SimulationDriver::FindTrafficConflict(SimBaseClass* traffic,
                                           AircraftClass* myAircraft,
                                           ObjectiveClass* self)
{

    float myHdg = 0.0F, trafficHdg = 0.0F;
    float xdiff = 0.0F, ydiff = 0.0F, angle = 0.0F;
    int hdgToTraffic = 0; // heading to traffic
    int hdgToMyPlane = 0; // heading to my plane
    int relativeBearing =
        0; // relative heading from nose of my plane to traffic
    int parallelTrafficHdg =
        0; // normalized parallel hdg referenced to hdgToMyPlane
    int normalizedTrafficHdg =
        0; // normalized hdg of traffic referenced to hdgToMyPlane
    float myKIAS = 0, trafficKIAS = 0; // airspeed of me and traffic

    // Fine Tune conflict resolution with these numbers
    int pureFwdOffset = 10; // add or subtract this heading from hdgToMyPlane
    int pureRearOffset = 5;
    int leadFwdOffset =
        10; // fwd of 3/9 line, add or sub hdg from traffic's hdg
    int leadRearOffset =
        5; // behind 3/9 line, add or sub hdg from traffic's hdg
    float speedThreshold = 5.0F; // overtaking speed threshold in GetKias


    myHdg = myAircraft->Yaw() * RTD; // Find my heading

    if (myHdg < 0.0F)
        myHdg += 360.0F;

    myKIAS = myAircraft->GetKias(); // My airspeed

    trafficHdg = traffic->Yaw() * RTD; // Find traffic's heading

    if (trafficHdg < 0.0F)
        trafficHdg += 360.0F;

    trafficKIAS = traffic->GetKias(); // Traffic's airspeed


    xdiff = traffic->XPos() - myAircraft->XPos(); // get traffic's X Pos
    ydiff = traffic->YPos() - myAircraft->YPos(); // get traffic's Y Pos

    angle =
        (float)atan2(ydiff, xdiff); // get radian angle from traffic to my plane
    //angle = angle - myAircraft->Yaw();
    hdgToTraffic = FloatToInt32(RTD * angle); // convert to degrees

    if (hdgToTraffic < 0)
        hdgToTraffic = 360 + hdgToTraffic;

    // Find heading to Traffic
    hdgToMyPlane = hdgToTraffic - 180;

    if (hdgToMyPlane < 0)
        hdgToMyPlane = 360 + hdgToMyPlane;

    // Setup Relative bearing
    if (hdgToTraffic > myHdg)
    {
        relativeBearing = static_cast<int>(hdgToTraffic - myHdg);

        if (relativeBearing < 0)
            relativeBearing = 360 + relativeBearing;
    }
    else
    {
        static_cast<int>(relativeBearing = hdgToTraffic - (int)myHdg);

        if (relativeBearing < 0)
            relativeBearing = 360 + relativeBearing;
    }

    // Normalize all values so they are based off of hdgToMyPlane = 0 (makes it all relative)
    parallelTrafficHdg = static_cast<int>(myHdg - hdgToMyPlane);

    if (parallelTrafficHdg < 0)
        parallelTrafficHdg = 360 + parallelTrafficHdg;

    normalizedTrafficHdg = static_cast<int>(trafficHdg - hdgToMyPlane);

    if (normalizedTrafficHdg < 0)
        normalizedTrafficHdg = 360 + normalizedTrafficHdg;

    // Check for conflicts start here.  There are 6 sectors described below.

    // Sector's are setup relative to my planes heading
    // Sector I = 005 to 090
    // Sector II = 091 to 175
    // Sector III = 185 to 269
    // Sector IV = 270 to 355
    // Front Sector = 356 to 004
    // Rear Sector = 176 to 184


    // In Sector I
    if (relativeBearing >= 5 and relativeBearing <= 90)
    {
        if ((normalizedTrafficHdg <= (parallelTrafficHdg - leadFwdOffset)) and
            (normalizedTrafficHdg >= pureFwdOffset))
        {
            self->brain->trafficCheck = conflictTraffic; //possible conflict
        }
        else
            self->brain->trafficCheck = noTraffic; //no conflict

        return;
    }


    // In Sector II
    if (relativeBearing >= 91 and relativeBearing <= 180)
    {
        if ((normalizedTrafficHdg <= (parallelTrafficHdg - leadRearOffset)) and
            (normalizedTrafficHdg >= pureRearOffset))
        {
            if (abs(trafficKIAS - myKIAS) > speedThreshold)
                self->brain->trafficCheck = conflictTraffic; //possible conflict
        }
        else
            self->brain->trafficCheck = noTraffic; //no conflict

        return;
    }


    // In Sector III
    if (relativeBearing >= 181 and relativeBearing <= 269)
    {
        if ((normalizedTrafficHdg >= (parallelTrafficHdg + leadRearOffset)) and
            (normalizedTrafficHdg <= 360 - pureRearOffset))
        {
            if (abs(trafficKIAS - myKIAS) >= speedThreshold)
                self->brain->trafficCheck = conflictTraffic; //possible conflict
        }
        else
            self->brain->trafficCheck = noTraffic; //no conflict

        return;
    }


    // In Sector IV
    if (relativeBearing >= 270 and relativeBearing <= 355)
    {
        if ((normalizedTrafficHdg >= (parallelTrafficHdg + leadFwdOffset)) and
            (normalizedTrafficHdg <= 360 - pureFwdOffset))
        {
            self->brain->trafficCheck = conflictTraffic; //possible conflict
        }
        else
            self->brain->trafficCheck = noTraffic; //no conflict

        return;
    }


    // In Forward Sector
    if (relativeBearing >= 355 and relativeBearing <= 359 or
        relativeBearing >= 0 and relativeBearing <= 4)
    {
        if (relativeBearing >= 355 and relativeBearing <= 360)
        {
            if ((normalizedTrafficHdg >= parallelTrafficHdg) and
                (normalizedTrafficHdg <= 360))
            {
                self->brain->trafficCheck = conflictTraffic; //possible conflict
            }
            else
                self->brain->trafficCheck = noTraffic; //no conflict
        }
        else
        {
            if ((normalizedTrafficHdg <= parallelTrafficHdg) and
                (normalizedTrafficHdg >= 0))
            {
                self->brain->trafficCheck = conflictTraffic; //possible conflict
            }
            else
                self->brain->trafficCheck = noTraffic; //no conflict
        }

        return;
    }


    // In Rear Sector
    if (relativeBearing >= 176 and relativeBearing <= 184)
    {
        if (relativeBearing >= 176 and relativeBearing <= 180)
        {
            if (normalizedTrafficHdg <= parallelTrafficHdg)
                if (abs(trafficKIAS - myKIAS) >= speedThreshold)
                    self->brain->trafficCheck =
                        conflictTraffic; //possible conflict
                else
                    self->brain->trafficCheck = noTraffic; //no conflict
        }
        else
        {
            if ((normalizedTrafficHdg >= parallelTrafficHdg) and
                (normalizedTrafficHdg <= 360))
                if (abs(trafficKIAS - myKIAS) >= speedThreshold)
                    self->brain->trafficCheck =
                        conflictTraffic; //possible conflict
                else
                    self->brain->trafficCheck = noTraffic; //no conflict
        }

        return;
    }
}
