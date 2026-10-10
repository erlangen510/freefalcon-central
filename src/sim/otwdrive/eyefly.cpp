#include "stdhdr.h"
#include "graphics/include/drawbsp.h"
#include "graphics/include/renderow.h"
#include "graphics/include/rviewpnt.h"
#include "otwdrive.h"
#include "simdrive.h"
#include "simfiltr.h"
#include "simio.h"
#include "simbase.h"
#include "falclist.h"
#include "simmath.h"
#include "include/native_observer.h"
#include "falcsess.h"
#include "cmpclass.h"
#include "timerthread.h"
#include "dispopts.h"
#include "campbase.h"
#include <vector>
#include <string>
#include "simveh.h"
#include "sms.h"
#include "guns.h"
#include "bomb.h"
#include "vehicle.h"

#ifdef FF_NATIVE_OBSERVER
extern int endAbort;
extern short NumWeaponTypes;
static VU_ID observerSelection;
static bool observerClickDown=false;
void OTWDriverClass::BeginObserver(float x, float y)
{
    observerSelection=FalconNullId;
    observerClickDown=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    otwPlatform.reset(NULL); lastotwPlatform.reset(NULL); otwTrackPlatform.reset(NULL);
    eyeFly=TRUE; mOTWDisplayMode=ModeSatellite;
    cameraPos.x=cameraPos.y=cameraPos.z=0;
    flyingEye->SetPosition(x,y,GetApproxGroundLevel(x,y)-8000.0f);
    flyingEye->SetYPR(0,-0.8f,0);
    FalconLocalSession->ClearCameras();
    FalconLocalSession->AttachCamera(flyingEye);
    UpdateObserverCamera(0);
}
void OTWDriverClass::DrawObserverOverlay()
{
    if(!NativeObserver::Active() || !renderer || endFlightTimer) return;
    const float width=float(DisplayOptions.DispWidth), height=float(DisplayOptions.DispHeight);
    if(width<=0 || height<=0) return;
    DWORD process=0;
    HWND window=GetForegroundWindow();
    GetWindowThreadProcessId(window,&process);
    POINT mouse={}; RECT client={};
    const bool focused=process==GetCurrentProcessId();
    bool mouseValid=focused && GetCursorPos(&mouse) && ScreenToClient(window,&mouse) && GetClientRect(window,&client)
        && client.right>0 && client.bottom>0;
    float mx=mouseValid?mouse.x*width/client.right:-100, my=mouseValid?mouse.y*height/client.bottom:-100;
    const bool pressed=focused && (GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    const bool click=pressed && !observerClickDown;
    observerClickDown=pressed;
    struct Marker { VU_ID id; float x,y; bool air; int team; };
    std::vector<Marker> markers;
    VU_ID nearest=FalconNullId;
    float best=22.0f*22.0f;
    char name[80]="Selected unit unavailable";
    char details[160]="Destroyed or outside the detailed simulation area";
    char equipment[100]="Equipment: unavailable";
    std::vector<std::string> stores;
    bool found=false;
    Tpoint camera={flyingEye->XPos(),flyingEye->YPos(),flyingEye->ZPos()};
    renderer->SetCamera(&camera,&cameraRot);
    // Keep entity pointers within this traversal; selection persists only as a VU ID.
    VuEnterCriticalSection();
    VuListIterator it(SimDriver.objectList);
    for(SimBaseClass* actor=(SimBaseClass*)it.GetFirst();actor;actor=(SimBaseClass*)it.GetNext()) {
        if(actor->IsDead() || !actor->IsAwake() || !actor->GetCampaignObject()) continue;
        if(actor->Id()==observerSelection) {
            actor->GetCampaignObject()->GetName(name,sizeof(name),FALSE);
            snprintf(details,sizeof(details),"Team %d | %s | %.0f kt | %.0f ft MSL | Health %.0f%%",
                actor->GetTeam(),actor->IsAirplane()?"AIR":"GROUND/SEA",actor->GetVt()*0.592484f,
                -actor->ZPos(),max(0.0f,min(100.0f,actor->pctStrength*100.0f)));
            found=true;
            if(actor->IsVehicle()) {
                VehicleClassDataType* vehicle=GetVehicleClassData(actor->Type()-VU_LAST_ENTITY_TYPE);
                snprintf(equipment,sizeof(equipment),"Equipment: %.60s",vehicle?vehicle->Name:"Unknown");
                SMSBaseClass* sms=static_cast<SimVehicleClass*>(actor)->GetSMS();
                if(sms && sms->hardPoint) {
                    for(int hp=0;hp<sms->NumHardpoints();++hp) {
                        BasicWeaponStation* station=sms->hardPoint[hp];
                        if(!station || station->weaponId<=0 || station->weaponId>=NumWeaponTypes) continue;
                        const char* weaponName=WeaponDataTable[station->weaponId].Name;
                        char item[180];
                        if(GunClass* gun=station->GetGun()) {
                            snprintf(item,sizeof(item),"S%d: %.45s | %d rounds",hp,weaponName,max(0,gun->numRoundsRemaining));
                        } else {
                            int rockets=0,launchers=0;
                            for(SimWeaponClass* weapon=station->weaponPointer.get();weapon;weapon=weapon->GetNextOnRail()) {
                                if(weapon->IsLauncher()) {++launchers;rockets+=max(0,static_cast<BombClass*>(weapon)->LauGetRoundsOnboard());}
                            }
                            if(launchers) snprintf(item,sizeof(item),"S%d: %.45s | %d pods / %d rockets",hp,weaponName,launchers,rockets);
                            else snprintf(item,sizeof(item),"S%d: %.45s | x%d%s",hp,weaponName,max(0,int(station->weaponCount)),station->weaponCount<=0?" (empty)":"");
                        }
                        stores.push_back(item);
                    }
                }
                if(stores.empty()) stores.push_back(sms?"No stores recorded":"Weapon inventory unavailable");
            }
        }
        Tpoint world={actor->XPos(),actor->YPos(),actor->ZPos()};
        const float dx=world.x-camera.x,dy=world.y-camera.y,dz=world.z-camera.z;
        if(dx*cameraRot.M11+dy*cameraRot.M21+dz*cameraRot.M31<=10.0f) continue;
        ThreeDVertex screen; renderer->TransformPoint(&world,&screen);
        if(screen.x<12 || screen.x>width-12 || screen.y<45 || screen.y>height-12) continue;
        markers.push_back({actor->Id(),screen.x,screen.y,actor->IsAirplane()!=0,int(actor->GetTeam())});
    }
    VuExitCriticalSection();
    const float panelTop=height-145.0f-24.0f*float((stores.size()+1)/2);
    for(const Marker& marker:markers) {
        if(marker.y>=panelTop-14) continue;
        float distance=(marker.x-mx)*(marker.x-mx)+(marker.y-my)*(marker.y-my);
        if(distance<best) { best=distance; nearest=marker.id; }
    }
    if(click && mouseValid && my<panelTop-14) {
        observerSelection=nearest;
        NativeObserver::Log(nearest==FalconNullId?"selection-cleared":"unit-selected");
        found=false; // Details are resolved by ID on the next frame.
    }
    const DWORD previousColor=renderer->Color();
    const int previousFont=VirtualDisplay::CurFont();
    VirtualDisplay::SetFont(2); // Largest native UI font, selected for the render resolution.
    auto line=[&](float x,float y,float x2,float y2) {renderer->Render2DLine(x,y,x2,y2);};
    auto box=[&](float x,float y,float radius) {line(x-radius,y-radius,x+radius,y-radius);line(x+radius,y-radius,x+radius,y+radius);line(x+radius,y+radius,x-radius,y+radius);line(x-radius,y+radius,x-radius,y-radius);};
    for(const Marker& marker:markers) {
        if(marker.y>=panelTop-14) continue;
        const bool selected=marker.id==observerSelection;
        renderer->SetColor(0xff000000); box(marker.x,marker.y,10);
        renderer->SetColor(selected?0xff00ffff:(marker.team==FalconLocalSession->GetTeam()?0xff80ff80:0xff8080ff));
        if(marker.air) {line(marker.x,marker.y-8,marker.x+8,marker.y+7);line(marker.x+8,marker.y+7,marker.x-8,marker.y+7);line(marker.x-8,marker.y+7,marker.x,marker.y-8);}
        else box(marker.x,marker.y,7);
        if(selected || marker.id==nearest) box(marker.x,marker.y,13);
    }
    renderer->SetColor(0xff101820);
    renderer->context.RestoreState(STATE_SOLID);
    renderer->Render2DTri(12,panelTop,width-12,panelTop,width-12,height-12);
    renderer->Render2DTri(12,panelTop,width-12,height-12,12,height-12);
    renderer->SetColor(0xffffffff);
    renderer->ScreenText(22,18,"OBSERVER | Click a marker to inspect | WASD move | Q/E height | RMB look | Space pause | F12 map",2);
    renderer->ScreenText(24,panelTop+12,observerSelection==FalconNullId?"UNIT INSPECTOR - Select an air triangle or ground/sea square":name);
    if(observerSelection!=FalconNullId && found) {
        renderer->ScreenText(24,panelTop+36,equipment);
        renderer->ScreenText(24,panelTop+60,details);
        for(size_t i=0;i<stores.size();++i) renderer->ScreenText(24+float(i%2)*width*0.5f,panelTop+87+24*float(i/2),stores[i].c_str());
        renderer->ScreenText(24,height-35,"Current stores / ammunition. Firing also depends on target, range and weapon-system state.");
    } else renderer->ScreenText(24,panelTop+40,observerSelection==FalconNullId?"Green: own team | Red: other teams | Yellow: selected | Markers include units behind terrain":details);
    if(mouseValid) {renderer->SetColor(0xff000000);box(mx,my,5);renderer->SetColor(0xffffffff);line(mx-7,my,mx+7,my);line(mx,my-7,mx,my+7);}
    renderer->SetColor(previousColor);
    VirtualDisplay::SetFont(previousFont);
}
void OTWDriverClass::UpdateObserverCamera(float dT)
{
    static DWORD previousTick=0;
    static POINT previousMouse={};
    static bool dragging=false, pauseDown=false;
    const DWORD now=GetTickCount();
    float seconds=previousTick ? min(0.1f,(now-previousTick)*0.001f) : 0;
    previousTick=now;
    DWORD foregroundProcess=0;
    GetWindowThreadProcessId(GetForegroundWindow(),&foregroundProcess);
    const bool focused=foregroundProcess==GetCurrentProcessId();
    auto down=[&](int key) { return focused && (GetAsyncKeyState(key)&0x8000)!=0; };
    float yaw=flyingEye->Yaw(), pitch=flyingEye->Pitch();
    POINT mouse; GetCursorPos(&mouse);
    if(down(VK_RBUTTON) && dragging) {
        yaw+=(mouse.x-previousMouse.x)*0.005f;
        pitch-=(mouse.y-previousMouse.y)*0.005f;
    }
    dragging=down(VK_RBUTTON); previousMouse=mouse;
    yaw+=(down(VK_RIGHT)-down(VK_LEFT))*seconds;
    pitch+=(down(VK_UP)-down(VK_DOWN))*seconds;
    pitch=max(-1.5f,min(-0.05f,pitch));
    float speed=2500.0f*seconds*(down(VK_SHIFT)?4.0f:1.0f);
    float forward=float(down('W')-down('S')), right=float(down('D')-down('A'));
    float x=flyingEye->XPos()+speed*(cosf(yaw)*forward-sinf(yaw)*right);
    float y=flyingEye->YPos()+speed*(sinf(yaw)*forward+cosf(yaw)*right);
    float z=flyingEye->ZPos()+speed*(down('E')-down('Q'));
    x=max(0.0f,min(x,float(TheCampaign.TheaterSizeX)*3280.839895f));
    y=max(0.0f,min(y,float(TheCampaign.TheaterSizeY)*3280.839895f));
    z=min(z,GetApproxGroundLevel(x,y)-100.0f); z=max(z,-100000.0f);
    flyingEye->SetPosition(x,y,z); flyingEye->SetYPR(yaw,pitch,0);
    const float cy=cosf(yaw),sy=sinf(yaw),cp=cosf(pitch),sp=sinf(pitch);
    cameraRot.M11=cp*cy; cameraRot.M21=cp*sy; cameraRot.M31=-sp;
    cameraRot.M12=-sy; cameraRot.M22=cy; cameraRot.M32=0;
    cameraRot.M13=sp*cy; cameraRot.M23=sp*sy; cameraRot.M33=cp;
    ownshipPos.x=focusPoint.x=x; ownshipPos.y=focusPoint.y=y; ownshipPos.z=focusPoint.z=z;
    cameraPos.x=cameraPos.y=cameraPos.z=0;
    NativeObserver::Sample(x,y,z);
    const bool pause=down(VK_SPACE);
    if(pause && !pauseDown) SetTimeCompression(gameCompressionRatio?0:1);
    pauseDown=pause;
    if(down(VK_F12)) { endAbort=FALSE; endFlightTimer=vuxRealTime+1; }
}
#endif

extern int keyboardPickleOverride;
extern int keyboardTriggerOverride;

void DecomposeMatrix(Trotation* matrix, float* pitch, float* roll, float* yaw);
SimBaseClass* eyeFlyTgt = NULL;

void OTWDriverClass::SetEyeFlyCameraPosition(float dT)
{
#ifdef FF_NATIVE_OBSERVER
    if(NativeObserver::Active()) { UpdateObserverCamera(dT); return; }
#endif
    float e1dot, e2dot, e3dot, e4dot;
    float enorm;
    float p, q, r, throttle;
    float psi, theta, phi;
    float tmpX, tmpY, tmpZ;

    if (lastotwPlatform)
    {
        InsertObjectIntoDrawList(lastotwPlatform.get());
    }

    ownshipPos.x = flyingEye->XPos();
    ownshipPos.y = flyingEye->YPos();
    ownshipPos.z = flyingEye->ZPos();

    focusPoint.x = ownshipPos.x;
    focusPoint.y = ownshipPos.y;
    focusPoint.z = ownshipPos.z;


    //all joystick button functionality is now in sijoy
    //if (IO.ReadDigital(0))
    if (keyboardTriggerOverride) //THW 2003-11-11 fixed
    {
        p = Math.DeadBand(IO.ReadAnalog(AXIS_ROLL), -0.05F, 0.05F) *
            (1.0F / 0.95F);  // Retro 31Dec2003
        p = max(min(p, 1.0F), -1.0F) * dT * 1.0f;

        q = Math.DeadBand(IO.ReadAnalog(AXIS_PITCH), -0.05F, 0.05F) *
            (1.0F / 0.95F);  // Retro 31Dec2003
        q = max(min(q, 1.0F), -1.0F) * dT * 1.0f;

        r = -Math.DeadBand(-IO.ReadAnalog(AXIS_YAW), -0.05F, 0.05F) *
            (1.0F / 0.95F);  // Retro 31Dec2003
        r = max(min(r, 1.0F), -1.0F) * dT * 1.0f;

        if (IO.AnalogIsUsed(AXIS_THROTTLE))  // Retro 31Dec2003
        {
            //throttle = (1.0F - Math.DeadBand(IO.ReadAnalog(2), -0.03F, 0.03F)) * 0.5F;
            throttle =
                0.66666F * IO.ReadAnalog(AXIS_THROTTLE); // Retro 31Dec2003
            throttle *= throttle;
        }
        else
            throttle = 0.25F;

        //if (IO.ReadDigital(1))
        if (keyboardPickleOverride) //THW 2003-11-11 fixed
            throttle *= -1.0F;
    }
    else
    {
        p = q = r = throttle = 0.0F;
    }

    /*-----------------------------------*/
    /* quaternion differential equations */
    /*-----------------------------------*/
    e1dot = (-e4 * p - e3 * q - e2 * r) * 0.5F;
    e2dot = (-e3 * p + e4 * q + e1 * r) * 0.5F;
    e3dot = (e2 * p + e1 * q - e4 * r) * 0.5F;
    e4dot = (e1 * p - e2 * q + e3 * r) * 0.5F;

    /*-----------------------*/
    /* integrate quaternions */
    /*-----------------------*/
    e1 += e1dot;
    e2 += e2dot;
    e3 += e3dot;
    e4 += e4dot;

    /*--------------------------*/
    /* quaternion normalization */
    /*--------------------------*/
    enorm = (float)sqrt(e1 * e1 + e2 * e2 + e3 * e3 + e4 * e4);
    e1 /= enorm;
    e2 /= enorm;
    e3 /= enorm;
    e4 /= enorm;

    /*-------------------*/
    /* direction cosines */
    /*-------------------*/
    cameraRot.M11 = e1 * e1 - e2 * e2 - e3 * e3 + e4 * e4;
    cameraRot.M21 = 2.0F * (e3 * e4 + e1 * e2);
    cameraRot.M31 = 2.0F * (e2 * e4 - e1 * e3);

    cameraRot.M12 = 2.0F * (e3 * e4 - e1 * e2);
    cameraRot.M22 = e1 * e1 - e2 * e2 + e3 * e3 - e4 * e4;
    cameraRot.M32 = 2.0F * (e2 * e3 + e4 * e1);

    cameraRot.M13 = 2.0F * (e1 * e3 + e2 * e4);
    cameraRot.M23 = 2.0F * (e2 * e3 - e1 * e4);
    cameraRot.M33 = e1 * e1 + e2 * e2 - e3 * e3 - e4 * e4;

    tmpX = flyingEye->XPos() + throttle * 20000.0F * cameraRot.M11 * dT;
    tmpY = flyingEye->YPos() + throttle * 20000.0F * cameraRot.M21 * dT;
    tmpZ = flyingEye->ZPos() + throttle * 20000.0F * cameraRot.M31 * dT;
    flyingEye->SetPosition(tmpX, tmpY, tmpZ);

    DecomposeMatrix(&cameraRot, &theta, &phi, &psi);
    flyingEye->SetYPR(psi, theta, phi);
    FindNearestBuilding();
}

void OTWDriverClass::FindNearestBuilding(void)
{
    Tpoint at;
    Tpoint intersect;
    SimBaseClass* testFeature;
    char labelStr[40];
    float radius;

    if (eyeFlyTgt)
    {
        eyeFlyTgt->drawPointer->SetLabel("", 0xff00ff00);
        eyeFlyTgt = NULL;
    }

    renderer->GetAt(&at);

    if (viewPoint->GroundIntersection(&at, &intersect))
    {
        // Find a building
        VuListIterator featureWalker(SimDriver.featureList);
        testFeature = (SimBaseClass*)featureWalker.GetFirst();

        while (testFeature)
        {
            if (testFeature->drawPointer)
                radius = testFeature->drawPointer->Radius();
            else
                radius = 30.0F;

            if (fabs(intersect.x - testFeature->XPos()) < radius and
                fabs(intersect.y - testFeature->YPos()) < radius)
            {
                eyeFlyTgt = testFeature;
                break;
            }

            testFeature = (SimBaseClass*)featureWalker.GetNext();
        }
    }

    if (eyeFlyTgt)
    {
        sprintf(labelStr, "State %d\n",
                eyeFlyTgt->Status() bitand VIS_TYPE_MASK);
        ShiAssert(strlen(labelStr) < 40);
        eyeFlyTgt->drawPointer->SetLabel(labelStr, 0xff00ff00);
    }
}
