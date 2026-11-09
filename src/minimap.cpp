// Minimap (see minimap.h), ported from Hellraiser True Minimap v0.12 (MapSource.lua, Ui.lua, main.lua).
//
// What made the Lua mod slow, and what this does instead:
//  - It looked up 9 menu screens with FindFirstOf every 250 ms (each a scan of the whole object array, so ~36 scans a
//    second, plus the player controller and the map subsystem): here one pass every 2 s finds the menu screens, the TAB
//    map screen and the map subsystem together, and the 4 Hz status check only reads the cached widgets.
//  - It forced the game's map setup functions and a full copy of the map's widget tree every 5 s even when nothing
//    changed: here the map refreshes when the zone or floor changes, with a 20 s fallback.
//  - It moved the 3840x2160 map widget by its canvas slot 60 times a second, which re-lays out the whole map tree
//    each time: here the map is placed once and moved with its render translation (paint only), only when it moved.
//  - It re-applied every marker's brush and colour every second: here only what changed is written.
//  - Its widget-tree walks ran in Lua through pcall: here they're native.
// Shapes: Slate clips to rectangles, but rotated ones too, so the map sits inside nested clip panels: one at 45 deg for
// a diamond, 0 + 45 for an octagon, 0 + 22.5 + 45 + 67.5 for a 16-sided circle. Rotate-with-player turns the content
// panel (map + markers) around the centre while the player arrow stays fixed.
#include "minimap.h"
#include "features.h"
#include "log.h"
#include "ue.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace ue;

