#include "menu.h"
#include "config.h"
#include "esp.h"
#include "features.h"
#include "fixes.h"
#include "fov.h"
#include "jump.h"
#include "minimap.h"
#include "lang.h"
#include "log.h"
#include "overlay.h"
#include "text.h"
#include "theme.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace menu {
namespace {

using feat::Toggle;
using feat::Value;
using namespace theme;
using lang::T;

constexpr int kNotBinding = -1, kBindMenu = -2, kBindMenu2 = -3, kBindFovUp = -4, kBindFovDown = -5, kBindJump = -6;
constexpr double kToastLife = 2.2;
constexpr float kBaseFont = 16.f;
const char* const kTabs[] = {"Player", "Combat", "Movement", "ESP", "Inventory", "World", "FOV", "Minimap", "Fixes", "Progress", "Settings"};
constexpr int kTabCount = IM_ARRAYSIZE(kTabs);

HMODULE g_module;
std::atomic<bool> g_open;
std::atomic<unsigned> g_menuKey{VK_F1};
std::atomic<unsigned> g_menuKey2{VK_F8}; // second menu key: not every keyboard has Insert (0 = none)
std::atomic<int> g_hotkey[feat::ToggleCount];
std::atomic<unsigned> g_fovUp{VK_PRIOR}, g_fovDown{VK_NEXT}; // World FOV steps, menu closed (0 = none)
std::atomic<int> g_binding{kNotBinding};
float g_scale = 0; // 0 = automatic, from the screen height
float g_appliedScale = -1;
struct FontPair {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};
FontPair g_fonts[3]; // by theme::Style
bool g_themeDirty = false; // palette or opacity changed: re-apply the theme at the start of the next frame
int DefaultSkin() // Win95
{
    for (int i = 0; i < theme::kSkinCount; i++)
        if (!strcmp(theme::kSkins[i].name, "Win95")) return i;
    return 0;
}
int g_skin = DefaultSkin();
unsigned g_accent = 0x1EF8FF, g_highlight = 0xECECEC;
int g_tab = 0;
std::string g_game, g_cpu, g_ram; // footer; the GPU comes from the overlay

struct Toast {
    std::string text;
    int state; // -1 plain message, 0 off, 1 on
    double start;
};
std::mutex g_toastMutex;
std::deque<Toast> g_toasts;

double Now() { return GetTickCount64() / 1000.0; }
double PreciseNow()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}
int DefaultHotkey(int t) { return t == feat::CustomFov ? VK_END : 0; } // End: FOV editor on/off, as in TrueFOV
float S() { return g_appliedScale; }

void PushToast(std::string text, int state)
{
    std::lock_guard lock(g_toastMutex);
    g_toasts.push_back({std::move(text), state, Now()});
    if (g_toasts.size() > 4) g_toasts.pop_front();
}

std::string KeyName(unsigned vk)
{
    if (!vk) return T("None");
    LONG scan = LONG(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
    switch (vk) { // navigation keys share scan codes with the numpad: mark them extended
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
        scan |= 1 << 24;
    }
    wchar_t buf[64];
    int n = GetKeyNameTextW(scan, buf, 64);
    if (n <= 0) return "Key " + std::to_string(vk);
    return km::Utf8(std::wstring(buf, size_t(n)));
}

// Drops vendor boilerplate: "AMD Ryzen 7 9800X3D 8-Core Processor" -> "Ryzen 7 9800X3D".
std::string ShortHardware(std::string s)
{
    for (const char* noise : {"(R)", "(TM)", "(tm)", "AMD ", "Intel ", "NVIDIA ", " Processor", " CPU", " with Radeon Graphics"})
        for (size_t p; (p = s.find(noise)) != std::string::npos;) s.erase(p, strlen(noise));
    if (auto core = s.find("-Core"); core != std::string::npos) { // " 8-Core"
        size_t start = s.rfind(' ', core);
        s.erase(start == std::string::npos ? 0 : start, core + 5 - (start == std::string::npos ? 0 : start));
    }
    if (auto at = s.find(" @"); at != std::string::npos) s.resize(at);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

void ReadHardware()
{
    wchar_t cpu[128] = {};
    DWORD size = sizeof(cpu);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                 RRF_RT_REG_SZ, nullptr, cpu, &size);
    g_cpu = ShortHardware(km::Utf8(cpu));
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    char ram[32];
    snprintf(ram, sizeof(ram), "%.0f GB", double(mem.ullTotalPhys) / (1ull << 30));
    g_ram = ram;
}

ImFont* LoadFont(int id)
{
    HRSRC res = FindResourceW(g_module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(g_module, res) : nullptr;
    void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false; // the data lives in the DLL image
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(SizeofResource(g_module, res)), kBaseFont, &cfg);
}

// Read-only view of a Windows font: only the pages ImGui actually reads get loaded.
void* MapFont(const wchar_t* file, int& size)
{
    wchar_t dir[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    std::wstring path = std::wstring(dir) + L"\\Fonts\\" + file;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER bytes{};
    GetFileSizeEx(f, &bytes);
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    void* view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (map) CloseHandle(map);
    size = int(bytes.QuadPart);
    return view;
}

// Inter has no Chinese, Japanese or Korean glyphs: merge the Windows fonts for them into ours, so every language name in
// Settings and the current translation render. The current language goes first: it wins for the shared ideographs.
void MergeScriptFonts()
{
    int order[] = {lang::Get(), lang::Zh, lang::Ja, lang::Ko};
    std::vector<const wchar_t*> done;
    for (int l : order) {
        const wchar_t* const* files = lang::kLangs[l].fonts;
        if (!files[0] || std::find(done.begin(), done.end(), files[0]) != done.end()) continue;
        done.push_back(files[0]);
        for (int b = 0; b < 2; b++) {
            ImFont* dst = b ? bold : regular;
            int size = 0;
            void* data = MapFont(files[b] ? files[b] : files[0], size);
            if (!data && b) data = MapFont(files[0], size);
            if (!data || !dst) continue;
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.DstFont = dst;
            cfg.FontDataOwnedByAtlas = false; // the mapped view stays for the process lifetime
            ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, size, kBaseFont, &cfg);
        }
        km::Log("Fonts: merged %ls", files[0]);
    }
}

void ApplyTheme(float scale)
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.FontSizeBase = kBaseFont;
    st.FontScaleMain = scale;
    st.WindowPadding = {20, 18};
    st.FramePadding = {10, 6};
    st.ItemSpacing = {10, 8};
    st.ItemInnerSpacing = {8, 6};
    st.CellPadding = {6, 4};
    st.WindowRounding = 14;
    st.ChildRounding = 10;
    st.FrameRounding = 7;
    st.PopupRounding = 10;
    st.GrabRounding = 6;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 1;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.ScrollbarSize = 8;
    st.GrabMinSize = 12;
    st.DisabledAlpha = 0.4f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = Text;
    c[ImGuiCol_TextDisabled] = Muted;
    c[ImGuiCol_WindowBg] = Alpha(Bg, Opacity);
    c[ImGuiCol_ChildBg] = Panel;
    c[ImGuiCol_PopupBg] = Panel2;
    c[ImGuiCol_Border] = Border;
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = Panel2;
    c[ImGuiCol_FrameBgHovered] = Mix(Panel2, Border, 0.6f);
    c[ImGuiCol_FrameBgActive] = Border;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = Bg;
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = Border;
    c[ImGuiCol_ScrollbarGrabHovered] = Mix(Border, Muted, 0.4f);
    c[ImGuiCol_ScrollbarGrabActive] = Violet;
    c[ImGuiCol_CheckMark] = Violet;
    c[ImGuiCol_SliderGrab] = Violet;
    c[ImGuiCol_SliderGrabActive] = Pink;
    c[ImGuiCol_Button] = Panel2;
    c[ImGuiCol_ButtonHovered] = Mix(Panel2, Border, 0.7f);
    c[ImGuiCol_ButtonActive] = Alpha(Violet, 0.55f);
    c[ImGuiCol_Header] = Alpha(Violet, 0.14f);
    c[ImGuiCol_HeaderHovered] = Alpha(Violet, 0.22f);
    c[ImGuiCol_HeaderActive] = Alpha(Violet, 0.30f);
    c[ImGuiCol_Separator] = Border;
    c[ImGuiCol_ResizeGrip] = {0, 0, 0, 0};
    c[ImGuiCol_PlotHistogram] = Violet;
    c[ImGuiCol_TableRowBg] = c[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0};
    c[ImGuiCol_TextSelectedBg] = Alpha(Violet, 0.35f);
    c[ImGuiCol_NavCursor] = Violet;
    c[ImGuiCol_ModalWindowDimBg] = Alpha(Black, 0.55f);
    if (Style != StyleModern) {
        st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.GrabRounding = st.ScrollbarRounding = 0;
        st.ScrollbarSize = 12;
        st.FrameBorderSize = 1;
        st.GrabMinSize = 14;
        c[ImGuiCol_ScrollbarBg] = Panel2;
    }
    if (Style == StyleRetro) { // bevels: a light edge with a dark shadow under it, like Windows 9x controls
        st.WindowBorderSize = 2;
        st.ChildBorderSize = 1;
        c[ImGuiCol_Border] = Border;
        c[ImGuiCol_BorderShadow] = Mix(Black, Bg, 0.35f);
        c[ImGuiCol_ScrollbarGrab] = Panel2;
        c[ImGuiCol_ScrollbarGrabHovered] = Mix(Panel2, Border, 0.4f);
        c[ImGuiCol_Header] = Alpha(Violet, 0.85f); // selection: accent with readable text below
        c[ImGuiCol_HeaderHovered] = Alpha(Violet, 0.6f);
        c[ImGuiCol_HeaderActive] = Violet;
    }
    if (Style == StyleTerminal) c[ImGuiCol_Border] = Mix(Border, Violet, 0.35f);
    // The font of the look (Inter, Tahoma or Consolas); a missing one falls back to Inter.
    const FontPair& fp = g_fonts[Style].regular ? g_fonts[Style] : g_fonts[StyleModern];
    if (fp.regular) {
        regular = fp.regular, bold = fp.bold ? fp.bold : fp.regular;
        ImGui::GetIO().FontDefault = regular;
    }
    st.ScaleAllSizes(scale);
    // ScaleAllSizes rounds the cursor scale down: any UI scale below 100% made it 0 and the mouse cursor vanished.
    st.MouseCursorScale = std::max(1.f, scale);
}

