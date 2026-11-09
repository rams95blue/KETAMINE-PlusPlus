// Function hooks (see hooks.h). Game thread only: the script VM and ProcessEvent run there.
#include "hooks.h"
#include "log.h"
#include "scan.h"
#include "ue.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <map>
#include <utility>
#include <vector>

using namespace ue;

namespace hooks {
namespace {

constexpr uint8_t EX_EndFunctionParms = 0x16;
constexpr uint32_t FUNC_Native = 0x400;
constexpr uint64_t CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;
constexpr int kMaxParams = 1024;

// FFrame field offsets, measured by Calibrate (UE5 defaults).
struct {
    int node = 0x10, object = 0x18, code = 0x20, locals = 0x28;
} F;
bool g_ready = false, g_canEdit = false, g_initDone = false;

using NativeFn = void (*)(UObject* ctx, void* stack, void* result);
NativeFn* g_natives = nullptr; // GNatives[256]

struct Slot {
    UFunction* fn = nullptr;
    NativeThunk orig = nullptr;
    std::vector<Callback> pre, post; // several features may hook the same function
    std::function<bool()> want;
    Edit edit;
    bool script = false;
    std::vector<Param> params; // without the return value
    Param ret;
    bool hasRet = false;
};
constexpr int kSlots = 64;
Slot g_slots[kSlots];
int g_used = 0;
thread_local bool t_redispatch = false;

uint8_t** CodeRef(void* stack) { return reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(stack) + F.code); }
uint8_t* Locals(void* stack) { return *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(stack) + F.locals); }

void Safe(const Callback& cb, UObject* ctx, uint8_t* params, const char* what, UFunction* fn)
{
    if (!cb) return;
    std::function<void()> work = [&] { cb(ctx, params); };
    static int faults = 0;
    if (!Guard(work) && faults++ < 10) km::Log("Hook %s %s faulted (skipped)", Name(reinterpret_cast<UObject*>(fn)).c_str(), what);
}

// FFrame::Step: run the next bytecode expression, writing its value to `result`.
void Step(void* stack, UObject* scriptObj, void* result)
{
    uint8_t*& code = *CodeRef(stack);
    uint8_t op = *code++;
    g_natives[op](scriptObj, stack, result);
}

void Run(int i, UObject* ctx, void* stack, void* result)
{
    Slot& h = g_slots[i];
    uint8_t* code = *CodeRef(stack);
    if (h.edit && !h.script) {
        if (t_redispatch) return h.orig(ctx, stack, result); // our own call, below
        if (!code) { // called through ProcessEvent: the parameters are already a block
            if (g_canEdit) {
                std::function<void()> work = [&] {
                    if (!h.want || h.want()) h.edit(ctx, h.fn, Locals(stack));
                };
                Guard(work);
            }
            return h.orig(ctx, stack, result);
        }
        bool want = false;
        if (g_canEdit) {
            std::function<void()> w = [&] { want = !h.want || h.want(); };
            if (!Guard(w)) want = false;
        }
        int32_t size = ParamsSize(h.fn);
        if (!want || size <= 0 || size > kMaxParams) return h.orig(ctx, stack, result);

        // Evaluate the parameters from the caller's bytecode, as the native thunk's P_GET macros would.
        alignas(16) uint8_t block[kMaxParams];
        memset(block, 0, size_t(size));
        UObject* scriptObj = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(stack) + F.object);
        uint8_t* start = code;
        // Each value goes through a scratch slot: some expressions write a whole 32-bit word for a bool.
        alignas(16) uint8_t scratch[256];
        for (const Param& p : h.params) {
            memset(scratch, 0, sizeof(scratch));
            Step(stack, scriptObj, scratch);
            memcpy(block + p.offset, scratch, size_t(std::min(p.size, int32_t(sizeof(scratch)))));
        }
        uint8_t*& now = *CodeRef(stack);
        if (*now != EX_EndFunctionParms) { // not what we expected: undo and let the function read them itself
            static int bad = 0;
            if (bad++ < 5) km::Log("Hook %s: unexpected bytecode after the parameters, passed through", Name(reinterpret_cast<UObject*>(h.fn)).c_str());
            now = start;
            return h.orig(ctx, stack, result);
        }
        now++; // P_FINISH
        std::function<void()> work = [&] { h.edit(ctx, h.fn, block); };
        Guard(work);
        t_redispatch = true;
        ProcessEventRaw(ctx, h.fn, block);
        t_redispatch = false;
        if (h.hasRet && result) memcpy(result, block + h.ret.offset, size_t(h.ret.size));
        return;
    }
    // Blueprint functions get a parameter block of their own (the new frame's locals); a native function called from
    // script reads its parameters from the caller's bytecode, so there's no block to show.
    uint8_t* params = h.script || !code ? Locals(stack) : nullptr;
    for (const Callback& cb : h.pre) Safe(cb, ctx, params, "pre", h.fn);
    h.orig(ctx, stack, result);
    for (const Callback& cb : h.post) Safe(cb, ctx, params, "post", h.fn);
}