namespace minimap {

const char* const kShapeNames[ShapeCount] = {"Square", "Circle", "Octagon", "Diamond"};

namespace {

using feat::g_on;
using feat::g_value;

struct V2 {
    double x, y;
};
struct Anchors {
    V2 min, max;
};
struct LinearColor {
    float r, g, b, a;
};

constexpr double kMapW = 3840, kMapH = 2160;
constexpr double kMarkerSize = 56;
constexpr uint8_t VisVisible = 0, VisCollapsed = 1, VisHidden = 2, VisHitTestInvisible = 3;
const char* const kBlockers[] = {"W_TabMenu_C", "W_PauseMenu_C", "W_CinematicScreen_C", "W_DeathScreen_C", "W_LoadScreen_C",
                                 "W_SaveScreen_C", "W_Shop_C", "W_Storage_C", "W_OptionsMenuScreen_C"};
constexpr int kBlockerCount = int(sizeof(kBlockers) / sizeof(kBlockers[0]));

std::atomic<const char*> g_status{"Off"};
struct {
    std::atomic<float> x{0}, y{0}, size{0};
    std::atomic<int> shape{0};
} g_frame; // the minimap on screen, in pixels (for the overlay's frame)

// ---------------------------------------------------------------- small helpers

UObject* Ptr(UObject* o, const char* prop)
{
    auto p = o ? Member<UObject*>(o, prop) : nullptr;
    return p && IsLive(*p) ? *p : nullptr;
}
std::string ClassName(const UObject* o) { return o ? Name(reinterpret_cast<const UObject*>(ClassOf(o))) : ""; }
UObject* Cdo(const char* cls)
{
    static std::map<std::string, Weak> cache;
    Weak& w = cache[cls];
    if (!w.Get()) w = Weak(FindObject((std::string("Default__") + cls).c_str()));
    return w.Get();
}
bool CallBool(UObject* o, const char* fn, bool def = false)
{
    Call c(o, fn);
    return c && c.Run() ? c.Out<bool>("ReturnValue") : def;
}
double ReadNum(UObject* o, const char* name, double def)
{
    Prop p = o ? FindProp(ClassOf(o), name) : Prop{};
    if (!p) return def;
    uint8_t* a = reinterpret_cast<uint8_t*>(o) + p.offset;
    switch (p.kind) {
    case 'i': return *reinterpret_cast<int32_t*>(a);
    case 'f': return *reinterpret_cast<float*>(a);
    case 'd': return *reinterpret_cast<double*>(a);
    default: return def;
    }
}

// Widget calls (all BlueprintCallable UMG functions, through ProcessEvent).
UObject* Spawn(const char* cls, UObject* outer)
{
    UClass* c = FindClass(cls);
    Call sp(Cdo("GameplayStatics"), "SpawnObject");
    if (!c || !outer || !sp.Arg(0, c).Arg(1, outer).Run()) return nullptr;
    UObject* o = sp.Out<UObject*>("ReturnValue");
    return IsLive(o) ? o : nullptr;
}
UObject* AddToCanvas(UObject* canvas, UObject* child)
{
    Call c(canvas, "AddChildToCanvas");
    return c.Arg(0, child).Run() ? c.Out<UObject*>("ReturnValue") : nullptr;
}
void SlotAnchors(UObject* slot, V2 mn, V2 mx) { Call(slot, "SetAnchors").Arg(0, Anchors{mn, mx}).Run(); }
void SlotAlign(UObject* slot, V2 a) { Call(slot, "SetAlignment").Arg(0, a).Run(); }
void SlotPos(UObject* slot, V2 p) { Call(slot, "SetPosition").Arg(0, p).Run(); }
void SlotSize(UObject* slot, V2 s) { Call(slot, "SetSize").Arg(0, s).Run(); }
void SlotZ(UObject* slot, int z) { Call(slot, "SetZOrder").Arg(0, int32_t(z)).Run(); }
void SetVis(UObject* w, uint8_t v) { Call(w, "SetVisibility").Arg(0, v).Run(); }
void SetOpacity(UObject* w, float a) { Call(w, "SetRenderOpacity").Arg(0, a).Run(); }
void SetClip(UObject* w, bool on) { Call(w, "SetClipping").Arg(0, uint8_t(on ? 1 : 0)).Run(); }
void SetPivot(UObject* w, V2 p) { Call(w, "SetRenderTransformPivot").Arg(0, p).Run(); }
void SetAngle(UObject* w, float deg) { Call(w, "SetRenderTransformAngle").Arg(0, deg).Run(); }
void SetScale(UObject* w, V2 s) { Call(w, "SetRenderScale").Arg(0, s).Run(); }
void SetTranslation(UObject* w, V2 t) { Call(w, "SetRenderTranslation").Arg(0, t).Run(); }
void SetColor(UObject* img, LinearColor c) { Call(img, "SetColorAndOpacity").Arg(0, c).Run(); }

uint8_t Visibility(UObject* w)
{
    uint8_t* v = Member<uint8_t>(w, "Visibility");
    return v ? *v : VisVisible;
}
float Opacity(UObject* w)
{
    float* o = Member<float>(w, "RenderOpacity");
    return o ? *o : 1.f;
}
bool Hidden(uint8_t v) { return v == VisCollapsed || v == VisHidden; }

UObject* TreeRoot(UObject* w) { return Ptr(Ptr(w, "WidgetTree"), "RootWidget"); }
int ChildCount(UObject* panel)
{
    Call c(panel, "GetChildrenCount");
    return c && c.Run() ? c.Out<int32_t>("ReturnValue") : 0;
}
UObject* ChildAt(UObject* panel, int i)
{
    Call c(panel, "GetChildAt");
    UObject* o = c && c.Arg(0, int32_t(i)).Run() ? c.Out<UObject*>("ReturnValue") : nullptr;
    return IsLive(o) ? o : nullptr;
}

// FSlateBrush on an Image: its bytes, and its ResourceObject.
struct BrushInfo {
    const UClass* type = nullptr;
    int32_t offset = -1, size = 0, resource = -1;
};
BrushInfo BrushOf(UObject* img)
{
    BrushInfo b;
    Prop p = img ? FindProp(ClassOf(img), "Brush") : Prop{};
    if (!p || p.kind != 's' || !p.inner) return b;
    b.type = static_cast<const UClass*>(p.inner);
    b.offset = p.offset;
    b.size = StructSize(b.type);
    Prop r = FindProp(b.type, "ResourceObject");
    b.resource = r ? r.offset : -1;
    return b;
}
UObject* BrushResource(UObject* img)
{
    BrushInfo b = BrushOf(img);
    if (b.offset < 0 || b.resource < 0) return nullptr;
    UObject* r = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(img) + b.offset + b.resource);
    return IsLive(r) ? r : nullptr;
}
void CopyBrush(UObject* from, UObject* to)
{
    BrushInfo b = BrushOf(from);
    if (b.offset < 0 || b.size <= 0 || b.size > 512) return;
    Call(to, "SetBrush").ArgRaw(0, reinterpret_cast<uint8_t*>(from) + b.offset, size_t(b.size)).Run();
}
bool IsImage(UObject* w)
{
    std::string c = ClassName(w);
    return c == "Image" || c == "MaterialImage";
}

// ---------------------------------------------------------------- game state (cached, refreshed every 2 s)

struct {
    Weak blockers[kBlockerCount];
    Weak screen, sub;
    uint64_t scanAt = 0;
} g_found;

// The map subsystem, asked from the engine's subsystem collections (no object scan).
UObject* MapSubsystem(UObject* pc, UClass* cls)
{
    if (!cls) return nullptr;
    UObject* lib = Cdo("SubsystemBlueprintLibrary");
    for (const char* fn : {"GetWorldSubsystem", "GetGameInstanceSubsystem"}) {
        Call c(lib, fn);
        if (c && c.Arg(0, pc).Arg(1, cls).Run())
            if (UObject* s = c.Out<UObject*>("ReturnValue"); IsLive(s)) return s;
    }
    UObject* found = nullptr; // fallback: a subsystem class the library doesn't cover
    ForEachInstance(cls, [&](UObject* o) {
        found = o;
        return false;
    });
    return found;
}

// One pass over the objects for the menu screens and the TAB map screen (class pointer compares only), plus the map
// subsystem from the engine.
void Rescan(UObject* pc)
{
    UClass* cls[kBlockerCount + 2] = {};
    const char* names[kBlockerCount + 2];
    for (int i = 0; i < kBlockerCount; i++) names[i] = kBlockers[i];
    names[kBlockerCount] = "W_MapScreen_C";
    names[kBlockerCount + 1] = "MapSubsystem";
    FindClasses(names, kBlockerCount + 2, cls);
    for (auto& w : g_found.blockers) w = {};
    UObject* screen = nullptr;
    UObject* sub = g_found.sub.Get();
    if (!sub) sub = MapSubsystem(pc, cls[kBlockerCount + 1]);
    ForEachObject([&](UObject* o) {
        const UClass* c = ClassOf(o);
        for (int i = 0; i < kBlockerCount; i++)
            if (c == cls[i] && cls[i] && !g_found.blockers[i].Get() && !IsDefaultObject(o)) g_found.blockers[i] = Weak(o);
        if (cls[kBlockerCount] && !screen && c == cls[kBlockerCount] && !IsDefaultObject(o)) {
            // The TAB menu's own map screen (the Lua mod's "/Engine/Transient ... W_TabMenu_C" test): an outer is it.
            for (UObject* outer = OuterOf(o); outer; outer = OuterOf(outer))
                if (ClassName(outer).rfind("W_TabMenu_C", 0) == 0) {
                    screen = o;
                    break;
                }
        }
        return true;
    });
    g_found.screen = Weak(screen);
    g_found.sub = Weak(sub);
}

// An obstructing screen is up: in the viewport, visible, not transparent.
bool Blocked()
{
    for (const Weak& w : g_found.blockers) {
        UObject* o = w.Get();
        if (!o || Hidden(Visibility(o)) || Opacity(o) <= 0.01f) continue;
        if (CallBool(o, "IsInViewport")) return true;
    }
    return false;
}

struct Bounds {
    double ox, oy, xx, xy, yx, yy, dx, dy;
};
bool ReadBounds(UObject* sub, Bounds& b)
{
    bool set = false;
    if (!sub || !GetBool(sub, "MapBoundsSet", &set) || !set) return false;
    auto vec = [&](const char* n, double& x, double& y) {
        Prop p = FindProp(ClassOf(sub), n);
        if (!p || p.kind != 's') return false;
        auto* d = reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(sub) + p.offset); // FVector / FVector2D: X, Y first
        x = d[0], y = d[1];
        return true;
    };
    if (!vec("BottomLeftPoint", b.ox, b.oy) || !vec("MapXAxis", b.xx, b.xy) || !vec("MapYAxis", b.yx, b.yy)) return false;
    b.dx = b.xx * b.xx + b.xy * b.xy;
    b.dy = b.yx * b.yx + b.yy * b.yy;
    return b.dx > 1e-5 && b.dy > 1e-5;
}

