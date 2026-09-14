#ifdef FF_HEADLESS
#include "headless/boundary.h"
#include <cmath>
#endif
#include "msginc/damagemsg.h"
#include "msginc/campweaponfiremsg.h"
#include "mesg.h"
#include "sim/include/simbase.h"
#include "sim/include/simdrive.h"
#include "find.h"
#include "flight.h"
#include "misseval.h"
#include "falclib.h"
#include "falcmesg.h"
#include "falcgame.h"
#include "falcsess.h"
#include "invalidbufferexception.h"

#include "ivibedata.h"
extern IntellivibeData g_intellivibeData;

FalconDamageMessage::FalconDamageMessage(VU_ID entityId, VuTargetEntity* target,
                                         VU_BOOL loopback)
    : FalconEvent(DamageMsg, FalconEvent::SimThread, entityId, target, loopback)
{
}

FalconDamageMessage::FalconDamageMessage(VU_MSG_TYPE type, VU_ID senderid,
                                         VU_ID target)
    : FalconEvent(DamageMsg, FalconEvent::SimThread, senderid, target)
{
}

FalconDamageMessage::~FalconDamageMessage(void)
{
}

int FalconDamageMessage::Process(uchar autodisp)
{
    if (autodisp)
    {
        return 0;
    }

    FalconEntity *theEntity, *shooter;
    theEntity = (FalconEntity*)vuDatabase->Find(dataBlock.dEntityID);

    if (theEntity)
    {
        if (theEntity->IsSim())
        {
#ifdef FF_HEADLESS
            ++ff_headless::combat.detailedDamageMessages;
    if(theEntity->IsAirplane()) ++ff_headless::combat.aircraftDamageMessages;
#endif
#ifdef FF_HEADLESS
            const float strengthBefore = static_cast<SimBaseClass*>(theEntity)->Strength();
#endif
            ((SimBaseClass*)theEntity)->ApplyDamage(this);
#ifdef FF_HEADLESS
            if (!std::isfinite(static_cast<SimBaseClass*>(theEntity)->Strength()))
                throw std::runtime_error("Non-finite native damage: victim="+std::to_string(dataBlock.dEntityID.num_)+
                    " shooter="+std::to_string(dataBlock.fEntityID.num_)+" weapon="+std::to_string(dataBlock.fWeaponID)+
                    " type="+std::to_string(dataBlock.damageType)+" input="+std::to_string(dataBlock.damageStrength)+
                    " random="+std::to_string(dataBlock.damageRandomFact)+" before="+std::to_string(strengthBefore));
            shooter = static_cast<FalconEntity*>(vuDatabase->Find(dataBlock.fEntityID));
            if(ff_headless::combat.trackProjectileEnds && theEntity->IsHelicopter() && static_cast<SimBaseClass*>(theEntity)->Strength()<strengthBefore)
                fprintf(stderr,"[helo-damage-received] victim=%lu shooter=%lu projectile=%lu strength=%.2f->%.2f\n",theEntity->Id().num_,dataBlock.fEntityID.num_,dataBlock.fWeaponUID.num_,strengthBefore,static_cast<SimBaseClass*>(theEntity)->Strength());
            if(ff_headless::combat.trackProjectileEnds && static_cast<SimBaseClass*>(theEntity)->Strength()<strengthBefore)
            {
                ++ff_headless::combat.projectileDamage[{dataBlock.fWeaponUID.creator_,dataBlock.fWeaponUID.num_}];
                ++ff_headless::combat.projectileVictimDamage[{dataBlock.fWeaponUID.creator_,dataBlock.fWeaponUID.num_,dataBlock.dEntityID.creator_,dataBlock.dEntityID.num_}];
            }
            if (shooter && shooter->IsSim() && shooter->IsHelicopter() &&
                static_cast<SimBaseClass*>(theEntity)->Strength() < strengthBefore)
            {
                ++ff_headless::combat.helicopterDamageDealt;
                if(ff_headless::combat.trackProjectileEnds)
                    fprintf(stderr,"[helo-damage-dealt] time=%lu shooter=%lu victim=%lu weapon=%u projectile=%lu type=%u strength=%.2f->%.2f\n",SimLibElapsedTime,dataBlock.fEntityID.num_,dataBlock.dEntityID.num_,dataBlock.fWeaponID,dataBlock.fWeaponUID.num_,dataBlock.damageType,strengthBefore,static_cast<SimBaseClass*>(theEntity)->Strength());
            }
#endif

            // Record any hits directly
            if (TheCampaign.MissionEvaluator and
                not(theEntity->IsSetFalcFlag(FEC_INVULNERABLE)))
            {
                TheCampaign.MissionEvaluator->RegisterHit(this);
            }
        }
        else if (theEntity->IsCampaign())
        {
            CampEntity campTarget = (CampEntity)theEntity;
            CampEntity campShooter = NULL;

            shooter = (FalconEntity*)vuDatabase->Find(dataBlock.fEntityID);

            // ShiAssert ( not "This is a bad thing I think");
            if (not shooter)
            {
                return TRUE;
            }

            if (shooter->IsSim())
            {
                campShooter = ((SimBaseClass*)shooter)->GetCampaignObject();
            }
            else
            {
                campShooter = (CampEntity)shooter;
            }

            if (not campTarget->IsAggregate())
            {
                // This thing is actually deaggregated (probably happened while
                // the missile was in flight). Chalk it up as a miss if it
                // doesn't happen very often.
                // ShiAssert ( not "This probably shouldn't happen.");
            }
            else if (campShooter->IsUnit())
            {
                // Otherwise, apply the damage to the campaign unit (we have to convert
                // the the campaign's parameter expectations).
                campShooter->ReturnToSearch();

                if (campTarget->IsLocal())
                {
                    FalconCampWeaponsFire* cwfm = new FalconCampWeaponsFire(
                        campTarget->Id(), FalconLocalGame);
                    cwfm->dataBlock.shooterID = campShooter->Id();
                    cwfm->dataBlock.fPilotId = dataBlock.fPilotID;
                    cwfm->dataBlock.dPilotId = dataBlock.dPilotID;
                    cwfm->dataBlock.weapon[0] =
                        (short)GetWeaponIdFromDescriptionIndex(
                            dataBlock.fWeaponID - VU_LAST_ENTITY_TYPE);
                    cwfm->dataBlock.weapon[1] = 0;
                    cwfm->dataBlock.shots[0] = 1;
                    cwfm->dataBlock.fWeaponUID = dataBlock.fWeaponUID;
                    campTarget->ApplyDamage(cwfm, 200);
                }
            }

            // KCK: Currently Apply Damage calls register hit (sometimes multiple times). Theoretically,
            // it should be possible to call it here, like we do for sim entities - but this is a task
            // for another time.
            // if (TheCampaign.MissionEvaluator and ( not theEntity or not theEntity->IsSetFalcFlag(FEC_INVULNERABLE)))
            // TheCampaign.MissionEvaluator->RegisterHit(this);
        }
    }

    return TRUE;
}

