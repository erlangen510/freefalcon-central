# Current detailed runtime (2026-09-14)

Control/action command readers share file deletion so clients can atomically
replace commands while the observer polls. Commands larger than 4096 bytes are
ignored. The Godot action writer retries transient rename failures up to twenty
times asynchronously and releases its pending state if transmission fails.

Aircraft radar power uses the same `action.txt` protocol with `radar_on` and
`radar_off`. These commands call the original sensor SetPower immediately,
including while paused; `applied` confirms the resulting power state. Power-off
clears the radar track and emission through the original radar implementation.
Power-on also invokes native SetEmitting(TRUE), since SetPower alone retains
the prior shutdown emission state. Native fault/range gates still apply.
Missing radar and a denied
power change return `radar_unavailable` and `sensor_power_denied` respectively.
Only local, awake, living aircraft inside the active region accept these
commands. Radar power is retained by flight ID and pilot slot across reaggregation/re-entry
within this process. Save/resume persistence is not implemented.

Native per-aircraft countermeasure commands use a separate atomic `action.txt`:
`sequence creator_id entity_number chaff|flare`. IDs are the two decimal halves
of a snapshot actor ID. Sequence must strictly increase; malformed numeric fields
and trailing fields are ignored. `state.json.unit_action` returns the last
acknowledged sequence and acceptance/rejection status. `queued` acknowledges a
native command flag, not a guaranteed release; aircraft/fault gates still run
on the next physical frame. While paused, inventories remain unchanged until
the user steps or resumes. Only awake local airborne aircraft inside the active
region with available stores can accept commands. Duplicates, unsupported
actions/actors, empty stores and inactive regions are rejected. The existing
control heartbeat is still required; action files do not extend its lease.

Snapshots distinguish `chaff`, `flare` and `debris`, retain `native_class_name`,
and associate weapons/decoys with `parent_id`; displayed team is the launcher's.
Aircraft `countermeasures` contains current native chaff/flare station counts.

The aggregate-only boundaries below describe the earlier checkpoint.
`FF_BUILD_DETAILED_ENGINE=ON` now links original actors, AI, sensors, weapons,
terrain/collision and damage into the observer with a 20 ms physical clock.
Regional capabilities advertise native detailed combat and projectile state.
The parent workspace's `scripts/verify-detailed.py` exercises ten diagnostics,
including five-minute ground and opposing-aircraft combat with campaign loss
round-trip. This remains experimental; all platforms/weapons and long detailed
runs have not been validated. Player cockpit input remains unsupported.
See the parent `headless-validation/PROGRESS.md` for current evidence and limits.

---

# Offline campaign host

`ff-campaign run` executes the native aggregate FreeFalcon campaign engine in a
console process. It initializes real class tables, VU entities and team managers,
loads a campaign, runs initial planning, then advances in five-second steps using
`DoCampaignLoop`, `UpdateRealUnits` and both VU queues. No graphics or audio device
is created. ATM/GTM/NTM, movement, aggregate combat, supply and triggers remain
native model operations.

## Build

Windows x64, CMake 3.21+, Visual Studio C++ and ATL are required for execution.
The tested toolchain is VS 2022/v143 with Windows SDK 10.0.26100.0.
From the repository root:

```powershell
cmake -S src/headless -B build/headless -A x64 -DFF_BUILD_CAMPAIGN_ENGINE=ON
cmake --build build/headless --config Release
ctest --test-dir build/headless -C Release --output-on-failure
```

For extracted ATL, pass `FF_ATL_INCLUDE` and `FF_ATL_LIB`. Set
`FF_BUILD_CAMPAIGN_ENGINE=OFF` for the C++17 inspector without Windows/ATL.
`FF_BUILD_DEBUG_RUNNER=ON` builds an optional Windows exception investigation tool.
Set `_NO_DEBUG_HEAP=1` when reproducing normal-runtime heap faults under this
helper. Windows otherwise changes allocation behavior for a debugged child.
The helper inherits redirected standard handles and logs exception registers;
the generated `ff-campaign.map` can resolve module-relative addresses if PDB
symbol loading is unavailable.

## Run

```powershell
.\build\headless\Release\ff-campaign.exe run C:\FF6Data save0 60 1
.\build\headless\Release\ff-campaign.exe inspect C:\FF6Data\campaign\SAVE\save0.cam
```

`run <data-root> <scenario-basename> <minutes> [seed]` accepts 1?10080 minutes
and a 32-bit unsigned initial seed (default 1). Only the three FF6 Korea
format-99 scenarios have been exercised so far. The data root must contain:

- `campaign/SAVE`: scenarios and their companion files, Korea terrain/names,
  Falcon4.AII/TT, priorities, strings and other campaign support files.
- `terrdata/objects`: native Falcon4 class tables and associated object data.
- `terrdata/korea/terrain/Theater.map` and `Theater.MEA`: coarse elevation data.
- `sim/MISDATA`, `sim/RADAR`, and other sim data extracted from the shipped
  `Zips/Simdata.zip` into the data root. Leaving only the ZIP is insufficient.

Game data is not shipped. The local parent workspace already has the extracted
FF6 installer data; the installer itself does not need to run. Missing required
files and invalid campaign archive bounds are checked before engine startup.