bool ZoneDepth(UObject* sub, std::string& zone, double& depth)
{
    UObject* marker = Ptr(sub, "PlayerMarker");
    if (!marker) return false;
    Prop z = FindProp(ClassOf(marker), "Zone");
    if (!z) return false;
    zone = NameToString(reinterpret_cast<uint8_t*>(marker) + z.offset); // FGameplayTag: TagName first
    depth = ReadNum(marker, "Depth", 0);
    return true;
}

// The game's two map functions: SetCurrentDepth is absolute, UpdateMapAreaDepth takes a depth delta.
bool RefreshScreen(UObject* screen, bool deep, int delta)
{
    for (const char* fn : {"SetupMapZones", "SetupMapMarkers", "SetupAreaDepthList"}) {
        if (!deep && !strcmp(fn, "SetupAreaDepthList")) continue;
        if (!Call(screen, fn).Run()) return false;
    }
    if (delta) {
        if (std::abs(delta) > 6) return false;
        UFunction* fn = FindFunction(ClassOf(screen), "UpdateMapAreaDepth");
        Call c(screen, "UpdateMapAreaDepth");
        if (!fn || !c) return false;
        std::vector<Param> ps = Params(fn);
        if (!ps.empty() && ps[0].kind == 'i') c.Arg(0, int32_t(delta));
        else if (!ps.empty() && ps[0].kind == 'd') c.Arg(0, double(delta));
        else c.Arg(0, float(delta));
        if (ps.size() > 1) c.Arg(1, false);
        if (!c.Run()) return false;
    }
    return true;
}

// ---------------------------------------------------------------- the widget

struct Marker {
    Weak img;
    UObject* slot = nullptr;
    UObject* resource = nullptr;
    double x = 1e9, y = 1e9;
    bool shown = false;
};

struct Ui {
    Weak root;            // our copy of the game's map widget class (the root added to the viewport)
    UObject* tree = nullptr;
    UObject* canvas = nullptr;     // fullscreen root panel
    UObject* base = nullptr;       // the minimap window: size x size, top right
    UObject* baseSlot = nullptr;
    UObject* inner = nullptr;      // last clip level (shape)
    UObject* content = nullptr;    // rotates with the player when that's on
    UObject* mapRoot = nullptr;    // the copy's original root: the map itself
    UObject* layer = nullptr;      // markers
    UObject* backdrop = nullptr;
    UObject* arrow = nullptr;
    UObject* frame = nullptr;
    UObject* frameSlot = nullptr;
    std::vector<Marker> marks;
    int shape = -1;
    float size = 0, contentBase = 0, opacity = -1;
    double zoom = -1, lastX = 1e9, lastY = 1e9, lastYaw = 1e9, lastAngle = 1e9, markAngle = 1e9;
    double right = -1e9, top = -1e9;
    bool visible = false, installed = false, backdropDone = false;
    Weak source;                   // the TAB screen's map this copy mirrors
};
Ui g_ui;

