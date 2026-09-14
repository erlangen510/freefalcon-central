// Presentation-only services. These never resolve combat or discard model events.
#include "stdhdr.h"
#include "acmi/src/include/acmirec.h"
#include "fsound.h"
#include "mfd.h"
#include "drawbsp.h"
#include "aircrft.h"
#include "helo.h"
#include "sms.h"
#include "falcsnd/voicemanager.h"
#include "ivibedata.h"
#include "boundary.h"
ACMIRecorder gACMIRec;
ACMI_Hash* ACMIIDTable = nullptr;
ACMIRecorder::ACMIRecorder() : _fd(nullptr), _csect(nullptr), _recording(FALSE), _bytesWritten(0), _maxBytesToWrite(0) {}
ACMIRecorder::~ACMIRecorder() {}
void ACMIRecorder::StationarySfxRecord(ACMIStationarySfxRecord*) {}
void ACMIRecorder::MissilePositionRecord(ACMIMissilePositionRecord*) {}
void ACMIRecorder::FeaturePositionRecord(ACMIFeaturePositionRecord*) {}
long ACMI_Hash::Add(VU_ID, char*, long) { ff_headless::unsupported("ACMI recording disabled"); }
void F4SoundPos::Sfx(int,int,float,float) {}
void F4SoundPos::UpdatePos(SimBaseClass*) {}
MfdDrawable::~MfdDrawable() {}
void MfdDrawable::DisplayInit(ImageBuffer*) {}
void MfdDrawable::PushButton(int,int) {}
void DrawableClass::DisplayExit() { privateDisplay = display = nullptr; }
// No drawable object is constructed by the headless missile runtime.









VoiceManager* VM = nullptr;
void VoiceManager::RemoveRadioCalls(VU_ID) {}
IntellivibeData g_intellivibeData = {};
float g_nboostguidesec = 0; //me123 how many sec we are in boostguide mode
float g_nterminalguiderange =
    0; //me123 what range we transfere to terminal guidence
float g_nboostguideSensorPrecision = 0; //me123
float g_nsustainguideSensorPrecision = 0; //me123
float g_nterminalguideSensorPrecision = 0; //me123
float g_nboostguideLead = 0; //me123
float g_nsustainguideLead = 0; //me123
float g_nterminalguideLead = 0; //me123
float g_nboostguideGnav = 0; //me123
float g_nsustainguideGnav = 0; //me123
float g_nterminalguideGnav = 0; //me123
float g_nboostguideBwap = 0; //me123
float g_nsustainguideBwap = 0; //me123
float g_nterminalguideBwap = 0; //me123

#include "drawparticlesys.h"
#include "airframe.h"

int gNumWeaponsInAir = 0;
unsigned long DrawableParticleSys::PS_EmitTrail(unsigned long,int,float,float,float,float,float) { return 0; }
void DrawableParticleSys::PS_KillTrail(unsigned long) {}

#include "flightdata.h"
FlightData cockpitFlightData;
extern "C" void F4SoundFXSetDist(int,int,float,float) {}
F4SoundPos::F4SoundPos() : uid(0), pos{}, vel{}, velVec{}, relPos{}, orientation{}, velocity(0), distance(0), inMachShadow(0), sonicBoom(0), inPurgeList(0), platform(nullptr) {}
F4SoundPos::~F4SoundPos() {}

#include "hud.h"
HudClass* TheHud = nullptr;
float gSfxLOD = 1;
int AddParticleEffect(int, Tpoint*, Tpoint*) { return 0; }
int keyboardPickleOverride = 0, keyboardTriggerOverride = 0;
float throttleOffset = 0, rudderOffset = 0, rudderOffsetRate = 0;
float pitchStickOffset = 0, rollStickOffset = 0, pitchStickOffsetRate = 0, rollStickOffsetRate = 0;
float throttleOffsetRate = 0, pitchElevatorTrimRate = 0, pitchAileronTrimRate = 0, pitchRudderTrimRate = 0;



#include "sfx.h"
int gTotSfx = 0, gSfxLODCutoff = 0, gSfxLODTotCutoff = 0;
int gSfxCount[SFX_NUM_TYPES] = {};
void ACMIRecorder::GenPositionRecord(ACMIGenPositionRecord*) {}
void ACMIRecorder::TracerRecord(ACMITracerStartRecord*) {}
void ACMIRecorder::FlarePositionRecord(ACMIFlarePositionRecord*) {}
void ACMIRecorder::ChaffPositionRecord(ACMIChaffPositionRecord*) {}

#include "otwdrive.h"
#include "render3d.h"
#include "colorbank.h"
#include "trackir.h"
#include "lantirn.h"
#include "smsdraw.h"
#include "cphsi.h"
#include "icp.h"
#include "cpmisc.h"
#include "mavdisp.h"

