#pragma once
#include <cstdint>
#include <string>

namespace wv2 {

// BGRA, top-down. Called on a capture thread; copy out, don't keep the pointer.
using FrameSink = void (*)(const uint8_t* bgra, int w, int h, int pitch);
// Called on the engine thread before every top-level navigation; true = cancel it.
using NavFilter = bool (*)(const char* url);

enum EvType {
    EV_READY,
    EV_FAILED,
    EV_URL,
    EV_FINISHED,
    EV_TITLE,
    EV_CLEARED,
    EV_CRASHED,
    EV_JS,      // botox: ExecJsResult done, a = result json, n = tag
};
struct Event { EvType type; std::string a, b; int n = 0; };

enum Mouse { M_MOVE, M_LDOWN, M_LUP, M_RDOWN, M_RUP, M_WHEEL };
enum Key   { K_DOWN, K_UP, K_CHAR };

bool RuntimeVersion(char* out, size_t cap);
bool Start(int w, int h, FrameSink sink, NavFilter filter, const char* extraArgs = nullptr);
void Stop();
void AbandonForExit();

void SetCaptureEnabled(bool on);

void Navigate(const char* url);
void ExecJs(const char* js);
void ExecJsResult(const char* js, int tag);
void AddStartScript(const char* js);
void Resize(int w, int h);
void MouseInput(Mouse m, int x, int y, int wheelDelta = 0);
enum { KMOD_ALT = 1, KMOD_CTRL = 2, KMOD_SHIFT = 8 };
void KeyInput(Key k, uint32_t code, int mods = 0);
void Back();
void Forward();
void Reload();
void ClearData();

bool PollEvent(Event& out);
std::string Diag();

// botox: botox network screens, one browser each, slot 1..k_peers. the calls above = slot 0 (own screen)
constexpr int k_peers = 4;
using PeerSink = void (*)(int slot, const uint8_t* bgra, int w, int h, int pitch);
bool PeerStart(int slot, int w, int h, PeerSink sink, NavFilter filter);   // main thread, after RuntimeVersion
void PeerStop(int slot);                                                    // joins its engine thread
void PeerNavigate(int slot, const char* url);
void PeerExecJs(int slot, const char* js);
void PeerAddStartScript(int slot, const char* js);
void PeerResize(int slot, int w, int h);
bool PeerPollEvent(int slot, Event& out);

}
