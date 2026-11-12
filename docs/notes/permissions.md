# Permissions: microphone and System Audio Recording

fixomic needs two TCC permissions:

| permission | service | how fixomic checks | how it asks |
|---|---|---|---|
| Microphone | `kTCCServiceMicrophone` | `AVCaptureDevice authorizationStatusForMediaType:` (public) | `requestAccessForMediaType:` before any HAL call (Task 8) |
| System Audio Recording ("Registrazione schermo e audio di sistema") | `kTCCServiceAudioCapture` | `TCCAccessPreflight` (SPI, via `dlsym`; absent → "not determined") | throwaway tap + aggregate + IOProc on a background queue |

Both prompts block the thread that triggers them, so neither may be triggered on the main thread.

## What was measured (macOS 27.2, MacBook Air, Apple Development signed bundle)

A scratch probe (not in the repo) did, on a background thread, exactly what `SystemAudioTap` +
`AggregateIO` do: `AudioHardwareCreateProcessTap` (global mono tap excluding itself, unmuted,
private), `AudioHardwareCreateAggregateDevice` (private, the tap in the tap list, TapAutoStart 0),
`AudioDeviceCreateIOProcID`, `AudioDeviceStart`, then 8 s of IOProc while system sounds played. It
was run as an app bundle (`LSUIElement`, `NSAudioCaptureUsageDescription`, a bundle id never seen
by TCC) launched with `open`, so the bundle was its own responsible process.

Fresh bundle id (prompt shown):

```
preflight audio=2 mic=2 (dlsym ok)
AudioHardwareCreateProcessTap -> 0 in 0.004 s; preflight=2
AudioHardwareCreateAggregateDevice -> 0 in 0.014 s; preflight=2
step 3 blocked for 5.0 s ... 20.1 s
AudioDeviceCreateIOProcID -> 0 in 22.054 s; preflight=2      <- blocked until the prompt was answered
AudioDeviceStart -> 0 in 0.048 s; preflight=2
t+1s calls=96 nonzero=49 preflight=2                       <- audio flows, preflight still 2
==== round 2 (same process): tap + aggregate + IOProc in 0.04 s, audio flows, preflight=2
```

Next launch of the same bundle: `preflight audio=0`, every call returns in milliseconds, audio flows.

Findings:

1. **Creating the tap does not block, nor does creating the aggregate.** The prompt is raised by
   `AudioDeviceCreateIOProcID` on the aggregate that contains the tap, and that call blocks its
   thread until the user answers (17.7 s and 22.1 s in two trials). On the main thread this is a
   frozen app (and menu bar item) for as long as the prompt is up. So the risk is real, just one
   call later than suspected: in `MacEngineBackend::openIO` it is inside `AggregateIO::create`.
2. **`TCCAccessPreflight("kTCCServiceAudioCapture")`** resolves (`dlsym` OK) and returns 0 =
   granted, 2 = not determined (1 = denied by the SPI's convention; a denial was not reproduced:
   that needs the user to click "Non consentire"). Its answer is **cached per process**: after the
   prompt was answered it kept returning 2 until the app was relaunched. So "not determined" must
   be read as "unknown", never as "denied", and never as a reason to prompt again.
3. After the prompt the same process can create a new tap and it delivers audio (round 2).
4. In one earlier trial (another fresh bundle id) the IOProc unblocked after 17.7 s but the tap
   delivered only zeros for the rest of the run; the next launch showed preflight 0 and audio. The
   outcome of that prompt is not known (it may have been dismissed and approved later in System
   Settings); this is the case the silence fallback below covers.
5. From a terminal (fxprobe, fixomic_tests: the responsible process is the terminal, which has no
   `NSAudioCaptureUsageDescription`) preflight answers 2, there is no prompt and the tap delivers the
   system audio anyway (as already noted in aggregate.md).
6. **The tap does not start the default output device**: with nothing playing,
   `kAudioDevicePropertyDeviceIsRunningSomewhere` of the default output stays 0 while the tap's
   IOProc runs and the tap delivers exact zeros; when a sound plays it goes to 1 and the tap delivers
   non-zero samples. So "silent tap while the output runs" is a usable signal.

## What fixomic does (`MacEngineBackend::openIO`, `Permissions`)

- Microphone first (Task 8).
- With a tap requested, `systemAudioPermission()`:
  - **Granted** → the tap is created as before.
  - **Denied** → the IO opens without the tap (AEC inactive) and `onWarning` says
    "Permesso audio di sistema negato: la cancellazione dell'eco è disattivata. Abilitalo in
    Impostazioni di Sistema → Privacy e sicurezza → Registrazione schermo e audio di sistema."
  - **NotDetermined** (or no SPI) → once per backend, `requestSystemAudio()` runs the throwaway
    tap + private aggregate + `AudioDeviceCreateIOProcID` on a background queue (that is where the
    prompt shows and blocks), destroys them and calls back on the main queue. Meanwhile `openIO`
    fails with "In attesa del permesso per l'audio di sistema" (engine state Error with that
    message, the controller's normal retry). On the callback (`onSystemAudioPermission`)
    AppController re-evaluates the engine at once; from then on the tap is created as usual, even
    though the preflight still says "not determined" (finding 2). When permission was already
    granted but the SPI is missing, the throwaway IO returns in milliseconds: one extra aggregate
    at the first start, no prompt.
- **Silence fallback** (unless the preflight says Granted): while an IO with a tap is open, a 1 s
  main-queue timer checks whether the IO thread saw any non-zero reference sample and whether the
  default output device `IsRunningSomewhere`. 10 s of running output with a silent tap
  (`TapSilenceWatch`) → `onWarning`: "Il permesso audio di sistema è probabilmente negato: …
  Controlla Impostazioni di Sistema → Privacy e sicurezza → Registrazione schermo e audio di
  sistema." Once per open; one non-zero sample settles it. Skipped when the permission is known to
  be granted, because an app can keep the output running while playing digital silence (that
  would be a false alarm).

## Resetting for a manual test

`tccutil reset AudioCapture it.manolozocco.fixomic` (resets fixomic's System Audio Recording
decision; the next start with AEC on prompts again). `tccutil` needs the bundle to be known to
Launch Services (launched at least once with `open`).
