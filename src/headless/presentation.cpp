// Application services for the single-threaded, offline campaign process.
#include "stdhdr.h"
#include "dispcfg.h"
#include "weather.h"
#include "fakerand.h"
#include "simloop.h"
#include "tacan.h"
#include "uicomms.h"
#include "ui/include/queue.h"
#include "boundary.h"
#include "ui_ia.h"
#include "radardata.h"
#include "drawparticlesys.h"
#include <cstdarg>
#include <cstdio>

FalconDisplayConfiguration FalconDisplay;
ff_headless::CombatCounters ff_headless::combat;
RealWeather* realWeather = nullptr;

class NavigationSystem;
#ifndef FF_DETAILED_ENGINE
NavigationSystem* gNavigationSys = nullptr;
#endif
UIComms* gCommsMgr = nullptr;
class C_Handler;
C_Handler* gMainHandler = nullptr;
CommsQueue* gUICommsQ = nullptr;
int doUI = 0;
int NumHats = 0;
bool g_bUseD3D12 = false, g_bVulkanProfile = false, g_bVulkanValidation = false;
bool g_bVulkanSyncValidation = false, g_bVulkanTerrain = false, g_bUseGpu = false;
bool g_bSleepAll = false;
#ifndef FF_DETAILED_ENGINE
int SimLibErrno = 0;
#endif

#ifndef FF_DETAILED_ENGINE
float SimLibMajorFrameTime = 0;
#endif
UI_IA InstantActionSettings = {};
VU_ID gCurrentFlightID;
int DestroyObjective = 0, RepairObjective = 0;
RadarDataType* RadarDataTable = nullptr;
struct IRSTDataType; IRSTDataType* IRSTDataTable = nullptr;
struct RwrDataType; RwrDataType* RwrDataTable = nullptr;
struct VisualDataType; VisualDataType* VisualDataTable = nullptr;
short NumRadarEntries = 0, NumIRSTEntries = 0, NumVisualEntries = 0;
#ifndef FF_DETAILED_ENGINE
short NumRwrEntries = 0;
#endif
// Only used by dogfight callsign allocation (unsupported in this host).
unsigned char calltable[5][5] = {};
extern const char* FREE_FALCON_VERSION = "FreeFalcon headless";
SimulationLoopControl::SimLoopControlMode SimulationLoopControl::currentMode = SimulationLoopControl::Stopped;

extern "C" void MonoPrint(char* format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
}
// No graphical consumers exist. Model events still travel through the VU queue.
void UI_Refresh() {}
void UI_UpdateEventList() {}
void UI_UpdateOccupationMap() {}
void UI_AddMovieToList(long, long, char*) {}
void INFOSetupRulesControls() {}
void DrawableParticleSys::PS_AddParticleEx(int, Tpoint*, Tpoint*) {}
void AircraftLaunch(FlightClass*) {}
void ReceiveChatString(VU_ID, char*) { ff_headless::unsupported("network chat"); }
bool ControlsXml_ActiveProfilePath(char* out, int size) { if (size) out[0] = 0; return false; }
int tactical_is_training() { return 0; }
class BattalionClass;
void tactical_set_orders(BattalionClass*, VU_ID, short, short) { ff_headless::unsupported("tactical editor orders"); }
int UIComms::LookAtGame(VuGameEntity*) { ff_headless::unsupported("UI game selection"); }
void UIComms::StartCommsDoneCB(int) { ff_headless::unsupported("network startup"); }
FalconSessionEntity* UIComms::FindCampaignPlayer(VU_ID, unsigned char) {
    // This process has no human aircraft slots.
    return nullptr;
}
void update_active_flight(UnitClass*) {}
class SimBaseClass;
void SetLabel(SimBaseClass*) {}
void CommsQueue::Add(short, VU_ID, VU_ID) { ff_headless::unsupported("UI communications queue"); }
unsigned char AssignUIImageID(unsigned char) { return 0; }
unsigned char AssignUISquadronID(short) { return 0; }

// The standalone host requires extracted data; no virtual ZIP resource manager.
extern "C" FILE* ResFOpen(const char* name, const char* mode) { return fopen(name, mode); }
extern "C" int ResFClose(FILE* file) { return fclose(file); }
extern "C" size_t ResFRead(void* buffer, size_t size, size_t count, FILE* file) { return fread(buffer, size, count, file); }
extern "C" int ResFSeek(FILE* file, long offset, int origin) { return fseek(file, offset, origin); }

// The native WeatherClass owns campaign weather evolution. Its rendering base
// holds scalar conditions, without allocating textures or drawable clouds.
float RealWeather::WeatherQuality = 0;
RealWeather::RealWeather() {
    metar = nullptr; renderer = nullptr; real2DClouds = nullptr; real3DClouds = nullptr;
    viewerZ = viewerX = viewerY = rainX = rainY = rainZ = 0;
    ZeroMemory(&lightningPos, sizeof(lightningPos));
    belowLayer = insideLayer = greenMode = bSetup = FALSE;
    LinearFogStatus = false; LinearFogLimit = 10000.0f;
    oldWeatherCondition = 0;
}
RealWeather::~RealWeather() { delete metar; }
void RealWeather::GenerateClouds(bool) { UpdateCondition(); }
void RealWeather::UpdateCondition() {
    if (weatherCondition < FAIR) ShadingFactor = PRANDFloatPos() * 3.0f;
    if (weatherCondition == FAIR) ShadingFactor = PRANDFloatPos() * 9.0f;
    LinearFogStatus = weatherCondition > FAIR;
    if (LinearFogStatus) {
        ShadingFactor = PRANDFloatPos() * 5.0f + 5.0f;
        WeatherQuality = (((stratusZ + 5000.0f) / -15000.0f) + PRANDFloatPos()) * 0.5f;
        if (WeatherQuality > 1.0f) WeatherQuality = 1.0f;
        if (WeatherQuality < 0.0f) WeatherQuality = 0.0f;
    }
}