// ---------------------------------------------------------------- widgets

float ButtonWidth(const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; }

// Next item on the same line, flush right.
void SameLineRight(float width)
{
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - width));
}

// Subtle highlight behind the row under the mouse. Call before drawing the row's widgets.
void RowHover(ImVec2 rowMin, float width)
{
    ImVec2 rowMax{rowMin.x + width, rowMin.y + ImGui::GetFrameHeight()};
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(rowMin, rowMax))
        ImGui::GetWindowDrawList()->AddRectFilled({rowMin.x - 8 * S(), rowMin.y}, {rowMax.x + 4 * S(), rowMax.y},
                                                  Raw(Alpha(Text, 0.035f)), 7 * S());
}

// Pill switch, vertically centred on a frame-height row. Fades with BeginDisabled like any widget.
bool Switch(const char* id, bool* v)
{
    float frameH = ImGui::GetFrameHeight(), h = frameH * 0.70f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, {w, frameH});
    if (clicked) *v = !*v;
    ImGuiStorage* store = ImGui::GetStateStorage();
    ImGuiID sid = ImGui::GetID(id);
    float target = *v ? 1.f : 0.f, t = store->GetFloat(sid, target);
    t += (target - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 16.f);
    store->SetFloat(sid, t);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a{p.x, p.y + (frameH - h) * 0.5f}, b{a.x + w, a.y + h};
    ImVec4 off = ImGui::IsItemHovered() ? Mix(Panel2, Border, 0.8f) : Panel2;
    dl->AddRectFilled(a, b, U32(Mix(off, Violet, t)), R(h * 0.5f));
    if (t < 0.99f) dl->AddRect(a, b, U32(Alpha(Border, 1 - t)), R(h * 0.5f));
    float r = h * 0.5f - 3.f * S();
    ImVec2 c{a.x + h * 0.5f + t * (w - h), a.y + h * 0.5f};
    if (Style == StyleModern) {
        dl->AddCircleFilled(c, r, U32(Mix(Muted, Text, t)), 20);
    } else { // square knob, in a colour that reads on the accent
        float lum = Violet.x * 0.3f + Violet.y * 0.59f + Violet.z * 0.11f;
        ImVec4 on = lum < 0.5f ? ImVec4{1, 1, 1, 1} : ImVec4{0, 0, 0, 1};
        dl->AddRectFilled({c.x - r, c.y - r}, {c.x + r, c.y + r}, U32(Mix(Muted, on, t)));
    }
    return clicked;
}

void Tooltip(ImVec2 rowMin, ImVec2 rowMax, const std::string& text)
{
    if (!text.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseHoveringRect(rowMin, rowMax)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
        ImGui::TextUnformatted(T(text.c_str()));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Section(const char* title)
{
    float s = S();
    ImGui::Dummy({0, 6 * s});
    ImGui::PushFont(bold, kBaseFont * 0.78f);
    ImGui::PushStyleColor(ImGuiCol_Text, Style == StyleTerminal ? Violet : Muted);
    if (Style == StyleTerminal) ImGui::Text("> %s", T(title));
    else ImGui::TextUnformatted(T(title));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine({p.x, p.y - 2 * s}, {p.x + ImGui::GetContentRegionAvail().x, p.y - 2 * s}, U32(Border));
    ImGui::Dummy({0, 3 * s});
}

// Small pill showing a key (hotkey hint).
void KeyBadge(const char* text)
{
    float s = S(), size = kBaseFont * 0.78f * s, frameH = ImGui::GetFrameHeight();
    ImVec2 ts = regular->CalcTextSizeA(size, FLT_MAX, 0, text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ts.y + 4 * s, w = ts.x + 12 * s, y = p.y + (frameH - h) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({p.x, y}, {p.x + w, y + h}, U32(Panel2), R(5 * s));
    dl->AddRect({p.x, y}, {p.x + w, y + h}, U32(Border), R(5 * s));
    dl->AddText(regular, size, {p.x + 6 * s, y + 2 * s}, U32(Muted), text);
    ImGui::Dummy({w, frameH});
}

void ToggleRow(Toggle t, const feat::Snapshot& snap)
{
    const feat::ToggleInfo& info = feat::kToggles[t];
    const std::string& why = snap.why[t];
    ImGui::PushID(t);
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    RowHover(rowMin, avail);
    ImGui::BeginDisabled(!why.empty());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(info.label));
    if (int key = g_hotkey[t]) {
        ImGui::SameLine();
        KeyBadge(KeyName(unsigned(key)).c_str());
    }
    float sw = ImGui::GetFrameHeight() * 0.70f * 1.85f;
    ImGui::SameLine(x0 + avail - sw);
    bool v = feat::g_on[t];
    if (Switch("##sw", &v)) {
        feat::g_on[t] = v;
        feat::Save();
    }
    ImGui::EndDisabled();
    Tooltip(rowMin, {rowMin.x + avail, rowMin.y + ImGui::GetFrameHeight()}, why.empty() ? info.help : why);
    ImGui::PopID();
}

// Values where 0 means "follow something else": shown as text below the slider's usable range.
const char* ZeroLabel(Value v)
{
    switch (v) {
    case feat::ViewmodelFov: return "Same as world FOV";
    case feat::AdsFov: return "Game's own";
    case feat::BhopCap: return "Unlimited";
    default: return nullptr;
    }
}

// Label, slider (value printed inside it), Reset.
void SliderRow(Value v, const char* label, const char* fmt, const feat::Snapshot& snap, const char* help,
               const char* resetLabel = "Reset")
{
    const feat::ValueInfo& info = feat::kValues[v];
    const std::string& why = snap.valueWhy[v];
    ImGui::PushID(100 + v);
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    RowHover(rowMin, avail);
    ImGui::BeginDisabled(!why.empty());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(label));
    ImGui::SameLine(x0 + avail * 0.40f);
    resetLabel = T(resetLabel);
    ImGui::SetNextItemWidth(avail * 0.60f - ButtonWidth(resetLabel) - ImGui::GetStyle().ItemSpacing.x);
    const char* zero = ZeroLabel(v);
    float lo = zero ? (v == feat::BhopCap ? 1.f : 30.f) : info.min; // 0 = follow / unlimited, below any usable value
    float val = feat::g_value[v];
    const char* shown = zero && val < lo ? T(zero) : fmt;
    if (ImGui::SliderFloat("##v", &val, lo, info.max, shown, ImGuiSliderFlags_AlwaysClamp)) feat::g_value[v] = val;
    if (ImGui::IsItemDeactivatedAfterEdit()) feat::Save();
    ImGui::SameLine();
    if (ImGui::Button(resetLabel)) {
        feat::g_value[v] = info.def;
        feat::Save();
    }
    ImGui::EndDisabled();
    Tooltip(rowMin, {rowMin.x + avail * 0.40f, rowMin.y + ImGui::GetFrameHeight()}, why.empty() ? help : why);
    ImGui::PopID();
}

// Muted help text that wraps instead of running off the panel.
void Hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Muted);
    ImGui::TextWrapped("%s", T(text));
    ImGui::PopStyleColor();
}

// Label, then any control laid out from the 40% column.
float ControlColumn(const char* label, float avail, float x0)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(label));
    ImGui::SameLine(x0 + avail * 0.40f);
    return avail * 0.60f;
}

// Never touches the scale: S() is used by everything still drawn in this frame, and an invalid scale there faulted the
// overlay (which then turned itself off until the game restarted).
void SetSkin(int i, bool save = true)
{
    g_skin = std::clamp(i, 0, theme::kSkinCount - 1);
    theme::ApplySkin(g_skin, g_accent, g_highlight);
    g_themeDirty = true;
    if (!save) return;
    cfg::SetString("Menu", "Skin", theme::kSkins[g_skin].name);
    char buf[16];
    snprintf(buf, sizeof(buf), "#%06X", g_accent);
    cfg::SetString("Menu", "Accent", buf);
    snprintf(buf, sizeof(buf), "#%06X", g_highlight);
    cfg::SetString("Menu", "Highlight", buf);
}

// ---------------------------------------------------------------- tabs

// Speed units for the hop readout and the speedometer; speeds come in cm/s (1 Unreal unit = 1 cm).
struct Unit {
    const char* name; // menu
    const char* suffix;
    double perCmS;
    const char* fmt;
};
const Unit kUnits[4] = {
    {"km/h", "km/h", 0.036, "%.1f"},
    {"mph", "mph", 0.0223694, "%.1f"},
    {"Hammer units/s", "u/s", 1 / 2.54, "%.0f"},
    {"Unreal units/s", "uu/s", 1.0, "%.0f"},
};
int SpeedUnit() { return std::clamp(int(feat::g_value[feat::SpeedUnit]), 0, 3); }
std::string FormatSpeed(double cmPerS)
{
    const Unit& u = kUnits[SpeedUnit()];
    char num[32], out[48];
    snprintf(num, sizeof(num), u.fmt, cmPerS * u.perCmS);
    snprintf(out, sizeof(out), "%s %s", num, u.suffix);
    return out;
}

void TabPlayer(const feat::Snapshot& s)
{
    Section("STATUS");
    { // health: drag to set, applied on release
        static float editing = -1;
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
        RowHover(rowMin, avail);
        ImGui::BeginDisabled(!s.player || s.maxHealth <= 0);
        float w = ControlColumn("Health", avail, x0);
        ImGui::SetNextItemWidth(w - ButtonWidth(T("Heal")) - ImGui::GetStyle().ItemSpacing.x);
        float hp = editing >= 0 ? editing : s.health;
        char fmt[32];
        snprintf(fmt, sizeof(fmt), s.maxHealth > 0 ? "%%.0f / %.0f" : "-", s.maxHealth);
        if (ImGui::SliderFloat("##hp", &hp, 1, std::max(s.maxHealth, 1.f), fmt, ImGuiSliderFlags_AlwaysClamp)) editing = hp;
        if (ImGui::IsItemDeactivated() && editing >= 0) {
            feat::SetHealth(editing);
            editing = -1;
        }
        ImGui::SetItemTooltip("%s", T("Drag to set your health."));
        ImGui::SameLine();
        if (ImGui::Button(T("Heal"))) feat::Heal();
        ImGui::EndDisabled();
    }
    Section("SURVIVAL");
    for (Toggle t : {feat::GodMode, feat::InfiniteHealth, feat::NoSprintCooldown, feat::Invisible}) ToggleRow(t, s);
    Section("DAMAGE");
    ToggleRow(feat::OneHitKill, s);
    SliderRow(feat::DamageMult, "Damage multiplier", "x%.1f", s, "Multiplies the damage you deal to enemies.");
}

