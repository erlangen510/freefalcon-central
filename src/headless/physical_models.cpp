// Load the physics-relevant original DXH metadata without texture/LOD streaming.
#include "stdhdr.h"
#include "objectparent.h"
#include "drawobj.h"
#include "boundary.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <stdexcept>

ObjectParent* TheObjectList = nullptr;
int TheObjectListLength = 0;
ObjectParent::ObjectParent() : radius(0), minX(0), maxX(0), minY(0), maxY(0), minZ(0), maxZ(0),
    RadarSign(0), IRSign(0), pLODs(nullptr), pSlotAndDynamicPositions(nullptr), nTextureSets(0),
    nDynamicCoords(0), nLODs(0), nSwitch(0), nDOF(0), nSlots(0), nSwitches(0), nDOFs(0), Locked(false), refCount(0) {}
ObjectParent::~ObjectParent() { delete[] pLODs; delete[] pSlotAndDynamicPositions; }
void ObjectParent::Reference() { ++refCount; }
void ObjectParent::ReferenceWithFetch() { Reference(); }
void ObjectParent::Release(bool) {
    if (refCount <= 0) throw std::runtime_error("Unbalanced native model reference");
    --refCount;
}
void ObjectParent::ReferenceTexSet(DWORD, DWORD) {}
void ObjectParent::ReleaseTexSet(DWORD, DWORD) {}

namespace {
struct Reader {
    std::vector<char> bytes;
    size_t at = 0;
    explicit Reader(const char* path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in || in.tellg() <= 0 || in.tellg() > 128 * 1024 * 1024)
            throw std::runtime_error("Missing or invalid native model header");
        bytes.resize(static_cast<size_t>(in.tellg())); in.seekg(0);
        in.read(bytes.data(), bytes.size());
        if (!in) throw std::runtime_error("Truncated native model header");
    }
    void take(void* out, size_t n) {
        if (n > bytes.size() - at) throw std::runtime_error("Native model header overrun");
        if (out) memcpy(out, bytes.data() + at, n);
        at += n;
    }
    int count() {
        int n; take(&n, 4);
        if (n < 0 || n > 1000000) throw std::runtime_error("Invalid native model record count");
        return n;
    }
};
}

void LoadDetailedModels(const char* path) {
    Reader in(path);
    unsigned long version; in.take(&version, 4);
    if (version != FORMAT_VERSION)
        throw std::runtime_error("Unsupported original DXH model version");
    int colors = in.count(); in.count(); in.take(nullptr, size_t(colors) * 16);
    int palettes = in.count(); in.take(nullptr, size_t(palettes) * 1032);
    int textures = in.count(); int textureVersion = 0;
    if (textures) { textureVersion = in.count(); in.take(nullptr, size_t(textures) * 40); }
    in.count(); // longest LOD tag list
    const int lods = in.count(); in.take(nullptr, size_t(lods) * 20);
    TheObjectListLength = in.count();
    if (!TheObjectListLength) throw std::runtime_error("Empty original model database");
    TheObjectList = new ObjectParent[TheObjectListLength];
    static_assert(sizeof(ParentFileRecord) == 48, "DXH parent disk layout");
    for (int i = 0; i < TheObjectListLength; ++i) {
        ParentFileRecord p; in.take(&p, sizeof(p)); auto& obj = TheObjectList[i];
        obj.radius = p.radius; obj.minX = p.minX; obj.maxX = p.maxX;
        obj.minY = p.minY; obj.maxY = p.maxY; obj.minZ = p.minZ; obj.maxZ = p.maxZ;
        obj.RadarSign = p.RadarSign; obj.IRSign = p.IRSign;
        obj.nTextureSets = p.nTextureSets; obj.nDynamicCoords = p.nDynamicCoords;
        obj.nLODs = p.nLODs; obj.nSlots = p.nSlots;
        obj.nSwitches = textureVersion ? p.nSwitches : p.nSwitch;
        obj.nDOFs = textureVersion ? p.nDOFs : p.nDOF;
        if (obj.nDynamicCoords < 0 || obj.nSwitches < 0 || obj.nDOFs < 0 || !std::isfinite(obj.radius))
            throw std::runtime_error("Invalid model geometry metadata");
    }
    for (int i = 0; i < TheObjectListLength; ++i) {
        auto& obj = TheObjectList[i]; if (!obj.nLODs) continue;
        const int points = obj.nSlots + obj.nDynamicCoords;
        if (points) { obj.pSlotAndDynamicPositions = new Ppoint[points]; in.take(obj.pSlotAndDynamicPositions, size_t(points) * sizeof(Ppoint)); }
        // Each DX LOD reference is a 32-byte name, 32-bit index and float range.
        in.take(nullptr, size_t(obj.nLODs) * 40);
    }
    if (in.at != in.bytes.size()) throw std::runtime_error("Unexpected trailing DXH data");
}

DrawableObject::~DrawableObject() { if (parentList) std::terminate(); }

#include "drawbrdg.h"
#include "drawrdbd.h"
float DrawableBridge::GetGroundLevel(float x, float y, Tpoint *normal)
{
    DrawableRoadbed *roadbed;
    Tpoint pos;

    pos.x = x;
    pos.y = y;

    // Find the right segment to govern this point
    // (Note:  The cast below is safe because only roadbeds get added to this list)
    roadbedObjects.ResetTraversal();
    roadbed = (DrawableRoadbed *)roadbedObjects.GetNextAndAdvance();

    while (roadbed)
    {
        ShiAssert(roadbed->GetClass() == Roadbed);

        if (roadbed->OnRoadbed(&pos, normal))
        {
            return pos.z;
        }

        roadbed = (DrawableRoadbed *)roadbedObjects.GetNextAndAdvance();
    }

    // We didn't find a containing segment
    // We used to assert here, but in instant action, we let tanks go on water right now...
    // So, we use the position of the container object (presumably ground level)
    if (normal)
    {
        normal->x = 0.0f;
        normal->y = 0.0f;
        normal->z = -1.0f;
    }

    return position.z;
}
BOOL DrawableRoadbed::OnRoadbed(Tpoint *pos, Tpoint *normal)
{
    float x, y, d;


    // First see if the point is too far away
    if ((fabs(pos->x - position.x) > radius) or
        (fabs(pos->y - position.y) > radius))
    {
        return FALSE;
    }

    // See if we can do a simple case for level segments
    if (tanRampAngle == 0.0f)
    {
        if (normal)
        {
            normal->x = 0.0f;
            normal->y = 0.0f;
            normal->z = -1.0f;
        }

        pos->z =
            position.z - ramp.Y(0.0f); // -Z is up, but height is positive up
    }
    else
    {
        // Tranlate into object space
        x = pos->x - position.x;
        y = pos->y - position.y;

        // Rotate into object space (only worried about distance along bridge axis)
        d = x * cosInvYaw - y * sinInvYaw;

        // Construct the worldspace rotated normal to the ramp
        if (normal)
        {
            normal->x = -tanRampAngle * cosInvYaw;
            normal->y = tanRampAngle * sinInvYaw;
            normal->z = -1.0f;
        }

        // Return the altitude of the ramp at the given point
        pos->z = position.z - ramp.Y(d); // -Z is up, but height is positive up
    }

    return TRUE;
}