#pragma once
// vgui Win32Font text 1:1 with the engine, for donors that draw through ISurface (requiem). Kisak-Strike
// vgui2/vgui_surfacelib Win32Font.cpp Create / GetCharABCWidths / GetCharRGBA (render-to-bitmap path = fonts
// without FONTFLAG_ANTIALIAS), FontEffects.cpp ApplyDropShadowToTexture / ApplyOutlineToTexture,
// FontManager GetTextSize, vguimatsurface DrawPrintText. GDI bilevel glyphs: texels are 0 or 255.
// Glyphs cached per char (GDI opened only for new chars), strings cached as colour runs. Render thread only.
// Same file in botox csgo (hacks/misc), botox-tf2 + botox-css (src/hacks/hud): edit one, copy to the others.
// Gate: csgo-sdk/tools/requiem_check.py (body pixels vs one GDI ExtTextOutW + shadow / outline rules).
#include <windows.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "gdi32")

namespace n_vgui
{
    enum : int { k_dropshadow = 0x080, k_outline = 0x200 };   // FONTFLAG_DROPSHADOW / FONTFLAG_OUTLINE

    struct run_t  { int x0, x1, y; uint32_t rgba; };                // one row of equal texels, IM_COL32 byte order
    struct text_t { int wide = 0, tall = 0, frame = 0; std::vector<run_t> runs; };
    struct glyph_t { int a = 0, b = 0, c = 0; std::vector<unsigned char> rgba; };   // abc widened like the abc cache, rgba = b x tall

    struct font_t
    {
        const char* name;
        int tall, weight, flags;
        int height = 0;   // m_iHeight = tmHeight + shadow + 2 * outline = GetTextSize tall = glyph cell tall
        std::unordered_map<wchar_t, glyph_t> glyphs;
        std::unordered_map<std::string, text_t> texts;
        font_t(const char* n, int t, int w, int f) : name(n), tall(t), weight(w), flags(f) {}
    };

