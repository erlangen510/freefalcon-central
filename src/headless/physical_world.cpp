// Synchronous access to the original L0 terrain posts. No graphics streamer.
#include "stdhdr.h"
#include "falcsess.h"
#include "otwdrive.h"
#include "tviewpnt.h"
#include "simbase.h"
#include "drawbsp.h"
#include "drawgrnd.h"
#include "drawguys.h"
#include "entity.h"
#include "classtbl.h"
#include "falcent.h"
#include "tpost.h"
#include "tmap.h"
#include "tdskpost.h"
#include "boundary.h"
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

float g_MaximumTheaterAltitude = 0;
namespace {
std::vector<unsigned char> posts;
std::vector<unsigned long> offsets;
int width = 0, height = 0, postSize = 0;
float spacing = 0, minimumZ = 0, maximumZ = 0;
float post(int row, int col) {
    if (row < 0 || col < 0 || row >= height * 16 || col >= width * 16)
        throw std::runtime_error("Detailed terrain coordinate outside loaded theater");
    const auto offset = offsets[(row / 16) * width + col / 16]
        + ((row % 16) * 16 + col % 16) * postSize;
    short elevation;
    memcpy(&elevation, &posts.at(offset + (postSize == 7 ? 2 : 4)), sizeof(elevation));
    return -static_cast<float>(elevation);
}
}

void LoadDetailedTerrain(const char* path) {
    std::ifstream header(std::string(path) + "/Theater.map", std::ios::binary);
    header.read(reinterpret_cast<char*>(&spacing), 4);
    int levels;
    header.seekg(16); header.read(reinterpret_cast<char*>(&levels), 4);
    header.seekg(28 + 256 * 4);
    header.read(reinterpret_cast<char*>(&width), 4);
    header.read(reinterpret_cast<char*>(&height), 4);
    if (!header || !std::isfinite(spacing) || spacing <= 0 || levels < 1 || levels > 9
        || width < 1 || height < 1 || width > 4096 || height > 4096)
        throw std::runtime_error("Invalid detailed terrain header");
    unsigned long flags = 0;
    header.seekg(28 + 256 * 4 + levels * 8);
    header.read(reinterpret_cast<char*>(&flags), 4);
    // TMAP_LARGETERRAIN (tmap.h) stores a 32-bit texture index.
    postSize = (flags & 0x1) ? sizeof(TNewdiskPost) : sizeof(TdiskPost);
    std::ifstream index(std::string(path) + "/Theater.o0", std::ios::binary);
    offsets.resize(static_cast<size_t>(width) * height);
    index.read(reinterpret_cast<char*>(offsets.data()), offsets.size() * 4);
    std::ifstream data(std::string(path) + "/Theater.l0", std::ios::binary | std::ios::ate);
    if (!index || !data) throw std::runtime_error("Detailed terrain requires Theater.o0 and Theater.l0");
    const auto size = data.tellg();
    if (size <= 0 || size > 1024LL * 1024 * 1024) throw std::runtime_error("Invalid L0 terrain size");
    posts.resize(static_cast<size_t>(size)); data.seekg(0);
    data.read(reinterpret_cast<char*>(posts.data()), posts.size());
    if (!data) throw std::runtime_error("Truncated detailed terrain");
    for (auto offset : offsets)
        if (offset > posts.size() || posts.size() - offset < 256 * postSize)
            throw std::runtime_error("Detailed terrain block offset outside L0 data");
    minimumZ = maximumZ = post(0, 0);
    for (int r = 0; r < height * 16; ++r)
        for (int c = 0; c < width * 16; ++c) {
            const float z = post(r, c); minimumZ = min(minimumZ, z); maximumZ = max(maximumZ, z);
        }
    g_MaximumTheaterAltitude = minimumZ;
}

OTWDriverClass OTWDriver;
OTWDriverClass::OTWDriverClass() {
    viewPoint = nullptr; renderer = nullptr; otwPlatform.reset();
    objectScale = 1; todOffset = 0;
}
OTWDriverClass::~OTWDriverClass() {}