FalconDamageMessage* CreateGroundCollisionMessage(SimVehicleClass* vehicle,
                                                  int damage,
                                                  VuTargetEntity* target)
{
#if defined(FF_HEADLESS) && !defined(FF_DETAILED_ENGINE)
    ff_headless::unsupported("CreateGroundCollisionMessage");
#else

    ShiAssert(vehicle);

    if (FalconLocalSession and vehicle == FalconLocalSession->GetPlayerEntity())
        g_intellivibeData.CollisionCounter++;

    FalconEntity* lastToHit =
        (SimVehicleClass*)vuDatabase->Find(vehicle->LastShooter());
    CampBaseClass* campUnit = NULL;

    FalconDamageMessage* message;
    message = new FalconDamageMessage(vehicle->Id(), target);

    if (lastToHit and not lastToHit->IsEject())
    {
        message->dataBlock.fEntityID = lastToHit->Id();
        message->dataBlock.fIndex = lastToHit->Type();

        if (lastToHit->IsSim())
        {
            message->dataBlock.fPilotID =
                ((SimVehicleClass*)lastToHit)->pilotSlot;
            message->dataBlock.fCampID =
                ((SimVehicleClass*)lastToHit)->GetCampaignObject()->GetCampID();
            message->dataBlock.fSide =
                ((SimVehicleClass*)lastToHit)->GetCampaignObject()->GetOwner();
            campUnit = ((SimBaseClass*)lastToHit)->GetCampaignObject();
        }
        else
        {
            message->dataBlock.fPilotID = 0;
            message->dataBlock.fCampID =
                ((CampBaseClass*)lastToHit)->GetCampID();
            message->dataBlock.fSide = ((CampBaseClass*)lastToHit)->GetOwner();
            campUnit = (CampBaseClass*)lastToHit;
        }

        message->dataBlock.fCampID = campUnit->GetCampID();
        message->dataBlock.fSide = campUnit->GetOwner();
    }
    else
    {
        message->dataBlock.fEntityID = vehicle->Id();
        message->dataBlock.fCampID = vehicle->GetCampaignObject()->GetCampID();
        message->dataBlock.fSide = vehicle->GetCampaignObject()->GetOwner();
        message->dataBlock.fPilotID = vehicle->pilotSlot;
        message->dataBlock.fIndex = vehicle->Type();
    }

    message->dataBlock.fWeaponID = vehicle->Type();
    message->dataBlock.fWeaponUID.num_ = 0;
    message->dataBlock.damageType = FalconDamageType::GroundCollisionDamage;
    message->dataBlock.dEntityID = vehicle->Id();
    message->dataBlock.dCampID = vehicle->GetCampaignObject()->GetCampID();
    message->dataBlock.dSide = vehicle->GetCampaignObject()->GetOwner();
    message->dataBlock.dPilotID = vehicle->pilotSlot;
    message->dataBlock.dIndex = vehicle->Type();

    message->dataBlock.damageStrength = (float)damage;
    message->dataBlock.damageRandomFact = 0.0F;
    message->RequestOutOfBandTransmit();

    return message;

#endif
}
