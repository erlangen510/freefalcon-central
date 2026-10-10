// Per-objective site layouts: see objective_layout.h.
//
// Feature placements go through ObjectiveClass::feature_layout, read by GetFeatureOffset and
// GetFeatureFacing. Point lists (runway, taxi, parking, SAM, artillery, dock points) move through
// ObjectiveClass::point_transform, applied by TranslatePointData, the one place that turns point
// offsets into positions. A rotated airbase also needs its own runway headings, which every user
// reaches through the objective's class data (PtDataIndex): the objective gets a private copy of
// its class record whose PtDataIndex points at a copy of the template's point headers with the
// headings turned (the headers keep pointing at the template's points).
#include "stdhdr.h"
#include "f4vu.h"
#include "entity.h"
#include "classtbl.h"
#include "camplist.h"
#include "objectiv.h"
#include "ptdata.h"
#include "sim/include/atcbrain.h"
#include "objective_layout.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

extern short NumPtHeaders;

namespace {

struct Applied {
    ObjectiveLayout layout;
    std::vector<ObjectiveFeaturePlacement> placements;
    ObjectivePointTransform points{};
};

std::map<VU_ID, Applied> layouts;
std::deque<ObjClassDataType> classCopies;  // stable addresses for static_data.class_data

constexpr float Pi = 3.14159265358979f;

ObjClassDataType* TemplateClass(ObjectiveClass* o) {
    return static_cast<ObjClassDataType*>(Falcon4ClassTable[o->Type() - VU_LAST_ENTITY_TYPE].dataPtr);
}

// Rotates an (east, north) offset clockwise by the layout rotation, then shifts it.
void Move(const ObjectiveLayout& l, float& x, float& y) {
    const float a = l.rotation * Pi / 180.0f, c = std::cos(a), s = std::sin(a);
    const float east = x * c + y * s, north = -x * s + y * c;
    x = east + l.shiftX;
    y = north + l.shiftY;
}

template <typename T>
T* Grow(T* table, int count, int extra) {
    T* grown = new T[count + extra];
    std::copy(table, table + count, grown);
    delete[] table;
    return grown;
}

// Appends a copy of the template's header chain with runway headings turned; returns its first header.
short CopyHeaders(const ObjectiveLayout& l, short firstHeader) {
    std::vector<short> headers;
    for (short h = firstHeader; h > 0; h = PtHeaderDataTable[h].nextHeader) {
        if (h >= NumPtHeaders || headers.size() > 256) throw std::runtime_error("Broken point header chain");
        headers.push_back(h);
    }
    if (NumPtHeaders + int(headers.size()) > SHRT_MAX) throw std::runtime_error("Point header table is full (16-bit indices)");
    PtHeaderDataTable = Grow(PtHeaderDataTable, NumPtHeaders, int(headers.size()));
    const short base = NumPtHeaders;
    const float a = l.rotation * Pi / 180.0f;
    for (size_t i = 0; i < headers.size(); ++i) {
        PtHeaderDataType h = PtHeaderDataTable[headers[i]];
        h.nextHeader = i + 1 < headers.size() ? short(base + i + 1) : 0;
        if (h.cosHeading != 0 || h.sinHeading != 0) {
            // Runway heading: data in degrees; cosHeading north, sinHeading east (ATCBrain).
            const float c = h.cosHeading, s = h.sinHeading;
            h.cosHeading = c * std::cos(a) - s * std::sin(a);
            h.sinHeading = s * std::cos(a) + c * std::sin(a);
            h.data = short(((h.data + int(std::lround(l.rotation))) % 360 + 360) % 360);
        }
        PtHeaderDataTable[base + i] = h;
    }
    NumPtHeaders = short(NumPtHeaders + headers.size());
    return base;
}

void ResetBrain(ObjectiveClass* o) {
    delete o->brain;
    o->brain = nullptr;
    if (o->GetType() == TYPE_AIRBASE || o->GetType() == TYPE_AIRSTRIP) o->brain = new ATCBrain(o);
}

}  // namespace