struct {
    Weak pawn;
    std::string level, zone;
    double depth = 0, handledDepth = 1e9, screenDepth = 1e9;
    int stable = 0;
    uint64_t statusAt = 0, refreshAt = 0, markersAt = 0, alignAt = 0, uiAt = 0, retryAt = 0;
    bool dirty = true, deep = true, unsafe = false, haveBounds = false;
    int mapBlocked = -1;
    Bounds bounds{};
    int builtShape = -1;
    float builtSize = 0;
} g;

void Retire()
{
    if (UObject* r = g_ui.root.Get()) {
        SetVis(r, VisHidden);
        Call(r, "RemoveFromParent").Run();
    }
    g_ui = Ui{};
}

void Show(bool want)
{
    want = want && g_ui.installed;
    UObject* r = g_ui.root.Get();
    if (!r || want == g_ui.visible) return;
    g_ui.visible = want;
    SetVis(r, want ? VisHitTestInvisible : VisHidden);
}

UObject* Texture(const char* name) { return FindObject(name); }

// The nested clip panels for a shape: absolute angle and side length (relative to the minimap size).
struct Level {
    double angle, side;
};
std::vector<Level> ShapeLevels(int shape)
{
    switch (shape) {
    case Circle: return {{0, 1}, {22.5, 1}, {45, 1}, {67.5, 1}};
    case Octagon: return {{0, 1}, {45, 1}};
    case Diamond: return {{45, 1 / std::sqrt(2.0)}};
    default: return {{0, 1}};
    }
}

// Builds the minimap around a fresh copy of the TAB map's widget class. Everything is created inside the copy's own
// widget tree, so the copy (in the viewport) keeps it all alive.
bool Build(UObject* pc, UObject* source)
{
    Retire();
    float S = std::clamp(float(g_value[feat::MinimapSize]), 160.f, 700.f);
    int shape = std::clamp(int(g_value[feat::MinimapShape]), 0, ShapeCount - 1);
    Call create(Cdo("WidgetBlueprintLibrary"), "Create");
    if (!create.Arg(0, pc).Arg(1, ClassOf(source)).Arg(2, pc).Run()) return false;
    UObject* root = create.Out<UObject*>("ReturnValue");
    UObject* tree = Ptr(root, "WidgetTree");
    UObject* mapRoot = TreeRoot(root);
    auto** rootSlot = tree ? Member<UObject*>(tree, "RootWidget") : nullptr;
    if (!IsLive(root) || !tree || !mapRoot || !rootSlot) return false;

    Ui ui;
    ui.root = Weak(root);
    ui.tree = tree;
    ui.mapRoot = mapRoot;
    ui.shape = shape;
    ui.size = S;
    ui.source = Weak(source);
    ui.canvas = Spawn("CanvasPanel", tree);
    ui.base = Spawn("CanvasPanel", tree);
    if (!ui.canvas || !ui.base) return false;
    *rootSlot = ui.canvas; // our panels become the root; the map moves inside them

    ui.baseSlot = AddToCanvas(ui.canvas, ui.base);
    SlotAnchors(ui.baseSlot, {0.5, 0}, {0.5, 0});
    SlotAlign(ui.baseSlot, {1, 0});
    SlotSize(ui.baseSlot, {S, S});
    SlotPos(ui.baseSlot, {0, 36});

    // Clip levels: each a square centred in its parent, rotated to its absolute angle.
    UObject* parent = ui.base;
    double parentSide = S, parentAngle = 0;
    for (const Level& l : ShapeLevels(shape)) {
        UObject* clip = Spawn("CanvasPanel", tree);
        if (!clip) return false;
        double side = S * l.side;
        UObject* slot = AddToCanvas(parent, clip);
        SlotPos(slot, {(parentSide - side) / 2, (parentSide - side) / 2});
        SlotSize(slot, {side, side});
        SetPivot(clip, {0.5, 0.5});
        SetAngle(clip, float(l.angle - parentAngle));
        SetClip(clip, true);
        parent = clip;
        parentSide = side;
        parentAngle = l.angle;
    }
    ui.inner = parent;
    ui.contentBase = float(parentAngle);

    // Backdrop (the TAB screen's background), filling the shape, not rotating.
    ui.backdrop = Spawn("Image", tree);
    if (ui.backdrop) {
        UObject* bs = AddToCanvas(ui.inner, ui.backdrop);
        SlotAnchors(bs, {0, 0}, {1, 1});
        SlotPos(bs, {0, 0});
        SlotSize(bs, {0, 0});
        SlotZ(bs, 0);
        SetVis(ui.backdrop, VisHidden);
    }
    // Content panel: minimap-sized, centred in the last clip level; map and markers move inside it.
    ui.content = Spawn("CanvasPanel", tree);
    if (!ui.content) return false;
    UObject* cs = AddToCanvas(ui.inner, ui.content);
    SlotPos(cs, {(parentSide - S) / 2, (parentSide - S) / 2});
    SlotSize(cs, {S, S});
    SlotZ(cs, 1);
    SetPivot(ui.content, {0.5, 0.5});
    SetAngle(ui.content, -ui.contentBase);

    UObject* ms = AddToCanvas(ui.content, mapRoot);
    if (!ms) return false;
    SlotAlign(ms, {0.5, 0.5});
    SlotSize(ms, {kMapW, kMapH});
    SlotPos(ms, {S / 2, S / 2});
    SlotZ(ms, 2);
    SetPivot(mapRoot, {0.5, 0.5});

    ui.layer = Spawn("CanvasPanel", tree);
    if (!ui.layer) return false;
    UObject* ls = AddToCanvas(ui.content, ui.layer);
    SlotAlign(ls, {0.5, 0.5});
    SlotSize(ls, {kMapW, kMapH});
    SlotPos(ls, {S / 2, S / 2});
    SlotZ(ls, 5);
    SetPivot(ui.layer, {0.5, 0.5});

    // Player arrow, at the centre, above everything (not clipped by the shape's rotation).
    ui.arrow = Spawn("Image", tree);
    if (ui.arrow) {
        if (UObject* tex = Texture("T_UI_PlayerPosition")) Call(ui.arrow, "SetBrushFromTexture").Arg(0, tex).Arg(1, false).Run();
        else SetColor(ui.arrow, {0.52f, 0.84f, 0.97f, 1});
        UObject* as = AddToCanvas(ui.base, ui.arrow);
        SlotAlign(as, {0.5, 0.5});
        SlotSize(as, {40, 40});
        SlotPos(as, {S / 2, S / 2});
        SlotZ(as, 15);
        SetPivot(ui.arrow, {0.5, 0.5});
    }
    // The frame is drawn by the overlay (FrameRect), for every shape.
    Call(root, "AddToViewport").Arg(0, int32_t(8)).Run();
    SetVis(root, VisHidden);
    g_ui = std::move(ui);
    g.builtShape = shape;
    g.builtSize = S;
    km::Log("Minimap: built (%s, %.0f px, copy of %s)", kShapeNames[shape], S, ClassName(source).c_str());
    return true;
}