    // CWin32Font::Create, then GetCharABCWidths + GetCharRGBA once per new char
    inline bool load_glyphs(font_t& font, const std::wstring& text)
    {
        const int shadow = (font.flags & k_dropshadow) ? 1 : 0, outline = (font.flags & k_outline) ? 1 : 0;

        std::wstring missing;
        for (const wchar_t ch : text)
            if (!font.glyphs.count(ch) && missing.find(ch) == std::wstring::npos) missing += ch;
        if (font.height && missing.empty()) return true;

        const HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        const HFONT hfont = CreateFontA(font.tall, 0, 0, 0, font.weight, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, font.name);
        const HGDIOBJ oldFont = hfont ? SelectObject(dc, hfont) : nullptr;

        TEXTMETRICA tm{};
        HBITMAP dib = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        unsigned char* bits = nullptr;
        int bitmapWide = 0, bitmapTall = 0;

        if (hfont) {
            SetMapMode(dc, MM_TEXT);
            SetTextAlign(dc, TA_LEFT | TA_TOP | TA_UPDATECP);
        }
        if (hfont && GetTextMetricsA(dc, &tm)) {
            bitmapWide = tm.tmMaxCharWidth + outline * 2;
            bitmapTall = tm.tmHeight + shadow + outline * 2;
            BITMAPINFOHEADER header{};
            header.biSize = sizeof(header);
            header.biWidth = bitmapWide;
            header.biHeight = -bitmapTall;
            header.biPlanes = 1;
            header.biBitCount = 32;
            header.biCompression = BI_RGB;
            dib = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
            if (dib) oldBitmap = SelectObject(dc, dib);
        }

        if (oldBitmap && bits) {
            const int height = font.height = tm.tmHeight + shadow + outline * 2;
            for (const wchar_t ch : missing) {
                glyph_t& glyph = font.glyphs[ch];

                int a = 0, b = tm.tmMaxCharWidth, c = 0;
                if (ABC abc{}; GetCharABCWidthsW(dc, ch, ch, &abc)) { a = abc.abcA; b = (int)abc.abcB; c = abc.abcC; }
                else if (SIZE size{}; GetTextExtentPoint32W(dc, &ch, 1, &size)) b = size.cx;

                // abc cache widens the cell for the outline + shadow, advance a + b + c unchanged
                glyph.a = a - outline;
                glyph.b = b + outline * 2 + shadow;
                glyph.c = c - shadow - outline;

                if (iswspace(ch)) continue;   // DrawPrintText never asks the texture cache for spaces

                const int rgbaWide = (std::max)(glyph.b, 1), rgbaTall = (std::max)(height, 1);
                std::vector<unsigned char>& rgba = glyph.rgba;
                rgba.assign((size_t)rgbaWide * rgbaTall * 4, 0);

                // render-to-bitmap: pen at -a, so the glyph's black box lands at column `outline`
                SetBkColor(dc, RGB(0, 0, 0));
                SetTextColor(dc, RGB(255, 255, 255));
                SetBkMode(dc, OPAQUE);
                MoveToEx(dc, -glyph.a, 0, nullptr);
                const RECT rect{ 0, 0, glyph.b, height };
                ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rect, nullptr, 0, nullptr);
                ExtTextOutW(dc, 0, 0, 0, nullptr, &ch, 1, nullptr);
                SetBkMode(dc, TRANSPARENT);
                GdiFlush();

                const int wide = (std::min)(glyph.b, bitmapWide), tall = (std::min)(height, bitmapTall);
                for (int j = outline; j < tall - outline; j++)
                    for (int i = outline; i < wide - shadow - outline; i++) {
                        if (i >= rgbaWide || j >= rgbaTall) continue;
                        const unsigned char* src = bits + (i + j * bitmapWide) * 4;
                        unsigned char* dst = &rgba[(i + j * rgbaWide) * 4];
                        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                        dst[3] = (unsigned char)((float)src[0] * 0.34f + (float)src[1] * 0.55f + (float)src[2] * 0.11f);
                    }

                // drop shadow: bottom row cleared first (winME noise fix), then every empty texel takes the alpha
                // of the texel one up-left, black, walking bottom-right up so it reads the unshadowed image
                if (shadow) {
                    for (int i = 0; i < (std::min)(wide, rgbaWide); i++)
                        std::fill_n(&rgba[((height - 1) * rgbaWide + i) * 4], 4, (unsigned char)0);
                    for (int y = rgbaTall - 1; y >= shadow; y--)
                        for (int x = rgbaWide - 1; x >= shadow; x--) {
                            unsigned char* dst = &rgba[(x + y * rgbaWide) * 4];
                            if (dst[3]) continue;
                            const unsigned char* src = &rgba[(x - shadow + (y - shadow) * rgbaWide) * 4];
                            dst[0] = dst[1] = dst[2] = 0;
                            dst[3] = src[3];
                        }
                }

                // outline: an empty texel next to a lit one (all 4 channels != 0) turns opaque black
                if (outline)
                    for (int y = 0; y < rgbaTall; y++)
                        for (int x = 0; x < rgbaWide; x++) {
                            unsigned char* texel = &rgba[(x + y * rgbaWide) * 4];
                            if (texel[3]) continue;
                            for (int sx = -outline; sx <= outline; sx++)
                                for (int sy = -outline; sy <= outline; sy++) {
                                    const int tx = x + sx, ty = y + sy;
                                    if ((!sx && !sy) || tx < 0 || tx >= rgbaWide || ty < 0 || ty >= rgbaTall) continue;
                                    const unsigned char* test = &rgba[(tx + ty * rgbaWide) * 4];
                                    if (test[0] && test[1] && test[2] && test[3]) { texel[0] = texel[1] = texel[2] = 0; texel[3] = 255; }
                                }
                        }
            }
        }

        if (oldBitmap) SelectObject(dc, oldBitmap);
        if (dib) DeleteObject(dib);
        if (oldFont) SelectObject(dc, oldFont);
        if (hfont) DeleteObject(hfont);
        DeleteDC(dc);