void SetObjectiveLayout(ObjectiveClass* o, const ObjectiveLayout& layout) {
    ObjClassDataType* c = TemplateClass(o);
    if (!c || layout.classId != o->Type() - VU_LAST_ENTITY_TYPE)
        throw std::runtime_error("Layout class does not match objective " + std::to_string(o->Id().num_));
    Applied& applied = layouts[o->Id()];
    applied.layout = layout;
    applied.placements.assign(c->Features, {});
    for (int f = 0; f < c->Features; ++f) {
        const FeatureEntry& e = FeatureEntryDataTable[c->FirstFeature + f];
        auto& p = applied.placements[f];
        p.x = e.Offset.x;
        p.y = e.Offset.y;
        p.z = e.Offset.z;
        Move(layout, p.x, p.y);
        p.facing = short(((e.Facing + int(std::lround(layout.rotation))) % 360 + 360) % 360);
    }
    for (const auto& f : layout.features) {
        if (f.index < 0 || f.index >= c->Features) throw std::runtime_error("Layout feature index out of range");
        auto& p = applied.placements[f.index];
        p.x = f.x;
        p.y = f.y;
        p.facing = short((f.facing % 360 + 360) % 360);
    }
    o->feature_layout = applied.placements.data();
    const float a = layout.rotation * Pi / 180.0f;
    applied.points = {std::cos(a), std::sin(a), layout.shiftX, layout.shiftY};
    o->point_transform = &applied.points;
    if (c->PtDataIndex > 0 && layout.rotation != 0) {
        classCopies.push_back(*c);
        classCopies.back().PtDataIndex = CopyHeaders(layout, c->PtDataIndex);
        o->static_data.class_data = &classCopies.back();
    } else {
        o->static_data.class_data = c;
    }
    ResetBrain(o);
}

int LoadObjectiveLayouts(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) return 0;
    std::vector<std::pair<VU_ID, ObjectiveLayout>> read;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "layout") {
            std::string id;
            ObjectiveLayout l;
            if (!(in >> id >> l.classId >> l.rotation >> l.shiftX >> l.shiftY) || id.find(':') == std::string::npos)
                throw std::runtime_error("Malformed layout line: " + line);
            const auto colon = id.find(':');
            read.emplace_back(VU_ID(std::stoul(id.substr(0, colon)), std::stoul(id.substr(colon + 1))), l);
        } else if (kind == "feature") {
            ObjectiveLayout::Feature f{};
            if (read.empty() || !(in >> f.index >> f.x >> f.y >> f.facing))
                throw std::runtime_error("Malformed layout feature line: " + line);
            read.back().second.features.push_back(f);
        } else {
            throw std::runtime_error("Unknown layout line: " + line);
        }
    }
    int applied = 0;
    for (const auto& [id, l] : read) {
        auto* o = dynamic_cast<ObjectiveClass*>(vuDatabase->Find(id));
        if (!o || o->Type() - VU_LAST_ENTITY_TYPE != l.classId) continue;
        SetObjectiveLayout(o, l);
        ++applied;
    }
    return applied;
}

void SaveObjectiveLayouts(const std::filesystem::path& file) {
    std::ofstream out(file);
    out << "# objective site layouts: layout <id> <class> <rotation-deg> <shift-x-ft> <shift-y-ft>; "
           "feature <index> <x-ft> <y-ft> <facing-deg>\n";
    for (const auto& [id, a] : layouts) {
        auto* o = dynamic_cast<ObjectiveClass*>(vuDatabase->Find(id));
        if (!o || o->feature_layout != a.placements.data()) continue;
        const auto& l = a.layout;
        out << "layout " << id.creator_ << ':' << id.num_ << ' ' << l.classId << ' ' << l.rotation << ' ' << l.shiftX
            << ' ' << l.shiftY << '\n';
        for (const auto& f : l.features) out << "feature " << f.index << ' ' << f.x << ' ' << f.y << ' ' << f.facing << '\n';
    }
    if (!out) throw std::runtime_error("Cannot write objective layouts: " + file.u8string());
}

int ObjectiveLayoutCount() {
    int n = 0;
    for (const auto& [id, a] : layouts) {
        auto* o = dynamic_cast<ObjectiveClass*>(vuDatabase->Find(id));
        n += o && o->feature_layout == a.placements.data();
    }
    return n;
}