void TabCombat(const feat::Snapshot& s)
{
    Section("AIM");
    ToggleRow(feat::SilentAim, s);
    SliderRow(feat::AimFov, "Aim FOV", "%.0f°", s, "Size of the circle around the crosshair where enemies are targeted.");
    Section("WEAPONS");
    for (Toggle t : {feat::InfiniteAmmo, feat::Unbreakable}) ToggleRow(t, s);
    Section("GENESIS CONFIGURATION");
    {
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
        RowHover(rowMin, avail);
        ImGui::BeginDisabled(!s.player || s.maxSuffering <= 0);
        float w = ControlColumn("Suffering", avail, x0);
        char text[32];
        snprintf(text, sizeof(text), s.maxSuffering > 0 ? "%.0f / %.0f" : "-", s.suffering, s.maxSuffering);
        ImGui::ProgressBar(s.maxSuffering > 0 ? s.suffering / s.maxSuffering : 0.f,
                           {w - ButtonWidth(T("Fill")) - ImGui::GetStyle().ItemSpacing.x, 0}, text);
        ImGui::SameLine();
        if (ImGui::Button(T("Fill"))) feat::FillSuffering();
        ImGui::EndDisabled();
    }
    for (Toggle t : {feat::NoSufferingCost, feat::NoPowerCooldown}) ToggleRow(t, s);
    Hint("This section shows what your save has. These two start off every time a save loads: turn them on here after "
         "loading if you want them.");
    for (int p = 0; p < feat::PowerCount; p++) {
        ImGui::PushID(p);
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x;
        RowHover(rowMin, avail);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(feat::kPowerNames[p]));
        if (s.powers[p]) {
            SameLineRight(ImGui::CalcTextSize(T("Unlocked")).x);
            ImGui::TextDisabled("%s", T("Unlocked"));
        } else {
            SameLineRight(ButtonWidth(T("Unlock")));
            ImGui::BeginDisabled(!s.powersWhy.empty());
            if (ImGui::Button(T("Unlock"))) feat::UnlockPower(feat::Power(p));
            ImGui::EndDisabled();
        }
        Tooltip(rowMin, {rowMin.x + avail, rowMin.y + ImGui::GetFrameHeight()},
                s.powers[p]                ? ""
                : !s.powersWhy.empty()     ? s.powersWhy
                                           : "Gives the power now, even before the story does. Save first: it can skip its tutorial.");
        ImGui::PopID();
    }
    { // the game's own mode, shown as it is: a scripted part whose end was skipped can leave it on
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
        RowHover(rowMin, avail);
        bool on = s.genesisOnly == 1, locked = s.genesisOnly < 0 || (!on && !s.powersWhy.empty());
        ImGui::BeginDisabled(locked);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("Genesis Configuration only"));
        ImGui::SameLine(x0 + avail - ImGui::GetFrameHeight() * 0.70f * 1.85f);
        if (Switch("##genesisOnly", &on)) feat::SetGenesisOnly(on);
        ImGui::EndDisabled();
        Tooltip(rowMin, {rowMin.x + avail, rowMin.y + ImGui::GetFrameHeight()},
                s.genesisOnly < 0 ? "Not found in this version of the game."
                : locked          ? s.powersWhy
                                  : "Some scripted parts leave you only the Genesis Configuration. Turn this off if it "
                                    "stayed in your hand for good.");
    }
}

void BindButton(const char* id, int target, unsigned key);

void TabMovement(const feat::Snapshot& s)
{
    Section("MOVEMENT");
    for (Toggle t : {feat::Fly, feat::Noclip, feat::FreeCam, feat::PauseTime, feat::UnlockPitch}) ToggleRow(t, s);
    SliderRow(feat::SpeedMult, "Speed", "x%.2f", s, "Movement speed multiplier (also flying and free camera speed).");

    Section("JUMP");
    ToggleRow(feat::Jump, s);
    {
        float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
        ControlColumn("Jump key", avail, x0);
        BindButton("jumpkey", kBindJump, jump::g_key);
        ImGui::SameLine();
        std::string clash = jump::Conflict();
        bool space = jump::g_key == VK_SPACE;
        if (space || !clash.empty()) { // Space is Evade by default; anything else the game binds is named from its mappings
            char line[320];
            if (space)
                snprintf(line, sizeof(line), "%s", T("Space is the game's Evade key by default: rebind Evade in the game's "
                                                    "Controls settings, or pick another jump key, or every jump also evades."));
            else
                snprintf(line, sizeof(line), T("%s is also the game's %s key: pressing it does both. Pick another jump key."),
                         KeyName(jump::g_key).c_str(), clash.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, Pink);
            ImGui::TextWrapped("%s", line);
            ImGui::PopStyleColor();
        }
    }
    SliderRow(feat::JumpMult, "Jump height", "x%.1f", s, "Jump velocity multiplier.");
    ToggleRow(feat::Bhop, s);
    ImGui::BeginDisabled(!feat::g_on[feat::Bhop]);
    ToggleRow(feat::BhopAuto, s);
    SliderRow(feat::BhopGain, "Speed gain per hop", "+%.0f%%", s,
              "Speed added by each chained hop. A perfectly timed hop (within 30 ms of landing) adds double.");
    SliderRow(feat::BhopCap, "Max hop speed", "x%.1f", s, "Hop speed cap, as a multiple of your walking speed.", "Unlimited");
    SliderRow(feat::BhopAirStrafe, "Air strafe", "%.0f", s,
              "How quickly strafing while turning the mouse builds speed in the air. 0 turns air strafing off.");
    ToggleRow(feat::BhopHud, s);
    ImGui::EndDisabled();
    ToggleRow(feat::Speedometer, s);
    {
        float av = ImGui::GetContentRegionAvail().x, xx = ImGui::GetCursorPosX();
        ControlColumn("Speed unit", av, xx);
        int unit = SpeedUnit();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::BeginCombo("##speedunit", T(kUnits[unit].name))) {
            for (int i = 0; i < 4; i++)
                if (ImGui::Selectable(T(kUnits[i].name), i == unit)) {
                    feat::g_value[feat::SpeedUnit] = float(i);
                    feat::Save();
                }
            ImGui::EndCombo();
        }
    }
    Hint("Unit for the hop readout and the speedometer. Hammer units are Source's (1 unit = 1 inch); Unreal units are cm.");
    if (feat::g_on[feat::Bhop])
        Hint(feat::g_on[feat::BhopAuto] ? "Hold the jump key: you hop on every landing."
                                         : "Press jump as you land: up to 0.12 s early or 0.15 s late chains the hop, within "
                                           "0.03 s of the landing it's perfect (double gain). Works in every direction, "
                                           "backwards too. Hold strafe and turn the mouse the same way in the air to build speed.");

    Section("SAVED POSITIONS");
    for (int i = 0; i < 3; i++) {
        ImGui::PushID(i);
        RowHover(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
        ImGui::AlignTextToFramePadding();
        ImGui::Text(T("Slot %d"), i + 1);
        ImGui::SameLine(0, 12 * S());
        ImGui::TextDisabled("%s", s.slots[i].empty() ? T("empty") : s.slots[i].c_str());
        SameLineRight(ButtonWidth(T("Save")) + ImGui::GetStyle().ItemSpacing.x + ButtonWidth(T("Go")));
        ImGui::BeginDisabled(!s.player);
        if (ImGui::Button(T("Save"))) feat::SaveSlot(i);
        ImGui::SameLine();
        ImGui::BeginDisabled(s.slots[i] != s.map);
        if (ImGui::Button(T("Go"))) feat::LoadSlot(i);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    Section("LEVEL CHECKPOINTS");
    if (s.checkpoints.empty()) ImGui::TextDisabled("%s", T(s.player ? "No checkpoint in this level." : "Load a save first."));
    for (const feat::Named& cp : s.checkpoints) {
        ImGui::PushID(int(cp.fname));
        RowHover(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cp.name.c_str());
        SameLineRight(ButtonWidth(T("Go")));
        if (ImGui::Button(T("Go"))) feat::TeleportCheckpoint(cp.fname);
        ImGui::PopID();
    }
}

void TabEsp(const feat::Snapshot& s)
{
    Section("ESP");
    ToggleRow(feat::Esp, s);
    ToggleRow(feat::EspAimFov, s); // independent of the ESP switch
    ImGui::BeginDisabled(!feat::g_on[feat::Esp]);
    Section("ENEMIES");
    for (Toggle t : {feat::EspBox, feat::EspHealth, feat::EspName, feat::EspDistance, feat::EspSkeleton, feat::EspHead,
                     feat::EspChams, feat::EspSnaplines, feat::EspOffscreen})
        ToggleRow(t, s);
    SliderRow(feat::EspRange, "Enemy range", "%.0f m", s, "Enemies farther than this are not shown.");
    Section("WORLD");
    for (Toggle t : {feat::EspItems, feat::EspContainers, feat::EspSavePoints}) ToggleRow(t, s);
    SliderRow(feat::EspWorldRange, "Object range", "%.0f m", s, "Objects farther than this are not shown.");
    ImGui::EndDisabled();
}

void TabInventory(const feat::Snapshot& s)
{
    static int amount = 1000, giveCount = 1;
    static char filter[64];

    Section("MONEY");
    {
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
        RowHover(rowMin, avail);
        ImGui::BeginDisabled(!s.player);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("Money"));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Pink);
        ImGui::PushFont(bold, 0);
        ImGui::Text("%s", s.money >= 0 ? std::to_string(s.money).c_str() : "-");
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SameLine(x0 + avail * 0.40f);
        float w = avail * 0.60f - ButtonWidth(" + ") - ButtonWidth(" - ") - ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetNextItemWidth(w);
        ImGui::InputInt("##amount", &amount, 0, 0); // the + / - buttons next to it add or remove this amount
        amount = std::clamp(amount, 1, 1000000);
        ImGui::SameLine();
        if (ImGui::Button(" + ")) feat::AddMoney(amount);
        ImGui::SameLine();
        if (ImGui::Button(" - ")) feat::AddMoney(-amount);
        ImGui::EndDisabled();
    }

    Section("GIVE ITEMS");
    Hint("Key items can break scripted sequences: save before giving them.");
    float qtyW = ImGui::GetFontSize() * 4;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - qtyW - ImGui::CalcTextSize(T("Quantity")).x - ImGui::GetStyle().ItemSpacing.x * 2);
    ImGui::InputTextWithHint("##filter", T("Search an item..."), filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", T("Quantity"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(qtyW);
    ImGui::InputInt("##qty", &giveCount, 0, 0);
    giveCount = std::clamp(giveCount, 1, 9999);
    if (s.items.empty()) ImGui::TextDisabled("%s", T(s.player ? "Loading item list..." : "Load a save first."));
    auto lower = [](std::string t) {
        for (char& c : t) c = char(std::tolower(uint8_t(c)));
        return t;
    };
    std::string needle = lower(filter);
    for (const auto& [category, list] : s.items) {
        if (!ImGui::CollapsingHeader((std::string(T(category.c_str())) + "###" + category).c_str())) continue;
        for (const feat::Named& item : list) {
            std::string label = Pretty(item.name);
            if (!needle.empty() && lower(label).find(needle) == std::string::npos) continue;
            ImGui::PushID(item.name.c_str());
            RowHover(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            SameLineRight(ButtonWidth(T("Give")));
            if (ImGui::Button(T("Give"))) feat::GiveItem(item.name, giveCount);
            ImGui::PopID();
        }
    }

    Section("COSMETICS");
    ImGui::BeginDisabled(!s.player || !s.cheatManager);
    if (ImGui::Button(T("Unlock weapon skins & charms"))) feat::GiveAllSkins();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("Check them in the weapon customisation screen."));
}