Success is exit code 0 and a final JSON report on stdout; diagnostics are on
stderr. Invalid input/runtime errors use exit 1, usage errors exit 2, and an
inspector-only build returns exit 3 for `run`.

## Report and interpretation

The report includes campaign times, completed steps, active unit/objective counts,
new flights, units whose grid location/strength/supply/orders changed, native combat
message counts and reported losses. Units are sampled after every step; all must
remain aggregate. Active-list counts exclude inactive units in save headers.
`reported_combat_losses` combines losses reported by native unit/feature damage
processing; it is not an aircraft-loss statistic. Execution does not validate
historical accuracy, balance, or victory outcomes.

## Boundaries

- `engine_host.cpp` owns process startup and stepping. The process exits after one
  run. Global state is not reset in-process, and save/resume is not yet exposed.
- Native history working files go into the supplied campaign/save directory.
  Concurrent runs require separate data copies. Initial seed is exposed, but
  deterministic replay is not yet guaranteed.
- The offline session has the native default player country for initiative
  calculations, with no player aircraft. Player-side assumptions remain in the model.
- `FF_HEADLESS` excludes UI, online and detailed aircraft/sim transitions. Unsupported
  simulation paths throw explicit errors; presentation-only callbacks have no renderer.
- `terrain.cpp` reads the native MEA header/table and preserves axis conversion and
  vertical flip. `presentation.cpp` retains scalar weather conditions used by the
  native `WeatherClass`; drawable clouds and textures are absent.
- `model_support.cpp` contains native waypoint timing/airspeed, geometry, entity
  comparison and radar-table loading routines extracted from their UI/sim files.
  FF6's radar list declares 170 names but contains 169. Its final slot explicitly
  reuses the preceding radar, matching the original reader's reuse behavior.
- Legacy runtime paths use the Windows ANSI code page, must fit MAX_PATH and must
  not contain `%`. Inspection itself uses Unicode filesystem paths.

The parent workspace provides `scripts/verify-campaigns.py`: three one-hour runs,
a six-hour run, and malformed-request/missing-data rejection checks. Its measured
checkpoint is `headless-validation/PROGRESS.md` in the parent repository.

## Interactive observer host

`ff-campaign watch <data-root> <scenario> <session-directory> [seed]` runs the same
native aggregate campaign with wall-clock pacing and external pause/step controls.
Initial planning runs before the first paused snapshot. Rates are 1, 5, 20 and 100
campaign seconds per wall second; native steps remain five campaign seconds.

The host exports `catalog.json` (teams, vehicles, weapons, objectives/features),
`terrain.bin` (native cover/relief/road/rail cells) and `state.json` (latest dynamic
state, loadouts, routes, damage and control acknowledgement). JSON and terrain
replacement is atomic. Snapshots are published about every 500 ms, on the campaign
thread. Existing metadata is not intended for reuse across sessions or scenarios.

Write `control.txt` atomically as five whitespace-separated integers:
`sequence speed paused cumulative_steps stop`. Sequence must increase, speed must
be 1/5/20/100, paused/stop are 0/1, and cumulative_steps cannot decrease or jump by
more than 100. Each new step request is consumed once while paused. Steps queued
while running are discarded. Send a new sequence heartbeat every two seconds;
the process stops after 20 seconds without a valid new command. It also stops at
the existing seven-day run limit. Logs/final JSON are redirected into the session.

`watch.cpp` reads existing loadout records directly. It avoids the random-consuming
`GetUnitWeaponCount` getter. Default vehicle weapons are catalog data, not current
ammunition. Supply/morale getters are only meaningful for implementing subclasses.
Feature offsets use the native east/north ordering; feature status values are
0 normal, 1 repaired, 2 damaged, 3 destroyed. Model fidelity and deterministic
replay limitations of batch mode continue to apply.

Both `run` and `watch` hold an exclusive `.headless.lock` handle in the data root
to prevent competing history-file writers. The handle is released on process exit.
Use separate data copies for concurrent campaigns. The parent workspace contains
the Godot observer, launcher and real-data protocol/UI integration checks.

### Regional observer (schema 2)

Optional control suffix: `region <0|1> <east-grid> <north-grid> <radius-km>`.
Coordinates must be finite and in bounds; radius is 2..20 km. An active region
caps speed at 10x in the native host (10 is now a valid rate). Legacy five-field
commands retain the current region and cannot bypass the cap. Explicit region 0
leaves it. This currently selects an observer region, **not deaggregated combat**.

Catalog capabilities advertise `regional_3d: true`, `native_detailed_combat: false`.
Snapshots report region geometry, `combat_model: aggregate`,
`native_detailed_status: not_implemented`, `projectiles_available: false`.
The current headless detailed-transition guards remain active.

The catalog adds feature class IDs and headings; unit state adds campaign altitude
in meters and heading in degrees. `elevation.bin` is a grid-sized uint16 LE meter
array sampled from native MEA. It is coarse observer elevation, not collision
terrain. Consumers must not mistake expanded aggregate counts for native
individual-object positions.

`FF_BUILD_NATIVE_DYNAMICS=ON` compiles ten original missile dynamics sources into
`ff_native_missile_dynamics`. This archive is preparation for the native detailed
port; it is not linked into `ff-campaign` and is not an executable flight test.
