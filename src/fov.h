#pragma once
// FOV editor, ported from TrueFOV (https://github.com/Reynbow/HellraiserFOV, MIT, (c) 2026 Reynbow).
//
// Gameplay-only FOV: the player camera manager's ApplyCameraModifiers runs once per view target every frame, and the
// hook rescales the FOV only when that view target is the player's own view (the character or its first-person camera
// actor). Cutscene cameras, the free camera and other view targets are never touched; camera blends between them and
// the player interpolate naturally. Changes blend in tan(FOV/2) space so they are proportional on screen.
//
// On top of TrueFOV: the first-person meshes (arms and held items, FMinimalViewInfo::FirstPersonFOV) can have a
// viewmodel FOV of their own instead of following the world FOV.
//
// Settings live in feat::g_on (CustomFov, FovKeepEffects) and feat::g_value (WorldFov, ViewmodelFov, AdsFov, FovBlend).
#include <string>

namespace ue {
struct UObject;
}

namespace fov {

struct Status {
    bool resolved = false;     // reflection lookups done
    bool hookAvailable = false; // ApplyCameraModifiers' vtable slot found in the game's code
    bool cameraHooked = false;  // ...and hooked in the current camera manager
    bool player = false;        // a player character is being controlled
    bool applying = false;      // the player's camera is on screen and the editor is on
    bool aiming = false;
    std::string shown;          // what the camera shows ("player", "other camera", ...)
    std::string error;          // why the editor can't work (empty = fine)
    float gameBase = 0;         // the game's gameplay FOV (APlayerCharacter::GetDefaultCameraFov), 0 = unknown
    float gameAimFov = 0;       // where the game's aiming FOV settles, once seen
    float in = 0, out = 0;      // player camera FOV before / after the editor, last frame
    float viewmodelIn = 0, viewmodelOut = 0; // first-person meshes' FOV before / after (0 = not drawn with their own)
    float lockedFov = 0;        // APlayerCameraManager::LockedFOV (the console 'fov' command), overrides every camera
};

Status GetStatus();                      // any thread
void OnFrame(ue::UObject* playerController); // game thread, once per engine frame (may be null)
void ClearLockedFov();                    // any thread: clears the console FOV lock on the next frame

} // namespace fov
