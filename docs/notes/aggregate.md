# Aggregate IO: how fixomic does mic, reference and Bridge in one IOProc

`src/platform/mac/AggregateIO.mm` builds one private aggregate device per engine start:

| part | how | why |
|---|---|---|
| main sub-device | the selected mic (`kAudioAggregateDeviceMainSubDeviceKey`), switched to 48 kHz first | the mic is the clock; nothing resamples the voice |
| sub-device | fixomic Bridge (`fixomic_2_UID`, hidden), `kAudioSubDeviceDriftCompensationKey = 1` | the virtual mic's write side |
| tap | `SystemAudioTap` in `kAudioAggregateDeviceTapListKey`, `kAudioSubTapDriftCompensationKey = 1` | AEC reference (system audio) |
| flags | `IsPrivate = 1`, `IsStacked = 0`, `TapAutoStart = 0`, UID `fixomic_aggregate_<uuid>` | private: no other process sees it; the `fixomic_` prefix keeps it out of `listInputDevices()` (a process sees its own private aggregates) |

The tap is `initMonoGlobalTapButExcludeProcesses:` with fixomic's own process object
(`kAudioHardwarePropertyTranslatePIDToProcessObject` of `getpid()`), `CATapUnmuted`, `privateTap = YES`.

## Stream mapping

What the HAL reports for the aggregate (macOS 27.2, mic "Microfono esterno"):

```
agg in  stream 217 owner 215(aggregate) startCh 1 term 0x202 1 ch   <- mic (sub-device stream 179 has term 0x202)
agg in  stream 218 owner 215(aggregate) startCh 2 term 0      1 ch  <- tap
agg out stream 216 owner 215(aggregate) startCh 1 term 'spkr' 2 ch  <- fixomic Bridge
IOProc: in 2 buffers [1 ch][1 ch], out 1 buffer [2 ch]
```

- The aggregate owns its own stream objects (`kAudioObjectPropertyOwner` is the aggregate), so
  ownership cannot identify the sub-device.
- The IOProc gets one interleaved `AudioBuffer` per aggregate stream, in `kAudioDevicePropertyStreams`
  order, and the streams are laid out sub-device by sub-device in
  `kAudioAggregateDevicePropertyActiveSubDeviceList` order, followed by one stream per tap.

`create()` does not trust that order blindly. It builds the expected sequence of (channels, terminal
type) from the active sub-device list (each sub-device's own streams) plus the tap (its
`kAudioTapPropertyFormat` channels, terminal type 0), tries "sub-devices then taps", then "taps then
sub-devices", and accepts the first layout where every aggregate stream matches on channel count
and terminal type and the stream counts add up. Output streams are matched the same way (sub-devices
only). If nothing matches, `create()` fails with the observed layout in the error. `micChannel` is
then resolved inside the mic's block (stream + channel within it), outL/outR are channels 0/1 of the
Bridge block, and every output buffer that is not Bridge (e.g. a USB headset's own outputs) is
zeroed every cycle. Streams must be interleaved 32-bit float (checked).

`AggregateIO::describe()` prints the mapping found; `fxprobe aggregate-test` shows it.

## Pitfalls found

- **`kAudioAggregateDeviceTapAutoStartKey = 1` → the IOProc never runs.** `AudioDeviceStart` returns
  `noErr`, but no callback arrives (2 s, 0 calls). With `0` the tap starts with the IO and delivers
  audio. Reproduced with a minimal tool (scratch, not in the repo) on macOS 27.2.
- **TCC.** When fxprobe runs from iTerm, tccd logs `kTCCServiceAudioCapture authValue=0 authReason=8`
  for iTerm (iTerm has no `NSAudioCaptureUsageDescription`), yet the tap delivered the system audio
  anyway in every run. The app bundle carries `NSAudioCaptureUsageDescription`; fxprobe embeds
  `tools/fxprobe/Info.plist` in `__TEXT,__info_plist` for the case where it is its own responsible
  process (launchd job).
- Mic startup transient: the combo-jack mic delivers a pop up to +5 dBFS within the first ~0.5 s after
  `AudioDeviceStart`. The engine should mute or fade in the first cycles.
- **coreaudiod restart** (the driver installer's `killall coreaudiod`, or anything else). Measured on
  macOS 27.2 with a scratch probe (private aggregate on fixomic Bridge + IOProc, then
  `killall -9 coreaudiod`): within ~10 ms the client gets the aggregate's `IsAlive`, the system
  object's `Devices` and `kAudioHardwarePropertyServiceRestarted` notifications, with an empty device
  list; the IOProc stops being called; the aggregate's UID no longer resolves and its id answers
  `'!obj'`. The devices come back ~2.5 s later (new ids, `Devices` and `DefaultOutputDevice` fire:
  the system object listeners survived, although AudioHardware.h says a client must re-establish
  them; the HAL client registers them with the new daemon itself). Stopping / destroying the old
  IOProc answers `'!dev'`; destroying the old aggregate id answers `noErr`. So fixomic treats
  ServiceRestarted as "the IO is dead" (`EngineController::onServiceRestarted`) and destroys an
  aggregate only while its id still carries the UID fixomic gave it. It does not re-register its
  listeners: in one app run that removed and re-added them ~2 s after the kill (while the client was
  reconnecting) the process's device list stopped changing (the headset mic, back for other
  processes, stayed missing; a new device went unnoticed) until the next restart.
- **Never use the main queue as listener queue** (`PropertyWatcher`). A dying object's last
  notifications are delivered with `dispatch_sync` on the listener queue while the HAL holds the
  object's lock, and `AudioObjectRemovePropertyListenerBlock` waits for that lock: removing a
  main-queue listener on the main thread during the teardown deadlocks. Seen after
  `killall coreaudiod` (closing the IO froze the app: main thread in `HALObject::RemovePropertyListener`,
  the HAL notification thread in `HALDevice::Teardown` → `dispatch_sync` to the main queue); also
  reproduced by destroying an aggregate while a watcher on it is being destroyed (test "PropertyWatcher
  can be destroyed while Core Audio tears its object down"). Listener blocks run on a private serial
  queue and forward to the main queue.

## Contingency (two aggregates)

Not needed: the tap inside the main aggregate works (gate 2 passed, see the Task 6 report). The
fallback would put the tap in a second private aggregate clocked by the default output device and
move the reference through an SPSC FIFO with adaptive resampling.
