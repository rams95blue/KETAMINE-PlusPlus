// Changes to the game's own UI (see uitweaks.h). Game thread only.
#include "uitweaks.h"
#include "config.h"
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
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ue;

namespace uitweaks {
namespace {

using feat::g_on;
using feat::g_value;

struct FTextBytes { // an FText value (16 bytes in UE5; sizes always come from reflection). Kept alive on purpose.
    uint8_t b[32];
};
struct LinearColor {
    float r, g, b, a;
};

UObject* Ptr(UObject* o, const char* prop)
{
    auto p = o ? Member<UObject*>(o, prop) : nullptr;
    return p && IsLive(*p) ? *p : nullptr;
}
std::string ClassName(const UObject* o) { return o ? Name(reinterpret_cast<const UObject*>(ClassOf(o))) : ""; }
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
UObject* Cdo(const char* cls)
{
    static std::map<std::string, Weak> cache;
    Weak& w = cache[cls];
    if (!w.Get()) w = Weak(FindObject((std::string("Default__") + cls).c_str()));
    return w.Get();
}
UObject* TreeRoot(UObject* w) { return Ptr(Ptr(w, "WidgetTree"), "RootWidget"); }
// The widgets directly under w: a user widget's tree root, or a panel's children (read from reflection, no calls).
std::vector<UObject*> Kids(UObject* w)
{
    std::vector<UObject*> out;
    WidgetChildren(w, out, 64);
    return out;
}

// The size of an FText parameter, from the function itself.
size_t TextParamSize(UFunction* fn, int index)
{
    std::vector<Param> ps = fn ? Params(fn) : std::vector<Param>{};
    return index < int(ps.size()) && ps[size_t(index)].size > 0 && ps[size_t(index)].size <= 32 ? size_t(ps[size_t(index)].size) : 0;
}
FTextBytes MakeText(const std::string& s)
{
    std::wstring w(s.begin(), s.end());
    static std::vector<std::wstring> keep; // the FString data must outlive the call
    keep.push_back(w);
    FString str{keep.back().data(), int32_t(keep.back().size() + 1), int32_t(keep.back().size() + 1)};
    Call c(Cdo("KismetTextLibrary"), "Conv_StringToText");
    FTextBytes t{};
    if (c.Arg(0, str).Run()) t = c.Out<FTextBytes>("ReturnValue"); // the call's buffer is larger than 32 bytes
    return t;
}
// SetText(FText) on a text block or rich text block, with the parameter's real size.
bool SetTextOn(UObject* w, const FTextBytes& t)
{
    UFunction* fn = w ? FindFunction(ClassOf(w), "SetText") : nullptr;
    size_t n = TextParamSize(fn, 0);
    if (!n) return false;
    return Call(w, "SetText").ArgRaw(0, &t, n).Run();
}
bool IsText(UObject* w)
{
    std::string c = ClassName(w);
    return c == "TextBlock" || Has(c, "RichText") || Has(c, "TextWidget");
}
std::string TextOf(UObject* textBlock)
{
    UFunction* getFn = textBlock ? FindFunction(ClassOf(textBlock), "GetText") : nullptr;
    UFunction* convFn = FindFunction(ClassOf(Cdo("KismetTextLibrary")), "Conv_TextToString");
    size_t n = TextParamSize(convFn, 0);
    if (!getFn || !n) return {};
    Call get(textBlock, "GetText");
    if (!get.Run()) return {};
    FTextBytes t = get.Out<FTextBytes>("ReturnValue");
    Call conv(Cdo("KismetTextLibrary"), "Conv_TextToString");
    return conv.ArgRaw(0, &t, n).Run() ? conv.Out<FString>("ReturnValue").ToUtf8() : "";
}
// "<Input>F</>" -> "F", and the opening tag in `tag`.
std::string StripTags(const std::string& raw, std::string* tag = nullptr)
{
    std::string out;
    bool in = false;
    std::string cur;
    for (char c : raw) {
        if (c == '<') {
            in = true;
            cur = "<";
        } else if (c == '>' && in) {
            in = false;
            cur += '>';
            if (tag && tag->empty() && cur.size() > 2 && cur[1] != '/') *tag = cur;
        } else if (in) {
            cur += c;
        } else {
            out += c;
        }
    }
    return out;
}

// ================================================================ pause menu entry
//
// The game's pause menu keeps its items (W_MainMenu_ListEntry_C) in VB_Buttons and runs its own navigation over them,
// so nothing is added there: an extra item in that list broke the menu's input. The entry is a plain text block of
// our own, with no game code behind it, placed on the menu's root canvas just below VB_Buttons. It is clicked with
// the mouse (hover + left click), and opens this menu.

struct {
    Weak menu, entry;
    std::set<int32_t> failed; // pause menu instances where the layout didn't fit
    bool mouseWasDown = false, pressedOnEntry = false, hovered = false;
    uint64_t lookAt = 0, firstSeen = 0;
} g_pm;

UObject* Spawn(const char* cls, UObject* outer)
{
    UClass* c = FindClass(cls);
    Call sp(Cdo("GameplayStatics"), "SpawnObject");
    if (!c || !outer || !sp.Arg(0, c).Arg(1, outer).Run()) return nullptr;
    UObject* o = sp.Out<UObject*>("ReturnValue");
    return IsLive(o) ? o : nullptr;
}

std::string Lower(std::string v)
{
    for (char& c : v) c = char(tolower(static_cast<unsigned char>(c)));
    return v;
}

void DumpTree(UObject* w, int depth, int& lines)
{
    if (!w || depth > 12 || lines > 250) return;
    lines++;
    km::Log("  %*s%s (%s)", depth * 2, "", Name(w).c_str(), ClassName(w).c_str());
    for (UObject* k : Kids(w)) DumpTree(k, depth + 1, lines);
}

UObject* FindNamed(UObject* w, int depth, const char* part)
{
    if (!w || depth > 10) return nullptr;
    if (!Ptr(w, "WidgetTree") && Has(Lower(Name(w)), part) && Has(ClassName(w), "Box")) return w;
    for (UObject* k : Kids(w))
        if (UObject* f = FindNamed(k, depth + 1, part)) return f;
    return nullptr;
}

// A font the game actually ships: copied from a text block in its own UI (the engine's default font may not be cooked).
bool GameFont(uint8_t* out, int32_t size)
{
    UClass* tbCls = FindClass("TextBlock");
    Prop font = tbCls ? FindProp(tbCls, "Font") : Prop{};
    if (!font || font.kind != 's' || StructSize(static_cast<const UClass*>(font.inner)) != size) return false;
    bool found = false;
    ForEachInstance(tbCls, [&](UObject* tb) {
        UObject* fontObj = nullptr;
        SafeRead(reinterpret_cast<uint8_t*>(tb) + font.offset, &fontObj, sizeof(fontObj)); // FSlateFontInfo: FontObject first
        if (!IsLive(fontObj)) return true;
        found = SafeRead(reinterpret_cast<uint8_t*>(tb) + font.offset, out, size_t(size));
        return !found;
    });
    return found;
}

struct V2d {
    double x, y;
};

// 1 = added, 0 = not ready yet (try again shortly), -1 = failed.
int Inject(UObject* menuW, bool lastTry)
{
    UObject* root = TreeRoot(menuW);
    UObject* list = FindNamed(root, 0, "button");
    UObject* listSlot = Ptr(list, "Slot");
    bool ready = list && Kids(list).size() >= 2;
    if (list && !ready) return 0; // its items are added when the menu shows: wait for them
    (void)lastTry;
    if (!root || ClassName(root) != "CanvasPanel" || !listSlot || ClassName(listSlot) != "CanvasPanelSlot") {
        km::Log("Pause menu entry: the pause menu's layout isn't the expected one (root %s, buttons panel %s), nothing added. "
                "Its widgets:", root ? ClassName(root).c_str() : "none", list ? Name(list).c_str() : "none");
        int lines = 0;
        DumpTree(root, 0, lines);
        return -1;
    }
    UObject* tree = Ptr(menuW, "WidgetTree");
    UObject* tb = Spawn("TextBlock", tree ? tree : menuW);
    if (!tb) return -1;
    SetTextOn(tb, MakeText("KETAMINE++"));
    // Font: the game's, at a menu size.
    Prop font = FindProp(ClassOf(tb), "Font");
    const UClass* fontType = font && font.kind == 's' ? static_cast<const UClass*>(font.inner) : nullptr;
    int32_t fontSize = fontType ? StructSize(fontType) : 0;
    Prop sizeProp = fontType ? FindProp(fontType, "Size") : Prop{};
    UFunction* setFont = FindFunction(ClassOf(tb), "SetFont");
    if (fontSize > 0 && fontSize <= 256 && sizeProp && setFont) {
        uint8_t block[256] = {};
        if (!GameFont(block, fontSize)) memcpy(block, reinterpret_cast<uint8_t*>(tb) + font.offset, size_t(fontSize));
        if (sizeProp.kind == 'i') *reinterpret_cast<int32_t*>(block + sizeProp.offset) = 26;
        else if (sizeProp.kind == 'f') *reinterpret_cast<float*>(block + sizeProp.offset) = 26.f;
        std::vector<Param> ps = Params(setFont);
        if (!ps.empty() && ps[0].size == fontSize) Call(tb, "SetFont").ArgRaw(0, block, size_t(fontSize)).Run();
    }
    Call(tb, "SetVisibility").Arg(0, uint8_t(0)).Run(); // Visible: it can be hovered
    Call add(root, "AddChildToCanvas");
    UObject* slot = add.Arg(0, tb).Run() ? add.Out<UObject*>("ReturnValue") : nullptr;
    if (!IsLive(slot)) {
        km::Log("Pause menu entry: couldn't add the entry");
        return -1;
    }
    // Place: the buttons panel's anchors, just below it, same horizontal alignment.
    Prop layout = FindProp(ClassOf(listSlot), "LayoutData");
    UFunction* setLayout = FindFunction(ClassOf(slot), "SetLayout");
    std::vector<Param> lp = setLayout ? Params(setLayout) : std::vector<Param>{};
    if (layout && !lp.empty() && lp[0].size > 0 && lp[0].size <= 128)
        Call(slot, "SetLayout").ArgRaw(0, reinterpret_cast<uint8_t*>(listSlot) + layout.offset, size_t(lp[0].size)).Run();
    Call gp(listSlot, "GetPosition"), ga(listSlot, "GetAlignment"), gs(list, "GetDesiredSize");
    V2d pos = gp.Run() ? gp.Out<V2d>("ReturnValue") : V2d{0, 0};
    V2d align = ga.Run() ? ga.Out<V2d>("ReturnValue") : V2d{0, 0};
    V2d size = gs.Run() ? gs.Out<V2d>("ReturnValue") : V2d{0, 0};
    double gap = size.y > 0 ? size.y / double(std::max<size_t>(Kids(list).size(), 1)) * 0.35 : 16;
    Call(slot, "SetAutoSize").Arg(0, true).Run();
    Call(slot, "SetAlignment").Arg(0, V2d{align.x, 0}).Run();
    Call(slot, "SetPosition").Arg(0, V2d{pos.x, pos.y + (1 - align.y) * size.y + gap}).Run();
    Call(slot, "SetZOrder").Arg(0, int32_t(50)).Run();
    if (align.x > 0.3 && align.x < 0.7) Call(tb, "SetJustification").Arg(0, uint8_t(1)).Run(); // centred like the list
    g_pm.entry = Weak(tb);
    g_pm.hovered = false;
    g_pm.mouseWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    km::Log("Pause menu entry: added below %s (position %.0f, %.0f, list height %.0f)", Name(list).c_str(), pos.x, pos.y, size.y);
    return 1;
}

bool Hovered(UObject* w)
{
    Call h(w, "IsHovered");
    return w && h.Run() && h.Out<bool>("ReturnValue");
}

void SetColor(UObject* tb, float r, float g, float b, float a)
{
    UFunction* fn = FindFunction(ClassOf(tb), "SetColorAndOpacity");
    std::vector<Param> ps = fn ? Params(fn) : std::vector<Param>{};
    if (ps.empty() || ps[0].size < 20 || ps[0].size > 64) return;
    uint8_t block[64] = {}; // FSlateColor: FLinearColor, then the colour rule (0 = specified)
    LinearColor c{r, g, b, a};
    memcpy(block, &c, sizeof(c));
    Call(tb, "SetColorAndOpacity").ArgRaw(0, block, size_t(ps[0].size)).Run();
}

// Hover highlight, and a left click that starts and ends on the entry.
void CheckClick()
{
    UObject* entry = g_pm.entry.Get();
    UObject* menuW = g_pm.menu.Get();
    if (!entry || !menuW) return;
    bool hover = Hovered(entry);
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool open = false;
    if (down && !g_pm.mouseWasDown) g_pm.pressedOnEntry = hover;
    if (!down && g_pm.mouseWasDown && g_pm.pressedOnEntry && hover) open = true;
    if (!down) g_pm.pressedOnEntry = false;
    g_pm.mouseWasDown = down;
    if (hover != g_pm.hovered) {
        g_pm.hovered = hover;
        if (hover) SetColor(entry, 0.118f, 0.973f, 1.f, 1.f);
        else SetColor(entry, 0.92f, 0.92f, 0.92f, 1.f);
    }
    if (open) {
        g_pm.pressedOnEntry = false;
        menu::Open();
    }
}

void RemoveEntry()
{
    if (UObject* e = g_pm.entry.Get()) Call(e, "RemoveFromParent").Run();
    g_pm.entry = {};
}

void TickPauseMenu(UObject* pc)
{
    if (!g_on[feat::PauseMenuEntry] || !pc) {
        if (g_pm.entry.Get()) RemoveEntry();
        return;
    }
    CheckClick();
    uint64_t t = GetTickCount64();
    Call paused(Cdo("GameplayStatics"), "IsGamePaused");
    bool isPaused = paused.Arg(0, pc).Run() && paused.Out<bool>("ReturnValue");
    if (t - g_pm.lookAt < (isPaused ? 400u : 3000u)) return;
    g_pm.lookAt = t;
    UObject* found = nullptr;
    ForEachInstance(FindClass("W_PauseMenu_C"), [&](UObject* w) {
        Call in(w, "IsInViewport");
        if (in.Run() && in.Out<bool>("ReturnValue")) found = w;
        return !found;
    });
    if (!found) return;
    uint8_t* vis = Member<uint8_t>(found, "Visibility"); // the menu stays in the viewport, hidden, between pauses
    if (vis && (*vis == 1 || *vis == 2)) return;
    if (g_pm.menu.Get() == found && g_pm.entry.Get()) return; // already there
    if (g_pm.failed.count(ObjectIndex(found))) return;
    if (g_pm.menu.Get() != found) g_pm.firstSeen = t;
    g_pm.menu = Weak(found);
    bool lastTry = t - g_pm.firstSeen > 5000; // the buttons are added a moment after the menu opens
    std::function<void()> work = [&] {
        if (Inject(found, lastTry) < 0) g_pm.failed.insert(ObjectIndex(found));
    };
    if (!Guard(work)) {
        g_pm.failed.insert(ObjectIndex(found));
        km::Log("Pause menu entry: faulted, not retried for this menu");
    }
}

// ================================================================ simple prompts

struct HiddenW {
    Weak w;
    uint8_t vis; // the visibility it had
};
struct Label {
    Weak original, ours;
    std::string text;
    unsigned color = 0;
    float size = 0;
};
struct {
    std::vector<std::string> classes;
    std::map<int32_t, Weak> prompts;   // matched prompt widgets
    std::map<int32_t, HiddenW> hidden; // artwork (and the game's key text) we hid
    std::map<int32_t, Label> labels;   // our plain key labels, by the game's key text widget
    std::set<std::string> logged;
    uint64_t scanAt = 0, applyAt = 0;
    bool loaded = false, was = false;
} g_sp;

void LoadPromptClasses()
{
    g_sp.loaded = true;
    std::string list = cfg::GetString("Prompts", "Classes", "Interact,Prompt,KeyHint,InputHint,ButtonHint,KeyIcon,KeybindInfo");
    std::stringstream ss(list);
    for (std::string item; std::getline(ss, item, ',');) {
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (!item.empty()) g_sp.classes.push_back(item);
    }
}

// A prompt is on the HUD, never inside a menu, the map, the shop or the options.
bool InMenu(UObject* w)
{
    int depth = 0;
    for (UObject* o = OuterOf(w); o && depth < 12; o = OuterOf(o), depth++) {
        std::string c = ClassName(o);
        for (const char* k : {"Menu", "Map", "Shop", "Options", "Storage", "Puzzle", "Save", "Load", "Death"})
            if (Has(c, k)) return true;
    }
    return false;
}

bool PromptClass(const std::string& cls)
{
    if (Has(cls, "HUD") || Has(cls, "Hud") || Has(cls, "Menu") || Has(cls, "Map") || Has(cls, "Controller")) return false;
    for (const std::string& k : g_sp.classes)
        if (Has(cls, k.c_str())) return true;
    return false;
}

// Texts and images of one prompt. Nested prompts (a key hint inside an interact prompt) are walked too.
void Collect(UObject* w, int depth, std::vector<UObject*>& texts, std::vector<UObject*>& images)
{
    if (!w || depth > 12) return;
    std::string c = ClassName(w);
    if (IsText(w)) texts.push_back(w);
    else if (c == "Image" || c == "MaterialImage") images.push_back(w);
    for (UObject* k : Kids(w)) Collect(k, depth + 1, texts, images);
}

unsigned PromptColor()
{
    std::string v = cfg::GetString("Prompts", "Color", "#FFFFFF");
    return v.size() == 7 && v[0] == '#' ? unsigned(strtoul(v.c_str() + 1, nullptr, 16)) : 0xFFFFFF;
}

// The key text: the game shows it in rich text as "<Input>F</>" (InfernoRichTextWidget_InputKey). Descriptions
// ("<InputDescription_ColorOne>Barter</>") are not keys.
bool KeyText(UObject* t, const std::string& raw, std::string& key)
{
    std::string tag;
    key = StripTags(raw, &tag);
    while (!key.empty() && key.front() == ' ') key.erase(key.begin());
    while (!key.empty() && key.back() == ' ') key.pop_back();
    if (!key.empty() && key.front() == '[' && key.back() == ']') key = key.substr(1, key.size() - 2);
    if (key.empty() || key.size() > 12) return false;
    std::string name = Lower(Name(t)), ltag = Lower(tag);
    if (Has(name, "description") || Has(name, "action") || Has(name, "fail")) return false;
    return Has(name, "inputkey") || ltag == "<input>" || (ClassName(t) == "TextBlock" && Has(name, "key"));
}

uint8_t VisOf(UObject* w)
{
    uint8_t* v = Member<uint8_t>(w, "Visibility");
    return v ? *v : 0;
}
// Hidden (not collapsed: the layout stays), and kept hidden: the game's animations may show it again.
void HideW(UObject* w)
{
    int32_t idx = ObjectIndex(w);
    if (!g_sp.hidden.count(idx)) g_sp.hidden[idx] = {Weak(w), VisOf(w)};
    uint8_t v = VisOf(w);
    if (v != 1 && v != 2) Call(w, "SetVisibility").Arg(0, uint8_t(2)).Run();
}

// Our own plain text block showing the key, next to the game's (in the same panel, centred), in the game's font.
void ShowLabel(UObject* original, const std::string& text, float sizeFactor, uint8_t hAlign)
{
    unsigned color = PromptColor();
    float size = std::round(std::clamp(float(g_value[feat::PromptSize]), 8.f, 64.f) * sizeFactor);
    int32_t idx = ObjectIndex(original);
    Label& L = g_sp.labels[idx];
    UObject* ours = L.ours.Get();
    if (!ours) {
        Call gp(original, "GetParent");
        UObject* parent = gp.Run() ? gp.Out<UObject*>("ReturnValue") : nullptr;
        UObject* outer = OuterOf(original);
        if (!IsLive(parent) || !outer) return;
        ours = Spawn("TextBlock", outer);
        if (!ours) return;
        Call add(parent, "AddChild");
        UObject* slot = add.Arg(0, ours).Run() ? add.Out<UObject*>("ReturnValue") : nullptr;
        if (!IsLive(slot)) return;
        if (FindFunction(ClassOf(slot), "SetHorizontalAlignment")) {
            Call(slot, "SetHorizontalAlignment").Arg(0, hAlign); // 1 left, 2 centre
            Call(slot, "SetVerticalAlignment").Arg(0, uint8_t(2)).Run();
        }
        Call(ours, "SetVisibility").Arg(0, uint8_t(3)).Run(); // HitTestInvisible
        L.original = Weak(original);
        L.ours = Weak(ours);
        L.text.clear();
        L.size = 0;
    }
    if (L.text != text) {
        SetTextOn(ours, MakeText(text));
        L.text = text;
    }
    if (L.color != color || L.size != size) {
        SetColor(ours, ((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, 1.f);
        Prop font = FindProp(ClassOf(ours), "Font");
        const UClass* ft = font && font.kind == 's' ? static_cast<const UClass*>(font.inner) : nullptr;
        Prop sz = ft ? FindProp(ft, "Size") : Prop{};
        int32_t fontSize = ft ? StructSize(ft) : 0;
        std::vector<Param> ps = FindFunction(ClassOf(ours), "SetFont") ? Params(FindFunction(ClassOf(ours), "SetFont"))
                                                                        : std::vector<Param>{};
        if (sz && fontSize > 0 && fontSize <= 256 && !ps.empty() && ps[0].size == fontSize) {
            static uint8_t gameFont[256];
            static int haveFont = -1;
            if (haveFont < 0) haveFont = GameFont(gameFont, fontSize) ? 1 : 0;
            uint8_t block[256];
            memcpy(block, haveFont ? gameFont : reinterpret_cast<uint8_t*>(ours) + font.offset, size_t(fontSize));
            if (sz.kind == 'i') *reinterpret_cast<int32_t*>(block + sz.offset) = int32_t(size);
            else if (sz.kind == 'f') *reinterpret_cast<float*>(block + sz.offset) = size;
            Call(ours, "SetFont").ArgRaw(0, block, size_t(fontSize)).Run();
        }
        Call(ours, "SetShadowOffset").Arg(0, V2d{1, 1}).Run();
        L.color = color;
        L.size = size;
    }
}

void ApplyPrompt(UObject* w)
{
    std::vector<UObject*> texts, images;
    Collect(TreeRoot(w), 0, texts, images);
    // Only prompts that show their key as text get the minimal look: every piece of artwork (key cap, diamond,
    // pulses, plates) is hidden, the game's key text too, and our plain label shows the key. Descriptions stay.
    std::vector<std::pair<UObject*, std::string>> keys, descs;
    for (UObject* t : texts) {
        std::string raw = TextOf(t), key;
        if (KeyText(t, raw, key)) {
            keys.push_back({t, key});
        } else if (Has(Lower(Name(t)), "inputdescription")) { // "Barter", "(Hold) Take": our label, same colour
            std::string d = StripTags(raw);
            while (!d.empty() && d.front() == ' ') d.erase(d.begin());
            if (!d.empty() && d.size() < 48) descs.push_back({t, d});
        }
    }
    std::string cls = ClassName(w);
    if (keys.empty()) {
        if (g_sp.logged.insert(cls + "#nokey").second)
            km::Log("Simple prompts: %s shows no key as text right now (%zu texts, %zu images), left as it is", cls.c_str(),
                    texts.size(), images.size());
        return;
    }
    for (auto& [t, key] : keys) {
        ShowLabel(t, g_on[feat::PromptBrackets] ? "[" + key + "]" : key, 1.f, 2);
        if (g_sp.labels[ObjectIndex(t)].ours.Get()) HideW(t);
    }
    for (auto& [t, d] : descs) {
        ShowLabel(t, d, 0.8f, 1);
        if (g_sp.labels[ObjectIndex(t)].ours.Get()) HideW(t);
    }
    for (UObject* img : images) HideW(img);
    if (g_sp.logged.insert(cls).second)
        km::Log("Simple prompts: %s: key \"%s\" shown as plain text, %zu artwork image(s) hidden", cls.c_str(),
                keys[0].second.c_str(), images.size());
}

void RestorePrompts()
{
    for (auto& [i, h] : g_sp.hidden)
        if (UObject* w = h.w.Get()) Call(w, "SetVisibility").Arg(0, h.vis).Run();
    for (auto& [i, L] : g_sp.labels)
        if (UObject* o = L.ours.Get()) Call(o, "RemoveFromParent").Run();
    g_sp.hidden.clear();
    g_sp.labels.clear();
    g_sp.prompts.clear();
}

void TickPrompts()
{
    bool on = g_on[feat::SimplePrompts];
    if (g_sp.was && !on) RestorePrompts();
    g_sp.was = on;
    if (!on) return;
    if (!g_sp.loaded) LoadPromptClasses();
    // Every tick: what we hid stays hidden (the game's animations and prompt refreshes show it again).
    for (auto it = g_sp.hidden.begin(); it != g_sp.hidden.end();) {
        UObject* w = it->second.w.Get();
        if (!w) {
            it = g_sp.hidden.erase(it);
            continue;
        }
        uint8_t v = VisOf(w);
        if (v != 1 && v != 2) Call(w, "SetVisibility").Arg(0, uint8_t(2)).Run();
        ++it;
    }
    uint64_t t = GetTickCount64();
    if (t - g_sp.scanAt > 1000) { // new prompts appear as you walk up to things
        g_sp.scanAt = t;
        ForEachInstance(FindClass("UserWidget"), [](UObject* w) {
            if (g_sp.prompts.size() < 512 && !g_sp.prompts.count(ObjectIndex(w)) && PromptClass(ClassName(w)) && !InMenu(w))
                g_sp.prompts[ObjectIndex(w)] = Weak(w);
            return true;
        });
    }
    if (t - g_sp.applyAt < 200) return; // texts change as you look at different things
    g_sp.applyAt = t;
    for (auto it = g_sp.prompts.begin(); it != g_sp.prompts.end();) {
        UObject* w = it->second.Get();
        if (!w) {
            it = g_sp.prompts.erase(it);
            continue;
        }
        uint8_t v = VisOf(w);
        if (v != 1 && v != 2) { // only prompts the game shows
            std::function<void()> work = [&] { ApplyPrompt(w); };
            if (!Guard(work)) km::Log("Simple prompts: faulted on %s (skipped)", ClassName(w).c_str());
        }
        ++it;
    }
}

} // namespace

void Tick(UObject* pc)
{
    TickPauseMenu(pc);
    TickPrompts();
}

} // namespace uitweaks
