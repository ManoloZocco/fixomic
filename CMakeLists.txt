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

include(cmake/Dependencies.cmake)

# ---- core library (everything except src/app) -----------------------------
add_library(fixomic_core STATIC
    src/core/Config.cpp
    src/core/ConfigStore.cpp
)
target_include_directories(fixomic_core PUBLIC src)
target_compile_features(fixomic_core PUBLIC cxx_std_20)
target_link_libraries(fixomic_core
    PUBLIC
        juce::juce_core
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(fixomic_core PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
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
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:fixomic,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
)
target_link_libraries(fixomic
    PRIVATE
        fixomic_core
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_events
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- tests -------------------------------------------------------------------
add_executable(fixomic_tests tests/ConfigTests.cpp)
target_link_libraries(fixomic_tests PRIVATE fixomic_core Catch2::Catch2WithMain)
set_target_properties(fixomic_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(fixomic_tests)
