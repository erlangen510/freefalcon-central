// Terrain material classification from TViewPoint::GetGroundType and TextureDB.
// Uses native path/area metadata and the last near-textured LOD, without textures.
#include "stdhdr.h"
#include "otwdrive.h"
#include "terrtex.h"
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace {
struct Tile { std::vector<TexArea> areas; std::vector<TexPath> paths; };
struct Set { unsigned char type; std::vector<Tile> tiles; };
std::vector<Set> sets;
std::vector<unsigned char> posts;
std::vector<unsigned long> offsets;
int width, height, postSize;
float spacing;
template<class T> T read(std::ifstream& file) {
    T value{}; file.read(reinterpret_cast<char*>(&value),sizeof value);
    if (!file) throw std::runtime_error("Truncated detailed terrain material data");
    return value;
}
int count(std::ifstream& file,int limit) {
    int n=read<int>(file); if(n<0 || n>limit) throw std::runtime_error("Invalid terrain material count"); return n;
}
}
void LoadDetailedMaterials(const char* path) {
    std::ifstream map(std::string(path)+"/terrain/theater.map",std::ios::binary);
    const float base=read<float>(map); map.seekg(16);
    int levels=count(map,9), lod=count(map,8);
    if(!levels || lod>=levels || !std::isfinite(base) || base<=0) throw std::runtime_error("Invalid material LOD");
    spacing=base*(1<<lod);
    map.seekg(1052+lod*8); width=count(map,4096); height=count(map,4096);
    map.seekg(1052+levels*8); postSize=(read<unsigned long>(map)&1)?9:7;
    if(!width || !height) throw std::runtime_error("Empty material terrain");
    std::ifstream index(std::string(path)+"/terrain/theater.o"+std::to_string(lod),std::ios::binary);
    offsets.resize(static_cast<size_t>(width)*height);
    index.read(reinterpret_cast<char*>(offsets.data()),offsets.size()*4);
    std::ifstream data(std::string(path)+"/terrain/theater.l"+std::to_string(lod),std::ios::binary|std::ios::ate);
    if(!index || !data || data.tellg()<=0 || data.tellg()>1024LL*1024*1024) throw std::runtime_error("Missing detailed material terrain LOD");
    posts.resize(static_cast<size_t>(data.tellg()));data.seekg(0);data.read(reinterpret_cast<char*>(posts.data()),posts.size());
    if(!data) throw std::runtime_error("Truncated material terrain");
    for(auto offset:offsets) if(offset>posts.size() || posts.size()-offset<256*postSize) throw std::runtime_error("Invalid material block offset");
    std::ifstream bin(std::string(path)+"/texture/texture.bin",std::ios::binary);
    sets.clear(); sets.resize(count(bin,256)); const int total=count(bin,4096); int actual=0;
    for(auto& set:sets) {
        set.tiles.resize(count(bin,16)); actual+=static_cast<int>(set.tiles.size()); set.type=read<unsigned char>(bin);
        for(auto& tile:set.tiles) {
            char filename[20]; bin.read(filename,20);
            tile.areas.resize(count(bin,4096)); tile.paths.resize(count(bin,4096));
            for(auto& area:tile.areas) { area=read<TexArea>(bin); if(!std::isfinite(area.x)||!std::isfinite(area.y)||!std::isfinite(area.radius)||area.radius<0) throw std::runtime_error("Invalid terrain area"); }
            for(auto& segment:tile.paths) { segment=read<TexPath>(bin); if(!std::isfinite(segment.x1)||!std::isfinite(segment.x2)||!std::isfinite(segment.y1)||!std::isfinite(segment.y2)||!std::isfinite(segment.width)||segment.width<0) throw std::runtime_error("Invalid terrain path"); }
        }
    }
    if(actual!=total || bin.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("Terrain material catalog size mismatch");
}
int OTWDriverClass::GetGroundType(float x,float y) {
    if(posts.empty() || sets.empty() || !std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("Uninitialized or invalid material query");
    const int row=static_cast<int>(floor(x/spacing)),col=static_cast<int>(floor(y/spacing));
    if(row<0 || col<0 || row>=height*16 || col>=width*16) throw std::runtime_error("Material query outside theater");
    const size_t offset=offsets[(row/16)*width+col/16]+((row%16)*16+col%16)*postSize;
    unsigned long tex=0; memcpy(&tex,posts.data()+offset,postSize==7?2:4);
    const int setIndex=(tex>>4)&255,tileIndex=tex&15;
    // Matches native TextureDB's out-of-range classification.
    if(setIndex>=sets.size()) return 0;
    const auto& set=sets[setIndex]; if(tileIndex>=set.tiles.size()) return set.type;
    const auto& tile=set.tiles[tileIndex]; const float px=x-row*spacing,py=y-col*spacing;
    for(const auto& p:tile.paths) {
        const float radius=p.width*.5f;
        if(px+radius<min(p.x1,p.x2)||px-radius>max(p.x1,p.x2)||py+radius<min(p.y1,p.y2)||py-radius>max(p.y1,p.y2)) continue;
        const float dx=p.x2-p.x1,dy=p.y2-p.y1,length=sqrt(dx*dx+dy*dy);
        if(length>0 && fabs(dy*(px-p.x1)-dx*(py-p.y1))/length<radius) return p.type;
    }
    for(const auto& a:tile.areas) {
        const float dx=px-a.x,dy=py-a.y;
        if(dx*dx+dy*dy<a.radius*a.radius) return a.type;
    }
    return set.type;
}
