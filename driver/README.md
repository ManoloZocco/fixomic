# fixomic HAL driver

The virtual audio devices used by fixomic are a build of
[BlackHole](https://github.com/ExistentialAudio/BlackHole) by Existential Audio Inc.,
included unmodified as a git submodule (`driver/BlackHole`, tag `v0.7.2`) and configured only
through its documented preprocessor macros. Many thanks to Devin Roth and Existential Audio for
BlackHole.

BlackHole is licensed under the GNU General Public License v3.0, and so is the driver built
from it: see `driver/LICENSE`. The driver is a separate program (a CoreAudio HAL plug-in loaded
by `coreaudiod`); the fixomic app only talks to it through the public CoreAudio API.

## Devices

| Device           | UID             | Direction        | Visible | Used by                    |
|------------------|-----------------|------------------|---------|----------------------------|
| `fixomic Mic`    | `fixomic_UID`   | input, 2 ch      | yes     | Discord & co. read it      |
| `fixomic Bridge` | `fixomic_2_UID` | output, 2 ch     | hidden  | the fixomic app writes it  |

Both devices share one ring buffer inside the driver: whatever is written to Bridge at sample
time `t` is read from Mic at sample time `t`. Only 48000 Hz is offered, neither device can become
the default (system) device, the bundle id is `it.manolozocco.fixomic.driver`. The names and UIDs
are mirrored in `src/platform/mac/FixomicDevices.h`. Bridge is hidden, so it is not listed by
`kAudioHardwarePropertyDevices` or in System Settings; resolve it with
`kAudioHardwarePropertyTranslateUIDToDevice`.

## Build, install, uninstall

    scripts/build-driver.sh            # -> build/driver/fixomic.driver (arm64 + x86_64, ad-hoc signed)
    sudo scripts/install-driver.sh     # copies to /Library/Audio/Plug-Ins/HAL, restarts coreaudiod
    sudo scripts/uninstall-driver.sh

`build-driver.sh` copies the submodule to `build/driver/src`, applies `driver/patches/*.patch`
(if any) to that copy, and builds it with `xcodebuild` (macros in the script). It also gives the
bundle its own CFPlugIn factory UUID, so it never clashes with an installed BlackHole. Setting
`FIXOMIC_DRIVER_EXTRA_PATCHES=<dir>` applies extra patches from `<dir>` (local diagnostics only).

No patches are needed for v0.7.2: all `fxprobe check-driver` checks pass on the stock source.

## Demand detection (used by Task 7)

To know whether some app is reading `fixomic Mic`, poll
**`kAudioDevicePropertyDeviceIsRunningSomewhere`** on the Mic device. It goes 0 -> 1 when another
process starts reading Mic and back to 0 when it stops. Writing to Bridge does not change it.

`kAudioDevicePropertyDeviceIsRunning` does **not** work for this: read from another process it
stays 0 while a reader is running.

Measured with `fxprobe running` while `ffmpeg -f avfoundation -i ":fixomic Mic" -t 3 -f null -`
ran in another terminal:

    polling fixomic Mic (device 169) every 200 ms
         0.0 s  IsRunningSomewhere=0  IsRunning=0
         2.7 s  IsRunningSomewhere=1  IsRunning=0
         5.7 s  IsRunningSomewhere=0  IsRunning=0

## Diagnostics: fxprobe

    ./build/fxprobe list              # every device, hidden ones included
    ./build/fxprobe check-driver      # the five driver checks; exit 0 if all pass
    ./build/fxprobe running [secs]    # prints IsRunningSomewhere / IsRunning transitions of Mic
    ./build/fxprobe loopback-test     # 10 impulses Bridge -> Mic, latency and count

Latency is measured on the devices' shared sample timeline (the driver indexes its ring buffer by
sample time), so it is the latency the driver adds between Bridge and Mic: 0 samples. The IO
buffer sizes of the writer and the reader add to the end-to-end latency on top of that.

**Per-app volume tools mute the writer.** Apps that set per-app volume through process taps
(for example Vorssaint's mixer or SoundSource) mute the original output of the apps they manage
and play it again on the default output device. A muted process's audio never reaches Bridge:
the driver gets silence and Mic reads zeros. These tools match processes by the app responsible
for them, so `fxprobe` started from a managed terminal app is muted too (`loopback-test` then
reports 0/10 and prints a hint). Run it from a terminal app the tool does not manage, or as a
launchd job:

    launchctl submit -l fxprobe -o /tmp/fxprobe.txt -- "$PWD/build/fxprobe" check-driver
    sleep 25; launchctl remove fxprobe; cat /tmp/fxprobe.txt

The same applies to the fixomic app itself if a user gives it a per-app volume in such a tool.
