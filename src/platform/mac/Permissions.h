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

} // namespace fixomic::mac