template <int I> void Thunk(UObject* ctx, void* stack, void* result) { Run(I, ctx, stack, result); }
template <int... I> constexpr std::array<NativeThunk, sizeof...(I)> MakeThunks(std::integer_sequence<int, I...>)
{
    return {&Thunk<I>...};
}
constexpr auto kThunks = MakeThunks(std::make_integer_sequence<int, kSlots>{});

// ---------------------------------------------------------------- calibration

constexpr int32_t kMagic = 0x5A17C0DE;
struct {
    UFunction* fn = nullptr;
    NativeThunk orig = nullptr;
    int node = -1, object = -1, code = -1, locals = -1;
} c;

void CalibThunk(UObject* ctx, void* stack, void* result)
{
    auto* s = static_cast<uint8_t*>(stack);
    for (int off = 8; off <= 0x80; off += 8) {
        void* v = nullptr;
        if (!SafeRead(s + off, &v, sizeof(v))) continue;
        if (v == c.fn && c.node < 0) c.node = off;
        else if (v == ctx && c.object < 0) c.object = off;
        else if (v && c.locals < 0) {
            int32_t x = 0;
            if (SafeRead(v, &x, sizeof(x)) && x == kMagic) c.locals = off;
        }
    }
    void* code = reinterpret_cast<void*>(1);
    if (c.locals >= 8 && SafeRead(s + c.locals - 8, &code, sizeof(code)) && !code) c.code = c.locals - 8;
    c.orig(ctx, stack, result);
}

bool Calibrate()
{
    c.fn = FindFunction(FindClass("KismetMathLibrary"), "Abs_Int");
    if (!c.fn || !HookNative(c.fn, CalibThunk, &c.orig)) return false;
    Call call(FindObject("Default__KismetMathLibrary"), "Abs_Int");
    bool ran = call.Arg(0, kMagic).Run();
    NativeThunk mine;
    HookNative(c.fn, c.orig, &mine); // put the original back
    km::Log("Hooks: frame layout Node 0x%X, Object 0x%X, Code 0x%X, Locals 0x%X%s", c.node, c.object, c.code, c.locals,
            ran ? "" : " (call failed)");
    if (!ran || c.node < 0 || c.object < 0 || c.code < 0 || c.locals < 0 || c.object != c.node + 8 || c.code != c.object + 8)
        return false;
    F.node = c.node, F.object = c.object, F.code = c.code, F.locals = c.locals;
    return true;
}