float OTWDriverClass::GetGroundLevel(float x, float y, Tpoint* normal) {
    if (posts.empty() || !std::isfinite(x) || !std::isfinite(y)) {
        throw std::runtime_error("Detailed terrain queried before initialization or with invalid coordinates: " + std::to_string(x) + "," + std::to_string(y));
    }
    // Match TViewPoint::GetGroundLevel's no-data result. AI recovery-path
    // probes can extend beyond the theater even while the aircraft is inside.
    // Check before converting to int, since a finite probe may be very large.
    if (x < 0 || y < 0 || x >= (height * 16 - 1) * spacing || y >= (width * 16 - 1) * spacing) {
        if (normal) { normal->x = 0; normal->y = 0; normal->z = 1; }
        return 0;
    }
    const int row = static_cast<int>(floor(x / spacing));
    const int col = static_cast<int>(floor(y / spacing));
    const float dx = x - row * spacing, dy = y - col * spacing;
    const float z1 = post(row, col), z3 = post(row + 1, col + 1);
    float nx, ny;
    // Same triangle split and plane equation as TViewPoint::GetGroundLevel.
    if (dx >= dy) { const float z2 = post(row + 1, col); nx = z2 - z1; ny = z3 - z2; }
    else { const float z2 = post(row, col + 1); nx = z3 - z2; ny = z2 - z1; }
    if (normal) { normal->x = nx; normal->y = ny; normal->z = -spacing; }
    return z1 + nx / spacing * dx + ny / spacing * dy;
}
float OTWDriverClass::GetApproxGroundLevel(float x, float y) { return GetGroundLevel(x, y); }
void OTWDriverClass::GetAreaFloorAndCeiling(float* floor, float* ceiling) {
    if (posts.empty()) throw std::runtime_error("Detailed terrain not initialized");
    *ceiling = minimumZ; *floor = maximumZ;
}
int OTWDriverClass::CheckLOS(FalconEntity* from, FalconEntity* to) {
    // Height is linear within each native triangle. Check every grid/diagonal
    // crossing along the segment, so a narrow ridge cannot fall between samples.
    std::vector<double> ts{0, 1};
    auto crossings = [&](double a, double b) {
        if (a == b) return;
        const int lo = static_cast<int>(ceil(min(a, b) / spacing));
        const int hi = static_cast<int>(floor(max(a, b) / spacing));
        if (hi - lo > 131072) throw std::runtime_error("Invalid terrain LOS span");
        for (int k = lo; k <= hi; ++k) { const double t = (k * spacing - a) / (b - a); if (t > 0 && t < 1) ts.push_back(t); }
    };
    crossings(from->XPos(), to->XPos()); crossings(from->YPos(), to->YPos());
    crossings(from->XPos() - from->YPos(), to->XPos() - to->YPos());
    for (double t : ts) {
        const float x = from->XPos() + (to->XPos() - from->XPos()) * t;
        const float y = from->YPos() + (to->YPos() - from->YPos()) * t;
        const float z = from->ZPos() + (to->ZPos() - from->ZPos()) * t;
        if (z > GetGroundLevel(x, y) + 0.1f) return 0;
    }
    return 1;
}
int OTWDriverClass::CheckCloudLOS(FalconEntity*, FalconEntity*) {
    // Original RViewPoint::CloudLineOfSight returns 1 (weather code disabled).
    return 1;
}