// Mirrors the source map's state onto our copy: visibility, opacity, brushes and colours of its images.
void CopyTree(UObject* a, UObject* b, int depth, int& budget)
{
    if (depth > 9 || budget-- <= 0 || !a || !b) return;
    uint8_t va = Visibility(a);
    if (Visibility(b) != va) SetVis(b, va);
    float oa = Opacity(a);
    if (std::fabs(Opacity(b) - oa) > 0.001f) SetOpacity(b, oa);
    if (IsImage(a)) {
        if (BrushResource(a) != BrushResource(b)) CopyBrush(a, b);
        auto* ca = Member<LinearColor>(a, "ColorAndOpacity");
        auto* cb = Member<LinearColor>(b, "ColorAndOpacity");
        if (ca && cb && memcmp(ca, cb, sizeof(LinearColor))) SetColor(b, *ca);
    }
    UObject* ra = TreeRoot(a);
    UObject* rb = TreeRoot(b);
    if (ra && rb) return CopyTree(ra, rb, depth + 1, budget);
    int n = std::min({ChildCount(a), ChildCount(b), 200});
    for (int i = 0; i < n; i++) CopyTree(ChildAt(a, i), ChildAt(b, i), depth + 1, budget);
}

// Discovered rooms on our copy get the full-white, full-opacity treatment (never the game's own map).
void AssistZones(UObject* sub, UObject* from, UObject* to, int depth, bool visible, int& seen)
{
    if (depth > 10 || seen >= 250 || !from || !to) return;
    if (Hidden(Visibility(from))) visible = false;
    if (ClassName(from) == "W_MapZone_C") {
        seen++;
        Prop tag = visible ? FindProp(ClassOf(from), "ZoneTag") : Prop{};
        if (tag) {
            Call d(sub, "IsZoneDiscovered");
            if (d.ArgRaw(0, reinterpret_cast<uint8_t*>(from) + tag.offset, 8).Run() && d.Out<bool>("ReturnValue")) {
                UObject* target = Ptr(to, "MaterialImage_Zone");
                UObject* original = Ptr(from, "MaterialImage_Zone");
                if (target && original && !Hidden(Visibility(original))) {
                    SetColor(target, {1, 1, 1, 1});
                    SetOpacity(target, 1);
                    SetOpacity(to, 1);
                }
            }
        }
    }
    UObject* ra = TreeRoot(from);
    UObject* rb = TreeRoot(to);
    if (ra && rb) return AssistZones(sub, ra, rb, depth + 1, visible, seen);
    int n = std::min({ChildCount(from), ChildCount(to), 250});
    for (int i = 0; i < n; i++) AssistZones(sub, ChildAt(from, i), ChildAt(to, i), depth + 1, visible, seen);
}

void SyncBackdrop(UObject* screen)
{
    if (g_ui.backdropDone || !g_ui.backdrop || !screen) return;
    g_ui.backdropDone = true;
    UObject* src = Ptr(screen, "Image_Background_Background");
    if (!src) return;
    CopyBrush(src, g_ui.backdrop);
    if (auto* c = Member<LinearColor>(src, "ColorAndOpacity")) SetColor(g_ui.backdrop, *c);
    SetOpacity(g_ui.backdrop, Opacity(src));
    SetVis(g_ui.backdrop, Visibility(src));
}

