// Game fixes ported from the UE4SS Lua mods (see fixes.h), following each mod's main.lua: same functions hooked,
// same properties, same timings. Differences are noted where they are. Game thread only, except OnKey.
#include "fixes.h"
#include "config.h"
#include "features.h"
#include "hooks.h"
#include "log.h"
#include "menu.h"
#include "ue.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <map>
#include <sstream>
#include <mutex>
#include <set>
#include <vector>

using namespace ue;

namespace fixes {
namespace {

using feat::g_on;
using feat::g_value;

// ================================================================ helpers

UObject* Ptr(UObject* o, const char* prop)
{
    auto p = o ? Member<UObject*>(o, prop) : nullptr;
    return p && IsLive(*p) ? *p : nullptr;
}

std::string ClassName(const UObject* o) { return o ? Name(reinterpret_cast<const UObject*>(ClassOf(o))) : ""; }
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

double Now() { return PreciseSeconds(); }

// A float or double property (Blueprint "float" variables are doubles in UE5).
struct Num {
    void* p = nullptr;
    bool dbl = false;
    explicit operator bool() const { return p != nullptr; }
    double Get() const { return dbl ? *static_cast<double*>(p) : *static_cast<float*>(p); }
    void Set(double v) const
    {
        if (dbl) *static_cast<double*>(p) = v;
        else *static_cast<float*>(p) = float(v);
    }
};
Num NumAt(void* base, const UClass* type, const char* name)
{
    Prop p = base && type ? FindProp(type, name) : Prop{};
    if (!p || (p.kind != 'f' && p.kind != 'd')) return {};
    return {static_cast<uint8_t*>(base) + p.offset, p.kind == 'd'};
}
Num NumOf(UObject* o, const char* name) { return NumAt(o, o ? ClassOf(o) : nullptr, name); }

bool CallBool(UObject* o, const char* fn, bool* out)
{
    Call c(o, fn);
    if (!c || !c.Run()) return false;
    *out = c.Out<bool>("ReturnValue");
    return true;
}

float CallFloat(UObject* o, const char* fn, float def)
{
    Call c(o, fn);
    return c && c.Run() ? c.Out<float>("ReturnValue") : def;
}

// Work for later on the game thread (UE4SS's ExecuteWithDelay).
std::vector<std::pair<double, std::function<void()>>> g_later;
void Later(double seconds, std::function<void()> fn) { g_later.push_back({Now() + seconds, std::move(fn)}); }
void RunLater()
{
    if (g_later.empty()) return;
    double now = Now();
    std::vector<std::function<void()>> due;
    for (auto it = g_later.begin(); it != g_later.end();)
        if (it->first <= now) {
            due.push_back(std::move(it->second));
            it = g_later.erase(it);
        } else {
            ++it;
        }
    for (auto& f : due)
        if (!Guard(f)) km::Log("Fixes: a delayed task faulted (skipped)");
}

std::set<std::string> g_logged;
void LogOnce(const std::string& key, const char* fmt, ...)
{
    if (g_logged.size() > 2000 || !g_logged.insert(key).second) return;
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    km::Log("%s", buf);
}

// ---------------------------------------------------------------- hook registration
// Blueprint classes load with their levels: their hooks are retried every few seconds until the class exists, and
// redone when a level change unloads and reloads it (UE4SS's RegisterHook + NotifyOnNewObject retries).
struct Reg {
    std::string cls, fn;
    hooks::Callback pre, post;
    UFunction* hooked = nullptr;
};
std::vector<Reg> g_regs;
uint64_t g_regAt = 0;

void Want(const char* cls, const char* fn, hooks::Callback pre, hooks::Callback post)
{
    g_regs.push_back({cls, fn, std::move(pre), std::move(post), nullptr});
}

void TickRegs(uint64_t now)
{
    if (now - g_regAt < 3000) return;
    g_regAt = now;
    std::vector<Reg*> todo;
    for (Reg& r : g_regs)
        if (!r.hooked || !hooks::IsHooked(r.hooked)) {
            r.hooked = nullptr;
            todo.push_back(&r);
        }
    if (todo.empty()) return;
    std::vector<const char*> names;
    for (Reg* r : todo) {
        if (std::find_if(names.begin(), names.end(), [&](const char* n) { return r->cls == n; }) == names.end())
            names.push_back(r->cls.c_str());
    }
    std::vector<UClass*> found(names.size());
    FindClasses(names.data(), int(names.size()), found.data());
    for (size_t i = 0; i < names.size(); i++)
        if (found[i] && !IsLive(found[i])) { // unloaded since it was cached
            ForgetClass(names[i]);
            found[i] = nullptr;
        }
    for (Reg* r : todo) {
        size_t i = size_t(std::find_if(names.begin(), names.end(), [&](const char* n) { return r->cls == n; }) - names.begin());
        UClass* cls = found[i];
        if (!cls) continue;
        UFunction* f = FindFunction(cls, r->fn.c_str());
        // Only the class's own function: a Blueprint that doesn't override an event would otherwise hook the engine's
        // version for every actor.
        if (!f || !IsLive(f) || !NameIs(reinterpret_cast<UObject*>(f), r->fn.c_str()) ||
            OuterOf(reinterpret_cast<UObject*>(f)) != reinterpret_cast<UObject*>(cls))
            continue;
        if (hooks::Hook(f, r->pre, r->post)) {
            r->hooked = f;
            km::Log("Fixes: hooked %s:%s", r->cls.c_str(), r->fn.c_str());
        }
    }
}

// ================================================================ ToolInputFix (mod 42)
// GA_UseTool's K2_OnEndAbility calls CoreController.BP_FlushPressedKeys, which drops held movement keys when the
// flashlight turns off or a heal ends (GA_UseToolMedKit reaches it through its parent's K2_OnEndAbility).
// APlayerController::FlushPressedKeys null-checks PlayerInput, so clearing it for that one call turns the flush into a
// no-op. The flush itself still runs, so the script VM steps over the call normally.
struct {
    bool registered = false, armed = false;
    Weak stashPc;
    UObject* stashInput = nullptr;
    UFunction* flush = nullptr;
} g_tool;

void ToolFlushPre(UObject* pc, uint8_t*)
{
    if (!g_tool.armed || !g_on[feat::FixToolInput]) return;
    g_tool.armed = false;
    auto* slot = pc ? Member<UObject*>(pc, "PlayerInput") : nullptr;
    if (!slot || !IsLive(*slot)) return;
    g_tool.stashPc = Weak(pc);
    g_tool.stashInput = *slot;
    *slot = nullptr;
    LogOnce("toolflush", "Tool input fix: held keys kept through the flashlight / heal flush");
}

void ToolFlushPost(UObject*, uint8_t*)
{
    if (!g_tool.stashInput) return;
    UObject* pc = g_tool.stashPc.Get();
    if (auto* slot = pc ? Member<UObject*>(pc, "PlayerInput") : nullptr; slot && !*slot) *slot = g_tool.stashInput;
    g_tool.stashInput = nullptr;
    g_tool.stashPc = {};
}

void TickToolInput(std::string& why)
{
    if (!g_on[feat::FixToolInput]) return;
    if (!g_tool.registered) {
        g_tool.registered = true;
        Want("CoreController", "BP_FlushPressedKeys", ToolFlushPre, ToolFlushPost);
        for (const char* ga : {"GA_Flashlight_C", "GA_UseToolMedKit_C"})
            Want(ga, "K2_OnEndAbility", [](UObject*, uint8_t*) { g_tool.armed = true; }, [](UObject*, uint8_t*) { g_tool.armed = false; });
    }
    (void)why;
}

// ================================================================ CutsceneAutoSkip (mod 46)
// The game's hold-E skip lives on W_CinematicScreen_C: holding E ticks a skip timer and at SkipTime calls
// StopTimerAndSkip, which skips the widget's CallerTask (TASK_PlaySequencerAnim). Once the widget is visible, a level
// sequence plays and the task isn't marked unskippable, wait 1 s, call StopTimerAndSkip (SkipCinematicInstantly if
// that fails), retry every 1.5 s, at most 3 tries.
struct CutState {
    double since = 0, last = 0;
    int tries = 0;
};
struct {
    std::map<int32_t, CutState> seen; // by object index
    std::vector<Weak> widgets;
    uint64_t pollAt = 0, scanAt = 0;
} g_cut;

// Sequence players are collected every 2 s and only asked IsPlaying in between (the Lua mod scanned every object
// five times a second).
std::vector<Weak> g_seqPlayers;
uint64_t g_seqScanAt = 0;
int SequencesPlaying()
{
    uint64_t now = GetTickCount64();
    if (now - g_seqScanAt > 2000) {
        g_seqScanAt = now;
        g_seqPlayers.clear();
        ForEachInstance(FindClass("LevelSequencePlayer"), [](UObject* p) {
            g_seqPlayers.push_back(Weak(p));
            return g_seqPlayers.size() < 64;
        });
    }
    int n = 0;
    for (const Weak& w : g_seqPlayers) {
        bool playing = false;
        if (UObject* p = w.Get(); p && CallBool(p, "IsPlaying", &playing) && playing) n++;
    }
    return n;
}

// The task's IsSkippable() (a return value or a bool out parameter), else its bIsSkipable; -1 when unreadable.
int TaskSkippable(UObject* task)
{
    UFunction* fn = FindFunction(ClassOf(task), "IsSkippable");
    if (fn) {
        Call c(task, "IsSkippable");
        if (c.Run())
            for (const Param& p : Params(fn))
                if (p.kind == 'b') return c.Out<bool>(p.name.c_str()) ? 1 : 0;
    }
    bool v = false;
    if (GetBool(task, "bIsSkipable", &v)) return v ? 1 : 0;
    return -1;
}

void TickCutscene(uint64_t now, std::string&)
{
    if (!g_on[feat::CutsceneSkip]) {
        g_cut.seen.clear();
        return;
    }
    if (now - g_cut.pollAt < 200) return;
    g_cut.pollAt = now;
    if (g_cut.widgets.empty() || now - g_cut.scanAt > 2000) { // the widget is created once per level
        g_cut.scanAt = now;
        g_cut.widgets.clear();
        ForEachInstance(FindClass("W_CinematicScreen_C"), [](UObject* w) {
            g_cut.widgets.push_back(Weak(w));
            return g_cut.widgets.size() < 8;
        });
    }
    int playing = -1; // counted only when a widget is up (the Lua mod counts every poll)
    double t = Now();
    for (const Weak& ww : g_cut.widgets) {
        UObject* w = ww.Get();
        if (!w) continue;
        CutState& st = g_cut.seen[ObjectIndex(w)];
        bool visible = false;
        CallBool(w, "IsVisible", &visible);
        if (visible && playing < 0) playing = SequencesPlaying();
        UObject* task = Ptr(w, "CallerTask");
        int skippable = task ? TaskSkippable(task) : -1;
        if (!visible || playing <= 0 || skippable == 0) {
            st = {};
            continue;
        }
        if (!st.since) st.since = t;
        if (t - st.since < 1.0 || st.tries >= 3 || (st.last && t - st.last < 1.5)) continue;
        st.tries++;
        st.last = t;
        km::Log("Cutscene auto-skip: skipping %s (%s, try %d)", task ? Name(task).c_str() : "unknown task",
                skippable < 0 ? "skippable state unreadable" : "skippable", st.tries);
        bool ok = Call(w, "StopTimerAndSkip").Run();
        if (!ok) ok = Call(w, "SkipCinematicInstantly").Run();
        if (!ok) LogOnce("cutnoskip", "Cutscene auto-skip: the cutscene screen has no skip function");
    }
}

// ================================================================ QuickInteract (mod 45)
// Pickups (BP_Pickable_Base and children) and ladders (owner class containing BP_Ladder) get their three hold times
// capped (never raised); bear traps and other scripted holds are skipped by owner class name. While GA_ClimbingLadder
// is active, the playing montage runs at the ladder rate. The Lua mod patches new components 0.3 s after they're
// created; here they're patched by a scan every 1.5 s. Turning the fix off restores the original hold times.
const char* const kHoldProps[3] = {"HoldingTime", "KeyboardHoldingTime", "GamepadHoldingTime"};
struct Hold {
    Weak comp;
    double orig[3];
};
struct {
    std::map<int32_t, Hold> holds;  // patched, by object index
    std::set<int32_t> skipped;      // looked at and left alone
    uint64_t scanAt = 0;
    float applied = -1;
    bool registered = false, ladder = false, was = false;
    double ladderAt = 0;
} g_qi;

bool Mundane(UObject* owner)
{
    std::string cls = ClassName(owner);
    for (const char* deny : {"BearTrapRemove", "FreeHimself", "Struggle", "Stitch", "Strangle"})
        if (Has(cls, deny)) return false;
    if (Has(cls, "BP_Ladder")) return true;
    UClass* pickable = FindClass("BP_Pickable_Base_C");
    return pickable && IsA(owner, pickable);
}

void ApplyHold(Hold& h, float cap)
{
    UObject* c = h.comp.Get();
    for (int i = 0; c && i < 3; i++)
        if (Num n = NumOf(c, kHoldProps[i])) n.Set(cap > 0 ? std::min(h.orig[i], double(cap)) : h.orig[i]);
}

void RestoreHolds()
{
    for (auto& [i, h] : g_qi.holds) ApplyHold(h, 0);
    g_qi.holds.clear();
    g_qi.skipped.clear();
    g_qi.applied = -1;
}

void ScanHolds(float cap, std::string& why)
{
    UClass* comp = FindClass("COMP_Interact");
    if (!comp) comp = FindClass("COMP_Interact_C");
    if (!comp) {
        why = "Interaction component not found yet (load a save).";
        return;
    }
    int patched = 0;
    ForEachInstance(comp, [&](UObject* c) {
        int32_t idx = ObjectIndex(c);
        if (g_qi.holds.count(idx) || g_qi.skipped.count(idx)) return true;
        Call owner(c, "GetOwner");
        UObject* o = owner.Run() ? owner.Out<UObject*>("ReturnValue") : nullptr;
        if (!IsLive(o)) return true; // not set up yet: next scan
        Hold h{Weak(c), {}};
        bool ok = Mundane(o);
        for (int i = 0; i < 3 && ok; i++) {
            Num n = NumOf(c, kHoldProps[i]);
            if (!n) ok = false;
            else h.orig[i] = n.Get();
        }
        if (!ok) {
            g_qi.skipped.insert(idx);
            return true;
        }
        ApplyHold(h, cap);
        g_qi.holds[idx] = h;
        patched++;
        return true;
    });
    if (patched) km::Log("Quick interact: %d pickup / ladder interactions capped at %.2f s", patched, cap);
}

void TickQuickInteract(UObject* player, uint64_t now, std::string& why)
{
    bool on = g_on[feat::QuickInteract];
    if (g_qi.was && !on) RestoreHolds();
    g_qi.was = on;
    if (!on) return;
    if (!g_qi.registered) {
        g_qi.registered = true;
        Want("GA_ClimbingLadder_C", "K2_ActivateAbility", [](UObject*, uint8_t*) { g_qi.ladder = true; }, nullptr);
        Want("GA_ClimbingLadder_C", "K2_OnEndAbility", [](UObject*, uint8_t*) { g_qi.ladder = false; }, nullptr);
    }
    if (!player) return;
    float cap = g_value[feat::InteractHold];
    if (cap != g_qi.applied) { // slider moved: re-apply to everything patched so far
        g_qi.applied = cap;
        for (auto& [i, h] : g_qi.holds) ApplyHold(h, cap);
    }
    if (now - g_qi.scanAt > 1500) {
        g_qi.scanAt = now;
        for (auto it = g_qi.holds.begin(); it != g_qi.holds.end();) it = it->second.comp.Get() ? std::next(it) : g_qi.holds.erase(it);
        if (g_qi.skipped.size() > 20000) g_qi.skipped.clear();
        ScanHolds(cap, why);
    }
    double t = Now();
    if (g_qi.ladder && t - g_qi.ladderAt >= 0.1) { // every 100 ms while climbing, as the Lua mod
        g_qi.ladderAt = t;
        Call ga(Ptr(player, "Mesh"), "GetAnimInstance");
        UObject* anim = ga.Run() ? ga.Out<UObject*>("ReturnValue") : nullptr;
        Call cur(anim, "GetCurrentActiveMontage");
        UObject* m = cur.Run() ? cur.Out<UObject*>("ReturnValue") : nullptr;
        if (IsLive(m)) Call(anim, "Montage_SetPlayRate").Arg(0, m).Arg(1, g_value[feat::LadderSpeed].load()).Run();
    }
}

// ================================================================ KeypadInputFix (mod 43)
// GA_BaseInteractFocusEnvPuzzle sets bInputDelayX/Y after each axis input and ignores that axis until a timer
// (InputDelayTime, 0.4 s) clears it. Each press fires the axis twice within ~10 ms and the flag hides the duplicate, so
// the flag is released 30 ms after the last axis event, once no key of that axis is held, instead. Number keys select
// the matching keypad button and press it through the keypad's own PuzzleInputInteract.
// Unlike the Lua mod: number keys only act while a keypad is open (found through the player state, no object scan per
// key press), the poll runs inside the game's frame instead of a separate 8 ms loop, and every keypad call checks the
// keypad is still live and active first.
const char* const kAxisFlags[2] = {"bInputDelayX", "bInputDelayY"};
const char* const kAxisKeys[2][4] = {{"D", "A", "Right", "Left"}, {"W", "S", "Up", "Down"}};
struct {
    bool registered = false;
    Weak ga, numpad;
    double lastEvent[2] = {-1, -1};
    uint64_t keyNames[2][4] = {};
    bool namesReady = false, keyApiOk = true;
    std::atomic<bool> open{false};
} g_kp;

// The keypad a focus-puzzle ability serves, or nullptr.
UObject* KeypadOf(UObject* ga)
{
    if (!IsLive(ga)) return nullptr;
    UObject* r = Ptr(Ptr(ga, "TASK Focus Env Puzzle"), "PuzzleActorRef");
    UClass* np = FindClass("BP_EnvPuzzleNumpad_C");
    return r && np && IsA(r, np) ? r : nullptr;
}

// The open keypad, through the player state's puzzle task (works before the first cursor move too).
UObject* OpenKeypad(UObject* pc)
{
    UObject* task = Ptr(Ptr(pc, "PlayerState"), "PuzzleRef");
    UObject* r = Ptr(task, "PuzzleActorRef");
    UClass* np = FindClass("BP_EnvPuzzleNumpad_C");
    if (!r || !np || !IsA(r, np)) return nullptr;
    bool active = true, closing = false;
    if (GetBool(r, "bPuzzleActive", &active) && !active) return nullptr;
    if (GetBool(r, "PuzzleFinished", &closing) && closing) return nullptr;
    return r;
}

void AxisEvent(int axis, UObject* ga)
{
    if (!g_on[feat::KeypadFix] || !KeypadOf(ga)) return;
    g_kp.ga = Weak(ga);
    g_kp.lastEvent[axis] = Now();
}

bool MakeKeyNames()
{
    UObject* ksl = FindObject("Default__KismetStringLibrary");
    for (int a = 0; a < 2; a++)
        for (int k = 0; k < 4; k++) {
            static wchar_t buf[2][4][8];
            const char* n = kAxisKeys[a][k];
            size_t len = strlen(n);
            for (size_t i = 0; i <= len; i++) buf[a][k][i] = wchar_t(n[i]);
            FString s{buf[a][k], int32_t(len + 1), int32_t(len + 1)};
            Call c(ksl, "Conv_StringToName");
            if (!c.Arg(0, s).Run()) return false;
            g_kp.keyNames[a][k] = c.Out<uint64_t>("ReturnValue");
        }
    return true;
}

bool AnyDown(UObject* pc, int axis)
{
    struct FKey {
        uint64_t name;
        void* details[2];
    };
    for (int k = 0; k < 4; k++) {
        FKey key{g_kp.keyNames[axis][k], {}};
        Call c(pc, "IsInputKeyDown");
        if (!c.ArgRaw(0, &key, sizeof(key)).Run()) {
            if (g_kp.keyApiOk) km::Log("Keypad input fix: IsInputKeyDown failed");
            g_kp.keyApiOk = false;
            return false;
        }
        if (c.Out<bool>("ReturnValue")) return true;
    }
    return false;
}

void TickKeypad(UObject* pc, std::string& why)
{
    if (!g_on[feat::KeypadFix]) {
        g_kp.open = false;
        return;
    }
    if (!g_kp.registered) {
        g_kp.registered = true;
        const char* ga = "GA_BaseInteractFocusEnvPuzzle_C";
        Want(ga, "XAxisOverride", [](UObject* o, uint8_t*) { AxisEvent(0, o); }, nullptr);
        Want(ga, "YAxisOverride", [](UObject* o, uint8_t*) { AxisEvent(1, o); }, nullptr);
        Want(ga, "K2_ActivateAbilityFromEvent", nullptr, [](UObject* o, uint8_t*) {
            g_kp.numpad = {};
            if (KeypadOf(o)) g_kp.ga = Weak(o);
        });
        Want(ga, "K2_OnEndAbility", [](UObject*, uint8_t*) { g_kp.ga = g_kp.numpad = {}; }, nullptr);
        Want("BP_EnvPuzzleNumpad_C", "PuzzleStartedClosing", [](UObject*, uint8_t*) { g_kp.ga = g_kp.numpad = {}; }, nullptr);
    }
    if (!g_kp.namesReady) g_kp.namesReady = MakeKeyNames();
    if (!g_kp.namesReady) LogOnce("kpnames", "Keypad input fix: couldn't build the key names, the cursor fix is off");
    (void)why;
    UObject* np = OpenKeypad(pc);
    g_kp.numpad = Weak(np);
    g_kp.open = np != nullptr;
}

// Every frame while a keypad's ability is known: keep an axis blocked while one of its keys is held, open it once no
// key is held and the press has settled.
void FrameKeypad(UObject* pc, double now)
{
    UObject* ga = g_kp.ga.Get();
    if (!g_on[feat::KeypadFix] || !g_kp.namesReady || !g_kp.keyApiOk || !ga || !KeypadOf(ga) || !pc) return;
    for (int a = 0; a < 2; a++) {
        bool cur = false;
        if (!GetBool(ga, kAxisFlags[a], &cur)) continue;
        if (AnyDown(pc, a)) {
            if (!cur) SetBool(ga, kAxisFlags[a], true);
        } else if (cur && now - g_kp.lastEvent[a] > 0.030) {
            SetBool(ga, kAxisFlags[a], false);
        }
    }
}

void PressKeypad(int n)
{
    UObject* np = g_kp.numpad.Get();
    bool enabled = false;
    if (!np || !GetBool(np, "EnableKeyPress", &enabled) || !enabled) return;
    auto sphere = [&](int i) { return Ptr(np, ("Sphere" + std::to_string(i)).c_str()); };
    auto current = [&] {
        int32_t* v = Member<int32_t>(np, "CurrentHighlightedNumber");
        return v ? *v : -1;
    };
    UObject* s = sphere(n);
    if (!s) return;
    Call(np, "SetHighlightedKey").Arg(0, s).Run();
    for (int i = 0; i <= 10 && current() != n; i++)
        if (UObject* c = sphere(i)) Call(np, "SetHighlightedKey").Arg(0, c).Run();
    if (current() != n) {
        LogOnce("kpsel" + std::to_string(n), "Keypad input fix: could not select button %d", n);
        return;
    }
    Call(np, "PuzzleInputInteract").Run();
}

// ================================================================ SpeedUpInteractions (mod 44)
// Save point sequence, Lament Configuration puzzle, Episode 3 dock boat, cylinder puzzles and labyrinth reward pillars.
// Each is sped up as a whole: montage rates, the timers and fixed waits that pace the sequence, the task's timing
// values, and the actors' time dilation, all by the same factor, so every step still happens in the original order.
// A speed of 1 turns that part off. Turning the whole fix off restores what it changed.
constexpr const char* kSaveDir[4] = {"AM_FP_SaveGame_Player_IN", "AM_FP_SaveGame_Player_OUT", "AM_FP_SaveGame_Player_IN_WORM",
                                     "AM_FP_SaveGame_Player_OUT_WORM"};
const char* const kCommon[] = {"RepositioningDuration", "RotationDuration", "MotionWarpingDuration", "MW_MontageBlendIn",
                               "PlayerAnimationBlendInDuration", "PlayerAnimationBlendOutDuration"};
const char* const kRates[] = {"RepositioningSpeed", "FlashlightDimTimelinePlayRate"};
const double kConfigTimers[] = {0.1, 0.2, 0.25, 0.3, 0.5, 2.8, 3.0};

struct Window {
    double max, tail, t0 = 0, endT = 0;
    bool In()
    {
        if (!t0) return false;
        double now = Now();
        if (now - t0 > max || (endT && now > endT)) {
            t0 = endT = 0;
            return false;
        }
        return true;
    }
    bool Open() // true when it wasn't open already
    {
        if (In() && !endT) return false;
        t0 = Now();
        endT = 0;
        return true;
    }
    void Close()
    {
        if (t0 && !endT) endT = Now() + tail;
    }
};

struct {
    bool registered = false, was = false, editHooked = false;
    Window save{25, 3}, cfg{900, 6}, pillar{90, 3};
    std::map<std::string, Weak> montages;          // save montages by name
    std::map<std::string, double> montageOrig;     // their RateScale before
    std::map<std::string, double> lens;            // their play length
    std::map<std::string, std::pair<Weak, double>> pillarMontages;
    struct Field {
        Weak owner;
        Num num;
        double orig, set;
    };
    std::map<std::string, Field> fields;           // scaled task timing values, "index.prefix.name"
    std::map<int32_t, std::pair<Weak, double>> dilated; // actors' CustomTimeDilation before
    std::vector<Weak> saveActors, worms, puzzles, pillarActors;
    std::map<int32_t, std::pair<Weak, double>> inputDelay; // focus abilities' InputDelayTime before
    std::map<int32_t, std::pair<Weak, double>> cylRates;   // cylinder timelines' play rate before
    Weak boat;
    double boatOrig = 1;
    uint64_t boatAt = 0;
    std::map<int32_t, bool> boatKind;
} g_su;

bool SpeedOn() { return g_on[feat::SpeedUp]; }
// The save point and Lament puzzle are paced by timers and waits as much as by animations: without the parameter edits
// (GNatives not found) only their animations would speed up and the sequence would get out of step, so they stay off.
double SaveSpeed() { return SpeedOn() && hooks::CanEdit() ? g_value[feat::SaveSpeed].load() : 1.0; }
double ConfigSpeed() { return SpeedOn() && hooks::CanEdit() ? g_value[feat::ConfigSpeed].load() : 1.0; }
double BoatSpeed() { return SpeedOn() ? g_value[feat::BoatSpeed].load() : 1.0; }
double CylSpeed() { return SpeedOn() ? g_value[feat::CylinderSpeed].load() : 1.0; }
double CylInputSpeed() { return SpeedOn() ? g_value[feat::CylinderInputSpeed].load() : 1.0; }
double PillarSpeed() { return SpeedOn() ? g_value[feat::PillarSpeed].load() : 1.0; }

void Dilate(UObject* a, bool on, double speed)
{
    if (!IsLive(a)) return;
    float* d = Member<float>(a, "CustomTimeDilation");
    if (!d) return;
    int32_t idx = ObjectIndex(a);
    auto it = g_su.dilated.find(idx);
    if (on) {
        if (it == g_su.dilated.end()) g_su.dilated[idx] = {Weak(a), *d};
        if (*d != float(speed)) {
            *d = float(speed);
            LogOnce("dil" + ClassName(a), "Speed up: %s CustomTimeDilation -> %.2f", ClassName(a).c_str(), speed);
        }
    } else if (it != g_su.dilated.end()) {
        if (it->second.first.Get() == a) *d = float(it->second.second);
        g_su.dilated.erase(it);
    }
}

void DilateNiagara(UObject* a, double v, std::initializer_list<const char*> names)
{
    for (const char* n : names)
        if (UObject* c = Ptr(a, n)) Call(c, "SetCustomTimeDilation").Arg(0, float(v)).Run();
}

void ApplyMontages()
{
    double speed = SaveSpeed();
    for (const char* n : kSaveDir) {
        UObject* m = g_su.montages[n].Get();
        if (!m) {
            m = FindObject(n);
            g_su.montages[n] = Weak(m);
        }
        if (!m) continue;
        Num rate = NumOf(m, "RateScale");
        if (!rate) continue;
        if (!g_su.montageOrig.count(n)) g_su.montageOrig[n] = rate.Get();
        if (speed > 1 && rate.Get() != speed) {
            rate.Set(speed);
            LogOnce(std::string("ms") + n, "Speed up: %s RateScale -> %.2f", n, speed);
        }
        g_su.lens[n] = CallFloat(m, "GetPlayLength", 0);
    }
}

void RestoreMontages()
{
    for (auto& [n, orig] : g_su.montageOrig)
        if (UObject* m = g_su.montages[n].Get())
            if (Num rate = NumOf(m, "RateScale")) rate.Set(orig);
    g_su.montageOrig.clear();
}

// The task actor holds its timing twice: flat TASK_PlayerAction properties (read by the Blueprint) and the
// BaseInteractionConfig struct (read by GA_BaseInteract for repositioning). Durations shrink, rates grow.
void ScaleField(UObject* owner, void* base, const UClass* type, const std::string& prefix, const char* name, double mult)
{
    Num n = NumAt(base, type, name);
    if (!n) return;
    double cur = n.Get();
    if (cur <= 0) return;
    std::string key = std::to_string(ObjectIndex(owner)) + "." + prefix + name;
    auto it = g_su.fields.find(key);
    if (it != g_su.fields.end() && std::fabs(cur - it->second.set) < 1e-4) return; // already scaled
    double nv = cur * mult;
    n.Set(nv);
    g_su.fields[key] = {Weak(owner), n, cur, nv};
    LogOnce(key + std::to_string(cur), "Speed up: %s%s %.3f -> %.3f", prefix.c_str(), name, cur, nv);
}

void ScaleActor(UObject* a, double speed)
{
    if (!IsLive(a) || speed <= 1) return;
    const UClass* cls = ClassOf(a);
    Prop cfg = FindProp(cls, "BaseInteractionConfig");
    struct Box {
        void* base;
        const UClass* type;
        const char* prefix;
        const char* extra;
    } boxes[2] = {{a, cls, "", "NoAnimCompletionDelay"},
                  {cfg && cfg.inner ? reinterpret_cast<uint8_t*>(a) + cfg.offset : nullptr, static_cast<const UClass*>(cfg.inner),
                   "cfg.", "NoAnimCompletionTime"}};
    for (const Box& b : boxes) {
        if (!b.base) continue;
        for (const char* n : kCommon) ScaleField(a, b.base, b.type, b.prefix, n, 1 / speed);
        ScaleField(a, b.base, b.type, b.prefix, b.extra, 1 / speed);
        for (const char* n : kRates) ScaleField(a, b.base, b.type, b.prefix, n, speed);
    }
}

void RestoreFields()
{
    for (auto& [k, f] : g_su.fields)
        if (f.owner.Get() && std::fabs(f.num.Get() - f.set) < 1e-4) f.num.Set(f.orig);
    g_su.fields.clear();
}

void DilateWorms(bool on)
{
    if (on) {
        ForEachInstance(FindClass("BP_SaveTokenAnimatedWorm_C"), [](UObject* w) {
            g_su.worms.push_back(Weak(w));
            Dilate(w, true, SaveSpeed());
            return true;
        });
    } else {
        for (const Weak& w : g_su.worms) Dilate(w.Get(), false, 1);
        g_su.worms.clear();
    }
}

void SaveRestore()
{
    if (g_su.save.In()) return;
    for (const Weak& w : g_su.saveActors) {
        Dilate(w.Get(), false, 1);
        if (UObject* a = w.Get()) DilateNiagara(a, 1.0, {"VFX_BloodBurst", "NS_SaveGameDarkSmoke"});
    }
    g_su.saveActors.clear();
    DilateWorms(false);
}

void SaveOpen(UObject* a, uint8_t*)
{
    double speed = SaveSpeed();
    if (speed <= 1) return;
    ScaleActor(a, speed);
    ApplyMontages();
    if (!g_su.save.Open()) return;
    Later(g_su.save.max + 0.5, SaveRestore);
    if (IsLive(a)) {
        g_su.saveActors.push_back(Weak(a));
        Dilate(a, true, speed);
        DilateNiagara(a, speed, {"VFX_BloodBurst", "NS_SaveGameDarkSmoke"});
        DilateWorms(true);
    }
}

void SaveClose(UObject*, uint8_t*)
{
    if (!g_su.save.t0 || g_su.save.endT) return;
    km::Log("Speed up: save sequence took %.2f s", Now() - g_su.save.t0);
    g_su.save.Close();
    Later(g_su.save.tail + 0.1, SaveRestore);
}

// DoSave (IN length minus blend out) and OnSavePointFinished (OUT length) are timers on the montage length.
bool SaveTimerMatches(double t)
{
    for (const char* n : {"AM_FP_SaveGame_Player_IN", "AM_FP_SaveGame_Player_IN_WORM"})
        if (auto it = g_su.lens.find(n); it != g_su.lens.end() && t <= it->second + 0.01 && t >= it->second - 1.0) return true;
    for (const char* n : {"AM_FP_SaveGame_Player_OUT", "AM_FP_SaveGame_Player_OUT_WORM"})
        if (auto it = g_su.lens.find(n); it != g_su.lens.end() && std::fabs(t - it->second) < 0.01) return true;
    return false;
}

bool ConfigTimerMatches(double t)
{
    for (double c : kConfigTimers)
        if (std::fabs(t - c) < 0.001) return true;
    for (const Weak& w : g_su.puzzles)
        if (UObject* a = w.Get())
            if (Num l = NumOf(a, "CurrentAnimLength"); l && l.Get() > 0 && std::fabs(t - l.Get()) < 0.001) return true;
    return false;
}

void ConfigRestore()
{
    if (g_su.cfg.In()) return;
    for (const Weak& w : g_su.puzzles) Dilate(w.Get(), false, 1);
    g_su.puzzles.clear();
}

void ConfigOpen(UObject* a)
{
    double speed = ConfigSpeed();
    if (speed <= 1) return;
    if (g_su.cfg.Open()) Later(g_su.cfg.max + 0.5, ConfigRestore);
    if (IsLive(a)) {
        g_su.puzzles.push_back(Weak(a));
        Dilate(a, true, speed);
    }
}

void ConfigClose(UObject*, uint8_t*)
{
    if (!g_su.cfg.t0 || g_su.cfg.endT) return;
    g_su.cfg.Close();
    Later(g_su.cfg.tail + 0.1, ConfigRestore);
}

// ConfigurationItemUsed fires for every inventory item; the puzzle only reacts to the Lament Configuration.
// Looked up each time (a level reload replaces the class and its functions; the lookup verifies them).
UFunction* ItemUsedFn()
{
    UClass* c = FindClass("BP_EnvPuzzleBaseActor_ConfigurationPuzzle_C");
    return c ? FindFunction(c, "ConfigurationItemUsed") : nullptr;
}
void ConfigItemUsed(UObject* a, uint8_t* params)
{
    UFunction* itemUsed = ItemUsedFn();
    if (!params || !itemUsed) return;
    for (const Param& p : Params(itemUsed))
        if (p.kind == 'o') {
            UObject* item = *reinterpret_cast<UObject**>(params + p.offset);
            if (IsLive(item) && Has(Name(item), "Item_KeyItemGenesisConfiguration")) ConfigOpen(a);
            return;
        }
}

void PillarRestore()
{
    if (g_su.pillar.In()) return;
    for (const Weak& w : g_su.pillarActors) {
        Dilate(w.Get(), false, 1);
        if (UObject* a = w.Get()) DilateNiagara(a, 1.0, {"NS_LabReward"});
    }
    g_su.pillarActors.clear();
    for (auto& [k, r] : g_su.pillarMontages)
        if (UObject* m = r.first.Get())
            if (Num rate = NumOf(m, "RateScale"); rate && std::fabs(rate.Get() - PillarSpeed()) < 1e-4) rate.Set(r.second);
    g_su.pillarMontages.clear();
}

void PillarOpen(UObject* a, uint8_t*)
{
    double speed = PillarSpeed();
    if (speed <= 1 || !IsLive(a)) return;
    ScaleActor(a, speed);
    if (UObject* m = Ptr(a, "PillarAnimMontage")) {
        Num rate = NumOf(m, "RateScale");
        std::string k = std::to_string(ObjectIndex(m));
        if (rate && !g_su.pillarMontages.count(k)) g_su.pillarMontages[k] = {Weak(m), rate.Get()};
        if (rate && rate.Get() != speed) {
            rate.Set(speed);
            LogOnce("pm" + k, "Speed up: %s RateScale -> %.2f", Name(m).c_str(), speed);
        }
    }
    if (g_su.pillar.Open()) Later(g_su.pillar.max + 0.5, PillarRestore);
    g_su.pillarActors.push_back(Weak(a));
    Dilate(a, true, speed);
    DilateNiagara(a, speed, {"NS_LabReward"});
}

void PillarClose(UObject*, uint8_t*)
{
    if (!g_su.pillar.t0 || g_su.pillar.endT) return;
    km::Log("Speed up: pillar sequence took %.2f s", Now() - g_su.pillar.t0);
    g_su.pillar.Close();
    Later(g_su.pillar.tail + 0.1, PillarRestore);
}

// Lockpicking is never touched: a lockpick puzzle start ends any open window.
void LockpickStart(UObject*, uint8_t*)
{
    g_su.cfg.t0 = g_su.cfg.endT = g_su.save.t0 = g_su.save.endT = g_su.pillar.t0 = g_su.pillar.endT = 0;
    ConfigRestore();
    SaveRestore();
    PillarRestore();
}

// Cylinder puzzles: the only animation is the base class's 0.5 s RotationTimeline, so its play rate is the whole
// speed-up.
void ApplyCylinders()
{
    double speed = CylSpeed();
    ForEachInstance(FindClass("BP_EnvPuzzleBaseActor_CylinderBase_C"), [&](UObject* a) {
        UObject* tl = Ptr(a, "RotationTimeline");
        if (!tl) return true;
        float rate = CallFloat(tl, "GetPlayRate", 1);
        int32_t idx = ObjectIndex(tl);
        if (!g_su.cylRates.count(idx)) g_su.cylRates[idx] = {Weak(tl), rate};
        if (speed > 1 && rate != float(speed)) {
            Call(tl, "SetPlayRate").Arg(0, float(speed)).Run();
            LogOnce("cyl" + ClassName(a), "Speed up: %s RotationTimeline play rate -> %.2f", ClassName(a).c_str(), speed);
        }
        return true;
    });
}

// The focus ability locks each axis for InputDelayTime (0.4 s) after every input, for all focus puzzles. Only a
// cylinder puzzle gets a shorter lockout; the value is put back when the ability serves anything else.
void FocusAxis(UObject* ga, uint8_t*)
{
    if (!IsLive(ga)) return;
    Num delay = NumOf(ga, "InputDelayTime");
    if (!delay) return;
    UObject* pa = Ptr(Ptr(ga, "TASK Focus Env Puzzle"), "PuzzleActorRef");
    UClass* cyl = FindClass("BP_EnvPuzzleBaseActor_CylinderBase_C");
    bool isCyl = pa && cyl && IsA(pa, cyl);
    double speed = CylInputSpeed();
    int32_t idx = ObjectIndex(ga);
    auto it = g_su.inputDelay.find(idx);
    double cur = delay.Get();
    if (isCyl && speed > 1) {
        double orig = it != g_su.inputDelay.end() ? it->second.second : cur;
        double want = orig / speed;
        if (std::fabs(cur - want) > 1e-4) {
            if (it == g_su.inputDelay.end()) g_su.inputDelay[idx] = {Weak(ga), cur};
            delay.Set(want);
            LogOnce("inp" + std::to_string(orig), "Speed up: cylinder input lockout %.2f -> %.2f", orig, want);
        }
    } else if (it != g_su.inputDelay.end()) {
        delay.Set(it->second.second);
        g_su.inputDelay.erase(it);
    }
}

// Dock boat: a pure level sequence (arms, boat, camera, audio, no event tracks); the player's play rate scales it all.
bool IsBoat(UObject* player)
{
    int32_t idx = ObjectIndex(player);
    if (auto it = g_su.boatKind.find(idx); it != g_su.boatKind.end()) return it->second;
    UObject* actor = OuterOf(player);
    Call seq(actor, "GetSequence");
    UObject* s = seq.Run() ? seq.Out<UObject*>("ReturnValue") : nullptr;
    if (!IsLive(s)) return false; // not known yet: asked again on the next Play
    bool hit = Has(Name(s), "SQ_EP3_DockPulingBoat");
    g_su.boatKind[idx] = hit;
    return hit;
}

void BoatPlayed(UObject* pl, uint8_t*)
{
    double speed = BoatSpeed();
    if (speed <= 1 || !IsLive(pl) || !IsBoat(pl)) return;
    if (g_su.boat.Get() != pl) {
        g_su.boat = Weak(pl);
        g_su.boatOrig = CallFloat(pl, "GetPlayRate", 1);
    }
    if (std::fabs(g_su.boatOrig - speed) < 1e-4) g_su.boatOrig = 1;
    Call(pl, "SetPlayRate").Arg(0, float(speed)).Run();
    LogOnce("boat", "Speed up: dock boat sequence play rate -> %.2f", speed);
}

void TickBoat(uint64_t now)
{
    UObject* pl = g_su.boat.Get();
    if (!pl || now - g_su.boatAt < 200) return;
    g_su.boatAt = now;
    bool playing = false;
    CallBool(pl, "IsPlaying", &playing);
    double speed = BoatSpeed();
    if (playing && speed > 1) {
        if (std::fabs(CallFloat(pl, "GetPlayRate", 1) - speed) > 1e-4) Call(pl, "SetPlayRate").Arg(0, float(speed)).Run();
        return;
    }
    Call(pl, "SetPlayRate").Arg(0, float(g_su.boatOrig)).Run();
    g_su.boat = {};
}

// ---- parameter edits on engine functions (K2_SetTimerDelegate, Delay, MoveComponentTo)
std::map<std::pair<UFunction*, std::string>, std::pair<int32_t, int32_t>> g_paramOffsets; // offset, function's index
int32_t ParamOffset(UFunction* fn, const char* name, char* kind = nullptr)
{
    (void)kind;
    auto key = std::make_pair(fn, std::string(name));
    int32_t idx = ObjectIndex(reinterpret_cast<UObject*>(fn));
    if (auto it = g_paramOffsets.find(key); it != g_paramOffsets.end() && it->second.second == idx) return it->second.first;
    int32_t off = -1;
    for (const Param& p : Params(fn))
        if (p.name == name && (p.kind == 'f' || p.kind == 'd' || p.kind == 'o')) {
            off = p.offset | (p.kind == 'd' ? 0x40000000 : 0);
            break;
        }
    g_paramOffsets[key] = {off, idx};
    return off;
}

Num ParamNum(UFunction* fn, uint8_t* params, const char* name)
{
    int32_t off = ParamOffset(fn, name);
    if (off < 0) return {};
    return {params + (off & 0xFFFFFF), (off & 0x40000000) != 0};
}

void TimerEdit(UObject*, UFunction* fn, uint8_t* params)
{
    Num t = ParamNum(fn, params, "Time");
    if (!t) return;
    double v = t.Get();
    double save = SaveSpeed(), cfg = ConfigSpeed();
    if (save > 1 && g_su.save.In() && SaveTimerMatches(v)) {
        t.Set(v / save);
        LogOnce("st" + std::to_string(v), "Speed up: save timer %.2f -> %.2f", v, v / save);
    } else if (cfg > 1 && g_su.cfg.In() && ConfigTimerMatches(v)) {
        t.Set(v / cfg);
        LogOnce("ct" + std::to_string(v), "Speed up: configuration timer %.3f -> %.3f", v, v / cfg);
    }
}

// Delays started from the save point actor (sound and decal waits) and, during a sequence, from GA_BaseInteract.
void DelayEdit(UObject*, UFunction* fn, uint8_t* params)
{
    double speed = SaveSpeed();
    int32_t woff = ParamOffset(fn, "WorldContextObject");
    Num d = ParamNum(fn, params, "Duration");
    if (speed <= 1 || woff < 0 || !d) return;
    UObject* wc = *reinterpret_cast<UObject**>(params + (woff & 0xFFFFFF));
    std::string cls = IsLive(wc) ? ClassName(wc) : "";
    if (!Has(cls, "BP_TASK_SavePoint_C") && !Has(cls, "GA_BaseInteract_C")) return;
    double v = d.Get();
    d.Set(v / speed);
    LogOnce("dl" + std::to_string(v), "Speed up: Delay %.2f -> %.2f", v, v / speed);
}

void MoveEdit(UObject*, UFunction* fn, uint8_t* params)
{
    double speed = ConfigSpeed();
    Num o = ParamNum(fn, params, "OverTime");
    if (speed <= 1 || !o || o.Get() <= 0) return;
    double v = o.Get();
    o.Set(v / speed);
    LogOnce("mv" + std::to_string(v), "Speed up: MoveComponentTo %.2f -> %.2f", v, v / speed);
}

void RestoreSpeedUp()
{
    g_su.save.t0 = g_su.cfg.t0 = g_su.pillar.t0 = 0;
    SaveRestore();
    ConfigRestore();
    PillarRestore();
    RestoreMontages();
    RestoreFields();
    for (auto& [i, r] : g_su.cylRates)
        if (UObject* tl = r.first.Get()) Call(tl, "SetPlayRate").Arg(0, float(r.second)).Run();
    g_su.cylRates.clear();
    for (auto& [i, r] : g_su.inputDelay)
        if (UObject* ga = r.first.Get())
            if (Num d = NumOf(ga, "InputDelayTime")) d.Set(r.second);
    g_su.inputDelay.clear();
    if (UObject* pl = g_su.boat.Get()) Call(pl, "SetPlayRate").Arg(0, float(g_su.boatOrig)).Run();
    g_su.boat = {};
}

void TickSpeedUp(uint64_t now, std::string& why)
{
    bool on = SpeedOn();
    if (g_su.was && !on) RestoreSpeedUp();
    g_su.was = on;
    if (!on) return;
    if (!g_su.registered) {
        g_su.registered = true;
        const char* task = "BP_TASK_SavePoint_C";
        Want(task, "ReceiveBeginPlay", nullptr, [](UObject* a, uint8_t*) {
            ScaleActor(a, SaveSpeed());
            ApplyMontages();
        });
        Want(task, "OnInteractTaskCompleted", SaveOpen, nullptr);
        Want(task, "StartSaveSequence", SaveOpen, nullptr);
        Want(task, "OnSavePointFinished", nullptr, SaveClose);
        Want(task, "SpawnWorm", nullptr, [](UObject*, uint8_t*) {
            Later(0.2, [] {
                if (g_su.save.In()) DilateWorms(true);
            });
        });
        const char* cfg = "BP_EnvPuzzleBaseActor_ConfigurationPuzzle_C";
        Want(cfg, "ConfigurationItemUsed", ConfigItemUsed, nullptr);
        Want(cfg, "PuzzleStart", [](UObject* a, uint8_t*) { ConfigOpen(a); }, nullptr);
        Want(cfg, "PuzzleClosed", nullptr, ConfigClose);
        const char* pillar = "BP_TASK_LabyrinthReward_C";
        Want(pillar, "ReceiveBeginPlay", nullptr, [](UObject* a, uint8_t*) { ScaleActor(a, PillarSpeed()); });
        Want(pillar, "OnInteractTaskCompleted", PillarOpen, nullptr);
        Want(pillar, "ReleaseAbilities", nullptr, PillarClose);
        Want("BP_EnvPuzzleBaseActor_LockpickPuzzle_C", "PuzzleStart", LockpickStart, nullptr);
        for (const char* c : {"BP_EnvPuzzleBaseActor_CylinderBase_C", "BP_EnvPuzzleBaseActor_CylinderBase_Ep4_C",
                              "BP_EnvPuzzleBaseActor_EP2Well_C"})
            Want(c, "PuzzleStart", nullptr, [](UObject*, uint8_t*) { ApplyCylinders(); });
        Want("GA_BaseInteractFocusEnvPuzzle_C", "XAxisOverride", FocusAxis, nullptr);
        Want("GA_BaseInteractFocusEnvPuzzle_C", "YAxisOverride", FocusAxis, nullptr);
        Want("MovieSceneSequencePlayer", "Play", nullptr, BoatPlayed);
        Later(2.0, ApplyCylinders);
    }
    if (!g_su.editHooked && hooks::Ready()) {
        g_su.editHooked = true;
        UClass* ksl = FindClass("KismetSystemLibrary");
        bool t = hooks::HookEdit(FindFunction(ksl, "K2_SetTimerDelegate"), [] { return g_su.save.In() || g_su.cfg.In(); }, TimerEdit);
        bool d = hooks::HookEdit(FindFunction(ksl, "Delay"), [] { return g_su.save.In(); }, DelayEdit);
        bool m = hooks::HookEdit(FindFunction(ksl, "MoveComponentTo"), [] { return g_su.cfg.In(); }, MoveEdit);
        km::Log("Speed up: timer %s, delay %s, MoveComponentTo %s", t ? "hooked" : "OFF", d ? "hooked" : "OFF", m ? "hooked" : "OFF");
    }
    (void)why;
    TickBoat(now);
}

// ================================================================ Leftover HUD when idle
// The game's dynamic HUD hides itself when nothing happens, but some pieces stay: the "DROP [Z]" hint while you hold a
// breakable melee weapon (W_ToolUseParent_C > SizeBox_DropInput) never goes. The dynamic HUD's state is read from the
// player info panel (W_PlayerInfo_C: health and the rest), which the game hides when it goes idle. Once it has been
// hidden for the delay, the leftover pieces are faded (render opacity 0, kept at 0 while idle) and come back with the
// HUD. The pieces are "Class:WidgetName" pairs, KetaminePlusPlus.ini [HudIdle] Leftovers.
// The DROP [Z] hint, the flashlight toggle (X) and the crafting (wrench) icon.
constexpr const char* kLeftovers = "W_ToolUseParent_C:SizeBox_DropInput,"
                                   "W_EquipmentLoadout_C:W_EquipmentLoadout_SlotSmall_Flashlight,"
                                   "W_EquipmentLoadout_C:Overlay_CraftableIcon";
struct HudElem {
    Weak w;
    float orig;
};
struct {
    std::vector<std::pair<std::string, std::string>> targets; // class, widget name
    std::vector<Weak> leftovers;
    Weak info;
    std::map<int32_t, HudElem> forced; // by object index
    uint64_t scanAt = 0, pollAt = 0;
    double idleSince = 0;
    bool loaded = false, logged = false;
} g_hud;

bool Shown(UObject* w)
{
    uint8_t* vis = Member<uint8_t>(w, "Visibility");
    float* op = Member<float>(w, "RenderOpacity");
    return !(vis && (*vis == 1 || *vis == 2)) && !(op && *op < 0.05f);
}
bool GameShown(UObject* w) // visibility only: what the game itself shows (our fade only touches opacity)
{
    uint8_t* vis = Member<uint8_t>(w, "Visibility");
    return !(vis && (*vis == 1 || *vis == 2));
}

UObject* FindByName(UObject* w, const std::string& name, int depth)
{
    if (!w || depth > 16) return nullptr;
    if (Name(w) == name) return w;
    std::vector<UObject*> kids;
    WidgetChildren(w, kids, 64);
    for (UObject* k : kids)
        if (UObject* f = FindByName(k, name, depth + 1)) return f;
    return nullptr;
}

void RestoreHud()
{
    for (auto& [i, e] : g_hud.forced)
        if (UObject* w = e.w.Get()) Call(w, "SetRenderOpacity").Arg(0, e.orig).Run();
    g_hud.forced.clear();
    g_hud.idleSince = 0;
}

void TickHudIdle(UObject* player, uint64_t now)
{
    if (!g_on[feat::HudAutoHide] || !player || g_on[feat::HideHUD]) {
        if (!g_hud.forced.empty()) RestoreHud();
        return;
    }
    if (!g_hud.loaded) {
        g_hud.loaded = true;
        std::stringstream ss(cfg::GetString("HudIdle", "Leftovers", kLeftovers));
        for (std::string item; std::getline(ss, item, ',');) {
            size_t c = item.find(':');
            if (c != std::string::npos) g_hud.targets.push_back({item.substr(0, c), item.substr(c + 1)});
        }
    }
    if (now - g_hud.scanAt > 2000) {
        g_hud.scanAt = now;
        g_hud.leftovers.clear();
        auto inViewport = [](UObject* w) {
            bool in = false;
            return CallBool(w, "IsInViewport", &in) && in;
        };
        UObject* info = nullptr;
        ForEachInstance(FindClass("W_PlayerInfo_C"), [&](UObject* w) {
            if (inViewport(w)) info = w;
            return !info;
        });
        g_hud.info = Weak(info);
        for (auto& [cls, name] : g_hud.targets)
            ForEachInstance(FindClass(cls.c_str()), [&](UObject* w) {
                if (!inViewport(w)) return true;
                if (UObject* e = FindByName(Ptr(Ptr(w, "WidgetTree"), "RootWidget"), name, 0)) g_hud.leftovers.push_back(Weak(e));
                return true;
            });
        if (!g_hud.logged && (info || !g_hud.leftovers.empty())) {
            g_hud.logged = true;
            km::Log("Idle HUD: %s, %zu leftover piece(s) watched", info ? "player info panel found" : "no player info panel",
                    g_hud.leftovers.size());
        }
    }
    if (now - g_hud.pollAt < 100) return;
    g_hud.pollAt = now;
    UObject* info = g_hud.info.Get();
    bool always = g_on[feat::HudLeftoversAlways];
    bool idle = always || (info ? !Shown(info) : true); // without the panel: leftovers fade after the delay
    double t = Now();
    if (!idle) {
        if (!g_hud.forced.empty()) RestoreHud();
        g_hud.idleSince = 0;
        return;
    }
    if (!g_hud.idleSince) g_hud.idleSince = t;
    if (!always && t - g_hud.idleSince < g_value[feat::HudIdleDelay]) return;
    for (const Weak& wk : g_hud.leftovers) {
        UObject* e = wk.Get();
        if (!e || !GameShown(e)) continue;
        float* op = Member<float>(e, "RenderOpacity");
        int32_t idx = ObjectIndex(e);
        if (!g_hud.forced.count(idx)) {
            g_hud.forced[idx] = {Weak(e), op && *op > 0.05f ? *op : 1.f};
            LogOnce("hudforce" + Name(e), "Idle HUD: faded leftover %s (%s)", Name(e).c_str(), ClassName(e).c_str());
        }
        if (!op || *op > 0.01f) Call(e, "SetRenderOpacity").Arg(0, 0.f).Run(); // kept at 0: animations may bring it back
    }
}

// ================================================================ number keys (window thread -> game thread)
std::mutex g_keyMutex;
std::deque<int> g_keys;

} // namespace

void Tick(UObject* pc, UObject* player, uint64_t nowMs, std::string why[WhyCount])
{
    static bool init = false;
    if (!init) {
        init = true;
        hooks::Init();
    }
    if (!hooks::Ready()) {
        for (int i = 0; i < WhyCount; i++)
            if (i != WhyQuick && i != WhyCut) why[i] = "Function hooks unavailable on this game build (see the log).";
    }
    RunLater();
    if (hooks::Ready()) {
        TickToolInput(why[WhyTool]);
        TickKeypad(pc, why[WhyKeypad]);
        TickSpeedUp(nowMs, why[WhySpeed]);
    }
    TickQuickInteract(player, nowMs, why[WhyQuick]);
    TickHudIdle(player, nowMs);
    TickCutscene(nowMs, why[WhyCut]);
    TickRegs(nowMs);

    std::deque<int> keys;
    {
        std::lock_guard lock(g_keyMutex);
        keys.swap(g_keys);
    }
    for (int n : keys)
        if (g_kp.open) PressKeypad(n);
}

void Frame(UObject* pc, double now)
{
    if (!g_on[feat::KeypadFix] || !g_kp.ga.Get()) return;
    FrameKeypad(pc, now);
}

bool EditsAvailable() { return hooks::CanEdit(); }

// ---- On-screen UI dump, for building UI features: every in-viewport user widget's tree, with text of text blocks.
// Read-only: names, classes and visibility come from reflection; the only calls into the game are GetText on text
// widgets, each behind its own guard.
static void DumpWidget(UObject* w, int depth, int& budget)
{
    if (!w || depth > 22 || budget-- <= 0) return;
    std::string line(size_t(depth * 2), ' ');
    line += Name(w) + " [" + ClassName(w) + "]";
    if (!Shown(w)) line += " (hidden)";
    std::string cls = ClassName(w);
    if (Has(cls, "TextBlock") || Has(cls, "RichText")) {
        std::string text;
        std::function<void()> read = [&] {
            // FText is 16 bytes in UE5: the size always comes from the function's own parameter (a larger copy would
            // overwrite the returned string's slot, which the engine then frees: that was the dump's crash).
            UObject* lib = FindObject("Default__KismetTextLibrary");
            UFunction* convFn = lib ? FindFunction(ClassOf(lib), "Conv_TextToString") : nullptr;
            std::vector<Param> ps = convFn ? Params(convFn) : std::vector<Param>{};
            if (ps.empty() || ps[0].size <= 0 || ps[0].size > 32 || !FindFunction(ClassOf(w), "GetText")) return;
            Call get(w, "GetText");
            if (!get || !get.Run()) return;
            struct {
                uint8_t b[32];
            } t = get.Out<decltype(t)>("ReturnValue");
            Call conv(lib, "Conv_TextToString");
            if (conv && conv.ArgRaw(0, &t, size_t(ps[0].size)).Run()) text = conv.Out<FString>("ReturnValue").ToUtf8();
        };
        if (Guard(read) && !text.empty()) line += " \"" + text.substr(0, 80) + "\"";
    }
    km::Log("UI  %s", line.c_str());
    std::vector<UObject*> kids;
    WidgetChildren(w, kids, 64);
    for (UObject* k : kids) DumpWidget(k, depth + 1, budget);
}

void DumpUi()
{
    ue::RunOnGameThread([] {
        km::Log("UI dump: on-screen user widgets");
        int budget = 6000;
        UClass* uw = FindClass("UserWidget");
        UClass* treeCls = FindClass("WidgetTree");
        std::vector<UObject*> roots;
        // Top-level widgets only (their outer isn't a widget tree), and only those in the viewport.
        ForEachInstance(uw, [&](UObject* w) {
            UObject* outer = OuterOf(w);
            if (outer && treeCls && IsA(outer, treeCls)) return true;
            if (roots.size() < 256) roots.push_back(w);
            return true;
        });
        for (UObject* w : roots) {
            if (budget <= 0) break;
            bool in = false;
            std::function<void()> check = [&] { CallBool(w, "IsInViewport", &in); };
            if (!IsLive(w) || !Guard(check) || !in) continue;
            if (!Shown(w)) { // hidden screens: one line each, so the budget goes to what's on screen
                km::Log("UI  %s [%s] (hidden)", Name(w).c_str(), ClassName(w).c_str());
                continue;
            }
            DumpWidget(w, 0, budget);
        }
        km::Log("UI dump: end");
        menu::Toast("UI dumped to KetaminePlusPlus.log");
    });
}

bool OnKey(unsigned vk)
{
    if (!g_on[feat::KeypadFix] || !g_kp.open) return false;
    int n = -1;
    if (vk >= '0' && vk <= '9') n = int(vk - '0');
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) n = int(vk - VK_NUMPAD0);
    else if (vk == VK_BACK) n = 10; // the delete button (lower right)
    if (n < 0) return false;
    std::lock_guard lock(g_keyMutex);
    if (g_keys.size() < 16) g_keys.push_back(n);
    return true;
}

} // namespace fixes
