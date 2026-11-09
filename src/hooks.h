#pragma once
// Function hooks, the native equivalent of UE4SS's RegisterHook, built on ue::HookNative (the UFunction's Func swap):
//  - Hook: pre / post callbacks around any UFunction (Blueprint or native). The original always runs, so the script
//    VM's bytecode position is always advanced by the function itself (skipping a native call from Blueprint without
//    consuming its parameters is what froze the game in the first ToolInputFix port).
//  - HookEdit: change a native function's parameters before it runs. Called from Blueprint, a native function reads its
//    parameters from the caller's bytecode, so the hook evaluates them itself (FFrame::Step through GNatives), steps
//    over EX_EndFunctionParms as P_FINISH does, and makes the call through ProcessEvent with the edited block. It only
//    does that while `want` says so (a sequence window is open); otherwise the call passes straight through.
// Init calibrates the FFrame layout on a harmless function and finds GNatives; anything that fails a check turns the
// edit hooks off (they then pass through untouched) and is logged.
#include <cstdint>
#include <functional>

namespace ue {
struct UObject;
struct UFunction;
} // namespace ue

namespace hooks {

using Callback = std::function<void(ue::UObject* ctx, uint8_t* params)>; // params: the parameter block, or nullptr
using Edit = std::function<void(ue::UObject* ctx, ue::UFunction* fn, uint8_t* params)>;

bool Init();          // game thread, once; false = no hooks at all
bool Ready();         // frame layout known: Hook works
bool CanEdit();       // GNatives found too: HookEdit works

bool Hook(ue::UFunction* fn, Callback pre, Callback post); // true if hooked now or before
bool HookEdit(ue::UFunction* fn, std::function<bool()> want, Edit edit);
bool IsHooked(const ue::UFunction* fn);

// The object whose script made the current call (FFrame::Object of the calling frame), or nullptr.
ue::UObject* FrameObject(void* stack);

} // namespace hooks