// The first visible image with a texture inside a marker widget.
UObject* IconOf(UObject* w, int depth)
{
    if (depth > 7 || !w || Hidden(Visibility(w))) return nullptr;
    if (IsImage(w)) return BrushResource(w) ? w : nullptr;
    if (UObject* r = TreeRoot(w)) return IconOf(r, depth + 1);
    int n = std::min(ChildCount(w), 30);
    for (int i = 0; i < n; i++)
        if (UObject* icon = IconOf(ChildAt(w, i), depth + 1)) return icon;
    return nullptr;
}

void Markers(UObject* screen)
{
    UObject* panel = Ptr(screen, "Overlay_MapMarkers");
    if (!panel || !g_ui.layer) return;
    int count = std::min(ChildCount(panel), 300), shown = 0;
    double size = g_ui.zoom > 0 ? kMarkerSize / g_ui.zoom : kMarkerSize;
    for (int i = 0; i < count; i++) {
        UObject* w = ChildAt(panel, i);
        if (!w || Hidden(Visibility(w))) continue;
        Prop rt = FindProp(ClassOf(w), "RenderTransform");
        UObject* icon = rt ? IconOf(w, 0) : nullptr;
        if (!icon) continue;
        auto* t = reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(w) + rt.offset); // FWidgetTransform: Translation first
        double x = t[0], y = t[1];
        if (size_t(shown) == g_ui.marks.size()) {
            Marker m;
            UObject* img = Spawn("Image", g_ui.tree);
            if (!img) break;
            m.img = Weak(img);
            m.slot = AddToCanvas(g_ui.layer, img);
            SlotAlign(m.slot, {0.5, 0.5});
            SlotSize(m.slot, {size, size});
            SetPivot(img, {0.5, 0.5});
            g_ui.marks.push_back(m);
        }
        Marker& m = g_ui.marks[size_t(shown++)];
        UObject* img = m.img.Get();
        if (!img) continue;
        if (x != m.x || y != m.y) {
            SlotPos(m.slot, {kMapW / 2 + x, kMapH / 2 + y});
            m.x = x, m.y = y;
        }
        UObject* res = BrushResource(icon);
        if (res != m.resource) {
            CopyBrush(icon, img);
            if (auto* c = Member<LinearColor>(icon, "ColorAndOpacity")) SetColor(img, *c);
            m.resource = res;
        }
        if (!m.shown) {
            SetVis(img, VisHitTestInvisible);
            m.shown = true;
        }
    }
    for (size_t i = size_t(shown); i < g_ui.marks.size(); i++)
        if (UObject* img = g_ui.marks[i].img.Get(); img && g_ui.marks[i].shown) {
            SetVis(img, VisHidden);
            g_ui.marks[i].shown = false;
        }
    g_ui.markAngle = 1e9; // counter-rotation re-applied by the next frame
}

void Install(UObject* source)
{
    g_ui.installed = true;
    g_ui.source = Weak(source);
    g_ui.zoom = -1;
    g_ui.lastX = g_ui.lastY = g_ui.lastYaw = g_ui.lastAngle = 1e9;
}

bool Refresh(UObject* pc, UObject* screen, bool blockedMode)
{
    g.refreshAt = GetTickCount64();
    double before = ReadNum(screen, "CurrentDepth", 0);
    int delta = g.handledDepth < 1e8 ? int(std::lround(g.depth - g.handledDepth)) : 0;
    if (g.screenDepth < 1e8 && before != g.screenDepth) delta = 0; // the player moved the TAB map's floor
    if (blockedMode) delta = 0;                                    // the game owns the floor where its map is blocked
    if (!RefreshScreen(screen, g.deep, delta)) {
        km::Log("Minimap: the game's map refresh failed, minimap off for this level");
        return false;
    }
    g.handledDepth = g.depth;
    g.screenDepth = ReadNum(screen, "CurrentDepth", 0);
    UObject* source = Ptr(screen, "MapRef");
    if (!source) {
        km::Log("Minimap: the TAB map has no MapRef");
        return false;
    }
    int shape = std::clamp(int(g_value[feat::MinimapShape]), 0, ShapeCount - 1);
    float size = std::clamp(float(g_value[feat::MinimapSize]), 160.f, 700.f);
    if (!g_ui.root.Get() || g_ui.source.Get() != source || shape != g.builtShape || size != g.builtSize) {
        if (!Build(pc, source)) {
            km::Log("Minimap: couldn't build the widget");
            Retire();
            return false;
        }
        Install(source);
    }
    int budget = 6000, seen = 0;
    CopyTree(TreeRoot(source), g_ui.mapRoot, 0, budget);
    AssistZones(g_found.sub.Get(), TreeRoot(source), g_ui.mapRoot, 0, true, seen);
    SyncBackdrop(screen);
    Markers(screen);
    g.markersAt = GetTickCount64();
    g.dirty = g.deep = false;
    return true;
}

void Reset()
{
    Retire();
    g.stable = 0;
    g.zone.clear();
    g.depth = 0;
    g.handledDepth = g.screenDepth = 1e9;
    g.dirty = g.deep = true;
    g.unsafe = g.haveBounds = false;
    g.mapBlocked = -1;
    g.builtShape = -1;
    g_found.screen = g_found.sub = {};
    g_found.scanAt = 0;
}

