// FOV editor, ported from TrueFOV (https://github.com/Reynbow/HellraiserFOV, MIT, (c) 2026 Reynbow). See fov.h.
#include "fov.h"
#include "features.h"
#include "log.h"
#include "scan.h"
#include "ue.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

using namespace ue;

namespace fov {
namespace {

using ApplyCameraModifiersFn = void (*)(void* self, float deltaTime, void* pov);

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr float kMinOut = 5.f, kMaxOut = 170.f;
// If the player camera hasn't been rendered for this long it was off screen (cutscene, loading...): when it comes
// back there's nothing on screen to blend from.
constexpr double kOffScreenSeconds = 0.2;
// Aiming eases in and out like FInterpTo at this speed, close to the game's own aiming zoom.
constexpr float kZoomSpeed = 12.f;
// The engine's usual slot, for the log: a different value means the engine changed.
constexpr int kUsualApplyCameraModifiers = 253;

// Everything looked up by name through reflection (offsets in bytes, -1 = not found).
struct Offsets {
    int PCM_PCOwner = -1, PCM_ViewTarget = -1, PCM_PendingViewTarget = -1, PCM_LockedFOV = -1;
    int VT_Target = -1, VT_POV = -1;
    int POV_FOV = -1, POV_DesiredFOV = -1, POV_FirstPersonFOV = -1;
    Prop POV_UseFirstPerson;
    int Ctrl_Pawn = -1, PC_CameraManager = -1;
    int Char_FPCameraActor = -1, ChildActor_Actor = -1;
} O;
UClass* g_playerClass = nullptr;
int g_slot = -1; // APlayerCameraManager::ApplyCameraModifiers vtable slot

struct Hooked {
    void** vtable;
    ApplyCameraModifiersFn original;
};
Hooked g_hooked[8];
int g_numHooked = 0;
void** g_checkedVtable = nullptr;

// Game-thread state (the camera hook runs on the game thread too).
struct {
    bool resolved = false, failed = false;
    double lastResolveTry = -100, lastFrame = 0, lastPovTime = -1;
    float alpha = 0, zoomAlpha = 0, smoothedFov = 0;
    bool aiming = false;
    float gameBase = 0, gameAimFov = 0;
    float in = 0, out = 0, viewmodelIn = 0, viewmodelOut = 0;
    std::string error;
} g;
long g_faults = 0;
std::atomic<bool> g_clearLocked{false};

std::mutex g_statusMutex;
Status g_status;

template <class T> T& At(void* base, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(base) + offset); }
void* PtrAt(void* base, int offset) { return base && offset >= 0 ? At<void*>(base, offset) : nullptr; }

double TanHalf(double deg) { return std::tan(deg * 0.5 * kDegToRad); }
double FromTanHalf(double t) { return 2.0 * std::atan(t) / kDegToRad; }
double Ease(double a) { return a * a * (3.0 - 2.0 * a); } // smoothstep
bool Plausible(float v) { return v > 1.f && v < 179.f; }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool Enabled() { return feat::g_on[feat::CustomFov]; }

// ---------------------------------------------------------------- lookups

int OffsetOf(const void* ustruct, const char* name, std::vector<std::string>* missing, const char* what)
{
    Prop p = ustruct ? FindProp(static_cast<const UClass*>(ustruct), name) : Prop{};
    if (!p && missing) missing->push_back(std::string(what) + "::" + name);
    return p ? p.offset : -1;
}

// The slot of the last virtual call in |fn| that's passed &OutVT.POV as its third argument
// (`lea r8, [reg + POV]` shortly before `call [reg + slot*8]`), or -1.
int LastCallPassingPov(uintptr_t fn, int32_t povOffset)
{
    static const uint8_t kCall[] = {0xFF, 0x90}; // call qword ptr [reg + disp32]
    int found = -1;
    for (uintptr_t at : scan::FindInFunction(fn, kCall, "x?")) {
        const auto* b = reinterpret_cast<const uint8_t*>(at);
        if ((b[1] & 0xF8) != 0x90 || b[1] == 0x94) continue;
        int32_t disp = 0;
        memcpy(&disp, b + 2, 4);
        if (disp <= 0 || disp % 8 || disp / 8 >= 1024) continue;
        // lea r8, [reg + POV] (REX.WR or REX.WRB, ModRM reg = r8) in the 16 bytes before the call.
        bool passesPov = false;
        for (const uint8_t* p = b - 16; p + 4 <= b && !passesPov; ++p) {
            if ((p[0] != 0x4C && p[0] != 0x4D) || p[1] != 0x8D || (p[2] & 7) == 4) continue;
            if ((p[2] & 0xF8) == 0x40 && int32_t(int8_t(p[3])) == povOffset) passesPov = true; // disp8
            int32_t d32 = 0;
            if ((p[2] & 0xF8) == 0x80 && p + 7 <= b && (memcpy(&d32, p + 3, 4), d32 == povOffset)) passesPov = true;
        }
        if (passesPov) found = disp / 8;
    }
    return found;
}

// In UpdateViewTarget (found by its "FreeCam_Default" camera style name) the camera modifiers are applied last: the
// final virtual call on the camera manager that's passed &OutVT.POV.
int FindApplyCameraModifiersSlot(void** vtable, int32_t povOffset)
{
    if (!vtable || povOffset < 0) return -1;
    std::vector<uintptr_t> candidates = scan::FunctionsReferencing(L"FreeCam_Default");
    for (int slot = 0; slot < 1024; ++slot) {
        auto fn = reinterpret_cast<uintptr_t>(vtable[slot]);
        if (!scan::InText(fn)) break;
        if (std::find(candidates.begin(), candidates.end(), fn) == candidates.end()) continue;
        int found = LastCallPassingPov(fn, povOffset);
        if (found < 0) continue;
        km::Log("FOV: ApplyCameraModifiers is vtable slot %d (UpdateViewTarget: slot %d, exe+0x%llX)%s", found, slot,
                static_cast<unsigned long long>(fn - scan::Img().base),
                found == kUsualApplyCameraModifiers ? "" : " - not the usual slot");
        return found;
    }
    km::Log("FOV: UpdateViewTarget not found in the camera manager vtable (%zu candidates)", candidates.size());
    return -1;
}

bool Resolve()
{
    std::vector<std::string> missing;
    UClass* pcm = FindClass("PlayerCameraManager");
    UClass* controller = FindClass("Controller");
    UClass* playerController = FindClass("PlayerController");
    UClass* childActor = FindClass("ChildActorComponent");
    g_playerClass = FindClass("PlayerCharacter");
    if (!pcm) missing.push_back("APlayerCameraManager");
    if (!g_playerClass) missing.push_back("APlayerCharacter");

    O.PCM_PCOwner = OffsetOf(pcm, "PCOwner", &missing, "APlayerCameraManager");
    O.PCM_ViewTarget = OffsetOf(pcm, "ViewTarget", &missing, "APlayerCameraManager");
    O.PCM_PendingViewTarget = OffsetOf(pcm, "PendingViewTarget", &missing, "APlayerCameraManager");
    O.PCM_LockedFOV = OffsetOf(pcm, "LockedFOV", nullptr, nullptr);
    // FTViewTarget and FMinimalViewInfo are script structs: reached through the struct properties that hold them.
    Prop vt = pcm ? FindProp(pcm, "ViewTarget") : Prop{};
    const void* viewTarget = vt.inner;
    Prop pov = viewTarget ? FindProp(static_cast<const UClass*>(viewTarget), "POV") : Prop{};
    const void* viewInfo = pov.inner;
    if (!viewTarget) missing.push_back("FTViewTarget");
    if (!viewInfo) missing.push_back("FMinimalViewInfo");
    O.VT_Target = OffsetOf(viewTarget, "Target", &missing, "FTViewTarget");
    O.VT_POV = OffsetOf(viewTarget, "POV", &missing, "FTViewTarget");
    O.POV_FOV = OffsetOf(viewInfo, "FOV", &missing, "FMinimalViewInfo");
    O.POV_DesiredFOV = OffsetOf(viewInfo, "DesiredFOV", nullptr, nullptr);
    O.POV_FirstPersonFOV = OffsetOf(viewInfo, "FirstPersonFOV", nullptr, nullptr);
    O.POV_UseFirstPerson = viewInfo ? FindProp(static_cast<const UClass*>(viewInfo), "bUseFirstPersonParameters") : Prop{};
    O.Ctrl_Pawn = OffsetOf(controller, "Pawn", &missing, "AController");
    O.PC_CameraManager = OffsetOf(playerController, "PlayerCameraManager", &missing, "APlayerController");
    O.Char_FPCameraActor = OffsetOf(g_playerClass, "FPCameraActor", &missing, "APlayerCharacter");
    O.ChildActor_Actor = OffsetOf(childActor, "ChildActor", &missing, "UChildActorComponent");

    if (!missing.empty()) {
        g.error = "Not found: ";
        for (size_t i = 0; i < missing.size(); i++) g.error += (i ? ", " : "") + missing[i];
        return false;
    }
    km::Log("FOV: offsets ViewTarget 0x%X, Pending 0x%X, POV 0x%X, FOV 0x%X, DesiredFOV 0x%X, FirstPersonFOV 0x%X%s",
            O.PCM_ViewTarget, O.PCM_PendingViewTarget, O.VT_POV, O.POV_FOV, O.POV_DesiredFOV, O.POV_FirstPersonFOV,
            O.POV_UseFirstPerson ? "" : " (no bUseFirstPersonParameters: viewmodel FOV unavailable)");

    scan::Image img;
    if (!scan::Describe(GetModuleHandleW(nullptr), img)) {
        g.error = "Couldn't read the game executable.";
        return false;
    }
    scan::SetImage(img);
    UObject* cdo = FindObject("Default__PlayerCameraManager");
    void** vtable = cdo ? *reinterpret_cast<void***>(cdo) : nullptr;
    g_slot = FindApplyCameraModifiersSlot(vtable, O.VT_POV);
    if (g_slot < 0) {
        g.error = "ApplyCameraModifiers not found in the game's code.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- the camera hook

void* PlayerCameraActor(void* character)
{
    return PtrAt(PtrAt(character, O.Char_FPCameraActor), O.ChildActor_Actor);
}

// The player's own view: the character, or (as the game does it) its first-person camera actor.
bool IsPlayerView(void* target, void* character)
{
    return target && character && (target == character || target == PlayerCameraActor(character));
}

// The player character if |pov| is the view of the player's own camera, else nullptr. Only the current and pending
// view targets reach the screen.
void* PlayerViewOf(void* cameraManager, void* pov)
{
    char* vt = static_cast<char*>(pov) - O.VT_POV;
    char* mgr = static_cast<char*>(cameraManager);
    if (vt != mgr + O.PCM_ViewTarget && vt != mgr + O.PCM_PendingViewTarget) return nullptr;
    void* pc = PtrAt(cameraManager, O.PCM_PCOwner);
    void* pawn = PtrAt(pc, O.Ctrl_Pawn);
    if (!pawn || !IsA(static_cast<UObject*>(pawn), g_playerClass) || !IsPlayerView(PtrAt(vt, O.VT_Target), pawn))
        return nullptr;
    return pawn;
}

// Where the game's aiming FOV settles, to show in the menu: steady for a few frames while aiming.
void LearnAimFov(float in)
{
    static float prev = 0.f;
    static int steady = 0;
    if (g.aiming && g.zoomAlpha > 0.99f && std::fabs(in - prev) < 0.02f) {
        if (++steady >= 3) g.gameAimFov = in;
    } else {
        steady = 0;
    }
    prev = in;
}

// The first-person meshes (arms, held items) are drawn with a FOV of their own, which the game keeps equal to the
// camera's. Viewmodel FOV 0 (TrueFOV's behaviour) gives them the camera's change, so in first-person scenes (picking
// up a gun, opening a door) the hands stay lined up with the world. A set viewmodel FOV is scaled by the game's own FOV
// effects like the world FOV, and eases to the camera's FOV while aiming so the sights stay lined up.
// |a| is the eased blend weight, |tIn| / |tOut| the camera's tan(FOV/2) before and after the editor.
void ApplyViewmodel(void* pov, double a, double tIn, double tOut, double tBase)
{
    g.viewmodelOut = 0.f;
    if (O.POV_FirstPersonFOV < 0 || !O.POV_UseFirstPerson) return;
    if (!(At<uint8_t>(pov, O.POV_UseFirstPerson.offset) & O.POV_UseFirstPerson.boolMask)) return;
    float& fp = At<float>(pov, O.POV_FirstPersonFOV);
    const float game = fp;
    if (!Plausible(game)) return;
    g.viewmodelIn = game;
    if (a <= 0.0) return; // the editor is off: the game's own value
    const double tGame = TanHalf(game);
    const double tFollow = tGame * tOut / tIn; // the camera's change
    double t = tFollow;
    const float vm = feat::g_value[feat::ViewmodelFov];
    if (vm > 0.f && a > 0.0) {
        double tTarget = feat::g_on[feat::FovKeepEffects] ? tGame * TanHalf(vm) / tBase : TanHalf(vm);
        tTarget += (tFollow - tTarget) * g.zoomAlpha;
        t = tGame + (tTarget - tGame) * a;
    }
    const float out = Clamp(static_cast<float>(FromTanHalf(t)), kMinOut, kMaxOut);
    fp = out;
    g.viewmodelOut = out;
}

KM_NOINLINE void ApplyToPov(void* cameraManager, void* pov)
{
    if (!PlayerViewOf(cameraManager, pov)) return;

    float& fov = At<float>(pov, O.POV_FOV);
    const float in = fov;
    g.in = g.out = in;
    LearnAimFov(in);

    // Coming back on screen (a cutscene cut back to the player): start at the right FOV instead of blending from
    // whatever the blend weight decayed to while off screen.
    const double now = PreciseSeconds();
    if (now - g.lastPovTime > kOffScreenSeconds) {
        g.alpha = Enabled() ? 1.f : 0.f;
        g.zoomAlpha = g.aiming ? 1.f : 0.f;
    }
    g.lastPovTime = now;

    const double a = Ease(g.alpha);
    if (a <= 0.0 || !Plausible(in)) {
        ApplyViewmodel(pov, 0.0, 1.0, 1.0, 1.0); // only records the game's value
        return;
    }

    // Scaling around the game's gameplay FOV keeps any FOV change the game makes on top of it (sprinting, ladders...).
    const bool keep = feat::g_on[feat::FovKeepEffects];
    const double base = keep && Plausible(g.gameBase) ? g.gameBase : in;

    // Work in tan(fov/2) space so the scaling is proportional on screen.
    const double tIn = TanHalf(in), tBase = TanHalf(base);
    const double tUser = TanHalf(g.smoothedFov > 0.f ? g.smoothedFov : feat::g_value[feat::WorldFov].load());
    double tTarget = keep ? tIn * tUser / tBase : tUser;
    // Aiming: towards the chosen aim FOV, or by default the game's own aiming FOV (not scaled by the world FOV, so
    // aiming looks the way the game made it).
    if (g.zoomAlpha > 0.f) {
        const float ads = feat::g_value[feat::AdsFov];
        const double tZoom = ads > 0.f ? TanHalf(ads) : tIn;
        tTarget += (tZoom - tTarget) * g.zoomAlpha;
    }
    const double tOut = tIn + (tTarget - tIn) * a;
    const float out = Clamp(static_cast<float>(FromTanHalf(tOut)), kMinOut, kMaxOut);
    fov = out;
    g.out = out;

    if (O.POV_DesiredFOV >= 0) {
        float& desired = At<float>(pov, O.POV_DesiredFOV);
        if (Plausible(desired)) desired = Clamp(static_cast<float>(FromTanHalf(TanHalf(desired) * tOut / tIn)), kMinOut, kMaxOut);
    }
    ApplyViewmodel(pov, a, tIn, TanHalf(out), tBase);
}

void SafeApplyToPov(void* cameraManager, void* pov)
{
    __try {
        ApplyToPov(cameraManager, pov);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
    }
}

ApplyCameraModifiersFn OriginalFor(void* self)
{
    void** vtable = *static_cast<void***>(self);
    for (int i = 0; i < g_numHooked; ++i)
        if (g_hooked[i].vtable == vtable) return g_hooked[i].original;
    return nullptr;
}

// The FOV is set on the finished view, after the game's camera modifiers.
void HookApplyCameraModifiers(void* self, float deltaTime, void* pov)
{
    if (ApplyCameraModifiersFn original = OriginalFor(self)) original(self, deltaTime, pov);
    SafeApplyToPov(self, pov);
}

bool WriteSlot(void** slot, void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Camera managers get their vtable slot hooked; each vtable keeps the function it had.
bool EnsureCameraHook(void* cameraManager)
{
    void** vtable = *static_cast<void***>(cameraManager);
    static bool hooked = false;
    if (vtable == g_checkedVtable) return hooked;
    g_checkedVtable = vtable;
    hooked = false;
    if (g_slot < 0) return false;
    if (OriginalFor(cameraManager)) return hooked = true;
    void* hook = reinterpret_cast<void*>(&HookApplyCameraModifiers);
    void* current = vtable[g_slot];
    if (g_numHooked == int(sizeof g_hooked / sizeof g_hooked[0]) || current == hook) return false;
    // Recorded before the slot changes: the hook looks its original up by vtable.
    g_hooked[g_numHooked++] = {vtable, reinterpret_cast<ApplyCameraModifiersFn>(current)};
    if (!WriteSlot(&vtable[g_slot], hook)) {
        --g_numHooked;
        km::Log("FOV: couldn't hook ApplyCameraModifiers in %s", Name(reinterpret_cast<UObject*>(ClassOf(static_cast<UObject*>(cameraManager)))).c_str());
        return false;
    }
    km::Log("FOV: hooked ApplyCameraModifiers of %s (vtable exe+0x%llX, slot %d)",
            Name(reinterpret_cast<UObject*>(ClassOf(static_cast<UObject*>(cameraManager)))).c_str(),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vtable) - scan::Img().base), g_slot);
    return hooked = true;
}

float CallFloat(UObject* obj, const char* fn, float fallback)
{
    Call c(obj, fn);
    if (!c || !c.Run()) return fallback;
    return c.Out<float>("ReturnValue");
}

} // namespace

Status GetStatus()
{
    std::lock_guard lock(g_statusMutex);
    return g_status;
}

void ClearLockedFov() { g_clearLocked = true; }

void OnFrame(UObject* pc)
{
    const double now = PreciseSeconds();
    const float dt = g.lastFrame > 0 ? Clamp(static_cast<float>(now - g.lastFrame), 0.f, 0.1f) : 0.f;
    g.lastFrame = now;

    Status st;
    if (!g.resolved) {
        if (g.failed || now - g.lastResolveTry < 5.0) {
            st.error = g.error;
            std::lock_guard lock(g_statusMutex);
            g_status = st;
            return;
        }
        g.lastResolveTry = now;
        g.resolved = Resolve();
        static int tries = 0;
        if (!g.resolved && ++tries >= 6) { // ~30 s: the classes are native, they won't show up later
            g.failed = true;
            km::Log("FOV editor disabled: %s", g.error.c_str());
        }
        if (!g.resolved) return;
        g.error.clear();
        km::Log("FOV editor ready");
    }
    st.resolved = true;
    st.hookAvailable = g_slot >= 0;

    void* cameraManager = PtrAt(pc, O.PC_CameraManager);
    void* pawn = PtrAt(pc, O.Ctrl_Pawn);
    UObject* character = pawn && cameraManager && IsA(static_cast<UObject*>(pawn), g_playerClass) ? static_cast<UObject*>(pawn) : nullptr;

    if (cameraManager) {
        st.cameraHooked = EnsureCameraHook(cameraManager);
        if (O.PCM_LockedFOV >= 0) {
            float& locked = At<float>(cameraManager, O.PCM_LockedFOV);
            if (g_clearLocked.exchange(false)) {
                km::Log("FOV: cleared the console FOV lock (was %.1f)", locked);
                locked = 0.f;
            }
            st.lockedFov = locked;
        }
    }

    bool otherCamera = false;
    st.shown = "nothing";
    if (character) {
        g.gameBase = CallFloat(character, "GetDefaultCameraFov", 0.f);
        if (!Plausible(g.gameBase)) g.gameBase = 0.f;
        Call aim(character, "IsAimingDownSight");
        g.aiming = aim && aim.Run() && aim.Out<bool>("ReturnValue");
        void* target = PtrAt(cameraManager, O.PCM_ViewTarget + O.VT_Target);
        void* pending = PtrAt(cameraManager, O.PCM_PendingViewTarget + O.VT_Target);
        if (IsPlayerView(target, character)) {
            st.shown = pending && !IsPlayerView(pending, character) ? "player, blending away" : "player";
        } else if (IsPlayerView(pending, character)) {
            st.shown = "other camera, blending to player";
        } else {
            st.shown = "other camera";
            otherCamera = true;
        }
    } else {
        g.aiming = false;
    }

    // Another camera on screen doesn't change the player camera's blend weight: the camera blend between them takes
    // care of the transition.
    const float blend = feat::g_value[feat::FovBlend];
    const float target = character && Enabled() ? 1.f : 0.f;
    const float step = blend > 0.001f ? dt / blend : 1.f;
    g.alpha = target > g.alpha ? std::min(target, g.alpha + step) : std::max(target, g.alpha - step);
    const float zoomTarget = g.aiming ? 1.f : 0.f;
    g.zoomAlpha += (zoomTarget - g.zoomAlpha) * std::min(1.f, dt * kZoomSpeed);
    if (std::fabs(zoomTarget - g.zoomAlpha) < 0.001f) g.zoomAlpha = zoomTarget;

    // Ease hotkey and slider changes over ~0.1 s instead of snapping.
    const float user = feat::g_value[feat::WorldFov];
    if (g.smoothedFov <= 0.f) g.smoothedFov = user;
    g.smoothedFov += (user - g.smoothedFov) * (1.f - std::exp(-dt / 0.05f));

    static long reported = 0;
    if (g_faults != reported) {
        reported = g_faults;
        if (reported <= 5) km::Log("FOV: recovered from a fault in the camera hook (%ld so far)", reported);
    }

    st.player = character != nullptr;
    st.applying = character && Enabled() && !otherCamera && st.cameraHooked;
    st.aiming = g.aiming;
    st.gameBase = g.gameBase;
    st.gameAimFov = g.gameAimFov;
    st.in = g.in;
    st.out = g.out;
    st.viewmodelIn = g.viewmodelIn;
    st.viewmodelOut = g.viewmodelOut;
    std::lock_guard lock(g_statusMutex);
    g_status = std::move(st);
}

} // namespace fov
