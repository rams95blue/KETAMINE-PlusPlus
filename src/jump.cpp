// Jump and bunny hopping (see jump.h). Game thread only, once per engine frame, except OnKey (window thread) and Hud.
#include "jump.h"
#include "features.h"
#include "log.h"
#include "menu.h"
#include "ue.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>

using namespace ue;

namespace jump {

std::atomic<unsigned> g_key{VK_SPACE};

namespace {

using feat::g_on;
using feat::g_value;

struct Vec {
    double x, y, z;
};

constexpr uint8_t MOVE_Walking = 1, MOVE_NavWalking = 2, MOVE_Falling = 3;
// Manual timing: a press this long before landing hops on landing, or this long after landing still chains.
constexpr double kEarly = 0.120, kLate = 0.150;
constexpr double kPerfect = 0.030;  // within this of the landing moment: a perfect hop (double gain)
constexpr double kAutoChain = 0.25; // auto hop: landing-to-hop time that still keeps the air speed
// While hopping, the game's own air control is turned down so steering comes from air strafing (below), the way
// bunny hopping works in Source / Quake: you gain speed by strafing while turning the mouse, and the velocity vector
// carries through from hop to hop.
constexpr float kHopAirControl = 0.05f;
constexpr double kAirSpeedCap = 0.12; // air strafe: wished speed along the input, as a fraction of walking speed

struct {
    Weak cmc;
    float origAirControl = -1;
    bool prevHeld = false, airborne = false, logged = false;
    double bufferAt = -10, landedAt = -10, lastJumpAt = -10, lastFrame = 0;
    Vec airVel{};          // the last airborne velocity, before landing friction
    int hops = 0;
} g;

std::atomic<double> g_keyDownAt{-10.0}; // window thread: when the jump key went down (precise timing)

std::mutex g_hudMutex;
HudInfo g_hud;

void SetHud(const char* grade, int gain, double speed, double now)
{
    std::lock_guard lock(g_hudMutex);
    g_hud.grade = grade;
    g_hud.gain = gain;
    g_hud.speed = speed;
    g_hud.hops = g.hops;
    g_hud.at = now;
}

UObject* Ptr(UObject* o, const char* prop)
{
    auto p = o ? Member<UObject*>(o, prop) : nullptr;
    return p ? *p : nullptr;
}

bool GameHasFocus()
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

void SetAirControl(UObject* cmc, bool hopping)
{
    float* ac = Member<float>(cmc, "AirControl");
    if (!ac) return;
    if (hopping) {
        if (g.origAirControl < 0) g.origAirControl = *ac;
        *ac = kHopAirControl;
    } else if (g.origAirControl >= 0) {
        *ac = g.origAirControl;
        g.origAirControl = -1;
    }
}

void Reset(UObject* cmc)
{
    if (cmc) SetAirControl(cmc, false);
    g.prevHeld = g.airborne = false;
    g.bufferAt = -10;
    g.hops = 0;
}

// Nothing may jump the player out of a scripted moment: movement input ignored, a menu or the game's own cursor shown,
// or one of our movement modes on.
bool PlayerFree(UObject* pc)
{
    if (!pc || menu::IsOpen() || g_on[feat::Fly] || g_on[feat::Noclip] || g_on[feat::FreeCam]) return false;
    bool cursor = false;
    if (GetBool(pc, "bShowMouseCursor", &cursor) && cursor) return false;
    Call ignored(pc, "IsMoveInputIgnored");
    return !(ignored && ignored.Run() && ignored.Out<bool>("ReturnValue"));
}

void Launch(UObject* player, Vec v, bool xyOverride)
{
    Call(player, "LaunchCharacter").Arg(0, v).Arg(1, xyOverride).Arg(2, true).Run();
}

double Speed2D(const Vec& v) { return std::hypot(v.x, v.y); }

// Source-style air acceleration: speed is only added along the input direction up to a small cap of that component, so
// pure forward input gains nothing, while strafing and turning together keep adding speed.
void AirStrafe(UObject* cmc, Vec& vel, double dt)
{
    auto* accel = Member<Vec>(cmc, "Acceleration"); // the current input acceleration (world space)
    float* walk = Member<float>(cmc, "MaxWalkSpeed");
    double strength = g_value[feat::BhopAirStrafe];
    if (!accel || strength <= 0 || dt <= 0) return;
    double len = Speed2D(*accel);
    if (len < 1e-3) return; // no input: momentum is kept as it is
    double wx = accel->x / len, wy = accel->y / len;
    double walkSpeed = walk && *walk > 50 ? *walk : 600.0;
    double wishSpeed = walkSpeed * kAirSpeedCap;
    double current = vel.x * wx + vel.y * wy;
    double add = wishSpeed - current;
    if (add <= 0) return;
    double step = std::min(strength * walkSpeed * dt, add);
    vel.x += step * wx;
    vel.y += step * wy;
}

// ---------------------------------------------------------------- key conflicts

std::mutex g_conflictMutex;
std::string g_conflict;
unsigned g_conflictKey = ~0u;
double g_conflictAt = -100;

std::string Pretty(std::string action)
{
    for (const char* p : {"IA_", "InputAction_", "Input_"})
        if (action.rfind(p, 0) == 0) action.erase(0, strlen(p));
    std::string out;
    for (size_t i = 0; i < action.size(); i++) {
        char c = action[i] == '_' ? ' ' : action[i];
        if (i && std::isupper(uint8_t(c)) && std::islower(uint8_t(action[i - 1]))) out += ' ';
        out += c;
    }
    return out;
}

// Walks a TArray of structs (by its array property) and calls f(element) for each.
template <class F> void ForEachElement(UObject* owner, void* base, const UClass* type, const char* arrayName, F&& f)
{
    Prop arr = base && type ? FindProp(type, arrayName) : Prop{};
    if (!arr || arr.kind != 'a' || !arr.inner) return;
    const void* elemStruct = nullptr; // FArrayProperty::Inner is an FStructProperty: its UScriptStruct follows FProperty
    if (!SafeRead(static_cast<const uint8_t*>(arr.inner) + 0x70, &elemStruct, sizeof(elemStruct)) || !elemStruct) return;
    int32_t size = StructSize(static_cast<const UClass*>(elemStruct));
    struct Arr {
        uint8_t* data;
        int32_t num, max;
    } a{};
    if (size <= 0 || !SafeRead(static_cast<uint8_t*>(base) + arr.offset, &a, sizeof(a)) || a.num < 0 || a.num > 4096) return;
    for (int32_t i = 0; i < a.num && a.data; i++) f(a.data + size_t(i) * size, static_cast<const UClass*>(elemStruct));
    (void)owner;
}

void ScanConflicts(unsigned vk)
{
    std::string key = KeyLabel(vk), found;
    auto add = [&](const std::string& action) {
        std::string p = Pretty(action);
        if (!p.empty() && found.find(p) == std::string::npos) found += (found.empty() ? "" : ", ") + p;
    };
    auto check = [&](uint8_t* elem, const UClass* type, const char* nameField, bool isObject) {
        Prop k = FindProp(type, "Key"), n = FindProp(type, nameField);
        if (!k || !n || NameToString(elem + k.offset) != key) return;
        if (isObject) {
            UObject* action = *reinterpret_cast<UObject**>(elem + n.offset);
            if (IsLive(action)) add(Name(action));
        } else {
            add(NameToString(elem + n.offset));
        }
    };
    if (!key.empty()) {
        // Enhanced Input: every mapping context (UE 5.6 keeps them in DefaultKeyMappings.Mappings).
        ForEachInstance(FindClass("InputMappingContext"), [&](UObject* imc) {
            const UClass* cls = ClassOf(imc);
            auto visit = [&](uint8_t* e, const UClass* t) { check(e, t, "Action", true); };
            ForEachElement(imc, imc, cls, "Mappings", visit);
            Prop dkm = FindProp(cls, "DefaultKeyMappings");
            if (dkm && dkm.inner)
                ForEachElement(imc, reinterpret_cast<uint8_t*>(imc) + dkm.offset, static_cast<const UClass*>(dkm.inner), "Mappings", visit);
            return true;
        });
        // Legacy input settings.
        if (UObject* is = FindObject("Default__InputSettings")) {
            ForEachElement(is, is, ClassOf(is), "ActionMappings", [&](uint8_t* e, const UClass* t) { check(e, t, "ActionName", false); });
            ForEachElement(is, is, ClassOf(is), "AxisMappings", [&](uint8_t* e, const UClass* t) { check(e, t, "AxisName", false); });
        }
    }
    if (!found.empty()) km::Log("Jump: the jump key (%s) is also the game's %s", key.c_str(), found.c_str());
    std::lock_guard lock(g_conflictMutex);
    g_conflict = found;
}

} // namespace

std::string Conflict()
{
    std::lock_guard lock(g_conflictMutex);
    return g_conflict;
}

std::string KeyLabel(unsigned vk)
{
    if ((vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    static const char* const digits[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::string("NumPad") + digits[vk - VK_NUMPAD0];
    switch (vk) {
    case VK_SPACE: return "SpaceBar";
    case VK_SHIFT: case VK_LSHIFT: return "LeftShift";
    case VK_RSHIFT: return "RightShift";
    case VK_CONTROL: case VK_LCONTROL: return "LeftControl";
    case VK_RCONTROL: return "RightControl";
    case VK_MENU: case VK_LMENU: return "LeftAlt";
    case VK_RMENU: return "RightAlt";
    case VK_TAB: return "Tab";
    case VK_CAPITAL: return "CapsLock";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "BackSpace";
    case VK_ESCAPE: return "Escape";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_XBUTTON1: return "ThumbMouseButton";
    case VK_XBUTTON2: return "ThumbMouseButton2";
    case VK_MBUTTON: return "MiddleMouseButton";
    case VK_OEM_3: return "Tilde";
    default: return "";
    }
}


void OnKey(unsigned vk)
{
    if (vk && vk == g_key) g_keyDownAt = PreciseSeconds();
}

std::atomic<double> g_liveSpeed{0};
std::atomic<bool> g_liveValid{false};
double LiveSpeed(bool* valid)
{
    if (valid) *valid = g_liveValid;
    return g_liveSpeed;
}

HudInfo Hud()
{
    std::lock_guard lock(g_hudMutex);
    return g_hud;
}

void Frame(UObject* player, double now)
{
    // The game's bindings, checked when the key changes and every 10 s while jumping is on (mapping contexts load
    // with the player).
    if (g_on[feat::Jump] && (g_key != g_conflictKey || now - g_conflictAt > 10)) {
        g_conflictKey = g_key;
        g_conflictAt = now;
        std::function<void()> scan = [] { ScanConflicts(g_conflictKey); };
        if (!Guard(scan)) km::Log("Jump: reading the game's key bindings faulted");
    }
    double dt = g.lastFrame > 0 ? std::clamp(now - g.lastFrame, 0.0, 0.1) : 0.0;
    g.lastFrame = now;

    UObject* cmc = Ptr(player, "CharacterMovement");
    if (cmc != g.cmc.Get()) { // new pawn: the old component's air control went with it
        g.cmc = Weak(cmc);
        g.origAirControl = -1;
        Reset(nullptr);
    }
    if (Vec* v = player && cmc ? Member<Vec>(cmc, "Velocity") : nullptr) {
        g_liveSpeed = Speed2D(*v);
        g_liveValid = true;
    } else {
        g_liveValid = false;
    }
    if (!player || !cmc || !g_on[feat::Jump]) {
        Reset(cmc);
        return;
    }
    uint8_t* mode = Member<uint8_t>(cmc, "MovementMode");
    auto* vel = Member<Vec>(cmc, "Velocity");
    if (!mode || !vel) return;
    const bool bhop = g_on[feat::Bhop], autoHop = bhop && g_on[feat::BhopAuto];
    SetAirControl(cmc, bhop);

    UObject* pc = nullptr;
    Call controller(player, "GetController");
    if (controller && controller.Run()) pc = controller.Out<UObject*>("ReturnValue");

    unsigned key = g_key;
    bool held = key && GameHasFocus() && (GetAsyncKeyState(int(key)) & 0x8000) && PlayerFree(pc);
    bool pressed = held && !g.prevHeld;
    g.prevHeld = held;
    // When the press really happened: the window message's time when it's this press, else this frame.
    double keyAt = g_keyDownAt;
    double pressAt = pressed && now - keyAt < 0.1 && keyAt <= now ? keyAt : now;

    bool ground = *mode == MOVE_Walking || *mode == MOVE_NavWalking;
    if (*mode == MOVE_Falling) {
        if (bhop) AirStrafe(cmc, *vel, dt);
        g.airVel = *vel;
        g.airborne = true;
    } else if (ground && g.airborne) {
        g.airborne = false;
        g.landedAt = now;
    }
    if (pressed) g.bufferAt = pressAt;

    // Decide whether to hop now, and how well it was timed.
    bool hop = false, chained = false;
    double offset = 1e9; // press time relative to landing (negative = before)
    if (ground && now - g.lastJumpAt > 0.1) {
        bool buffered = g.bufferAt > g.lastJumpAt && g.bufferAt >= g.landedAt - kEarly; // pressed for this landing
        if (buffered) {
            hop = true;
            offset = g.bufferAt - g.landedAt;
            chained = bhop && offset <= kLate && g.landedAt > g.lastJumpAt;
        } else if (autoHop && held) {
            hop = true;
            offset = now - g.landedAt;
            chained = offset <= kAutoChain && g.landedAt > g.lastJumpAt;
        }
    }
    if (!hop) return;

    float* jz = Member<float>(cmc, "JumpZVelocity"); // already scaled by Jump height
    double z = jz && *jz > 50 ? *jz : 420.0 * g_value[feat::JumpMult];
    if (chained && Speed2D(g.airVel) > 50) {
        bool perfect = !autoHop && std::fabs(offset) <= kPerfect;
        double gain = g_value[feat::BhopGain] / 100.0 * (perfect ? 2.0 : 1.0);
        double speed = Speed2D(g.airVel) * (1.0 + gain);
        float* walk = Member<float>(cmc, "MaxWalkSpeed");
        double capMult = g_value[feat::BhopCap]; // 0 = unlimited
        if (capMult > 0) speed = std::min(speed, std::max((walk && *walk > 50 ? *walk : 600.0) * capMult, Speed2D(g.airVel)));
        double scale = speed / Speed2D(g.airVel);
        // The velocity vector carries over from the air: same direction, scaled by the hop's gain.
        Launch(player, {g.airVel.x * scale, g.airVel.y * scale, z}, true);
        g.hops++;
        SetHud(perfect ? "PERFECT" : "GOOD", int(std::lround(gain * 100)), speed, now);
    } else {
        Launch(player, {0, 0, z}, false); // keeps the ground speed you had
        bool late = bhop && g.hops > 0 && offset < 1.0; // a chain was going and the press came too late
        g.hops = 0;
        if (late) SetHud("LATE", 0, Speed2D(*vel), now);
    }
    g.lastJumpAt = now;
    g.bufferAt = -10;
    if (!g.logged) {
        g.logged = true;
        km::Log("Jump: first jump (%.0f cm/s up%s)", z, bhop ? (autoHop ? ", auto hop" : ", manual hop timing") : "");
    }
}

} // namespace jump
