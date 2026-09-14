cmake_minimum_required(VERSION 3.21)
project(ff_campaign_headless LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
get_filename_component(FF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_library(ff_scenario STATIC scenario.cpp ${FF_ROOT}/utils/lzss.cpp)
target_include_directories(ff_scenario PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${FF_ROOT})
add_executable(ff-campaign main.cpp)
target_link_libraries(ff-campaign PRIVATE ff_scenario)
option(BUILD_TESTING "Build standalone scenario tests" ON)
enable_testing()
if(BUILD_TESTING)
  add_executable(ff-campaign-tests tests.cpp)
  target_link_libraries(ff-campaign-tests PRIVATE ff_scenario)
  add_test(NAME scenario-and-codec COMMAND ff-campaign-tests)
endif()
option(FF_BUILD_CAMPAIGN_ENGINE "Build the offline Windows campaign runtime" ${MSVC})
if(NOT FF_BUILD_CAMPAIGN_ENGINE)
  return()
endif()
if(NOT MSVC)
  message(FATAL_ERROR "The campaign runtime currently requires MSVC. Use FF_BUILD_CAMPAIGN_ENGINE=OFF for inspection only.")
endif()
set(FF_ATL_INCLUDE "" CACHE PATH "Optional ATL include directory")
set(FF_ATL_LIB "" CACHE PATH "Optional ATL x64 library directory")
add_library(ff_headless_options INTERFACE)
target_compile_options(ff_headless_options INTERFACE /Zi /GL /Gy /Gw /Zc:inline- /source-charset:.1252 /W0 /EHsc)
target_compile_definitions(ff_headless_options INTERFACE FF_HEADLESS=1 NDEBUG _LIB WIN32 STRICT WIN32_LEAN_AND_MEAN WINDOWS_IGNORE_PACKING_MISMATCH _CRT_SECURE_NO_WARNINGS)
target_include_directories(ff_headless_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}
  ${FF_ROOT} ${FF_ROOT}/include ${FF_ROOT}/campaign/include ${FF_ROOT}/falclib/include
  ${FF_ROOT}/sim/include ${FF_ROOT}/graphics/include ${FF_ROOT}/graphics
  ${FF_ROOT}/codelib/include ${FF_ROOT}/vu2/include ${FF_ROOT}/ui/include
  ${FF_ROOT}/ui95 ${FF_ROOT}/ui95_ext ${FF_ROOT}/comms ${FF_ROOT}/comms/include
  ${FF_ROOT}/acmi/src/include ${FF_ATL_INCLUDE})

function(engine_library name project)
  get_filename_component(project_dir "${FF_ROOT}/${project}" DIRECTORY)
  file(READ "${FF_ROOT}/${project}" content)
  string(REGEX MATCHALL "<ClCompile Include=\"[^\"]+\"" entries "${content}")
  set(sources)
  foreach(entry ${entries})
    string(REGEX REPLACE "<ClCompile Include=\"([^\"]+)\"" "\\1" source "${entry}")
    string(REPLACE "\\" "/" source "${source}")
    string(TOLOWER "${source}" lower)
    if(lower MATCHES "(^|/)(ehandler|dispcfg)\\.cpp$")
      continue()
    endif()
    list(APPEND sources "${project_dir}/${source}")
    if(lower MATCHES "\\.c$")
      # Preserve the legacy C calling declarations. Register with the CXX
      # generator, but let MSVC compile these files as C (as upstream does).
      set_source_files_properties("${project_dir}/${source}" PROPERTIES LANGUAGE CXX COMPILE_FLAGS "/TC")
    endif()
  endforeach()
  add_library(${name} STATIC ${sources})
  target_link_libraries(${name} PUBLIC ff_headless_options)
endfunction()

engine_library(ff_camplib campaign/camplib/camplib.vcxproj)
engine_library(ff_camptask campaign/camptask/camptask.vcxproj)
engine_library(ff_campupd campaign/campupd/campupd.vcxproj)
engine_library(ff_falclib falclib/falclib.vcxproj)
engine_library(ff_vu vu2/vu2.vcxproj)
engine_library(ff_lists codelib/tools/lists/lists.vcxproj)
engine_library(ff_campui campaign/campui/campui.vcxproj)
engine_library(ff_comms comms/comms.vcxproj)
add_library(ff_campaign_engine STATIC engine_host.cpp ${FF_ROOT}/ui/src/f4config.cpp ${FF_ROOT}/utils/lzss.cpp ${FF_ROOT}/tools/isbad/isbad.cpp ${FF_ROOT}/extlibs/tinyxml2/tinyxml2.cpp)
target_sources(ff_campaign_engine PRIVATE presentation.cpp model_support.cpp terrain.cpp watch.cpp
  ${FF_ROOT}/falcsnd/voicemapper.cpp
  ${FF_ROOT}/sim/navaids/tacan.cpp
  ${FF_ROOT}/sim/missile/readin.cpp
  ${FF_ROOT}/sim/simlib/file.cpp
  ${FF_ROOT}/sim/simlib/datafile.cpp)
target_link_libraries(ff_campaign_engine PRIVATE ff_camplib ff_camptask ff_campupd ff_falclib ff_vu ff_lists ff_campui ff_comms user32 gdi32 winmm ws2_32 advapi32 ole32 oleaut32 uuid)
target_link_directories(ff_campaign_engine PUBLIC ${FF_ATL_LIB})
target_link_options(ff-campaign PRIVATE /DEBUG /MAP /LTCG /OPT:REF /OPT:ICF)
target_link_libraries(ff_campaign_engine PRIVATE ff_scenario)
target_link_libraries(ff-campaign PRIVATE ff_campaign_engine)
target_compile_definitions(ff-campaign PRIVATE FF_CAMPAIGN_ENGINE=1)
option(FF_BUILD_NATIVE_DYNAMICS "Also build the standalone missile dynamics compilation probe" OFF)
if(FF_BUILD_NATIVE_DYNAMICS)
  add_library(ff_native_missile_dynamics STATIC
    ${FF_ROOT}/sim/missile/accel.cpp ${FF_ROOT}/sim/missile/aero.cpp
    ${FF_ROOT}/sim/missile/atmos.cpp ${FF_ROOT}/sim/missile/engine.cpp
    ${FF_ROOT}/sim/missile/eom.cpp ${FF_ROOT}/sim/missile/fcs.cpp
    ${FF_ROOT}/sim/missile/gains.cpp ${FF_ROOT}/sim/missile/pitch.cpp
    ${FF_ROOT}/sim/missile/trig.cpp ${FF_ROOT}/sim/missile/yaw.cpp)
  target_link_libraries(ff_native_missile_dynamics PRIVATE ff_headless_options)
endif()
option(FF_BUILD_DEBUG_RUNNER "Build the Windows crash investigation helper" OFF)
option(FF_BUILD_DETAILED_ENGINE "Build the original individual simulation runtime" ON)
if(NOT FF_BUILD_DETAILED_ENGINE)
  message(FATAL_ERROR "The campaign observer now requires FF_BUILD_DETAILED_ENGINE=ON. Use FF_BUILD_CAMPAIGN_ENGINE=OFF for the standalone inspector.")
endif()
if(FF_BUILD_DETAILED_ENGINE)
  target_compile_definitions(ff_headless_options INTERFACE FF_DETAILED_ENGINE=1)
  target_sources(ff_campaign_engine PRIVATE ${FF_ROOT}/sim/simdrive/simatc.cpp detailed.cpp countermeasure_test.cpp region.cpp sim_services.cpp detailed_presentation.cpp physical_world.cpp physical_materials.cpp physical_models.cpp
    ${FF_ROOT}/sim/displays/sms.cpp ${FF_ROOT}/sim/displays/mslsms.cpp ${FF_ROOT}/sim/displays/bombsms.cpp ${FF_ROOT}/sim/displays/gunsms.cpp
    ${FF_ROOT}/sim/eject/simeject.cpp ${FF_ROOT}/sim/navaids/navsystem.cpp
    ${FF_ROOT}/graphics/objects/drawbsp.cpp
    ${FF_ROOT}/graphics/objects/drawtrcr.cpp ${FF_ROOT}/graphics/objects/drawgrnd.cpp ${FF_ROOT}/graphics/objects/drawguys.cpp
    ${FF_ROOT}/graphics/bsplib/objectinstance.cpp
    ${FF_ROOT}/graphics/utils/matrix.cpp ${FF_ROOT}/graphics/3dlib/math.cpp ${FF_ROOT}/graphics/weather/tod.cpp ${FF_ROOT}/graphics/weather/star.cpp ${FF_ROOT}/graphics/utils/timemgr.cpp)
  target_compile_definitions(ff_campaign_engine PRIVATE FF_DETAILED_ENGINE=1)
  engine_library(ff_simlib sim/simlib/simlib.vcxproj)
  engine_library(ff_missile sim/missile/missile.vcxproj)
  engine_library(ff_radar sim/radar/radar.vcxproj)
  engine_library(ff_irst sim/irst/irst.vcxproj)
  engine_library(ff_rwr sim/rwr/rwr.vcxproj)
  engine_library(ff_visual sim/visual/visual.vcxproj)
  target_link_libraries(ff_campaign_engine PRIVATE ff_simlib ff_missile ff_radar ff_irst ff_rwr ff_visual ff_faults ff_ground ff_guns ff_bomb ff_aircraft ff_airframe ff_digi ff_fcc ff_helo ff_hdigi ff_feature)
  foreach(component aircraft airframe ground feature helo digi hdigi fcc guns bomb faults)
    engine_library(ff_${component} sim/${component}/${component}.vcxproj)
    # Vehicle runtime linkage follows the native weapon runtime.
  endforeach()
endif()
if(FF_BUILD_DEBUG_RUNNER)
  add_executable(ff-debug-runner debug_runner.cpp)
  target_link_libraries(ff-debug-runner PRIVATE dbghelp)
endif()