void TabWorld(const feat::Snapshot& s)
{
    Section("TIME");
    SliderRow(feat::TimeScale, "Time scale", "x%.2f", s, "Slow motion below 1, fast forward above.");
    Section("ENEMIES");
    ToggleRow(feat::FreezeAI, s);
    SliderRow(feat::KillRadius, "Kill radius", "%.0f m", s, "Range of the button below.");
    ImGui::BeginDisabled(!s.player);
    if (ImGui::Button(T("Kill enemies nearby"))) feat::KillNearby();
    ImGui::EndDisabled();
    Section("HUD");
    ToggleRow(feat::HideHUD, s);
    Section("PROMPTS");
    ToggleRow(feat::SimplePrompts, s);
    ImGui::BeginDisabled(!feat::g_on[feat::SimplePrompts]);
    {
        float av = ImGui::GetContentRegionAvail().x, xx = ImGui::GetCursorPosX();
        ControlColumn("Key colour", av, xx);
        static float col[3] = {-1, 0, 0};
        if (col[0] < 0) {
            std::string v = cfg::GetString("Prompts", "Color", "#FFFFFF");
            unsigned rgb = v.size() == 7 && v[0] == '#' ? unsigned(strtoul(v.c_str() + 1, nullptr, 16)) : 0xFFFFFF;
            col[0] = ((rgb >> 16) & 255) / 255.f, col[1] = ((rgb >> 8) & 255) / 255.f, col[2] = (rgb & 255) / 255.f;
        }
        if (ImGui::ColorEdit3("##promptcol", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha)) {
            char buf[16];
            snprintf(buf, sizeof(buf), "#%06X", theme::ToHex({col[0], col[1], col[2], 1}));
            cfg::SetString("Prompts", "Color", buf);
        }
    }
    SliderRow(feat::PromptSize, "Key size", "%.0f", s, "Font size of the key.");
    ToggleRow(feat::PromptBrackets, s);
    ImGui::EndDisabled();
    Section("LOCKS");
    ToggleRow(feat::AutoLockpick, s);
}

// FOV editor (fov.cpp), ported from TrueFOV.
void TabFov(const feat::Snapshot& s)
{
    fov::Status st = fov::GetStatus();
    Section("FIELD OF VIEW");
    ToggleRow(feat::CustomFov, s);
    SliderRow(feat::WorldFov, "World FOV", "%.0f°", s,
              "Field of view of the world during gameplay, on the game's own camera scale. Cutscenes and other cameras "
              "keep the game's FOV.");
    SliderRow(feat::ViewmodelFov, "Viewmodel FOV", "%.0f°", s,
              "Field of view of your arms and held items. Default keeps them in step with the world FOV, the way the game "
              "does, so your hands line up with the world in first-person scenes. While aiming they follow the camera so "
              "the sights stay lined up.",
              "Default");
    SliderRow(feat::AdsFov, "Aiming FOV", "%.0f°", s,
              "Field of view while aiming down sights. Default uses the game's own aiming FOV, whatever your world FOV is.",
              "Default");

    Section("BEHAVIOUR");
    ToggleRow(feat::FovKeepEffects, s);
    SliderRow(feat::FovBlend, "Transition", "%.2f s", s,
              "Blend time between your FOV and the game's when gameplay starts or stops, from instant up to 2 seconds.");

    Section("HOTKEYS");
    {
        float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
        ControlColumn("World FOV + / -", avail, x0);
        BindButton("fovup", kBindFovUp, g_fovUp);
        ImGui::SameLine();
        BindButton("fovdown", kBindFovDown, g_fovDown);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("Shift: 5° steps"));
    }
    if (int key = g_hotkey[feat::CustomFov]) {
        char line[160];
        snprintf(line, sizeof(line), T("%s turns the FOV editor on and off (change it in Settings, Hotkeys)."),
                 KeyName(unsigned(key)).c_str());
        Hint(line);
    } else {
        Hint("Bind a key in Settings, Hotkeys to turn the FOV editor on and off.");
    }

    Section("STATUS");
    char line[256];
    if (!st.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Pink);
        ImGui::TextWrapped("%s %s", T("FOV editor unavailable:"), st.error.c_str());
        ImGui::PopStyleColor();
    } else if (!st.resolved) {
        ImGui::TextDisabled("%s", T(s.reflection ? "Looking for the camera..." : "Game engine not hooked: see KetaminePlusPlus.log."));
    } else if (!st.player) {
        ImGui::TextDisabled("%s", T("Load a save to see the camera."));
    } else {
        snprintf(line, sizeof(line), "%s %s  ·  %s %s%s", T("Camera shows:"), T(st.shown.c_str()), T("hook"),
                 T(st.cameraHooked ? "on" : "off"), st.aiming ? T("  ·  aiming") : "");
        ImGui::TextUnformatted(line);
        snprintf(line, sizeof(line), "%s %.1f° → %.1f°", T("World:"), st.in, st.out);
        ImGui::TextUnformatted(line);
        if (st.viewmodelOut > 0) {
            snprintf(line, sizeof(line), "%s %.1f° → %.1f°", T("Viewmodel:"), st.viewmodelIn, st.viewmodelOut);
            ImGui::TextUnformatted(line);
        }
        std::string game = std::string(T("Game's FOV:")) + " ";
        char buf[32];
        snprintf(buf, sizeof(buf), st.gameBase > 0 ? "%.0f°" : "-", st.gameBase);
        game += buf;
        game += std::string("  ·  ") + T("game's aiming FOV:") + " ";
        snprintf(buf, sizeof(buf), st.gameAimFov > 0 ? "%.0f°" : "-", st.gameAimFov);
        game += buf;
        ImGui::TextDisabled("%s", game.c_str());
        if (!st.hookAvailable) ImGui::TextDisabled("%s", T("ApplyCameraModifiers not found: the FOV can't be changed."));
    }
    if (st.lockedFov > 0) {
        snprintf(line, sizeof(line), T("The console 'fov %.0f' command overrides every camera."), st.lockedFov);
        ImGui::PushStyleColor(ImGuiCol_Text, Pink);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
        SameLineRight(ButtonWidth(T("Clear")));
        if (ImGui::Button(T("Clear"))) fov::ClearLockedFov();
    }
}

// Minimap (minimap.cpp), ported from True Minimap (Nexus Mods 39) with its performance problems fixed.
void TabMinimap(const feat::Snapshot& s)
{
    Section("MINIMAP");
    ToggleRow(feat::Minimap, s);
    ImGui::BeginDisabled(!feat::g_on[feat::Minimap]);
    {
        float av = ImGui::GetContentRegionAvail().x, xx = ImGui::GetCursorPosX();
        ControlColumn("Shape", av, xx);
        int shape = std::clamp(int(feat::g_value[feat::MinimapShape]), 0, minimap::ShapeCount - 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::BeginCombo("##shape", T(minimap::kShapeNames[shape]))) {
            for (int i = 0; i < minimap::ShapeCount; i++)
                if (ImGui::Selectable(T(minimap::kShapeNames[i]), i == shape)) {
                    feat::g_value[feat::MinimapShape] = float(i);
                    feat::Save();
                }
            ImGui::EndCombo();
        }
    }
    ToggleRow(feat::MinimapRotate, s);
    SliderRow(feat::MinimapSize, "Size", "%.0f px", s, "Side of the minimap, in UI pixels.");
    SliderRow(feat::MinimapZoom, "Zoom", "x%.1f", s, "Higher shows less of the map, larger.");
    SliderRow(feat::MinimapOpacity, "Transparency", "%.0f%%", s, "How see-through the map is.");
    Section("PLACEMENT");
    ToggleRow(feat::MinimapScreenEdge, s);
    SliderRow(feat::MinimapOffsetX, "Offset X", "%.0f", s, "Moves the minimap left (negative) or right.");
    SliderRow(feat::MinimapOffsetY, "Offset Y", "%.0f", s, "Moves the minimap up (negative) or down.");
    ToggleRow(feat::MinimapFrame, s);
    ToggleRow(feat::MinimapAlways, s);
    ImGui::EndDisabled();
    Section("STATUS");
    ImGui::TextDisabled("%s", T(minimap::Status()));
    Hint("Ported from True Minimap by Thidrec (Nexus Mods 39), rebuilt to avoid its stutter: no object scans 36 times a second, no "
         "forced map rebuild every 5 s, and the map moves without re-laying it out. Remove the UE4SS version if you use this.");
}

