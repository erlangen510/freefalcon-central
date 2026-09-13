#pragma once
#include <stdexcept>
#include <string>

namespace ff_headless {
struct CombatCounters {
    unsigned long long engagements = 0;
    unsigned long long losses = 0;
};
extern CombatCounters combat;
// An offline campaign host must never silently substitute detailed simulation.
[[noreturn]] inline void unsupported(const char* operation) {
    throw std::runtime_error(std::string("Unsupported headless operation: ") + operation);
}
}