void* gSharedIntellivibe = nullptr;
int JoystickPlayEffect(int,int) { return 0; }
void JoystickStopEffect(int) {}
bool F4SoundPos::IsPlaying(int,int) { return false; }
void F4SoundPos::Sfx(int,int,float,float,float,float,float) {}
void F4SoundPos::SfxRel(int,int,float,float,Tpoint&) {}
void Render3D::Render3DLine(Tpoint*,Tpoint*) {}
struct SfxDef; SfxDef* SFX_DEF = nullptr;
bool g_bEnableTrackIR = false;
TrackIR theTrackIRObject;
void ACMIRecorder::DOFRecord(ACMIDOFRecord*) {}
void ACMIRecorder::SwitchRecord(ACMISwitchRecord*) {}
void ACMIRecorder::AircraftPositionRecord(ACMIAircraftPositionRecord*) {}
void ResetVoices() {}
void HudClass::SetLightLevel() {}
void HudClass::SetEEGSData(float,float,float,float,float,float,float,float) {}
int ColorBankClass::PitLightLevel = 0;
void ACMIToggleRecording(unsigned long,int,void*) { ff_headless::unsupported("ACMI cockpit control"); }
OTWDriverClass::OTWDisplayMode OTWDriverClass::GetOTWDisplayMode() { return ModeNone; }
void OTWDriverClass::EndFlight() { ff_headless::unsupported("Player end-flight control"); }
void OTWDriverClass::SetExitMenu(int) {}
void OTWDriverClass::ToggleThrustReverseDisplay() {}
void OTWDriverClass::StartEjectCam(EjectedPilotClass*,int) {}
float OTWDriverClass::DistanceFromCloudEdge() { return -5000.0f; } // Original OTW implementation.
float ReadThrottle() { ff_headless::unsupported("Physical player throttle"); }
LantirnClass* theLantirn = nullptr;
void LantirnClass::Exec(AircraftClass*) { ff_headless::unsupported("Player LANTIRN avionics"); }
float LantirnClass::GetGLimit() { ff_headless::unsupported("Player LANTIRN autopilot"); }
float CPHsi::GetValue(HSIValues) { ff_headless::unsupported("Player HSI autopilot input"); }
void ICPClass::LeaveCNI() {}
void ICPClass::ClearStrings() {}
void ICPClass::ChangeToCNI() {}
int ICPClass::CheckBackupPages() { ff_headless::unsupported("Player backup instrument pages"); }
void CPMisc::SetRefuelState(int) {}
void SmsDrawable::UpdateGroundSpot() { ff_headless::unsupported("Player ground targeting display"); }


#include "cpmanager.h"
void HudClass::SetOwnship(AircraftClass*) { ff_headless::unsupported("Player HUD ownship"); }
void CockpitManager::SetOwnship(SimBaseClass*) { ff_headless::unsupported("Player cockpit ownship"); }
void CockpitManager::InitialiseInstruments() { ff_headless::unsupported("Player instruments"); }
MFDClass* MfdDisplay[4] = {};
int set3DTexture = 0;
void MFDClass::SetOwnship(AircraftClass*) { ff_headless::unsupported("Player MFD ownship"); }
void MFDClass::SetMode(MfdMode) { ff_headless::unsupported("Player MFD control"); }
void MFDClass::SetNewMode(MfdMode) { ff_headless::unsupported("Player MFD control"); }
void OTWDriverClass::SetGraphicsOwnship(SimBaseClass*) { ff_headless::unsupported("Player view ownship"); }
void OTWDriverClass::SetOTWDisplayMode(OTWDisplayMode) { ff_headless::unsupported("Player view mode"); }
void VirtualDisplay::GetViewport(float*,float*,float*,float*) { ff_headless::unsupported("Player HUD viewport"); }
VuEntity* HudClass::CanSeeTarget(int,VuEntity*,FalconEntity*) { ff_headless::unsupported("Player HUD targeting"); }
ViewportBounds hudViewportBounds = {};
void SmsDrawable::SetDisplayMode(SmsDisplayMode) { ff_headless::unsupported("Player SMS display"); }

int VoiceManager::GetRadioFreq(int radio)
{
    if (this)
    {
        if (radio >= 0 and radio <= 1)
            return radiofilter[radio];

        return radiofilter[0];
    }
    else
    {
        return 0;
    }
}
int OTWDriverClass::GetGroundIntersection(euler*,vector*) { ff_headless::unsupported("Player ground pipper designation"); }

#include "gmcomposit.h"
#include "gmradar.h"
void VirtualDisplay::AdjustOriginInViewport(float,float) {}
unsigned int DrawableClass::GetMfdColor(MfdColor) { return 0; }
void DrawableClass::GetButtonPos(int,float*,float*) { ff_headless::unsupported("Cockpit button geometry"); }
void DrawableClass::LabelButton(int,const char*,const char*,int) {}
void MfdDrawable::DefaultLabel(int) {}
void MFDSwapDisplays() { ff_headless::unsupported("Player MFD swap"); }
void RenderGMComposite::SetRange(float,int) {}
void RenderGMRadar::DrawBlip(float,float) {}
void RenderGMRadar::DrawBlip(DrawableObject*,float,bool) {}
void SmsDrawable::SetGroundSpotPos(float,float,float) { ff_headless::unsupported("Player ground spot display"); }

float OTWDriverClass::GetDoppler(float,float,float,float,float,float) { return 1; } // Audio pitch only.
void OTWDriverClass::RemoveFromLitList(DrawableBSP*) {}
void OTWDriverClass::AddToLitList(DrawableBSP*) {}
void ACMIRecorder::FeatureStatusRecord(ACMIFeatureStatusRecord*) {}

void OTWDriverClass::ExitMenu(unsigned long) { ff_headless::unsupported("Player flight exit menu"); }