void OTWDriverClass::CreateVisualObject(SimBaseClass* object, float scale) {
    const auto* data = &Falcon4ClassTable[object->Type() - VU_LAST_ENTITY_TYPE];
    const int vis = data->visType[object->Status() & VIS_TYPE_MASK];
    CreateVisualObject(object, vis, scale);
}
void OTWDriverClass::CreateVisualObject(SimBaseClass* object, int vis, float scale) {
    Tpoint pos{object->XPos(), object->YPos(), object->ZPos()};
    Trotation rot;
    rot.M11 = object->dmx[0][0]; rot.M12 = object->dmx[1][0]; rot.M13 = object->dmx[2][0];
    rot.M21 = object->dmx[0][1]; rot.M22 = object->dmx[1][1]; rot.M23 = object->dmx[2][1];
    rot.M31 = object->dmx[0][2]; rot.M32 = object->dmx[1][2]; rot.M33 = object->dmx[2][2];
    if (vis < 0 || vis >= TheObjectListLength || !TheObjectList[vis].nLODs)
        throw std::runtime_error("Missing native collision model");
    if (object->IsGroundVehicle()) {
        const auto* type = object->EntityType();
        if (type->classInfo_[VU_TYPE] == TYPE_FOOT && type->classInfo_[VU_STYPE] == STYPE_FOOT_SQUAD)
            object->drawPointer = new DrawableGuys(vis, &pos, object->Yaw(), 1, scale);
        else object->drawPointer = new DrawableGroundVehicle(vis, &pos, object->Yaw(), scale);
    } else object->drawPointer = new DrawableBSP(vis, &pos, &rot, scale);
}
void OTWDriverClass::CreateVisualObject(SimBaseClass* object, int vis, Tpoint* pos, Trotation* rot, float scale) {
    if (vis < 0 || vis >= TheObjectListLength || !TheObjectList[vis].nLODs)
        throw std::runtime_error("Missing native collision model");
    object->drawPointer = new DrawableBSP(vis, pos, rot, scale);
}
void OTWDriverClass::RemoveObject(DrawableObject* object, int destroy) { if (destroy) delete object; }
void OTWDriverClass::InsertObject(DrawableObject*) {}
void OTWDriverClass::StartExitMenuCountdown() {}

RViewPoint* OTWDriverClass::GetViewpoint() { return nullptr; }
int TViewPoint::GetGroundType(float, float) { ff_headless::unsupported("Render viewpoint material lookup"); }

void CreateDrawable(SimBaseClass* object, float scale) { OTWDriver.CreateVisualObject(object, scale); }
void OTWDriverClass::AttachObject(DrawableBSP* parent, DrawableBSP* child, int slot) { parent->AttachChild(child, slot); }
void OTWDriverClass::DetachObject(DrawableBSP* parent, DrawableBSP* child, int slot) { parent->DetachChild(child, slot); }

// Same object transform convention as OTWDriver::UpdateVehicleDrawables.
void SyncDetailedModel(SimBaseClass* object) {
    if (!object->drawPointer || object->IsExploding()) return;
    Tpoint pos{object->XPos(),object->YPos(),object->ZPos()};
    const auto kind=object->drawPointer->GetClass();
    if (kind==DrawableObject::GroundVehicle || kind==DrawableObject::Guys)
        static_cast<DrawableGroundVehicle*>(object->drawPointer)->Update(&pos,object->Yaw());
    else if (kind==DrawableObject::BSP) {
        Trotation rot;
        rot.M11=object->dmx[0][0];rot.M12=object->dmx[1][0];rot.M13=object->dmx[2][0];
        rot.M21=object->dmx[0][1];rot.M22=object->dmx[1][1];rot.M23=object->dmx[2][1];
        rot.M31=object->dmx[0][2];rot.M32=object->dmx[1][2];rot.M33=object->dmx[2][2];
        static_cast<DrawableBSP*>(object->drawPointer)->Update(&pos,&rot);
    }
}

#include <sstream>
std::string DetailedTerrainJson(double x,double y,double radius) {
    const double extent=radius*1.1,kmPerPost=spacing/3280.839895;
    const int c0=max(0,static_cast<int>(floor((x-extent)/kmPerPost))),r0=max(0,static_cast<int>(floor((y-extent)/kmPerPost)));
    const int c1=min(width*16-1,static_cast<int>(ceil((x+extent)/kmPerPost))),r1=min(height*16-1,static_cast<int>(ceil((y+extent)/kmPerPost)));
    std::ostringstream out;out.precision(10);
    out<<"{\"region\":["<<x<<','<<y<<','<<radius<<"],\"x\":"<<c0*kmPerPost<<",\"y\":"<<r0*kmPerPost<<",\"step_km\":"<<kmPerPost
       <<",\"columns\":"<<c1-c0+1<<",\"rows\":"<<r1-r0+1<<",\"heights_m\":[";
    bool comma=false;for(int r=r0;r<=r1;++r)for(int c=c0;c<=c1;++c){if(comma)out<<',';comma=true;out<<-post(r,c)*.3048;}
    out<<"]}";return out.str();
}
