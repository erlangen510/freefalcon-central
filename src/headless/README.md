# Headless campaign host: first stage

**Implemented:** a console executable that reads real `.cam` files, validates
container boundaries, decompresses the campaign/unit/objective/objective-delta
sections, and prints saved metadata as JSON. The checked decoder uses the same
format as FreeFalcon's native LZSS compressor, with source/output bounds and
dictionary-reference checks.

**Not implemented:** VU entity instantiation, complete world initialization,
campaign stepping, AI/mission execution, combat, logistics, network API or resets.
This is the data-loading stage of the host, not a working campaign simulator.
`run` returns exit code 3 and `simulation_advanced: false`; it does not pretend
to advance the game by incrementing a standalone clock.

## Build and test

From this repository's root (CMake 3.21+, C++17 compiler):

```sh
cmake -S src/headless -B build/headless
cmake --build build/headless --config Release
ctest --test-dir build/headless -C Release --output-on-failure
```

Windows uses a console `wmain` entry point so Korean/Unicode paths work. No ATL,
DirectX, graphics device, sound device, UI or installed game is needed for the
default target. Supply a scenario file explicitly; no registry lookup is used.

```powershell
.\build\headless\Release\ff-campaign.exe inspect C:\FreeFalcon6\campaign\SAVE\save0.cam
```

Exit codes: 0 = inspection succeeded, 1 = invalid/missing scenario, 2 = invalid
CLI usage, 3 = campaign execution not yet implemented. A successful inspection
does not certify all unit records, external theater dependencies or model behavior.
Only the metadata prefix layouts for format versions 73 and 99 are accepted.
Version 99 was exercised using the FF6 files linked by the upstream build guide.
`recorded_unit_count` and `recorded_objective_count` are save-header counts,
not counts of instantiated entities. Saved title strings can be placeholders.

## Legacy engine investigation

`-DFF_BUILD_ENGINE_PROBE=ON` additionally builds the actual campaign, campaign UI
support, VU, Falcon common and list libraries. This optional MSVC-only link probe
is intentionally unfinished and currently fails to link. It retains references
to the real `DoCampaignLoop` without calling it on uninitialized data.
Optional extracted ATL paths can be provided as `FF_ATL_INCLUDE` and `FF_ATL_LIB`.

The latest probe leaves 324 unresolved references, including presentation,
detailed simulation and application-owned state. That count depends on the
exact link command and is not a count of engine defects. No fake implementations
were introduced to silence those errors. The normal inspection executable has
no link dependency on this unfinished target.

The next step is an explicit host bootstrap retaining the required VU/Falcon
services, with presentation dependencies isolated and a bounded tick operation
driving the real campaign loop and event queue.
