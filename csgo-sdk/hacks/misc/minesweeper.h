#pragma once
// Minesweeper, fpsoverlay port (minesweeper.online look). Same file in botox csgo-sdk/hacks/misc and
// botox-tf2/src/hacks/misc: edit one, copy to the other. Rules below are engine-free so
// botox-tf2/tests/minesweeper_test.cpp drives them; drawing + input live in minesweeper.cpp.
#include <cstdint>
#include <cstring>
#include <random>

namespace MsGame {

enum State : uint8_t { IDLE, PLAYING, WIN, LOSE };
constexpr int kMaxW = 50, kMaxH = 30, kMinSide = 5;   // custom board bounds
constexpr int kCustom = 3;                              // difficulty index of the custom board
constexpr int kDims[3][3] = { {9, 9, 10}, {16, 16, 40}, {30, 16, 99} }; // w, h, mines

// First click keeps its 3x3 clear, so at most w*h - 9 mines fit
constexpr int MaxMines(int w, int h) { return w * h - 9; }

struct Cell { bool mine, revealed, flagged; uint8_t adj; };

struct Board {
    Cell  c[kMaxH][kMaxW];
    int   w = 9, h = 9, mines = 10, diff = 0;
    int   flags = 0, revealed = 0;
    int   boomR = -1, boomC = -1;   // mine that ended the game
    State state = IDLE;

    // d 0..2 = beginner / intermediate / expert, 3 = custom (cw x ch, cm mines, clamped)
    void Init(int d, int cw = 30, int ch = 16, int cm = 99) {
        diff = (d < 0 || d > kCustom) ? 1 : d;
        if (diff == kCustom) {
            w = cw < kMinSide ? kMinSide : (cw > kMaxW ? kMaxW : cw);
            h = ch < kMinSide ? kMinSide : (ch > kMaxH ? kMaxH : ch);
            mines = cm < 1 ? 1 : (cm > MaxMines(w, h) ? MaxMines(w, h) : cm);
        } else {
            w = kDims[diff][0]; h = kDims[diff][1]; mines = kDims[diff][2];
        }
        std::memset(c, 0, sizeof(c));
        flags = revealed = 0; boomR = boomC = -1; state = IDLE;
    }
    bool In(int r, int col) const { return r >= 0 && r < h && col >= 0 && col < w; }
    bool Over() const { return state == WIN || state == LOSE; }

    // First click: mines go anywhere except the 3x3 around it, so it always opens.
    void Place(int sr, int sc, uint32_t seed) {
        static int idx[kMaxW * kMaxH];
        int n = 0;
        for (int r = 0; r < h; r++) for (int col = 0; col < w; col++)
            if (r < sr - 1 || r > sr + 1 || col < sc - 1 || col > sc + 1) idx[n++] = r * w + col;
        std::mt19937 rng(seed);
        for (int i = 0; i < mines && i < n; i++) {          // partial Fisher-Yates
            int j = i + (int)(rng() % (uint32_t)(n - i));
            int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            c[idx[i] / w][idx[i] % w].mine = true;
        }
        for (int r = 0; r < h; r++) for (int col = 0; col < w; col++) {
            int k = 0;
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++)
                if (In(r + dr, col + dc) && c[r + dr][col + dc].mine) k++;
            c[r][col].adj = (uint8_t)k;
        }
    }

    void Reveal(int r, int col, uint32_t seed) {
        if (Over() || !In(r, col) || c[r][col].flagged || c[r][col].revealed) return;
        if (state == IDLE) { Place(r, col, seed); state = PLAYING; }
        if (c[r][col].mine) {
            c[r][col].revealed = true; boomR = r; boomC = col; state = LOSE;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                if (c[y][x].mine && !c[y][x].flagged) c[y][x].revealed = true;
            return;
        }
        // Iterative flood fill: a 1500 cell custom board would blow a recursive one
        static int q[kMaxW * kMaxH];
        int head = 0, tail = 0;
        auto Open = [&](int y, int x) {
            Cell& e = c[y][x];
            if (e.revealed || e.flagged || e.mine) return;
            e.revealed = true; revealed++;
            if (e.adj == 0) q[tail++] = y * w + x;
        };
        Open(r, col);
        while (head < tail) {
            int y = q[head] / w, x = q[head] % w; head++;
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++)
                if ((dr || dc) && In(y + dr, x + dc)) Open(y + dr, x + dc);
        }
        if (revealed == w * h - mines) {
            state = WIN;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                if (c[y][x].mine && !c[y][x].flagged) { c[y][x].flagged = true; flags++; }
        }
    }

    void Flag(int r, int col) {
        if (Over() || !In(r, col) || c[r][col].revealed) return;
        c[r][col].flagged = !c[r][col].flagged;
        flags += c[r][col].flagged ? 1 : -1;
    }

    // Number with exactly that many flags around it opens the rest (wrong flags lose).
    void Chord(int r, int col, uint32_t seed) {
        if (Over() || !In(r, col) || !c[r][col].revealed || c[r][col].adj == 0) return;
        int f = 0;
        for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++)
            if (In(r + dr, col + dc) && c[r + dr][col + dc].flagged) f++;
        if (f != c[r][col].adj) return;
        for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++)
            Reveal(r + dr, col + dc, seed);
    }
};

} // namespace MsGame

// ---- draw side (minesweeper.cpp) ------------------------------------------------------------
struct ImFont;
struct ImVec2;

// Saved settings the panel reads and writes back (tabs pick the difficulty, X closes it).
struct MsSettings {
    bool* open;
    bool* night;   // minesweeper.online night skin, off = classic xp skin
    int*  diff;    // 0..2 presets, 3 = custom
    int*  width;
    int*  height;
    int*  mines;
};

// Call inside the menu's style scope, after the menu window: reads the menu style colours +
// pushed regular font. input = false while the menu fades (drawn, not clickable).
void Minesweeper_Draw(const MsSettings& s, ImFont* bold, ImFont* digits, bool input, const ImVec2& default_pos);
const char* Minesweeper_WindowName();