void Align(UObject* pc)
{
    UObject* lib = Cdo("WidgetLayoutLibrary");
    Call size(lib, "GetViewportSize");
    Call scale(lib, "GetViewportScale");
    if (!size.Arg(0, pc).Run()) return;
    V2 vs = size.Out<V2>("ReturnValue");
    double sc = scale.Arg(0, pc).Run() ? scale.Out<float>("ReturnValue") : 1.0;
    if (vs.x <= 0 || vs.y <= 0) return;
    if (sc <= 0) sc = 1;
    double half = g_on[feat::MinimapScreenEdge] ? vs.x / 2 : std::min(vs.x, vs.y * 16 / 9) / 2;
    double right = half / sc - 36 + g_value[feat::MinimapOffsetX], top = 36 + g_value[feat::MinimapOffsetY];
    double S = g_ui.size;
    g_frame.x = float(vs.x / 2 + (right - S) * sc), g_frame.y = float(top * sc), g_frame.size = float(S * sc);
    g_frame.shape = g_ui.shape;
    if (right == g_ui.right && top == g_ui.top) return;
    SlotPos(g_ui.baseSlot, {right, top});
    if (g_ui.frameSlot) SlotPos(g_ui.frameSlot, {right + 8, top - 8});
    g_ui.right = right, g_ui.top = top;
}

} // namespace

const char* Status() { return g_status.load(); }

bool FrameRect(float& x, float& y, float& size, int& shape)
{
    if (!g_on[feat::Minimap] || !g_on[feat::MinimapFrame] || !g_ui.installed || !g_ui.visible) return false;
    x = g_frame.x, y = g_frame.y, size = g_frame.size, shape = g_frame.shape;
    return size > 1;
}

void Tick(UObject* pc, UObject* player)
{
    uint64_t now = GetTickCount64();
    if (!g_on[feat::Minimap]) {
        if (g_ui.root.Get()) Reset();
        g_status = "Off";
        return;
    }
    if (now - g.statusAt < 250) return; // status at 4 Hz, as the Lua mod
    g.statusAt = now;
    if (!pc || !player) {
        if (g_ui.root.Get() || g.stable) Reset();
        g.pawn = {};
        g_status = "Waiting for gameplay";
        return;
    }
    Call lvl(Cdo("GameplayStatics"), "GetCurrentLevelName");
    std::string level = lvl.Arg(0, pc).Arg(1, true).Run() ? lvl.Out<FString>("ReturnValue").ToUtf8() : "";
    std::string lower = level;
    for (char& c : lower) c = char(tolower(uint8_t(c)));
    if (level.empty() || lower.find("menu") != std::string::npos || lower.find("title") != std::string::npos) {
        if (!g.level.empty()) Reset();
        g.level.clear();
        g_status = "Waiting for gameplay";
        return;
    }
    if (level != g.level || g.pawn.Get() != player) { // new level or reloaded pawn: nothing old is reused
        Reset();
        g.level = level;
        g.pawn = Weak(player);
    }
    // Load safeguard: no map calls or widget building in the first 3 s of stable gameplay.
    if (++g.stable < 12) {
        Show(false);
        g_status = "Waiting for the level to settle";
        return;
    }
    if (now - g_found.scanAt > 2000 || !g_found.screen.Get()) {
        if (now - g_found.scanAt > 1000) {
            g_found.scanAt = now;
            Rescan(pc);
        }
    }
    bool inLab = false;
    GetBool(player, "bPlayerInLabyrinth", &inLab);
    if (inLab || Blocked()) {
        Show(false);
        g_status = inLab ? "Hidden in the Labyrinth" : "Hidden behind a game screen";
        return;
    }
    Call mb(player, "IsMapPageBlocked");
    int blocked = mb && mb.Run() ? int(mb.Out<bool>("ReturnValue")) : -1;
    if (blocked >= 0 && g.mapBlocked >= 0 && blocked != g.mapBlocked) {
        g.dirty = g.deep = true;
        g.handledDepth = g.screenDepth = 1e9;
    }
    g.mapBlocked = blocked;
    if (blocked == 1 && !g_on[feat::MinimapAlways]) {
        Show(false);
        g_status = "Hidden: the game's map is blocked here";
        return;
    }
    UObject* sub = g_found.sub.Get();
    if (!g.haveBounds || now % 1000 < 250) g.haveBounds = ReadBounds(sub, g.bounds) || g.haveBounds;
    if (!g.haveBounds) {
        Show(false);
        g_status = "Waiting for the map";
        return;
    }
    std::string zone;
    double depth = 0;
    if (ZoneDepth(sub, zone, depth) && (zone != g.zone || depth != g.depth)) {
        g.dirty = true;
        if (depth != g.depth) g.deep = true;
        g.zone = zone, g.depth = depth;
    }
    UObject* screen = g_found.screen.Get();
    if (!screen) {
        Show(false);
        g_status = "Waiting for the TAB map (open it once if this stays)";
        return;
    }
    if (g.unsafe) {
        Show(false);
        g_status = "Off for this level (the game's map refresh failed)";
        return;
    }
    if (g_ui.installed && now - g.refreshAt >= 20000) g.dirty = true; // fallback (the Lua mod: every 5 s)
    int shape = std::clamp(int(g_value[feat::MinimapShape]), 0, ShapeCount - 1);
    float size = std::clamp(float(g_value[feat::MinimapSize]), 160.f, 700.f);
    if (g_ui.installed && (shape != g.builtShape || size != g.builtSize)) g.dirty = true; // rebuilt by the refresh
    if (g.dirty && now - g.refreshAt >= 1500) {
        std::function<void()> work = [&] { g.unsafe = !Refresh(pc, screen, blocked == 1); };
        if (!Guard(work)) {
            g.unsafe = true;
            km::Log("Minimap: refresh faulted, minimap off for this level");
        }
        if (g.unsafe) {
            Retire();
            return;
        }
    }
    if (g_ui.installed && !g.dirty && now - g.markersAt >= 1000) {
        g.markersAt = now;
        Markers(screen);
    }
    if (g_ui.installed && now - g.alignAt >= 1000) {
        g.alignAt = now;
        Align(pc);
    }
    float opacity = 1.f - std::clamp(float(g_value[feat::MinimapOpacity]), 0.f, 90.f) / 100.f;
    if (g_ui.base && opacity != g_ui.opacity) {
        SetOpacity(g_ui.base, opacity);
        if (g_ui.frame) SetOpacity(g_ui.frame, std::min(1.f, opacity + 0.15f));
        g_ui.opacity = opacity;
    }
    Show(g_ui.installed);
    g_status = g_ui.installed ? "Showing" : "Building";
}