// GNatives: the VM's opcode table, FNativeFuncPtr GNatives[EX_Max], in a writable section. EX_Max is 0xFF in UE5 (255
// entries, 0x00..0xFE: reading 0xFF would be the next variable, which is why the first search found nothing). Every
// opcode the VM doesn't define (all of 0x80..0xFE) is UObject::execUndefined, so one pointer fills the top half.
// Confirmed by opcode 0x16, EX_EndFunctionParms, whose handler is `Stack.Code--`, which also confirms the Code offset.
// Protected executables can mark sections both writable and executable, so a section can be in both lists.
constexpr int kNatives = 0xFF;
struct Sections {
    std::vector<std::pair<uintptr_t, uintptr_t>> exec, data;
    bool IsCode(uintptr_t p) const
    {
        for (auto& [b, e] : exec)
            if (p >= b && p < e) return true;
        return false;
    }
};
Sections ReadSections(bool log)
{
    Sections s;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        uintptr_t b = base + sec->VirtualAddress, e = b + std::max<uint32_t>(sec->Misc.VirtualSize, sec->SizeOfRawData);
        DWORD ch = sec->Characteristics;
        if (ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) s.exec.push_back({b, e});
        if (ch & IMAGE_SCN_MEM_WRITE) s.data.push_back({b, e});
        if (log) {
            char name[9] = {};
            memcpy(name, sec->Name, 8);
            km::Log("Hooks:   section %-8s +0x%08X size 0x%08X flags 0x%08X", name, unsigned(sec->VirtualAddress),
                    unsigned(e - b), unsigned(ch));
        }
    }
    return s;
}

// The structure checks on a candidate table: every entry is code, the top half one shared pointer, the bottom half
// mostly distinct handlers.
bool TableShape(const uintptr_t* t, const Sections& s, int& distinctLow)
{
    if (t[0x80] != t[0xFE] || t[0x80] != t[0xC0] || t[0x80] != t[0xF0] || !s.IsCode(t[0x80]) || !s.IsCode(t[0x16]))
        return false;
    int high = 0, code = 0;
    distinctLow = 0;
    for (int i = 0; i < kNatives; i++) code += s.IsCode(t[i]);
    for (int i = 0x80; i < kNatives; i++) high += t[i] == t[0x80];
    for (int i = 0; i < 0x80; i++) distinctLow += t[i] != t[0x80];
    return code == kNatives && high >= 110 && distinctLow >= 50;
}

bool EndParmsHandler(uintptr_t f, const Sections& s)
{
    auto* b = reinterpret_cast<const uint8_t*>(f);
    if (s.IsCode(f) && b[0] == 0xE9) { // a jump thunk
        f = f + 5 + uintptr_t(intptr_t(*reinterpret_cast<const int32_t*>(b + 1)));
        if (!s.IsCode(f)) return false;
        b = reinterpret_cast<const uint8_t*>(f);
    }
    uint8_t code = uint8_t(F.code);
    auto at = [&](int i, std::initializer_list<int> pat) {
        int k = 0;
        for (int v : pat) {
            if (v >= 0 && b[i + k] != uint8_t(v)) return false;
            k++;
        }
        return true;
    };
    // Stack.Code-- (rdx = &Stack), in the encodings compilers use for it, optionally after a frame-pointer push.
    for (int i : {0, 1, 2}) {
        if (at(i, {0x48, 0xFF, 0x4A, code}) ||                                          // dec qword ptr [rdx+Code]
            at(i, {0x48, 0x83, 0x6A, code, 0x01}) ||                                    // sub qword ptr [rdx+Code], 1
            at(i, {0x48, 0x83, 0x42, code, 0xFF}) ||                                    // add qword ptr [rdx+Code], -1
            at(i, {0x48, 0x8B, 0x42, code, 0x48, 0xFF, 0xC8, 0x48, 0x89, 0x42, code})) // mov rax,[rdx+C]; dec rax; mov [rdx+C],rax
            return true;
    }
    return false;
}