// Game fixes (fixes.cpp), ported from the UE4SS mods 42-46 on Nexus Mods: no UE4SS needed.
void TabFixes(const feat::Snapshot& s)
{
    { // advisory card: these patch the game's own bugs, which an update may fix
        float sc = S(), w = ImGui::GetContentRegionAvail().x;
        const char* note = T("These fix problems in the game itself. A future game update may fix them too: if one starts "
                             "behaving oddly after an update, turn it off, the game may not need it anymore.");
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::CalcTextSize(note, nullptr, false, w - 30 * sc);
        ImVec2 b{p.x + w, p.y + size.y + 20 * sc};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, b, U32(Alpha(Violet, 0.12f)), R(8 * sc));
        dl->AddRectFilled(p, {p.x + 3 * sc, b.y}, U32(Violet), R(8 * sc), ImDrawFlags_RoundCornersLeft);
        ImGui::SetCursorScreenPos({p.x + 16 * sc, p.y + 10 * sc});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 30 * sc);
        ImGui::TextUnformatted(note);
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos({p.x, b.y + 6 * sc});
        ImGui::Dummy({0, 0});
    }
    Section("CONTROLS");
    ToggleRow(feat::FixToolInput, s);
    ToggleRow(feat::KeypadFix, s);
    Section("INTERACTIONS");
    ToggleRow(feat::QuickInteract, s);
    ImGui::BeginDisabled(!feat::g_on[feat::QuickInteract]);
    SliderRow(feat::InteractHold, "Hold time", "%.2f s", s,
              "How long a pickup or ladder hold takes once it starts (the game's is about 1 s). Never made slower.");
    SliderRow(feat::LadderSpeed, "Ladder climb speed", "x%.1f", s, "Speed of the ladder animations (getting on, climbing, off).");
    ImGui::EndDisabled();
    ToggleRow(feat::SpeedUp, s);
    ImGui::BeginDisabled(!feat::g_on[feat::SpeedUp]);
    const char* off = "1 turns this one off.";
    SliderRow(feat::SaveSpeed, "Save point", "x%.1f", s, off);
    SliderRow(feat::ConfigSpeed, "Lament Configuration puzzle", "x%.1f", s, off);
    SliderRow(feat::PillarSpeed, "Reward pillars", "x%.1f", s, off);
    SliderRow(feat::CylinderSpeed, "Cylinder puzzles", "x%.1f", s, off);
    SliderRow(feat::CylinderInputSpeed, "Cylinder puzzle input", "x%.1f", s, "Shorter lockout after each turn. 1 turns this one off.");
    SliderRow(feat::BoatSpeed, "Dock boat (episode 3)", "x%.1f", s, off);
    ImGui::EndDisabled();
    if (feat::g_on[feat::SpeedUp] && s.reflection && !fixes::EditsAvailable())
        Hint("This game build's script tables weren't found (see the log), so the save point and Lament puzzle stay at "
             "normal speed: only their animations could be sped up, which would put them out of step. The others work.");
    Section("HUD");
    ToggleRow(feat::HudAutoHide, s);
    ImGui::BeginDisabled(!feat::g_on[feat::HudAutoHide]);
    SliderRow(feat::HudIdleDelay, "Idle delay", "%.0f s", s, "How long most of the HUD must stay faded before the rest fades.");
    ImGui::EndDisabled();
    Section("CUTSCENES");
    ToggleRow(feat::CutsceneSkip, s);
    Hint("Unskippable cutscenes are left alone. Skipping uses the game's own skip, so story progress and rewards are "
         "the same as skipping by hand.");
    ImGui::Dummy({0, 6 * S()});
    Hint("Ported from uwumatic101's UE4SS mods ToolInputFix, KeypadInputFix, SpeedUpInteractions, QuickInteract and "
         "CutsceneAutoSkip (Nexus Mods 42-46). Remove those mods from ue4ss\\Mods if you use these, so they don't run twice.");
}

void TabProgress(const feat::Snapshot& s)
{
    { // warning card
        float sc = S(), w = ImGui::GetContentRegionAvail().x;
        const char* warn = T("Save your game before changing objectives: skipping steps can break scripted sequences.");
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::CalcTextSize(warn, nullptr, false, w - 30 * sc);
        ImVec2 b{p.x + w, p.y + size.y + 20 * sc};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, b, U32(Alpha(Violet, 0.12f)), R(8 * sc));
        dl->AddRectFilled(p, {p.x + 3 * sc, b.y}, U32(Violet), R(8 * sc), ImDrawFlags_RoundCornersLeft);
        ImGui::SetCursorScreenPos({p.x + 16 * sc, p.y + 10 * sc});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 30 * sc);
        ImGui::TextUnformatted(warn);
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos({p.x, b.y + 6 * sc});
        ImGui::Dummy({0, 0});
    }

    Section("CURRENT OBJECTIVES");
    if (s.objectivesActive.empty()) ImGui::TextDisabled("%s", T(s.player ? "No active objective found." : "Load a save first."));
    for (const feat::Named& o : s.objectivesActive) {
        ImGui::PushID(o.name.c_str());
        RowHover(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
        bool tracked = o.name == s.tracked;
        ImGui::AlignTextToFramePadding();
        if (tracked) ImGui::PushStyleColor(ImGuiCol_Text, Pink);
        ImGui::TextUnformatted(Pretty(o.name).c_str());
        if (tracked) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s%s%s", o.name.c_str(), tracked ? "  " : "", tracked ? T("(tracked)") : "");
        SameLineRight(ButtonWidth(T("Complete")));
        if (ImGui::Button(T("Complete"))) feat::CompleteObjective(o.fname);
        ImGui::PopID();
    }
    if (!s.objectivesDone.empty() && ImGui::CollapsingHeader((std::string(T("Completed objectives")) + "###done").c_str())) {
        for (const feat::Named& o : s.objectivesDone) {
            ImGui::TextDisabled("%s", Pretty(o.name).c_str());
            ImGui::SetItemTooltip("%s", o.name.c_str());
        }
    }

    Section("SHOP");
    static int level = 0;
    ImGui::BeginDisabled(s.shopLevel < 0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Shop level"));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Pink);
    ImGui::Text("%s", s.shopLevel >= 0 ? std::to_string(s.shopLevel).c_str() : "-");
    ImGui::PopStyleColor();
    SameLineRight(ImGui::GetFontSize() * 7 + ImGui::GetStyle().ItemSpacing.x + ButtonWidth(T("Set")));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::InputInt("##level", &level);
    level = std::clamp(level, 0, 99);
    ImGui::SameLine();
    if (ImGui::Button(T("Set"))) feat::SetShopLevel(level);
    ImGui::EndDisabled();
}

void BindButton(const char* id, int target, unsigned key)
{
    ImGui::PushID(id);
    bool waiting = g_binding == target;
    std::string label = waiting ? T("Press a key...") : KeyName(key);
    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, Alpha(Violet, 0.4f));
    if (ImGui::Button(label.c_str(), {ImGui::GetFontSize() * 8, 0})) g_binding = waiting ? kNotBinding : target;
    if (waiting) ImGui::PopStyleColor();
    ImGui::PopID();
}