// Every engine frame: centre the map on the player (render translation only), rotate it if asked.
void Frame(UObject* pc, UObject* player)
{
    if (!g_on[feat::Minimap] || !g_ui.installed || !g_ui.visible || !g.haveBounds || !pc || !player || !g_ui.mapRoot) return;
    Call loc(player, "K2_GetActorLocation");
    Call rot(pc, "GetControlRotation");
    if (!loc.Run() || !rot.Run()) return;
    struct Vec3 {
        double x, y, z;
    } p = loc.Out<Vec3>("ReturnValue");
    struct Rot {
        double pitch, yaw, roll;
    } r = rot.Out<Rot>("ReturnValue");
    const Bounds& b = g.bounds;
    double lx = p.x - b.ox, ly = p.y - b.oy;
    double x = (lx * b.xx + ly * b.xy) / b.dx, y = (lx * b.yx + ly * b.yy) / b.dy;
    if (x != x || y != y || std::fabs(x) > 5 || std::fabs(y) > 5) return;

    double zoom = std::clamp(double(g_value[feat::MinimapZoom]), 0.3, 6.0) * (g_ui.size / 320.0);
    if (zoom != g_ui.zoom) {
        SetScale(g_ui.mapRoot, {zoom, zoom});
        SetScale(g_ui.layer, {zoom, zoom});
        for (Marker& m : g_ui.marks) SlotSize(m.slot, {kMarkerSize / zoom, kMarkerSize / zoom});
        g_ui.zoom = zoom;
        g_ui.lastX = 1e9;
    }
    bool rotate = g_on[feat::MinimapRotate];
    if (std::fabs(x - g_ui.lastX) > 2e-6 || std::fabs(y - g_ui.lastY) > 2e-6) {
        V2 t{-(x - 0.5) * kMapW * zoom, (y - 0.5) * kMapH * zoom};
        SetTranslation(g_ui.mapRoot, t);
        SetTranslation(g_ui.layer, t);
        g_ui.lastX = x, g_ui.lastY = y;
    }
    // North-up: the arrow turns (yaw + 270, as the Lua mod). Rotating: the map turns the other way and the arrow
    // stays put, so the player's heading keeps the same relation to the map.
    // Rotating: the facing direction is projected onto the map exactly as positions are (the map's own axes, which
    // needn't line up with the world's), so the map turns by the heading's real on-screen angle and your first-person
    // forward points straight up. The arrow keeps the same offset from the heading it has in north-up mode.
    double heading = r.yaw + 270.0; // north-up arrow angle (the Lua mod's)
    if (rotate) {
        constexpr double kRad = 3.14159265358979 / 180.0;
        double fx = std::cos(r.yaw * kRad), fy = std::sin(r.yaw * kRad);
        double mx = (fx * b.xx + fy * b.xy) / b.dx, my = (fx * b.yx + fy * b.yy) / b.dy;
        if (std::fabs(mx) + std::fabs(my) > 1e-12) heading = std::atan2(mx * kMapW, my * kMapH) / kRad; // clockwise from up
    }
    double mapAngle = rotate ? -heading : 0.0;
    double arrowAngle = rotate ? (r.yaw + 270.0) - heading : r.yaw + 270.0;
    if (std::fabs(r.yaw - g_ui.lastYaw) > 0.15 || g_ui.lastAngle > 1e8) {
        if (g_ui.arrow) SetAngle(g_ui.arrow, float(std::fmod(arrowAngle, 360.0)));
        if (g_ui.content) SetAngle(g_ui.content, float(std::fmod(mapAngle - g_ui.contentBase, 360.0)));
        g_ui.lastYaw = r.yaw;
        g_ui.lastAngle = mapAngle;
    }
    // Markers stay upright on a rotating map.
    if (std::fabs(mapAngle - g_ui.markAngle) > 0.5) {
        for (Marker& m : g_ui.marks)
            if (UObject* img = m.img.Get(); img && m.shown) SetAngle(img, float(-mapAngle));
        g_ui.markAngle = mapAngle;
    }
}

} // namespace minimap
