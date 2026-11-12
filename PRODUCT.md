# Product

<!-- impeccable:product-schema 1 -->

> Written unattended (Task 10a). No one was available to answer the init interview: every fact
> below comes from the design spec (`docs/superpowers/specs/2026-11-11-fixomic-design.md`) and
> the implementation plan. Facts inferred rather than stated there are marked *(inferred)*.

## Platform

macos

Native macOS menu-bar extra (LSUIElement: no Dock icon, no main window), built with JUCE 8 in
C++. Not one of web / ios / android / adaptive: native macOS conventions (Control Center and
menu-bar extras) are the reference.

## Users

The author first: a Mac user on Discord calls with a HyperX headset on the combo jack, whose
callers hear the YouTube / Instagram audio he plays, through electrical crosstalk. Then other Mac
users with the same problem, once the app is published on GitHub. They talk, play and watch
videos while on calls; fixomic sits in the menu bar for hours and is looked at only when something
sounds wrong or a setting needs changing *(inferred from the spec's "c'è e basta")*.

## Product Purpose

Takes the physical microphone, removes the echo of whatever the Mac is playing (WebRTC AEC3 with
the system audio as reference), runs the result through the user's ordered AU / VST3 plugin chain
(gate, iZotope Nectar…) and publishes it as a virtual microphone, "fixomic Mic", that Discord,
Zoom or Meet select like any other mic. Success: callers hear only the voice, treated by the
chain, with no perceptible delay, for hours, across sleep / wake and unplugged headsets;
configured once, then it starts at login and just works.

## Positioning

Replaces "open Reaper, add a gate and Nectar, route through BlackHole": one small menu-bar app,
its own HAL driver, real echo cancellation instead of a gate, processing only while some app reads
fixomic Mic (no orange mic dot otherwise).

## Operating Context

- Glanced at from the menu bar during or before a call; the panel opens under the icon, is used
  for seconds and closes on focus loss, like the system's own menu-bar extras *(inferred)*.
- Used in both light and dark system appearance; follows the system.
- Plugin editors open in their own windows; the panel never hosts them.
- First run is a guided sequence: microphone permission → system audio recording permission →
  driver install (administrator password) → "choose fixomic Mic in Discord".

## Capabilities and Constraints

- Engine states: Idle (no app reads fixomic Mic), Starting, Running, MicMissing, DriverMissing,
  UnsupportedSampleRate (the mic lacks 48 kHz), Error. Waiting for a permission prompt is a
  waiting state, not an error.
- Never switches to another microphone by itself; a missing mic means fixomic Mic is silent.
- Global bypass sends the raw mic to fixomic Mic. AEC can be turned off. Total added latency is
  always visible (buffer + AEC + plugin-reported latency): ≤ 16 ms with AEC, ≤ 5 ms without.
- Echo-removed indicator comes from the AEC's ERLE estimate; nothing to show until it measured.
- One chain only in v1 (no presets). No recording, timeline, tracks, mixer, MIDI.
- UI language: Italian. Code and comments: English.
- Status texts are decided in the plan (e.g. Idle "In attesa — nessuna app usa fixomic Mic",
  Running "Attivo", bypass "Bypass — audio non elaborato").

## Brand Commitments

- Name is lowercase "fixomic"; the virtual device is "fixomic Mic".
- The author trusts the builder's taste for the interface (spec, "Interfaccia").
- No logo or brand assets exist yet.

## Evidence on Hand

None: no users, testimonials, benchmarks or screenshots beyond the measured latency budget in the
spec. Nothing of that kind may be invented.

## Product Principles

1. It is not a DAW: no Reaper-style complexity, nothing on screen that a call does not need.
2. State first: the panel answers "is my voice going out clean, and if not why" before anything else.
3. Silence over surprise: never a mic the user did not choose, never noise, never a hidden failure.
4. Latency is a number the user can see, not a promise.
5. Native citizen: behaves and looks like a macOS menu-bar extra, in light and dark.

## Accessibility & Inclusion

No product-specific requirement was established *(inferred: follow macOS defaults; text contrast
≥ 4.5:1 in both appearances; state never carried by colour alone)*.
