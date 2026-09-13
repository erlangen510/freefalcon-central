#include "stdhdr.h"
#include "tmap.h"
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cmath>

TMap TheMap;
// Read only the native coarse elevation table needed by campaign detection and
// routing. No terrain textures, streaming thread or graphics device is started.
int TMap::Setup(const char* path) {
    std::ifstream header(std::string(path) + "/Theater.map", std::ios::binary);
    float feetPerPost;
    header.read(reinterpret_cast<char*>(&feetPerPost), sizeof(feetPerPost));
    header.read(reinterpret_cast<char*>(&MEAwidth), sizeof(MEAwidth));
    header.read(reinterpret_cast<char*>(&MEAheight), sizeof(MEAheight));
    header.read(reinterpret_cast<char*>(&FTtoMEAcell), sizeof(FTtoMEAcell));
    if (!header || MEAwidth <= 0 || MEAheight <= 0 || MEAwidth > 8192 || MEAheight > 8192 ||
        !std::isfinite(FTtoMEAcell) || FTtoMEAcell <= 0)
        throw std::runtime_error("Invalid or missing terrain MEA header");
    std::ifstream data(std::string(path) + "/Theater.MEA", std::ios::binary);
    std::vector<Int16> values(static_cast<size_t>(MEAwidth) * MEAheight);
    for (int row = MEAheight - 1; row >= 0; --row)
        data.read(reinterpret_cast<char*>(&values[static_cast<size_t>(row) * MEAwidth]), MEAwidth * sizeof(Int16));
    if (!data) throw std::runtime_error("Invalid or missing terrain MEA data");
    if (initialized) Cleanup();
    MEAarray = new Int16[values.size()];
    std::copy(values.begin(), values.end(), MEAarray);
    initialized = TRUE;
    return 1;
}
float TMap::GetMEA(float north, float east) {
    if (!initialized || !std::isfinite(north) || !std::isfinite(east))
        throw std::runtime_error("Elevation requested without valid terrain/coordinates");
    const int row = min(max(FloatToInt32(north * FTtoMEAcell), 0), MEAheight - 1);
    const int col = min(max(FloatToInt32(east * FTtoMEAcell), 0), MEAwidth - 1);
    return MEAarray[row * MEAwidth + col];
}
void TMap::Cleanup() { if (initialized) delete[] MEAarray; MEAarray = nullptr; initialized = FALSE; }