        return font.height > 0 && std::all_of(text.begin(), text.end(), [&](wchar_t ch) { return font.glyphs.count(ch) != 0; });
    }

    // GetTextSize (wide = ceil of the widest line, '&' never counts) + DrawPrintText layout (cell x = total + a,
    // total += a + b + c, spaces skipped) composed into colour runs. quads draw in string order, texels are 0 / 255:
    // a later cell covers an earlier one
    inline void raster(font_t& font, text_t& out, const std::wstring& text)
    {
        out.wide = out.tall = 0;
        out.runs.clear();
        if (!load_glyphs(font, text)) return;
        const int height = out.tall = font.height;

        struct cell_t { const glyph_t* glyph; int x; };
        std::vector<cell_t> cells;
        float line = 0.f;
        int total = 0, minX = 0, maxX = 0;
        for (const wchar_t ch : text) {
            const glyph_t& g = font.glyphs.at(ch);
            if (ch == L'\n') line = 0.f;
            else if (ch != L'&') {
                line += (float)(g.a + g.b + g.c);
                if (line > (float)out.wide) out.wide = (int)ceilf(line);
            }
            if (!iswspace(ch) && !g.rgba.empty()) {
                cells.push_back({ &g, total + g.a });
                minX = (std::min)(minX, total + g.a);
                maxX = (std::max)(maxX, total + g.a + g.b);
            }
            total += g.a + g.b + g.c;
        }

        const int canvasWide = maxX - minX;
        std::vector<uint32_t> canvas((size_t)canvasWide * height, 0);
        for (const cell_t& cell : cells) {
            const int w = (std::max)(cell.glyph->b, 1);
            for (int j = 0; j < height; j++)
                for (int i = 0; i < w; i++) {
                    const unsigned char* t = &cell.glyph->rgba[(i + j * w) * 4];
                    if (t[3]) canvas[(size_t)(cell.x - minX + i) + (size_t)j * canvasWide] = t[0] | (t[1] << 8) | (t[2] << 16) | ((uint32_t)t[3] << 24);
                }
        }
        for (int y = 0; y < height; y++) {
            const uint32_t* row = canvas.data() + (size_t)y * canvasWide;
            for (int x = 0; x < canvasWide;) {
                int end = x + 1;
                while (end < canvasWide && row[end] == row[x]) end++;
                if (row[x]) out.runs.push_back({ x + minX, end + minX, y, row[x] });
                x = end;
            }
        }
    }

    // utf-8 string -> cached runs. past 64 strings, ones unused for 120 frames go
    inline const text_t& text(font_t& font, const std::string& utf8, int frame)
    {
        const auto [it, fresh] = font.texts.try_emplace(utf8);
        if (fresh) {
            std::wstring wide((size_t)(std::max)(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0), 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), (int)wide.size());
            wide.pop_back();
            raster(font, it->second, wide);
        }
        it->second.frame = frame;
        const text_t& out = it->second;
        if (font.texts.size() > 64)
            for (auto e = font.texts.begin(); e != font.texts.end();)
                e = (frame - e->second.frame > 120) ? font.texts.erase(e) : std::next(e);
        return out;   // node-based map: erasing other entries keeps this reference valid
    }

    // vgui modulates texels by the text colour: body = colour, shadow / outline = black at the colour's alpha
    inline uint32_t modulate(uint32_t texel, uint32_t color)
    {
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) out |= ((((texel >> s) & 0xFF) * ((color >> s) & 0xFF) + 127) / 255) << s;
        return out;
    }

    // DrawPrintText at int pos (x, y = cell top-left), clipped to [cx0, cx1) x [cy0, cy1) like MatSystemSurface clips
    // the glyph quads. fill(x0, y0, x1, y1, rgba) gets one rect per visible run
    template <class Fill>
    void draw(const text_t& t, int x, int y, uint32_t color, Fill&& fill, int cx0 = INT_MIN, int cy0 = INT_MIN, int cx1 = INT_MAX, int cy1 = INT_MAX)
    {
        for (const run_t& r : t.runs) {
            const int ry = y + r.y;
            if (ry < cy0 || ry >= cy1) continue;
            const int x0 = (std::max)(x + r.x0, cx0), x1 = (std::min)(x + r.x1, cx1);
            if (x0 < x1) fill(x0, ry, x1, ry + 1, modulate(r.rgba, color));
        }
    }
}