bool FindNatives()
{
    scan::Image img;
    if (scan::Describe(GetModuleHandleW(nullptr), img)) scan::SetImage(img);
    Sections s = ReadSections(false);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int candidates = 0, logged = 0;
    auto accept = [&](uintptr_t p, const char* how) {
        auto* t = reinterpret_cast<const uintptr_t*>(p);
        int distinctLow = 0;
        if (!scan::Readable(p, kNatives * 8) || !TableShape(t, s, distinctLow)) return false;
        candidates++;
        if (!EndParmsHandler(t[0x16], s)) {
            if (logged++ < 3) { // for the log: what this table's EX_EndFunctionParms handler starts with
                char hex[64] = {};
                const uint8_t* hb = reinterpret_cast<const uint8_t*>(t[0x16]);
                for (int i = 0; i < 12; i++) snprintf(hex + i * 3, 4, "%02X ", hb[i]);
                km::Log("Hooks: candidate table at exe+0x%llX (%s), handler 0x16 starts %s",
                        static_cast<unsigned long long>(p - base), how, hex);
            }
            return false;
        }
        g_natives = reinterpret_cast<NativeFn*>(p);
        km::Log("Hooks: GNatives at exe+0x%llX (%s; %d opcodes defined below 0x80, EX_EndFunctionParms verified)",
                static_cast<unsigned long long>(p - base), how, distinctLow);
        return true;
    };
    // 1. Every 8-byte aligned spot of the writable sections.
    for (auto& [begin, end] : s.data)
        for (uintptr_t p = (begin + 7) & ~7ull; p + kNatives * 8 <= end; p += 8) {
            if (!scan::Readable(p, 8)) {
                p = (p | 0xFFF) - 7; // skip to the next page
                continue;
            }
            auto* t = reinterpret_cast<const uintptr_t*>(p);
            if (!s.IsCode(t[0]) || !scan::Readable(p, kNatives * 8) || t[0x80] != t[0xC0]) continue;
            if (accept(p, "data scan")) return true;
        }
    // 2. From the code: the VM dispatches with `lea reg, [rip + GNatives]` then `call/jmp qword ptr [reg + idx*8]`.
    std::map<uintptr_t, int> refs;
    for (auto& [begin, end] : s.exec)
        for (uintptr_t p = begin; p + 16 <= end; p++) {
            if ((p & 0xFFF) == 0 && !scan::Readable(p, 0x1000)) {
                p += 0xFFF;
                continue;
            }
            auto* b = reinterpret_cast<const uint8_t*>(p);
            if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8D || (b[2] & 0xC7) != 0x05) continue;
            uintptr_t target = p + 7 + uintptr_t(intptr_t(*reinterpret_cast<const int32_t*>(b + 3)));
            bool data = false;
            for (auto& [db, de] : s.data) data |= target >= db && target + kNatives * 8 <= de;
            if (!data) continue;
            for (int k = 7; k < 7 + 16 && p + k + 3 <= end; k++) { // FF 14 / FF 24 with a SIB byte, scale 8
                const uint8_t* c = b + k + (b[k] == 0x41 || b[k] == 0x42 || b[k] == 0x43 ? 1 : 0);
                if (c[0] == 0xFF && (c[1] == 0x14 || c[1] == 0x24) && (c[2] >> 6) == 3) {
                    refs[target]++;
                    break;
                }
            }
        }
    std::vector<std::pair<int, uintptr_t>> ranked;
    for (auto& [t, n] : refs) ranked.push_back({n, t});
    std::sort(ranked.rbegin(), ranked.rend());
    for (size_t i = 0; i < ranked.size() && i < 64; i++)
        if (accept(ranked[i].second, "code reference")) return true;
    km::Log("Hooks: GNatives not found (%d candidate tables, %d dispatch references): parameter edits off. Speed up keeps "
            "the save point and Lament puzzle at normal speed (their timers can't follow). Sections:",
            candidates, int(ranked.size()));
    ReadSections(true);
    return false;
}

} // namespace

