# fixomic

A macOS menu-bar app that sits between your microphone and apps like Discord:
it cancels echo and runs a VST3/AU plugin chain on the microphone signal.

Work in progress. Licensed under AGPL-3.0-or-later (see `LICENSE` and `NOTICE`).

## Build

    cmake -S . -B build -G Ninja
    cmake --build build
    ./build/fixomic_tests
