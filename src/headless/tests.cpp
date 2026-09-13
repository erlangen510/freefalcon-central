#include "scenario.h"
#include "utils/lzss.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using Bytes = std::vector<unsigned char>;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void rejects(const Bytes& bytes) {
    try { ff::headless::InspectScenarioBytes(bytes); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("malformed archive was accepted");
}
int main() {
    try {
        std::mt19937 rng(12345);
        // Native compressor compatibility, dictionary wrap and overlapping copies.
        for (int n : {1, 2, 7, 8, 9, 17, 256, 4096, 8193, 20000}) {
            for (int mode = 0; mode < 3; ++mode) {
                Bytes input(n), packed(n * 2 + 1024), output(n + 2, 0xA5);
                for (int i = 0; i < n; ++i)
                    input[i] = mode == 0 ? 'A' : mode == 1 ? i % 17 : rng() & 255;
                const int size = LZSS_Compress(input.data(), packed.data(), n);
                check(size > 0, "native compression failed");
                check(LZSS_ExpandChecked(packed.data(), size, output.data() + 1, n) == size,
                      "checked decoder rejected native output");
                check(std::equal(input.begin(), input.end(), output.begin() + 1), "roundtrip mismatch");
                check(output.front() == 0xA5 && output.back() == 0xA5, "output overrun");
                check(LZSS_ExpandChecked(packed.data(), size - 1, output.data() + 1, n) < 0,
                      "truncated stream accepted");
            }
        }
        unsigned char out[8] = {};
        const unsigned char bad_reference[] = {0, 0, 0};
        const unsigned char overflow[] = {1, 'A', 0xF0, 1};
        check(LZSS_ExpandChecked(bad_reference, 3, out, 2) < 0, "unwritten dictionary accepted");
        check(LZSS_ExpandChecked(overflow, 4, out, 4) < 0, "oversized match accepted");
        check(LZSS_ExpandChecked(nullptr, 0, out, 1) < 0, "empty input accepted");
        check(LZSS_ExpandChecked(nullptr, -1, out, 1) < 0, "negative size accepted");
        rejects({});
        rejects({255,255,255,255,0,0,0,0});
        rejects({4,0,0,0,255,255,255,255});
        // One section claims an out-of-bounds payload; no parsing or allocation follows.
        rejects({4,0,0,0,1,0,0,0,5,'x','.','c','m','p',4,0,0,0,255,255,255,255});
        std::cout << "PASS: native codec roundtrips, bounds checks and malformed containers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