bool Init()
{
    if (g_initDone) return g_ready;
    g_initDone = true;
    g_ready = Calibrate();
    if (!g_ready) {
        km::Log("Hooks: frame calibration failed: function hooks off (the fixes that need them stay unavailable)");
        return false;
    }
    g_canEdit = FindNatives();
    return true;
}

bool Ready() { return g_ready; }
bool CanEdit() { return g_ready && g_canEdit; }

// Hooked = a slot holds it AND its Func is still that slot's thunk. A level change can unload a Blueprint class and a
// new object can take the same address: a slot that no longer matches is freed instead of trusted.
bool IsHooked(const UFunction* fn)
{
    for (int i = 0; i < g_used; i++) {
        if (g_slots[i].fn != fn) continue;
        if (IsLive(fn) && NativeOf(fn) == kThunks[size_t(i)]) return true;
        g_slots[i] = Slot{};
        return false;
    }
    return false;
}

static bool Install(UFunction* fn, Callback pre, Callback post, std::function<bool()> want, Edit edit)
{
    if (!g_ready || !fn) return false;
    IsHooked(fn); // frees a stale slot for this address
    for (int i = 0; i < g_used; i++) {
        Slot& h = g_slots[i];
        if (h.fn != fn) continue;
        if (edit || h.edit) return !!h.edit == !!edit; // one edit hook per function, never mixed with callbacks
        if (pre) h.pre.push_back(std::move(pre));
        if (post) h.post.push_back(std::move(post));
        return true;
    }
    int index = g_used;
    if (index == kSlots) // a level change can unload Blueprint classes: their functions' slots are free again
        for (int i = 0; i < kSlots && index == kSlots; i++)
            if (!g_slots[i].fn || !IsLive(g_slots[i].fn) || NativeOf(g_slots[i].fn) != kThunks[size_t(i)]) index = i;
    if (index == kSlots) {
        km::Log("Hooks: no free slot for %s", Name(reinterpret_cast<UObject*>(fn)).c_str());
        return false;
    }
    Slot& s = g_slots[index];
    s = Slot{};
    s.fn = fn;
    if (pre) s.pre.push_back(std::move(pre));
    if (post) s.post.push_back(std::move(post));
    s.want = std::move(want);
    s.edit = std::move(edit);
    s.script = !(FunctionFlags(fn) & FUNC_Native);
    for (Param& p : Params(fn)) {
        if (p.flags & CPF_ReturnParm) {
            s.ret = p;
            s.hasRet = true;
        } else {
            if (s.edit && (p.flags & CPF_OutParm)) { // by-reference parameters: not evaluated here, edits stay off
                km::Log("Hooks: %s has an out parameter, edit hook refused", Name(reinterpret_cast<UObject*>(fn)).c_str());
                return false;
            }
            if (s.edit && p.size > 256) {
                km::Log("Hooks: %s has a parameter too large to evaluate, edit hook refused", Name(reinterpret_cast<UObject*>(fn)).c_str());
                return false;
            }
            s.params.push_back(p);
        }
    }
    if (!HookNative(fn, kThunks[size_t(index)], &s.orig)) {
        s.fn = nullptr;
        return false;
    }
    if (index == g_used) g_used++;
    return true;
}

bool Hook(UFunction* fn, Callback pre, Callback post) { return Install(fn, std::move(pre), std::move(post), nullptr, nullptr); }

bool HookEdit(UFunction* fn, std::function<bool()> want, Edit edit)
{
    if (!CanEdit()) return false;
    return Install(fn, nullptr, nullptr, std::move(want), std::move(edit));
}

UObject* FrameObject(void* stack)
{
    if (!g_ready || !stack) return nullptr;
    void* o = nullptr;
    return SafeRead(static_cast<uint8_t*>(stack) + F.object, &o, sizeof(o)) && IsLive(o) ? static_cast<UObject*>(o) : nullptr;
}

} // namespace hooks
