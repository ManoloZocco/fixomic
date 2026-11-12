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

## Contingency (two aggregates)

Not needed: the tap inside the main aggregate works (gate 2 passed, see the Task 6 report). The
fallback would put the tap in a second private aggregate clocked by the default output device and
move the reference through an SPSC FIFO with adaptive resampling.
