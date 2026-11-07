// minesweeper.online look (night skin, classic skin when night is off), fpsoverlay misc.cpp Ms
// namespace 1:1 inside botox menu chrome (band, accent line, menu style colours). Same file in
// botox csgo-sdk/hacks/misc and botox-tf2/src/hacks/misc: edit one, copy to the other.
// The grid is one InvisibleButton hit-tested by math: per-cell buttons were the expert lag.
#include "minesweeper.h"
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/imgui/imgui_internal.h"

#include <chrono>
#include <cmath>
#include <cstdlib>

namespace {

using MsGame::IDLE; using MsGame::PLAYING; using MsGame::WIN; using MsGame::LOSE;

constexpr float kCell = 24.f;
constexpr const char* kWindow = "minesweeper##ms";

MsGame::Board s_b;
bool          s_inited = false;
int           s_cfg[4] = { };            // diff, w, h, mines the board was built from
uint64_t      s_t0 = 0, s_t1 = 0;        // ms at first reveal / game end
enum Hold : uint8_t { HOLD_NONE, HOLD_OPEN, HOLD_CHORD };
Hold          s_hold = HOLD_NONE;        // button held on the grid, released = act

uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t Seed() { return (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count(); }

constexpr ImU32 Hex(uint32_t rgb, int a = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

struct Skin {
    ImU32 body, lt, dk;                   // frame + bevels
    ImU32 closedLt, closedDk, closedIn;   // unopened tile
    ImU32 open, line;                     // opened tile, its top-left line
    ImU32 mineBg, boom;                   // revealed mine, the mine that ended it
    ImU32 flag, pole;
    ImU32 wrongLt, wrongDk, wrongIn, wrongFlag;  // flag on a safe cell after a loss
    bool  shadedMine;                     // night: grey radial ball, classic: black + white dot
    ImU32 num[9];
};

// Colors lifted from minesweeper.online's skin SVGs (night) and the classic XP skin.
const Skin kNight = {
    Hex(0x40484F), Hex(0x707880), Hex(0x1E262E),
    Hex(0x707880), Hex(0x222A32), Hex(0x4C545C),
    Hex(0x384048), Hex(0x1E262E),
    Hex(0x464E56), Hex(0xEE6666),
    Hex(0xF77070), Hex(0xD8E0E8),
    Hex(0xBB6666), Hex(0x661111), Hex(0x883333), Hex(0xFF8888),
    true,
    { 0, Hex(0x7CC7FF), Hex(0x66C266), Hex(0xFF7788), Hex(0xEE88FF),
         Hex(0xDDAA22), Hex(0x66CCCC), Hex(0x999999), Hex(0xD0D8E0) },
};
const Skin kClassic = {
    Hex(0xC0C0C0), Hex(0xFFFFFF), Hex(0x808080),
    Hex(0xFFFFFF), Hex(0x808080), Hex(0xC0C0C0),
    Hex(0xC0C0C0), Hex(0x808080),
    Hex(0xC0C0C0), Hex(0xFF0000),
    Hex(0xFF0000), Hex(0x000000),
    Hex(0xFFD0D0), Hex(0xA04040), Hex(0xE89090), Hex(0xFF0000),
    false,
    { 0, Hex(0x0000FF), Hex(0x008000), Hex(0xFF0000), Hex(0x000080),
         Hex(0x800000), Hex(0x008080), Hex(0x000000), Hex(0x808080) },
};

// New board from the saved settings; clamped values go back so the menu sliders show them.
void NewGame(const MsSettings& s) {
    s_b.Init(*s.diff, *s.width, *s.height, *s.mines);
    *s.diff = s_b.diff;
    if (s_b.diff == MsGame::kCustom) { *s.width = s_b.w; *s.height = s_b.h; *s.mines = s_b.mines; }
    s_cfg[0] = *s.diff; s_cfg[1] = *s.width; s_cfg[2] = *s.height; s_cfg[3] = *s.mines;
    s_hold = HOLD_NONE; s_inited = true;
}

// Timer edges: starts on the action that leaves IDLE, stops on the one that ends it.
void After(MsGame::State prev) {
    const uint64_t now = NowMs();
    if (prev == IDLE && s_b.state != IDLE) s_t0 = now;
    if (prev != WIN && prev != LOSE && s_b.Over()) s_t1 = now;
}

// Rect frame with mitred corners: tl color on top/left, br color on bottom/right.
void Bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, ImU32 tl, ImU32 br) {
    const ImDrawListFlags f = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // pixel-aligned, AA would soften the edges
    dl->AddQuadFilled(a, {b.x, a.y}, {b.x - t, a.y + t}, {a.x + t, a.y + t}, tl);
    dl->AddQuadFilled(a, {a.x + t, a.y + t}, {a.x + t, b.y - t}, {a.x, b.y}, tl);
    dl->AddQuadFilled({a.x, b.y}, {a.x + t, b.y - t}, {b.x - t, b.y - t}, b, br);
    dl->AddQuadFilled({b.x, a.y}, b, {b.x - t, b.y - t}, {b.x - t, a.y + t}, br);
    dl->Flags = f;
}

// Unopened tile: light top-left half, dark bottom-right half, inset face (skin closed.svg).
void ClosedTile(ImDrawList* dl, ImVec2 a, ImU32 lt, ImU32 dk, ImU32 in) {
    const float s = kCell, t = 3.f;
    dl->AddRectFilled(a, {a.x + s, a.y + s}, dk);
    const ImDrawListFlags f = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // AA fringe bleeds into the next tile
    dl->AddTriangleFilled(a, {a.x + s, a.y}, {a.x, a.y + s}, lt);
    dl->Flags = f;
    dl->AddRectFilled({a.x + t, a.y + t}, {a.x + s - t, a.y + s - t}, in);
}

void OpenTile(ImDrawList* dl, ImVec2 a, ImU32 fill, ImU32 line) {
    dl->AddRectFilled(a, {a.x + kCell, a.y + kCell}, line);
    dl->AddRectFilled({a.x + 1.f, a.y + 1.f}, {a.x + kCell, a.y + kCell}, fill);
}

// Shapes in tile units (0..1), taken from flag.svg / mine.svg.
void DrawFlag(ImDrawList* dl, ImVec2 a, ImU32 flag, ImU32 pole) {
    const float s = kCell;
    auto P = [&](float x, float y) { return ImVec2(a.x + x * s, a.y + y * s); };
    dl->AddRectFilled(P(0.50f, 0.45f), P(0.58f, 0.72f), pole);
    dl->AddRectFilled(P(0.28f, 0.70f), P(0.78f, 0.82f), pole, 1.f);
    dl->AddTriangleFilled(P(0.58f, 0.18f), P(0.58f, 0.53f), P(0.22f, 0.26f), flag);
}

void DrawMine(ImDrawList* dl, ImVec2 a, bool shaded) {
    const float s = kCell;
    const ImVec2 c = {a.x + s * 0.52f, a.y + s * 0.52f};
    const ImU32 blk = IM_COL32(0, 0, 0, 255);
    const float arm = s * 0.36f, diag = s * 0.25f, th = s * 0.075f;
    dl->AddLine({c.x - arm, c.y}, {c.x + arm, c.y}, blk, th);
    dl->AddLine({c.x, c.y - arm}, {c.x, c.y + arm}, blk, th);
    dl->AddLine({c.x - diag, c.y - diag}, {c.x + diag, c.y + diag}, blk, th);
    dl->AddLine({c.x + diag, c.y - diag}, {c.x - diag, c.y + diag}, blk, th);
    const float r = s * 0.27f;
    dl->AddCircleFilled(c, r, blk, 20);
    if (shaded) {   // stepped stand-in for the SVG's #000 -> #ccc radial gradient
        dl->AddCircleFilled(c, r * 0.72f, Hex(0x333333), 16);
        dl->AddCircleFilled(c, r * 0.48f, Hex(0x777777), 16);
        dl->AddCircleFilled(c, r * 0.26f, Hex(0xAAAAAA), 12);
        dl->AddCircleFilled(c, r * 0.10f, Hex(0xCCCCCC), 8);
    } else {
        dl->AddRectFilled({c.x - r * 0.45f, c.y - r * 0.45f}, {c.x - r * 0.1f, c.y - r * 0.1f},
                          IM_COL32(255, 255, 255, 255));
    }
}

// Digit centered on its glyph box, not the line box, so every number sits dead center.
void DrawNum(ImDrawList* dl, ImFont* f, float fsz, ImVec2 a, int n, ImU32 col) {
    const char s[2] = { (char)('0' + n), 0 };
    const ImFontGlyph* g = f->FindGlyph((ImWchar)s[0]);
    const float sc = fsz / f->FontSize;
    float x = a.x + kCell * 0.5f, y = a.y + kCell * 0.5f;
    if (g) { x -= (g->X0 + g->X1) * 0.5f * sc; y -= (g->Y0 + g->Y1) * 0.5f * sc; }
    dl->AddText(f, fsz, {std::floor(x + 0.5f), std::floor(y + 0.5f)}, col, s);
}

// 7-segment digit with hex segments (skin d0..d9.svg). bits: 0 top, 1 top-right,
// 2 bottom-right, 3 bottom, 4 bottom-left, 5 top-left, 6 middle.
void DrawSeg(ImDrawList* dl, float x, float y, float dw, float dh, uint8_t bits) {
    const ImU32 on = Hex(0xEE1111), off = Hex(0x3A0000);
    const float t = dw * 0.22f, h = t * 0.5f, g = 0.7f;
    auto Hz = [&](float yc, bool lit) {
        const ImVec2 p[6] = { {x + h + g, yc}, {x + t + g, yc - h}, {x + dw - t - g, yc - h},
                              {x + dw - h - g, yc}, {x + dw - t - g, yc + h}, {x + t + g, yc + h} };
        dl->AddConvexPolyFilled(p, 6, lit ? on : off);
    };
    auto Vt = [&](float xc, float y0, float y1, bool lit) {
        const ImVec2 p[6] = { {xc, y0 + h + g}, {xc + h, y0 + t + g}, {xc + h, y1 - t - g},
                              {xc, y1 - h - g}, {xc - h, y1 - t - g}, {xc - h, y0 + t + g} };
        dl->AddConvexPolyFilled(p, 6, lit ? on : off);
    };
    const float ym = y + dh * 0.5f;
    Hz(y + h,        bits & 1);
    Vt(x + dw - h, y, ym,      bits & 2);
    Vt(x + dw - h, ym, y + dh, bits & 4);
    Hz(y + dh - h,   bits & 8);
    Vt(x + h, ym, y + dh,      bits & 16);
    Vt(x + h, y, ym,           bits & 32);
    Hz(ym,           bits & 64);
}

constexpr float kLedDh = 26.f, kLedDw = 14.f, kLedGap = 2.f, kLedPad = 3.f;
constexpr float kLedW = 3 * kLedDw + 2 * kLedGap + 2 * kLedPad, kLedH = kLedDh + 2 * kLedPad;

void DrawLed(ImDrawList* dl, ImVec2 a, int v) {
    static const uint8_t kSegs[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    dl->AddRectFilled(a, {a.x + kLedW, a.y + kLedH}, IM_COL32(0, 0, 0, 255));
    v = v < -99 ? -99 : (v > 999 ? 999 : v);
    const int m = v < 0 ? -v : v;
    const uint8_t d[3] = { v < 0 ? (uint8_t)0x40 : kSegs[m / 100], kSegs[(m / 10) % 10], kSegs[m % 10] };
    for (int i = 0; i < 3; i++)
        DrawSeg(dl, a.x + kLedPad + i * (kLedDw + kLedGap), a.y + kLedPad, kLedDw, kLedDh, d[i]);
}

enum Face : uint8_t { FACE_SMILE, FACE_WOW, FACE_WIN, FACE_DEAD };

void DrawFace(ImDrawList* dl, ImVec2 a, float S, bool pressed, Face face, const Skin& k) {
    const ImVec2 b = {a.x + S, a.y + S};
    dl->AddRectFilled(a, b, k.dk);
    if (pressed) dl->AddRectFilled({a.x + 2.f, a.y + 2.f}, b, k.body);
    else {
        dl->AddRectFilled({a.x + 1.f, a.y + 1.f}, {b.x - 1.f, b.y - 1.f}, k.body);
        Bevel(dl, {a.x + 1.f, a.y + 1.f}, {b.x - 1.f, b.y - 1.f}, 3.f, k.lt, k.dk);
    }
    const float r = S * 0.31f, o = pressed ? 1.f : 0.f;
    const ImVec2 c = {a.x + S * 0.5f + o, a.y + S * 0.5f + o};
    const ImVec2 hc = {c.x - r * 0.12f, c.y - r * 0.12f};   // gradient center sits up-left
    const ImU32 blk = IM_COL32(0, 0, 0, 255);
    dl->AddCircleFilled(c,  r,         Hex(0xF5A83C), 32);
    dl->AddCircleFilled(hc, r * 0.80f, Hex(0xFACB0A), 32);
    dl->AddCircleFilled(hc, r * 0.52f, Hex(0xF3EE2A), 24);
    dl->AddCircleFilled(hc, r * 0.18f, Hex(0xFBF9C0), 12);
    dl->AddCircle(c, r, blk, 32, 1.3f);

    const float ex = r * 0.34f, ey = c.y - r * 0.24f;
    auto Arc = [&](ImVec2 ctr, float rad, float a0, float a1) {
        dl->PathArcTo(ctr, rad, a0 * 3.14159265f, a1 * 3.14159265f, 12);
        dl->PathStroke(blk, 0, 1.5f);
    };
    switch (face) {
    case FACE_DEAD:
        for (int s = -1; s <= 1; s += 2) {
            const float x = c.x + s * ex, e = r * 0.12f;
            dl->AddLine({x - e, ey - e}, {x + e, ey + e}, blk, 1.4f);
            dl->AddLine({x + e, ey - e}, {x - e, ey + e}, blk, 1.4f);
        }
        Arc({c.x, c.y + r * 0.78f}, r * 0.40f, 1.2f, 1.8f);
        break;
    case FACE_WIN:
        for (int s = -1; s <= 1; s += 2) {
            const float x = c.x + s * ex;
            dl->AddRectFilled({x - r * 0.24f, ey - r * 0.15f}, {x + r * 0.24f, ey + r * 0.14f}, blk, r * 0.1f);
            dl->AddLine({c.x + s * (ex + r * 0.2f), ey - r * 0.08f}, {c.x + s * r * 0.92f, ey - r * 0.22f}, blk, 1.2f);
        }
        dl->AddLine({c.x - ex, ey - r * 0.08f}, {c.x + ex, ey - r * 0.08f}, blk, 1.4f);
        Arc(c, r * 0.55f, 0.2f, 0.8f);
        break;
    case FACE_WOW:
        dl->AddCircleFilled({c.x - ex, ey}, r * 0.11f, blk, 8);
        dl->AddCircleFilled({c.x + ex, ey}, r * 0.11f, blk, 8);
        dl->AddCircle({c.x, c.y + r * 0.40f}, r * 0.18f, blk, 12, 1.5f);
        break;
    default:
        dl->AddCircleFilled({c.x - ex, ey}, r * 0.11f, blk, 8);
        dl->AddCircleFilled({c.x + ex, ey}, r * 0.11f, blk, 8);
        Arc(c, r * 0.55f, 0.2f, 0.8f);
        break;
    }
}

} // namespace

const char* Minesweeper_WindowName() { return kWindow; }

void Minesweeper_Draw(const MsSettings& s, ImFont* bold, ImFont* digits, bool input, const ImVec2& default_pos)
{
    if (!*s.open) { s_hold = HOLD_NONE; return; }

    // menu combo / sliders / config load changed the board -> new game
    const bool custom = *s.diff == MsGame::kCustom;
    if (!s_inited || *s.diff != s_cfg[0] ||
        (custom && (*s.width != s_cfg[1] || *s.height != s_cfg[2] || *s.mines != s_cfg[3])))
        NewGame(s);

    const MsGame::Board& b = s_b;
    const Skin& k = *s.night ? kNight : kClassic;
    ImGuiIO& io = ImGui::GetIO();
    if (!bold) bold = ImGui::GetFont();

    // ---- Layout ------------------------------------------------
    const float pad = 12.f, titleH = 25.f, bev = 3.f, gap = 6.f, hdrIn = 42.f, faceS = 34.f;   // 25 = menu_band_height
    const float boardW = b.w * kCell, boardH = b.h * kCell;
    const float bodyW  = 2 * bev + 2 * gap + 2 * bev + boardW;
    const float bodyH  = 2 * bev + 3 * gap + (2 * bev + hdrIn) + (2 * bev + boardH);

    static const char* const kDiffs[4] = { "beginner", "intermediate", "expert", "custom" };
    const float tabH = 17.f, tabPadX = 8.f, closeS = 17.f;
    const char* title = "minesweeper";
    const ImVec2 titleSz = bold->CalcTextSizeA(bold->FontSize, FLT_MAX, 0.f, title);
    float tabsW = 0.f;
    for (const char* d : kDiffs) tabsW += ImGui::CalcTextSize(d).x + 2 * tabPadX + 2.f;
    const float titleW = pad + titleSz.x + 20.f + tabsW + 6.f + closeS + pad;
    const float W = std::floor(ImMax(bodyW + 2 * pad, titleW));
    const float H = titleH + pad + bodyH + pad;

    ImGui::SetNextWindowPos(default_pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({W, H}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const bool shown = ImGui::Begin(kWindow, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | (input ? 0 : ImGuiWindowFlags_NoInputs));
    ImGui::PopStyleVar(2);
    if (!shown) { ImGui::End(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = {std::floor(ImGui::GetWindowPos().x), std::floor(ImGui::GetWindowPos().y)};
    const ImVec2 we = {wp.x + W, wp.y + H};

    // ---- Panel, botox menu chrome (live menu style) ------------
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rnd = st.WindowRounding;
    const ImU32 colAccent = ImGui::GetColorU32(ImGuiCol_Accent);
    const ImU32 colText   = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim    = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    dl->PushClipRect(wp, we, false);
    dl->AddRectFilled(wp, we, ImGui::GetColorU32(ImGuiCol_WindowBg), rnd);
    dl->AddRectFilled(wp, {we.x, wp.y + titleH}, ImGui::GetColorU32(ImGuiCol_MenuBand), rnd, ImDrawFlags_RoundCornersTop);
    dl->AddRect(wp, we, ImGui::GetColorU32(ImGuiCol_MenuWindowOutline), rnd);
    dl->PopClipRect();
    RenderFadedGradientLine(dl, ImVec2(wp.x, wp.y + titleH - 1.f), ImVec2(W, 1.f), ImColor(colAccent));

    const float textH = ImGui::GetTextLineHeight();
    dl->AddText(bold, bold->FontSize, {wp.x + pad, wp.y + std::floor((titleH - titleSz.y) * 0.5f)}, colText, title);

    // Close X, far right
    {
        const ImVec2 a = {we.x - pad - closeS, wp.y + std::floor((titleH - closeS) * 0.5f)};
        ImGui::SetCursorScreenPos(a);
        const bool hit = ImGui::InvisibleButton("##msclose", {closeS, closeS});
        const bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(a, {a.x + closeS, a.y + closeS}, IM_COL32(255, 255, 255, 6), 4.f);
        const float e = 4.f;
        const ImVec2 c = {a.x + closeS * 0.5f, a.y + closeS * 0.5f};
        const ImU32 col = hov ? colText : colDim;
        dl->AddLine({c.x - e, c.y - e}, {c.x + e, c.y + e}, col, 1.5f);
        dl->AddLine({c.x + e, c.y - e}, {c.x - e, c.y + e}, col, 1.5f);
        if (hit) *s.open = false;
    }
    // Difficulty tabs, right-aligned before the X
    {
        float x = we.x - pad - closeS - 6.f - tabsW;
        const float y = wp.y + std::floor((titleH - tabH) * 0.5f);
        for (int i = 0; i < 4; i++) {
            const float tw = ImGui::CalcTextSize(kDiffs[i]).x + 2 * tabPadX;
            ImGui::SetCursorScreenPos({x, y});
            ImGui::PushID(i);
            const bool hit = ImGui::InvisibleButton("##msdiff", {tw, tabH});
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered(), act = b.diff == i;
            if (act)      dl->AddRectFilled({x, y}, {x + tw, y + tabH}, IM_COL32(255, 255, 255, 10), 4.f);
            else if (hov) dl->AddRectFilled({x, y}, {x + tw, y + tabH}, IM_COL32(255, 255, 255, 6), 4.f);
            dl->AddText({x + tabPadX, y + std::floor((tabH - textH) * 0.5f)},
                        act ? colAccent : (hov ? colText : colDim), kDiffs[i]);
            if (hit) { *s.diff = i; s_cfg[0] = -1; }   // rebuilt next frame: this frame's layout is the old size
            x += tw + 2.f;
        }
    }

    // ---- Game body ---------------------------------------------
    const ImVec2 bA = {std::floor(wp.x + (W - bodyW) * 0.5f), wp.y + titleH + pad};
    const ImVec2 bB = {bA.x + bodyW, bA.y + bodyH};
    dl->AddRectFilled(bA, bB, k.body);
    Bevel(dl, bA, bB, bev, k.lt, k.dk);

    const ImVec2 hA = {bA.x + bev + gap, bA.y + bev + gap};
    const ImVec2 hB = {bB.x - bev - gap, hA.y + 2 * bev + hdrIn};
    Bevel(dl, hA, hB, bev, k.dk, k.lt);
    const ImVec2 gA = {hA.x, hB.y + gap};
    const ImVec2 gB = {hB.x, gA.y + 2 * bev + boardH};
    Bevel(dl, gA, gB, bev, k.dk, k.lt);
    const ImVec2 g0 = {gA.x + bev, gA.y + bev};

    // ---- Grid input --------------------------------------------
    ImGui::SetCursorScreenPos(g0);
    ImGui::InvisibleButton("##msgrid", {boardW, boardH}, ImGuiButtonFlags_MouseButtonMask_);
    const bool gridHov = ImGui::IsItemHovered();
    int hr = -1, hc = -1;
    if ((gridHov || ImGui::IsItemActive()) &&
        io.MousePos.x >= g0.x && io.MousePos.y >= g0.y &&
        io.MousePos.x < g0.x + boardW && io.MousePos.y < g0.y + boardH) {
        hc = (int)((io.MousePos.x - g0.x) / kCell);
        hr = (int)((io.MousePos.y - g0.y) / kCell);
        if (hc >= b.w) hc = b.w - 1;
        if (hr >= b.h) hr = b.h - 1;
    }
    if (input && !b.Over()) {
        // Right press flags now; left/middle press previews, release acts (like the site).
        // Left+right together or left on a number = chord.
        if (gridHov && hr >= 0) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (s_hold != HOLD_NONE || ImGui::IsMouseDown(ImGuiMouseButton_Left)) s_hold = HOLD_CHORD;
                else { const MsGame::State p = s_b.state; s_b.Flag(hr, hc); After(p); }
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                s_hold = (ImGui::IsMouseDown(ImGuiMouseButton_Right) || b.c[hr][hc].revealed) ? HOLD_CHORD : HOLD_OPEN;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) s_hold = HOLD_CHORD;
        }
        if (s_hold != HOLD_NONE && (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                                    ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
                                    ImGui::IsMouseReleased(ImGuiMouseButton_Middle))) {
            if (hr >= 0) {
                const MsGame::State p = s_b.state;
                if (s_hold == HOLD_OPEN) s_b.Reveal(hr, hc, Seed());
                else                     s_b.Chord(hr, hc, Seed());
                After(p);
            }
            s_hold = HOLD_NONE;
        }
    } else s_hold = HOLD_NONE;

    // ---- Header: mines left, face, timer -----------------------
    const float hy = std::floor((hA.y + hB.y - kLedH) * 0.5f);
    DrawLed(dl, {hA.x + bev + 6.f, hy}, b.mines - b.flags);
    const int secs = b.state == IDLE ? 0 : (int)(((b.Over() ? s_t1 : NowMs()) - s_t0) / 1000);
    DrawLed(dl, {hB.x - bev - 6.f - kLedW, hy}, secs);

    const ImVec2 fA = {std::floor((hA.x + hB.x - faceS) * 0.5f), std::floor((hA.y + hB.y - faceS) * 0.5f)};
    ImGui::SetCursorScreenPos(fA);
    if (ImGui::InvisibleButton("##msface", {faceS, faceS})) NewGame(s);
    const bool facePressed = ImGui::IsItemActive() && ImGui::IsItemHovered();
    const Face face = b.state == LOSE ? FACE_DEAD : b.state == WIN ? FACE_WIN
                    : s_hold != HOLD_NONE ? FACE_WOW : FACE_SMILE;
    DrawFace(dl, fA, faceS, facePressed, face, k);

    // ---- Tiles -------------------------------------------------
    ImFont* numFont = digits ? digits : ImGui::GetFont();
    const float numSz = numFont->FontSize;
    for (int r = 0; r < b.h; r++) for (int c = 0; c < b.w; c++) {
        const MsGame::Cell& e = b.c[r][c];
        const ImVec2 a = {g0.x + c * kCell, g0.y + r * kCell};
        if (e.revealed) {
            if (e.mine) {
                OpenTile(dl, a, (r == b.boomR && c == b.boomC) ? k.boom : k.mineBg, k.line);
                DrawMine(dl, a, k.shadedMine);
            } else {
                OpenTile(dl, a, k.open, k.line);
                if (e.adj) DrawNum(dl, numFont, numSz, a, e.adj, k.num[e.adj]);
            }
            continue;
        }
        const bool pressed = s_hold != HOLD_NONE && hr >= 0 && !e.flagged &&
            (s_hold == HOLD_OPEN ? (r == hr && c == hc) : (std::abs(r - hr) <= 1 && std::abs(c - hc) <= 1));
        if (pressed) OpenTile(dl, a, k.open, k.line);
        else if (e.flagged && b.state == LOSE && !e.mine) {
            ClosedTile(dl, a, k.wrongLt, k.wrongDk, k.wrongIn);
            DrawFlag(dl, a, k.wrongFlag, k.pole);
        } else {
            ClosedTile(dl, a, k.closedLt, k.closedDk, k.closedIn);
            if (e.flagged) DrawFlag(dl, a, k.flag, k.pole);
        }
    }

    ImGui::End();
}
