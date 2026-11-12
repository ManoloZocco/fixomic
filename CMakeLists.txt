cmake_minimum_required(VERSION 3.25)

# Must be set before project(). Architectures stay overridable (-DCMAKE_OSX_ARCHITECTURES=...)
# so release builds can be universal; dev builds default to the native arch.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum macOS version")

project(fixomic VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# AddressSanitizer for fixomic_core and everything linking it (app, tests). Use a separate build
# directory, e.g. cmake -S . -B build-asan -G Ninja -DFIXOMIC_ASAN=ON. The prebuilt WebRTC APM
# static library is not instrumented.
option(FIXOMIC_ASAN "Build with AddressSanitizer" OFF)

include(cmake/Dependencies.cmake)

# ---- core library (everything except src/app and the UI on top of it) --------------------------
add_library(fixomic_core STATIC
    src/core/Config.cpp
    src/core/ConfigStore.cpp
    src/core/LatencyBudget.cpp
    src/dsp/AecStage.cpp
    src/dsp/PluginChain.cpp
    src/dsp/ProcessingGraph.cpp
    src/engine/EngineController.cpp
    src/platform/mac/CoreAudioDevices.mm
    src/platform/mac/SystemAudioTap.mm
    src/platform/mac/AggregateIO.mm
    src/platform/mac/DemandMonitor.mm
    src/platform/mac/MacEngineBackend.mm
    src/platform/mac/WakeObserver.mm
    src/platform/mac/Permissions.mm
    src/platform/mac/DriverInstaller.mm
    src/platform/mac/LoginItem.mm
    src/platform/mac/StatusItem.mm
    src/plugins/PluginCatalog.cpp
    src/plugins/PluginWindows.cpp
    src/ui/ChainModel.cpp    # the UI's pure logic: no AppController, no GUI
    src/ui/StatusModel.cpp
)
# The Core Audio layer (and its Objective-C++ tests) uses ARC (JUCE's own .mm sources are built without it).
set_source_files_properties(
    src/platform/mac/CoreAudioDevices.mm
    src/platform/mac/SystemAudioTap.mm
    src/platform/mac/AggregateIO.mm
    src/platform/mac/DemandMonitor.mm
    src/platform/mac/MacEngineBackend.mm
    src/platform/mac/WakeObserver.mm
    src/platform/mac/Permissions.mm
    src/platform/mac/DriverInstaller.mm
    src/platform/mac/LoginItem.mm
    src/platform/mac/StatusItem.mm
    tests/MacEngineTests.mm
    tests/PlatformServicesTests.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(fixomic_core PUBLIC src)
target_compile_features(fixomic_core PUBLIC cxx_std_20)
if(FIXOMIC_ASAN)
    target_compile_options(fixomic_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fixomic_core PUBLIC -fsanitize=address)
endif()

# JUCE modules are INTERFACE libraries whose unity-build sources get compiled into every
# target that links them (even through COMPILE_ONLY). To compile them exactly once, only
# fixomic_core links them (PRIVATE: the sources and link-only requirements stay here). The
# compile-time usage requirements (definitions, include dirs, C++ standard) are re-exported
# explicitly to everything that links fixomic_core (fixomic_app_lib, and through it fixomic and
# fixomic_tests; fxprobe; the test scan worker).
set(FIXOMIC_JUCE_MODULES
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_gui_extra
)
target_link_libraries(fixomic_core
    PRIVATE
        ${FIXOMIC_JUCE_MODULES}
        webrtc::apm
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    PUBLIC
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework AVFoundation"
        "-framework ServiceManagement"
)

# Transitive closure of the modules above (property reads are not transitive), plus the
# header-only helper targets they pull in.
set(FIXOMIC_JUCE_USAGE_TARGETS
    juce_core juce_events juce_data_structures juce_audio_basics juce_audio_devices
    juce_audio_formats juce_audio_processors_headless juce_audio_processors juce_audio_utils
    juce_graphics juce_gui_basics juce_gui_extra
    juce_atomic_wrapper juce_vst3_headers juce_lilv_headers juce_ara_headers
)
foreach(usage_target IN LISTS FIXOMIC_JUCE_USAGE_TARGETS)
    target_compile_definitions(fixomic_core PUBLIC
        "$<TARGET_PROPERTY:${usage_target},INTERFACE_COMPILE_DEFINITIONS>")
    target_include_directories(fixomic_core SYSTEM PUBLIC
        "$<TARGET_PROPERTY:${usage_target},INTERFACE_INCLUDE_DIRECTORIES>")
endforeach()
target_compile_definitions(fixomic_core PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
)

# ---- app library: AppController and the menu-bar UI on top of it --------------------------------
# Everything of the app except its entry point (src/app/Main.cpp), so the tests can link the UI
# components too. Depends on fixomic_core, never the other way round.
add_library(fixomic_app_lib STATIC
    src/app/AppController.cpp
    src/ui/ChainList.cpp
    src/ui/MainPanel.cpp
    src/ui/PanelParts.cpp
    src/ui/PluginPicker.cpp
    src/ui/Snapshot.cpp
    src/ui/Theme.cpp
    src/ui/TrayIcon.cpp
)
target_link_libraries(fixomic_app_lib
    PUBLIC
        fixomic_core
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- app bundle ------------------------------------------------------------
# LSUIElement=true: menu-bar only, no Dock icon.
# JUCE parses PLIST_TO_MERGE as a full <plist><dict> document and merges it into Info.plist.
set(FIXOMIC_PLIST_ENTRIES [=[<plist>
  <dict>
    <key>LSUIElement</key>
    <true/>
    <key>NSAudioCaptureUsageDescription</key>
    <string>fixomic ascolta l'audio di sistema per riconoscere e cancellare l'eco che rientra nel microfono.</string>
  </dict>
</plist>]=])

juce_add_gui_app(fixomic
    PRODUCT_NAME "fixomic"
    BUNDLE_ID "it.manolozocco.fixomic"
    VERSION "${PROJECT_VERSION}"
    COMPANY_NAME "Manolo Zocco"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "fixomic ha bisogno del microfono per elaborare la tua voce, cancellare l'eco e inviarla alle altre app."
    PLIST_TO_MERGE "${FIXOMIC_PLIST_ENTRIES}"
)

target_sources(fixomic PRIVATE src/app/Main.cpp)
target_compile_definitions(fixomic PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:fixomic,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    # Developer command-line options (src/app/Main.cpp): every configuration except Release.
    FIXOMIC_DEV_COMMANDS=$<IF:$<CONFIG:Release>,0,1>
)
# Code signing. Ad-hoc signatures change with every build, and macOS then asks for the microphone
# and system-audio permissions again; a stable identity keeps the grants across rebuilds. Default:
# the first "Apple Development" identity in the keychain, else ad-hoc ("-"). The scan worker is
# this same binary, so it is covered. Hardened runtime only outside Debug.
if(NOT DEFINED FIXOMIC_CODESIGN_IDENTITY)
    set(fixomic_default_identity "-")
    execute_process(COMMAND security find-identity -v -p codesigning
                    OUTPUT_VARIABLE fixomic_identities ERROR_QUIET)
    if(fixomic_identities MATCHES "\"(Apple Development: [^\"]+)\"")
        set(fixomic_default_identity "${CMAKE_MATCH_1}")
    endif()
    set(FIXOMIC_CODESIGN_IDENTITY "${fixomic_default_identity}" CACHE STRING
        "Identity for codesign of fixomic.app (\"-\" = ad-hoc)")
endif()
message(STATUS "fixomic: signing with ${FIXOMIC_CODESIGN_IDENTITY}")

# The HAL driver ships inside the app (Contents/Resources/fixomic.driver) for the in-app installer.
# It is built separately by scripts/build-driver.sh (Xcode project, universal); without it the app
# still builds, with a warning, and the installer reports that the driver is not bundled. Copied
# before the codesign step below (POST_BUILD commands run in order), which seals it as a resource;
# the driver keeps its own signature.
set(FIXOMIC_DRIVER_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/build/driver/fixomic.driver" CACHE PATH
    "fixomic.driver to embed in fixomic.app (from scripts/build-driver.sh)")
if(EXISTS "${FIXOMIC_DRIVER_BUNDLE}/Contents/MacOS/fixomic")
    # Relink (and so copy again) when the driver is rebuilt.
    set_property(TARGET fixomic APPEND PROPERTY LINK_DEPENDS
        "${FIXOMIC_DRIVER_BUNDLE}/Contents/MacOS/fixomic" "${FIXOMIC_DRIVER_BUNDLE}/Contents/Info.plist")
else()
    message(WARNING "fixomic: ${FIXOMIC_DRIVER_BUNDLE} not found: run scripts/build-driver.sh, then "
                    "re-run CMake, or fixomic.app will not carry the driver (in-app install unavailable)")
endif()
add_custom_command(TARGET fixomic POST_BUILD
    COMMAND /bin/sh -c "rm -rf \"$1/Contents/Resources/fixomic.driver\" && if [ -d \"$0\" ]; then ditto --noqtn \"$0\" \"$1/Contents/Resources/fixomic.driver\"; else echo \"warning: $0 not found (run scripts/build-driver.sh): fixomic.app ships without the driver\" >&2; fi"
            "${FIXOMIC_DRIVER_BUNDLE}" "$<TARGET_BUNDLE_DIR:fixomic>"
    VERBATIM)
add_custom_command(TARGET fixomic POST_BUILD
    COMMAND codesign --force --timestamp=none --sign "${FIXOMIC_CODESIGN_IDENTITY}"
            --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fixomic.entitlements"
            "$<$<NOT:$<CONFIG:Debug>>:--options=runtime>"
            "$<TARGET_BUNDLE_DIR:fixomic>"
    COMMAND_EXPAND_LISTS
    VERBATIM)

target_link_libraries(fixomic
    PRIVATE
        fixomic_app_lib
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- tests -------------------------------------------------------------------
add_executable(fixomic_tests
    tests/ConfigTests.cpp
    tests/FrameFifoTests.cpp
    tests/AecStageTests.cpp
    tests/PluginChainTests.cpp
    tests/LatencyBudgetTests.cpp
    tests/ProcessingGraphTests.cpp
    tests/CoreAudioDevicesTests.cpp
    tests/AggregateIOTests.cpp
    tests/EngineControllerTests.cpp
    tests/MacEngineTests.mm
    tests/PluginCatalogTests.cpp
    tests/PluginWindowsTests.cpp
    tests/PlatformServicesTests.mm
    tests/UiModelTests.cpp
    tests/ChainModelTests.cpp
    tests/ChainViewTests.cpp
    tests/support/AllocCounter.cpp
)
target_include_directories(fixomic_tests PRIVATE tests)
target_link_libraries(fixomic_tests PRIVATE fixomic_app_lib Catch2::Catch2WithMain)
set_target_properties(fixomic_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Plugin scan worker for PluginCatalogTests: the production PluginScanWorker plus a test override
# that crashes / hangs / fakes results on marker file names. Lives next to fixomic_tests.
add_executable(fixomic_test_scan_worker tests/support/ScanWorkerMain.cpp)
target_link_libraries(fixomic_test_scan_worker PRIVATE fixomic_core)
set_target_properties(fixomic_test_scan_worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(fixomic_tests fixomic_test_scan_worker)

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(fixomic_tests)

# ---- fxprobe: driver and Core Audio diagnostics CLI --------------------------------------------
# Links fixomic_core for `aggregate-test` (AggregateIO, SystemAudioTap). The embedded Info.plist
# carries the microphone / system audio usage strings TCC asks for when fxprobe is its own
# responsible process.
add_executable(fxprobe tools/fxprobe/main.mm)
target_link_libraries(fxprobe PRIVATE fixomic_core)
target_link_options(fxprobe PRIVATE
    "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/tools/fxprobe/Info.plist")
set_property(TARGET fxprobe APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/fxprobe/Info.plist")
target_include_directories(fxprobe PRIVATE src)
target_compile_features(fxprobe PRIVATE cxx_std_20)
target_compile_options(fxprobe PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(fxprobe PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework Foundation"
)
set_target_properties(fxprobe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
