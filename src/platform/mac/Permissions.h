#pragma once

#include <functional>

namespace fixomic::mac {

// Microphone access (TCC) through AVCaptureDevice, Apple's sanctioned flow: ask before opening any
// input, so the system prompt never blocks a Core Audio call on the main thread.
enum class Permission { Granted, Denied, NotDetermined };

// Current authorization for audio capture. Restricted counts as Denied.
Permission microphonePermission();

// Shows the system prompt if the user has not decided yet (otherwise answers at once). The
// completion runs on the main queue with the outcome.
void requestMicrophone(std::function<void(bool granted)> completion);

// ---- System Audio Recording (the permission behind the system audio tap) ----------------------
//
// There is no public status API. This asks TCC's private TCCAccessPreflight for
// "kTCCServiceAudioCapture" when the symbol can be resolved at run time (dlopen / dlsym of
// /System/Library/PrivateFrameworks/TCC.framework), and answers NotDetermined when it cannot.
// Findings on macOS 27.2 (docs/notes/permissions.md): 0 = granted, 2 = not determined; the value
// is cached per process, so after the prompt is answered it keeps reporting NotDetermined until
// the app is relaunched. Treat NotDetermined as "unknown", never as "denied".
Permission systemAudioPermission();

// Raises the System Audio Recording prompt without blocking the caller: on a background queue it
// creates a throwaway system audio tap in a private aggregate and an IOProc on it (the call that
// shows the prompt and blocks until the user answers; creating the tap alone does not), then
// destroys them. The completion runs on the main queue once that returned (prompt answered, or no
// prompt needed); it does not tell the outcome (the preflight above is stale in this process).
void requestSystemAudio(std::function<void()> completion);

} // namespace fixomic::mac