void TabSettings(const feat::Snapshot& s)
{
    Section("MENU");
    float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    ControlColumn("Menu keys", avail, x0);
    BindButton("menukey", kBindMenu, g_menuKey);
    ImGui::SameLine();
    BindButton("menukey2", kBindMenu2, g_menuKey2);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("Esc cancels"));

    ControlColumn("Language", avail, x0);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::BeginCombo("##lang", lang::kLangs[lang::Get()].name)) {
        for (int i = 0; i < lang::Count; i++)
            if (ImGui::Selectable(lang::kLangs[i].name, i == lang::Get())) {
                lang::Set(i);
                cfg::SetString("Menu", "Language", lang::kLangs[i].code);
            }
        ImGui::EndCombo();
    }

    // Buttons rather than a slider: dragging a slider that rescales the window under the mouse runs away.
    ControlColumn("UI scale", avail, x0);
    float current = g_scale > 0 ? g_scale : g_appliedScale;
    auto setScale = [](float v) {
        g_scale = v > 0 ? std::clamp(std::round(v * 10) / 10, 0.7f, 2.5f) : 0;
        cfg::SetFloat("Menu", "Scale", g_scale);
    };
    if (ImGui::Button(" - ")) setScale(current - 0.1f);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.0f%%", current * 100);
    ImGui::SameLine();
    if (ImGui::Button(" + ")) setScale(current + 0.1f);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_scale <= 0);
    if (ImGui::Button(T("Auto"))) setScale(0);
    ImGui::EndDisabled();

    Section("SKIN");
    {
        float av = ImGui::GetContentRegionAvail().x, xx = ImGui::GetCursorPosX();
        ControlColumn("Skin", av, xx);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        if (ImGui::BeginCombo("##skin", theme::kSkins[g_skin].name)) {
            for (int i = 0; i < theme::kSkinCount; i++) {
                ImVec4 sw = theme::Hex(i == theme::kCustomSkin ? g_accent : theme::kSkins[i].accent);
                ImGui::ColorButton(("##sw" + std::to_string(i)).c_str(), sw, ImGuiColorEditFlags_NoTooltip,
                                   {ImGui::GetFontSize(), ImGui::GetFontSize()});
                ImGui::SameLine();
                if (ImGui::Selectable(theme::kSkins[i].name, i == g_skin)) SetSkin(i);
            }
            ImGui::EndCombo();
        }
        // Test hosts: KM_TEST_SKIN changes the skin every 60 frames right here, mid-window, as picking one does.
        static const bool testSkin = GetEnvironmentVariableA("KM_TEST_SKIN", nullptr, 0) != 0;
        if (testSkin && ImGui::GetFrameCount() % 60 == 0) {
            SetSkin((g_skin + 1) % theme::kSkinCount);
            km::Log("Test: skin -> %s", theme::kSkins[g_skin].name);
        }
        ControlColumn("Accent / highlight", av, xx);
        float acc[3] = {Violet.x, Violet.y, Violet.z}, hi[3] = {Pink.x, Pink.y, Pink.z};
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha;
        bool edited = ImGui::ColorEdit3("##accent", acc, flags);
        ImGui::SameLine();
        edited |= ImGui::ColorEdit3("##highlight", hi, flags);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("Editing switches to Custom"));
        static bool picking = false; // live while dragging in the picker, saved once when it stops
        if (edited) {
            g_accent = theme::ToHex({acc[0], acc[1], acc[2], 1});
            g_highlight = theme::ToHex({hi[0], hi[1], hi[2], 1});
            SetSkin(theme::kCustomSkin, false);
            picking = true;
        } else if (picking) {
            picking = false;
            SetSkin(theme::kCustomSkin);
        }
        ControlColumn("Window opacity", av, xx);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        float op = theme::Opacity;
        if (ImGui::SliderFloat("##opacity", &op, 0.6f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            theme::Opacity = op;
            g_themeDirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) cfg::SetFloat("Menu", "Opacity", theme::Opacity);
    }

    Section("GAME");
    ToggleRow(feat::SkipIntros, s);
    ToggleRow(feat::PauseMenuEntry, s);

    Section("HOTKEYS");
    Hint("They work with the menu closed. Click a key, then press the new one. Backspace clears, Esc cancels.");
    if (ImGui::BeginTable("hotkeys", 2, ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed);
        for (int t = 0; t < feat::ToggleCount; t++) {
            if (t == feat::SkipIntros) continue; // only matters at startup
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s%s%s", t > feat::Esp ? T("ESP") : "", t > feat::Esp ? "  ·  " : "", T(feat::kToggles[t].label));
            ImGui::TableNextColumn();
            BindButton(feat::kToggles[t].key, t, unsigned(g_hotkey[t].load()));
        }
        ImGui::EndTable();
    }

    Section("CREDITS");
    {
        static const char* kCredits[][2] = {
            {"rams95", "KETAMINE++: bundled it all together, with the added features (github.com/rams95blue)"},
            {"omxga (literalynn)", "Ketamine Menu, the trainer KETAMINE++ is built on"},
            {"Reynbow", "TrueFOV, the FOV editor"},
            {"uwumatic101", "ToolInputFix, KeypadInputFix, SpeedUpInteractions, QuickInteract, CutsceneAutoSkip"},
            {"Thidrec", "Hellraiser True Minimap"},
            {"Omar Cornut", "Dear ImGui"},
            {"Tsuda Kageyu", "MinHook"},
            {"Rasmus Andersson", "Inter font"},
        };
        for (auto& c : kCredits) {
            ImGui::PushFont(bold, 0);
            ImGui::TextColored(Violet, "%s", c[0]);
            ImGui::PopFont();
            ImGui::SameLine(ImGui::GetFontSize() * 11);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", c[1]);
            ImGui::PopTextWrapPos();
        }
        Hint("Full notices and links: THIRD-PARTY-NOTICES.txt and KETAMINE++.md, next to the DLL.");
    }

    Section("DIAGNOSTICS");
    if (ImGui::Button(T("Dump on-screen UI to the log"))) fixes::DumpUi();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("Lists every widget on screen: send the log to help add UI features."));

    Section("CONFIGURATION");
    if (ImGui::Button(T("Reset configuration"))) ImGui::OpenPopup("###reset");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("KetaminePlusPlus.ini and KetaminePlusPlus.log sit next to the DLL."));
    if (ImGui::BeginPopupModal((std::string(T("Reset?")) + "###reset").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("Turn every option off and forget hotkeys, positions and settings?"));
        if (ImGui::Button(T("Reset"))) {
            cfg::Reset();
            cfg::SetString("Menu", "Language", lang::kLangs[lang::Get()].code); // keep the language the menu shows
            for (int t = 0; t < feat::ToggleCount; t++) feat::g_on[t] = feat::kToggles[t].def;
            for (int v = 0; v < feat::ValueCount; v++) feat::g_value[v] = feat::kValues[v].def;
            for (int t = 0; t < feat::ToggleCount; t++) g_hotkey[t] = DefaultHotkey(t);
            g_fovUp = VK_PRIOR;
            g_fovDown = VK_NEXT;
            jump::g_key = VK_SPACE;
            g_menuKey = VK_F1;
            g_menuKey2 = VK_F8;
            g_scale = 1.f;
            g_accent = 0x1EF8FF, g_highlight = 0xECECEC, theme::Opacity = 1.f;
            SetSkin(DefaultSkin());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- frame

// The mouse cursor while the menu is open, drawn on top of everything, in the skin's look: the Windows 9x arrow
// (pixel art) for retro skins, the same arrow in the accent colour for terminal skins, a smooth arrow otherwise.
void DrawCursor()
{
    ImGuiIO& io = ImGui::GetIO();
    if (!g_open || !ImGui::IsMousePosValid()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 m = io.MousePos;
    float s = std::max(1.f, S());
    if (Style == StyleModern) {
        const ImVec2 shape[] = {{0, 0}, {0, 17}, {4.2f, 13.2f}, {7.2f, 20}, {10, 18.8f}, {7.1f, 12.3f}, {12.4f, 12.3f}};
        ImVec2 pts[7];
        for (int i = 0; i < 7; i++) pts[i] = {m.x + shape[i].x * s, m.y + shape[i].y * s};
        dl->AddConcavePolyFilled(pts, 7, Raw(Text));
        dl->AddPolyline(pts, 7, Raw(Alpha(Bg, 0.9f)), ImDrawFlags_Closed, 1.3f * s);
        dl->AddCircleFilled({m.x + 3.2f * s, m.y + 9 * s}, 1.6f * s, Raw(Violet), 10);
        return;
    }
    static const char* kArrow[] = {
        "B",          "BB",          "BWB",         "BWWB",         "BWWWB",      "BWWWWB",      "BWWWWWB",
        "BWWWWWWB",   "BWWWWWWWB",   "BWWWWWWWWB",  "BWWWWWBBBBB",  "BWWBWWB",    "BWB BWWB",    "BB  BWWB",
        "B    BWWB",  "     BWWB",   "      BWWB",  "      BWWB",   "       BB",
    };
    float px = std::round(1.5f * s); // chunky pixels
    ImU32 outline = Style == StyleRetro ? IM_COL32(0, 0, 0, 255) : Raw(Violet);
    ImU32 fill = Style == StyleRetro ? IM_COL32(255, 255, 255, 255) : Raw(Bg);
    float x0 = std::floor(m.x), y0 = std::floor(m.y);
    for (int r = 0; r < int(sizeof(kArrow) / sizeof(kArrow[0])); r++)
        for (int c = 0; kArrow[r][c]; c++) {
            char ch = kArrow[r][c];
            if (ch == ' ') continue;
            ImVec2 a{x0 + c * px, y0 + r * px};
            dl->AddRectFilled(a, {a.x + px, a.y + px}, ch == 'B' ? outline : fill);
        }
}

// The minimap's frame: an outline of its shape in the skin's accent, with a dark edge, drawn over the game.
void DrawMinimapFrame()
{
    float x, y, size;
    int shape;
    if (!minimap::FrameRect(x, y, size, shape)) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float s = S(), cx = x + size * 0.5f, cy = y + size * 0.5f, half = size * 0.5f;
    std::vector<ImVec2> pts;
    auto ring = [&](int n, float start, float radius) {
        for (int i = 0; i < n; i++) {
            float a = start + float(i) * 6.2831853f / float(n);
            pts.push_back({cx + std::cos(a) * radius, cy + std::sin(a) * radius});
        }
    };
    switch (shape) {
    case minimap::Circle: ring(64, 0, half); break;
    case minimap::Octagon: ring(8, 3.14159265f / 8, half / std::cos(3.14159265f / 8)); break; // flat top, apothem = half
    case minimap::Diamond: ring(4, 0, half); break;
    default: pts = {{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}; break;
    }
    dl->AddPolyline(pts.data(), int(pts.size()), Raw(Alpha(Black, 0.6f)), ImDrawFlags_Closed, 5 * s);
    dl->AddPolyline(pts.data(), int(pts.size()), Raw(Violet), ImDrawFlags_Closed, 2.2f * s);
}

// Under the crosshair, below the hop readout: the live horizontal speed.
void DrawSpeedometer()
{
    bool valid = false;
    double v = jump::LiveSpeed(&valid);
    if (!feat::g_on[feat::Speedometer] || !valid) return;
    float s = S(), size = kBaseFont * 0.95f * s;
    std::string text = FormatSpeed(v);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = bold->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    ImVec2 p{ImGui::GetIO().DisplaySize.x * 0.5f - ts.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f + 120 * s};
    dl->AddText(bold, size, {p.x + 1 * s, p.y + 1 * s}, Raw(Alpha(Black, 0.7f)), text.c_str());
    dl->AddText(bold, size, p, Raw(Alpha(Text, 0.92f)), text.c_str());
}

// Below the crosshair, fading out 1.4 s after each hop.
void DrawHopHud()
{
    if (!feat::g_on[feat::Jump] || !feat::g_on[feat::Bhop] || !feat::g_on[feat::BhopHud]) return;
    jump::HudInfo h = jump::Hud();
    double age = PreciseNow() - h.at;
    if (!h.grade[0] || age < 0 || age > 1.4) return;
    float a = float(std::clamp((1.4 - age) / 0.4, 0.0, 1.0)), s = S();
    char top[64], bottom[64];
    bool late = h.grade[0] == 'L';
    if (late) snprintf(top, sizeof(top), "%s", T("LATE"));
    else snprintf(top, sizeof(top), "%s  +%d%%", T(h.grade), h.gain);
    snprintf(bottom, sizeof(bottom), T("hop %d  ·  %s"), h.hops, FormatSpeed(h.speed).c_str());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 c{ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f + 60 * s};
    float big = kBaseFont * 1.1f * s, small = kBaseFont * 0.8f * s;
    ImVec2 ts = bold->CalcTextSizeA(big, FLT_MAX, 0, top), bs = regular->CalcTextSizeA(small, FLT_MAX, 0, bottom);
    ImVec4 col = late ? Muted : h.grade[0] == 'P' ? Pink : Violet;
    auto shadow = [&](ImFont* f, float size, ImVec2 p, ImVec4 c4, const char* t) {
        dl->AddText(f, size, {p.x + 1 * s, p.y + 1 * s}, Raw(Alpha(Black, 0.7f * a)), t);
        dl->AddText(f, size, p, Raw(Alpha(c4, a)), t);
    };
    shadow(bold, big, {c.x - ts.x * 0.5f, c.y}, col, top);
    shadow(regular, small, {c.x - bs.x * 0.5f, c.y + ts.y + 2 * s}, Text, bottom);
}

void DrawToasts()
{
    std::lock_guard lock(g_toastMutex);
    double now = Now();
    while (!g_toasts.empty() && now - g_toasts.front().start > kToastLife) g_toasts.pop_front();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float s = S(), size = kBaseFont * s, pad = 12 * s, y = 24 * s, right = ImGui::GetIO().DisplaySize.x - 24 * s;
    for (const Toast& t : g_toasts) {
        double age = now - t.start;
        float alpha = float(std::clamp(std::min(age / 0.15, (kToastLife - age) / 0.35), 0.0, 1.0));
        const char* state = t.state < 0 ? "" : T(t.state ? "ON" : "OFF");
        ImVec2 ts = regular->CalcTextSizeA(size, FLT_MAX, 0, t.text.c_str()), ss = bold->CalcTextSizeA(size, FLT_MAX, 0, state);
        float w = pad * 2 + 4 * s + ts.x + (t.state < 0 ? 0 : ss.x + 12 * s), h = ts.y + pad * 1.4f;
        ImVec2 a{right - w, y}, b{right, y + h};
        dl->AddRectFilled(a, b, Raw(Alpha(Panel, 0.97f * alpha)), R(8 * s));
        dl->AddRect(a, b, Raw(Alpha(Border, alpha)), R(8 * s));
        dl->AddRectFilled(a, {a.x + 4 * s, b.y}, Raw(Alpha(Violet, alpha)), R(8 * s), ImDrawFlags_RoundCornersLeft);
        ImVec2 tp{a.x + pad + 4 * s, a.y + (h - ts.y) * 0.5f};
        dl->AddText(regular, size, tp, Raw(Alpha(Text, alpha)), t.text.c_str());
        if (t.state >= 0)
            dl->AddText(bold, size, {tp.x + ts.x + 12 * s, tp.y}, Raw(Alpha(t.state ? Pink : Muted, alpha)), state);
        y = b.y + 8 * s;
    }
}

void Header(const feat::Snapshot& snap)
{
    float s = S();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(bold, kBaseFont * 1.45f);
    if (Style == StyleRetro) { // a Windows 98 title bar: accent to highlight gradient, white title
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        float th = ImGui::GetTextLineHeight();
        ImVec2 a{wp.x + 4 * s, wp.y + 4 * s}, b{wp.x + ws.x - 4 * s, p.y + th + 4 * s};
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(a, b, Raw(Violet), Raw(Pink), Raw(Pink), Raw(Violet));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
        ImGui::TextUnformatted("KETAMINE++");
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 4 * s});
    } else if (Style == StyleTerminal) { // a shell prompt with a blinking cursor
        bool on = std::fmod(ImGui::GetTime(), 1.0) < 0.55;
        ImGui::PushStyleColor(ImGuiCol_Text, Violet);
        ImGui::Text("root@ketamine:~# KETAMINE++%s", on ? "_" : " ");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted("KETAMINE++");
    }
    ImGui::PopFont();
    ImGui::TextDisabled("v%s  ·  %s  ·  Hellraiser: Revival  ·  bundled by rams95", KM_VERSION, overlay::ApiName());

    // Status pill on the right of the title.
    const char* status = T(!snap.reflection ? "Engine not hooked" : snap.player ? "Player found" : "Load a save");
    float size = kBaseFont * 0.85f * s;
    ImVec2 ts = regular->CalcTextSizeA(size, FLT_MAX, 0, status);
    float h = ts.y + 12 * s, w = ts.x + 30 * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Close button, top right: X.
    {
        ImVec2 cmin{p.x + avail - h, p.y + 4 * s}, cmax{cmin.x + h, cmin.y + h};
        ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(cmin);
        if (ImGui::InvisibleButton("##close", {h, h})) {
            g_open = false;
            km::Log("Menu closed (X)");
        }
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s", T("Close"));
        ImGui::SetCursorScreenPos(back);
        dl->AddRectFilled(cmin, cmax, Raw(hov ? Alpha(Pink, 0.85f) : Panel), R(h * 0.5f));
        dl->AddRect(cmin, cmax, Raw(Border), R(h * 0.5f));
        float k = h * 0.28f;
        ImVec2 c{(cmin.x + cmax.x) * 0.5f, (cmin.y + cmax.y) * 0.5f};
        ImU32 xc = Raw(hov ? Bg : Text);
        dl->AddLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, xc, 1.8f * s);
        dl->AddLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, xc, 1.8f * s);
        avail -= h + 8 * s;
    }
    ImVec2 a{p.x + avail - w, p.y + 4 * s}, b{a.x + w, a.y + h};
    dl->AddRectFilled(a, b, Raw(Panel), R(h * 0.5f));
    dl->AddRect(a, b, Raw(Border), R(h * 0.5f));
    ImVec4 dot = !snap.reflection ? Muted : snap.player ? Violet : Pink;
    dl->AddCircleFilled({a.x + 13 * s, a.y + h * 0.5f}, 3.5f * s, Raw(dot), 12);
    dl->AddText(regular, size, {a.x + 22 * s, a.y + 6 * s}, Raw(Text), status);
    ImGui::Dummy({0, 4 * s});
}

