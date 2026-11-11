include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.17-security
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.9.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(JUCE Catch2)

# ---- WebRTC audio processing (AEC3), prebuilt by scripts/build-webrtc-apm.sh -----------------
# Static library with abseil folded in; the arch slice must match what we build.
list(LENGTH CMAKE_OSX_ARCHITECTURES fixomic_num_archs)
if(fixomic_num_archs GREATER 1)
    set(FIXOMIC_APM_ARCH universal)
elseif(fixomic_num_archs EQUAL 1)
    set(FIXOMIC_APM_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(FIXOMIC_APM_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(FIXOMIC_APM_DIR "${CMAKE_SOURCE_DIR}/third_party/install/webrtc-apm/${FIXOMIC_APM_ARCH}")
if(NOT EXISTS "${FIXOMIC_APM_DIR}/lib/libwebrtc-audio-processing-2.a")
    message(FATAL_ERROR
        "WebRTC audio processing library for '${FIXOMIC_APM_ARCH}' not found at:\n"
        "  ${FIXOMIC_APM_DIR}\n"
        "Build it first with:\n"
        "  scripts/build-webrtc-apm.sh")
endif()

add_library(webrtc_apm STATIC IMPORTED GLOBAL)
set_target_properties(webrtc_apm PROPERTIES
    IMPORTED_LOCATION "${FIXOMIC_APM_DIR}/lib/libwebrtc-audio-processing-2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${FIXOMIC_APM_DIR}/include;${FIXOMIC_APM_DIR}/include/webrtc-audio-processing-2"
    INTERFACE_COMPILE_DEFINITIONS "WEBRTC_LIBRARY_IMPL;WEBRTC_POSIX"
)
target_link_libraries(webrtc_apm INTERFACE "-framework CoreFoundation" "-framework Foundation")
add_library(webrtc::apm ALIAS webrtc_apm)
