#ifdef FF_NATIVE_OBSERVER
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include "falclib.h"
#include "cmpclass.h"
#include "simdrive.h"
#include "simbase.h"
#include "falcsess.h"
#include "dispcfg.h"
#include "f4find.h"
#include "ui/include/falcuser.h"
#include "ui/include/uicomms.h"
#include "include/native_observer.h"
namespace NativeObserver {
static std::atomic<int> state{0};
static float centerX, centerY;
bool Requested() { return state.load() == 1; }
bool Active() { return state.load() != 0; }
void Position(float& x, float& y) { x=centerX; y=centerY; }
void Log(const char* event) {
    char path[MAX_PATH];
    snprintf(path,sizeof(path),"%s/observer-session.log",FalconDataDirectory);
    FILE* f=fopen(path,"a");
    if(f) { fprintf(f,"%s time=%lu center_ft=%.1f,%.1f player=%s\n",event,
        (unsigned long)TheCampaign.CurrentTime,centerX,centerY,
        SimDriver.GetPlayerAircraft()?"present":"none"); fclose(f); }
}
bool Request(float x,float y) {
    if(!FalconLocalGame || !FalconLocalSession ||
       FalconLocalGame->GetGameType()!=game_Campaign ||
       !FalconLocalGame->IsLocal() || (gCommsMgr && gCommsMgr->Online()) ||
       FalconLocalSession->GetFlyState()!=FLYSTATE_IN_UI ||
       SimDriver.GetPlayerEntity() || !std::isfinite(x) || !std::isfinite(y) ||
       x<0 || y<0 || TheCampaign.EndgameResult) return false;
    int expected=0;
    if(!state.compare_exchange_strong(expected,1)) return false;
    centerX=x; centerY=y;
    FalconLocalSession->SetPlayerFlight(NULL);
    FalconLocalSession->SetPilotSlot(255);
    Log("requested");
    PostMessage(FalconDisplay.appWin,FM_START_CAMPAIGN,0,0);
    return true;
}
void Entered() { state.store(2); Log("entered"); }
void Sample(float x,float y,float z) {
    static DWORD previous=0;
    DWORD now=GetTickCount();
    if(now-previous<5000) return;
    previous=now;
    unsigned awake=0,aircraft=0,ground=0;
    double positionSum=0;
    VuListIterator it(SimDriver.objectList);
    for(SimBaseClass* actor=(SimBaseClass*)it.GetFirst();actor;actor=(SimBaseClass*)it.GetNext()) {
        if(actor->IsDead() || !actor->IsAwake()) continue;
        ++awake; aircraft+=actor->IsAirplane()!=0; ground+=actor->OnGround()!=0;
        positionSum+=actor->XPos()+actor->YPos()+actor->ZPos();
    }
    char path[MAX_PATH]; snprintf(path,sizeof(path),"%s/observer-session.log",FalconDataDirectory);
    FILE* f=fopen(path,"a");
    if(f) { fprintf(f,"frame time=%lu camera_ft=%.1f,%.1f,%.1f awake=%u aircraft=%u ground=%u position_sum=%.2f player=%s\n",
        (unsigned long)TheCampaign.CurrentTime,x,y,z,awake,aircraft,ground,positionSum,
        SimDriver.GetPlayerAircraft()?"present":"none"); fclose(f); }
}
void Finished() { Log("returned"); state.store(0); }
}
#endif