void Sidebar()
{
    float s = S();
    ImGui::BeginChild("##nav", {168 * s, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 3 * s});
    for (int i = 0; i < kTabCount; i++) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() * 1.2f;
        if (ImGui::InvisibleButton(kTabs[i], {w, h}) && g_tab != i) {
            g_tab = i;
            cfg::SetInt("Menu", "Tab", i);
        }
        bool active = g_tab == i, hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (active || hovered)
            dl->AddRectFilled(p, {p.x + w, p.y + h}, Raw(active ? Alpha(Violet, 0.16f) : Panel2), R(7 * s));
        if (active) dl->AddRectFilled({p.x, p.y + h * 0.25f}, {p.x + 3 * s, p.y + h * 0.75f}, Raw(Violet), R(2 * s));
        const char* name = T(kTabs[i]);
        ImVec2 ts = ImGui::CalcTextSize(name);
        dl->AddText(active ? bold : regular, kBaseFont * s, {p.x + 14 * s, p.y + (h - ts.y) * 0.5f}, Raw(active ? Text : Muted), name);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// Spec line: GAME 1.1.1.227384   CPU Ryzen 7 9800X3D   GPU GeForce RTX 5090 32 GB   RAM 64 GB   DISPLAY 2560 × 1440   FPS 144
void Footer()
{
    float s = S(), size = kBaseFont * 0.78f * s, labelSize = kBaseFont * 0.66f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    ImGuiIO& io = ImGui::GetIO();

    std::string gpu = overlay::GpuName();
    for (size_t at; (at = gpu.find("  ·  ")) != std::string::npos;) gpu.replace(at, 5, " ");
    gpu = ShortHardware(gpu);
    char display[32], fps[16];
    snprintf(display, sizeof(display), "%.0f × %.0f", io.DisplaySize.x, io.DisplaySize.y);
    snprintf(fps, sizeof(fps), "%.0f", io.Framerate);
    const std::pair<const char*, std::string> items[] = {
        {"GAME", g_game}, {"CPU", g_cpu}, {"GPU", gpu}, {"RAM", g_ram}, {"DISPLAY", display}, {"FPS", fps}};

    std::string keys = KeyName(g_menuKey) + (g_menuKey2 ? " / " + KeyName(g_menuKey2) : "");
    char hintBuf[128];
    snprintf(hintBuf, sizeof(hintBuf), T("%s to close"), keys.c_str());
    std::string hint = hintBuf;
    ImVec2 hs = regular->CalcTextSizeA(size, FLT_MAX, 0, hint.c_str());
    float x = p.x, right = p.x + avail - hs.x - 20 * s, labelY = p.y + (size - labelSize) * 0.72f; // baseline-ish alignment with the values
    dl->PushClipRect(p, {right, p.y + size * 2}, true);
    for (const auto& [label, value] : items) {
        if (value.empty()) continue;
        dl->AddText(bold, labelSize, {x, labelY}, Raw(Muted), label);
        x += bold->CalcTextSizeA(labelSize, FLT_MAX, 0, label).x + 5 * s;
        dl->AddText(regular, size, {x, p.y}, Raw(Alpha(Text, 0.82f)), value.c_str());
        x += regular->CalcTextSizeA(size, FLT_MAX, 0, value.c_str()).x + 16 * s;
    }
    dl->PopClipRect();
    dl->AddText(regular, size, {p.x + avail - hs.x, p.y}, Raw(Muted), hint.c_str());
    ImGui::Dummy({avail, size});
}

void DrawWindow()
{
    feat::Snapshot snap = feat::GetSnapshot();
    float s = S();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize({880 * s, 620 * s}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::Begin("KETAMINE++", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    Header(snap);
    float footerH = kBaseFont * 0.78f * s + ImGui::GetStyle().ItemSpacing.y * 2 + 4 * s;
    ImGui::BeginChild("##body", {0, -footerH}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    Sidebar();
    ImGui::SameLine(0, 14 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2 * s, 0}); // keeps switches clear of the scrollbar
    ImGui::BeginChild("##content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    { // test host: KM_TEST_SCROLL=<fraction 0..1> scrolls the tab (screenshots of long tabs)
        static char env[16] = {};
        static const bool test = GetEnvironmentVariableA("KM_TEST_SCROLL", env, sizeof(env)) != 0;
        if (test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * std::clamp(float(atof(env)), 0.f, 1.f));
    }
    ImGui::PopStyleVar();
    switch (g_tab) {
    case 0: TabPlayer(snap); break;
    case 1: TabCombat(snap); break;
    case 2: TabMovement(snap); break;
    case 3: TabEsp(snap); break;
    case 4: TabInventory(snap); break;
    case 5: TabWorld(snap); break;
    case 6: TabFov(snap); break;
    case 7: TabMinimap(snap); break;
    case 8: TabFixes(snap); break;
    case 9: TabProgress(snap); break;
    default: TabSettings(snap); break;
    }
    ImGui::Dummy({0, 6 * s});
    ImGui::EndChild();
    ImGui::EndChild();
    Footer();
    ImGui::End();
}

} // namespace

void Init(void* dllModule, const std::string& gameVersion)
{
    g_module = static_cast<HMODULE>(dllModule);
    g_game = gameVersion.substr(0, gameVersion.find('_')); // "1.0.1.227219_HellraiserGame_Shipping..."
    if (g_game.empty() && GetEnvironmentVariableA("KM_PREVIEW", nullptr, 0)) g_game = "1.1.1.227384"; // test host
    g_menuKey = unsigned(cfg::GetInt("Menu", "Key", VK_F1));
    g_menuKey2 = unsigned(cfg::GetInt("Menu", "Key2", VK_F8));
    int language = lang::FromCode(cfg::GetString("Menu", "Language").c_str());
    lang::Set(language >= 0 ? language : lang::FromSystem()); // first run: the Windows display language
    g_scale = cfg::GetFloat("Menu", "Scale", 1.f);
    if (g_scale > 0) g_scale = std::clamp(g_scale, 0.7f, 2.5f);
    g_tab = std::clamp(cfg::GetInt("Menu", "Tab", 0), 0, kTabCount - 1);
    {
        std::string skin = cfg::GetString("Menu", "Skin", "Win95");
        for (int i = 0; i < theme::kSkinCount; i++)
            if (_stricmp(skin.c_str(), theme::kSkins[i].name) == 0) g_skin = i;
        auto hex = [](const std::string& v, unsigned def) {
            return v.size() == 7 && v[0] == '#' ? unsigned(strtoul(v.c_str() + 1, nullptr, 16)) : def;
        };
        g_accent = hex(cfg::GetString("Menu", "Accent"), g_accent);
        g_highlight = hex(cfg::GetString("Menu", "Highlight"), g_highlight);
        theme::Opacity = std::clamp(cfg::GetFloat("Menu", "Opacity", 1.f), 0.6f, 1.f);
        theme::ApplySkin(g_skin, g_accent, g_highlight);
    }
    for (int t = 0; t < feat::ToggleCount; t++) {
        g_hotkey[t] = cfg::GetInt("Hotkeys", feat::kToggles[t].key, DefaultHotkey(t));
        if (g_hotkey[t] == int(g_menuKey2)) g_menuKey2 = 0; // F8 already bound to a cheat before 0.4.8: the cheat keeps it
    }
    jump::g_key = unsigned(cfg::GetInt("Hotkeys", "JumpKey", VK_SPACE));
    g_fovUp = unsigned(cfg::GetInt("Hotkeys", "FovUp", VK_PRIOR));
    g_fovDown = unsigned(cfg::GetInt("Hotkeys", "FovDown", VK_NEXT));
    ReadHardware();
    feat::Init();
}

// A Windows font, the first of the files that exists (the retro and terminal skins use the fonts Windows ships).
ImFont* SystemFont(std::initializer_list<const wchar_t*> files)
{
    for (const wchar_t* f : files) {
        int size = 0;
        void* data = MapFont(f, size);
        if (!data) continue;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false; // the mapped view stays for the process lifetime
        if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, size, kBaseFont, &cfg)) return font;
    }
    return nullptr;
}

