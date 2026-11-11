# fixomic

A macOS menu-bar app that sits between your microphone and apps like Discord:
it cancels echo and runs a VST3/AU plugin chain on the microphone signal.

Work in progress. Licensed under AGPL-3.0-or-later (see `LICENSE` and `NOTICE`).

## Build

One-time: build the WebRTC audio processing (AEC3) static libraries (needs `brew install meson ninja`):

    scripts/build-webrtc-apm.sh

Then:

    cmake -S . -B build -G Ninja
    cmake --build build
    ./build/fixomic_tests

## Virtual microphone driver

`fixomic Mic` comes from a HAL driver built from BlackHole (GPL-3.0); see `driver/README.md`:

    scripts/build-driver.sh && sudo scripts/install-driver.sh
    ./build/fxprobe check-driver