void SetupImGui()
{
    regular = LoadFont(101); // first font added = default font
    bold = LoadFont(102);
    if (!regular) {
        km::Log("Embedded font missing, using ImGui's default");
        regular = ImGui::GetIO().Fonts->AddFontDefault();
    }
    if (!bold) bold = regular;
    MergeScriptFonts();
    g_fonts[StyleModern] = {regular, bold};
    // Retro: Tahoma (Windows 2000 / XP's dialog font, the closest TrueType to MS Sans Serif). Terminal: Consolas.
    FontPair retro{SystemFont({L"tahoma.ttf", L"micross.ttf", L"arial.ttf"}), SystemFont({L"tahomabd.ttf", L"arialbd.ttf"})};
    FontPair term{SystemFont({L"consola.ttf", L"lucon.ttf", L"cour.ttf"}), SystemFont({L"consolab.ttf", L"courbd.ttf"})};
    for (FontPair* f : {&retro, &term}) {
        if (!f->regular) continue;
        if (!f->bold) f->bold = f->regular;
        regular = f->regular, bold = f->bold;
        MergeScriptFonts(); // Chinese, Japanese and Korean in these too
    }
    g_fonts[StyleRetro] = retro;
    g_fonts[StyleTerminal] = term;
    km::Log("Fonts: retro skins %s, terminal skins %s", retro.regular ? "Tahoma" : "Inter (Tahoma not found)",
            term.regular ? "Consolas" : "Inter (Consolas not found)");
    regular = g_fonts[StyleModern].regular, bold = g_fonts[StyleModern].bold;
}

void Draw()
{
    float scale = g_scale > 0 ? g_scale : std::clamp(ImGui::GetIO().DisplaySize.y / 1080.f, 1.f, 2.f);
    if (scale != g_appliedScale || g_themeDirty) { // before anything is drawn this frame
        ApplyTheme(scale);
        g_appliedScale = scale;
        g_themeDirty = false;
    }
    ImGui::GetIO().MouseDrawCursor = false; // our own cursor, drawn last (DrawCursor), so it's always on top
    { // warn once each time the jump key turns out to be one of the game's keys
        static std::string warned;
        std::string clash = feat::g_on[feat::Jump] ? jump::Conflict() : "";
        if (feat::g_on[feat::Jump] && jump::g_key == VK_SPACE && clash.empty()) clash = "Evade (by default)";
        if (clash != warned) {
            warned = clash;
            if (!clash.empty()) {
                char line[256];
                snprintf(line, sizeof(line), T("Jump key %s is also the game's %s"), KeyName(jump::g_key).c_str(), clash.c_str());
                PushToast(line, -1);
            }
        }
    }
    esp::Draw();
    DrawMinimapFrame();
    DrawHopHud();
    DrawSpeedometer();
    if (g_open) DrawWindow();
    DrawToasts();
    DrawCursor();
}

bool IsOpen() { return g_open; }

void Open()
{
    if (g_open) return;
    g_open = true;
    ClipCursor(nullptr);
    km::Log("Menu opened from the game's pause menu");
}

bool WantsFrame()
{
    if (g_open || feat::g_on[feat::Esp] || (feat::g_on[feat::SilentAim] && feat::g_on[feat::EspAimFov])) return true;
    {
        float x, y, size;
        int shape;
        if (minimap::FrameRect(x, y, size, shape)) return true;
    }
    if (feat::g_on[feat::Speedometer]) {
        bool valid = false;
        jump::LiveSpeed(&valid);
        if (valid) return true;
    }
    if (feat::g_on[feat::Jump] && feat::g_on[feat::Bhop] && feat::g_on[feat::BhopHud]) { // hop readout over the game
        jump::HudInfo h = jump::Hud();
        double age = PreciseNow() - h.at;
        if (h.grade[0] && age >= 0 && age <= 1.5) return true;
    }
    std::lock_guard lock(g_toastMutex);
    return !g_toasts.empty();
}

void Toast(std::string text) { PushToast(std::move(text), -1); }

bool OnKeyDown(unsigned vk)
{
    jump::OnKey(vk); // precise jump timing; never consumes the key
    int target = g_binding;
    if (target != kNotBinding) {
        g_binding = kNotBinding;
        if (vk == VK_ESCAPE) return true;
        bool clear = vk == VK_BACK || vk == VK_DELETE;
        if (target == kBindMenu) {
            if (!clear) {
                g_menuKey = vk;
                cfg::SetInt("Menu", "Key", int(vk));
            }
        } else if (target == kBindMenu2) {
            g_menuKey2 = clear ? 0 : vk;
            cfg::SetInt("Menu", "Key2", int(g_menuKey2.load()));
        } else if (target == kBindJump) {
            jump::g_key = clear ? 0 : vk;
            cfg::SetInt("Hotkeys", "JumpKey", int(jump::g_key.load()));
        } else if (target == kBindFovUp || target == kBindFovDown) {
            auto& key = target == kBindFovUp ? g_fovUp : g_fovDown;
            key = clear ? 0 : vk;
            cfg::SetInt("Hotkeys", target == kBindFovUp ? "FovUp" : "FovDown", int(key.load()));
        } else {
            g_hotkey[target] = clear ? 0 : int(vk);
            cfg::SetInt("Hotkeys", feat::kToggles[target].key, g_hotkey[target]);
        }
        return true;
    }
    if (vk == g_menuKey || (g_menuKey2 && vk == g_menuKey2)) {
        bool open = !g_open;
        g_open = open;
        km::Log("Menu %s%s", open ? "opened" : "closed", overlay::Drawing() ? "" : " (overlay not drawing, see the DX lines)"); // a crash right after this line points at the overlay
        if (open) ClipCursor(nullptr); // the game confines the cursor; free it for the menu
        return true;
    }
    if (g_open) return false;
    if (fixes::OnKey(vk)) return true; // number keys on an open keypad
    if (vk && (vk == g_fovUp || vk == g_fovDown)) { // World FOV steps; turns the editor on if it was off
        float step = (GetKeyState(VK_SHIFT) < 0 ? 5.f : 1.f) * (vk == g_fovUp ? 1.f : -1.f);
        const feat::ValueInfo& info = feat::kValues[feat::WorldFov];
        float v = std::clamp(std::round(feat::g_value[feat::WorldFov] + step), info.min, info.max);
        feat::g_value[feat::WorldFov] = v;
        feat::g_on[feat::CustomFov] = true;
        feat::Save();
        char text[64];
        snprintf(text, sizeof(text), "%s  %.0f°", T("World FOV"), v);
        PushToast(text, -1);
        return true;
    }
    for (int t = 0; t < feat::ToggleCount; t++) {
        if (g_hotkey[t] != int(vk)) continue;
        bool on = !feat::g_on[t];
        feat::g_on[t] = on;
        feat::Save();
        PushToast(T(feat::kToggles[t].label), on ? 1 : 0);
        return true;
    }
    return false;
}

} // namespace menu
