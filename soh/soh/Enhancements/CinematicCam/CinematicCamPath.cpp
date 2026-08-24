#include "CinematicCamPath.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilderGetNode: verify a saved dock node still exists before re-docking
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <clocale>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <array>
#include <ctime>
#include <cstdarg>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "CinematicCamBridge.h"

// C bridge into the free camera (z_camera.c).
extern "C" {
void CinematicCam_GetPose(float* eye, float* at, float* roll, float* fov);
void CinematicCam_SetPlayback(int active, float* eye, float* at, float roll, float fov);
int CinematicCam_WorldToNdc(float* world, float* outNdcX, float* outNdcY);
int CinematicCam_GetPlayerPos(float* out);
}

// In-window hint text that WRAPS at the window edge (TextDisabled draws one long unwrapped line, so the longer
// help texts ran off screen). Every disabled-color hint/label in this editor goes through here.
static void CineHint(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    va_end(args);
}

// Tooltip that WRAPS. ImGui::SetTooltip never wraps, so the longer help texts ran off screen; every tooltip in
// this editor goes through here instead.
static void CineTooltip(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    va_end(args);
}

// ---------------------------------------------------------------------------
// Perf diagnostic (hunt the UI freeze)
// ---------------------------------------------------------------------------
// PlaybackTick() runs every frame via the camera hook, so the wall-clock gap between consecutive ticks is the
// true frame period (game logic + render + present). We separately time our own editor draw, the world overlay
// and the letterbox update, so each frame can be split into "our code" vs "engine / GPU". When enabled
// (CinematicCam.PerfDiag), a corner HUD shows it live and every spike over the threshold is logged with the
// current state - so when it freezes, we can see WHETHER it's us and WHAT the game was doing.
static double CineNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static bool sPerfOn = false;        // CVar mirror, refreshed each tick
static double sPerfTickPrev = 0.0;  // timestamp of the previous PlaybackTick (frame boundary)
static double sPerfFrameMs = 0.0;   // last frame period
static double sPerfPeakMs = 0.0;    // rolling max frame period (reset ~every 2 s)
static double sPerfPeakAt = 0.0;    // when the current peak window started
static double sPerfDrawMs = 0.0;    // last DrawElement() duration (includes the world overlay)
static double sPerfOverlayMs = 0.0; // last DrawWorldOverlay() duration (a subset of draw)
static double sPerfUpdateMs = 0.0;  // last UpdateElement() duration (letterbox / grid / HUD)
static int sPerfSpikeCount = 0;     // spikes logged since enable (also shown on the HUD)

// Draw heartbeat: UpdateElement() runs EVERY frame (unconditionally), but DrawElement() only runs when the
// window's ImGui::Begin returns true. If the window visually "freezes" while the rest of the UI is fine, this
// tells us which layer: if sDrawStaleMs climbs (DrawElement skipped while UpdateElement keeps ticking) the
// window's Begin is returning false; if DrawElement keeps running yet the screen is stale, it's viewport present.
static uint64_t sUpdateCalls = 0;      // ++ each UpdateElement (every frame, regardless of Begin)
static uint64_t sDrawCalls = 0;        // ++ each DrawElement (only when the window content is actually drawn)
static uint64_t sLastDrawAtUpdate = 0; // sUpdateCalls value when DrawElement last ran
static double sLastDrawTimeMs = 0.0;   // wall-clock of the last DrawElement
static double sDrawStaleMs = 0.0;      // time since the last DrawElement (climbs while the window isn't redrawing)
static double sWorstDrawStaleMs = 0.0; // worst redraw gap observed since enable

// ---------------------------------------------------------------------------
// Compact / single-monitor UI mode
// ---------------------------------------------------------------------------
// The full editor competes with the game for the screen (especially on a single monitor), so it can collapse
// into a slim "shooting bar" (transport controls only) while the world gizmos + keyboard shortcuts carry the
// actual shot work. Always available via "Minimize to bar"; Play also enters it automatically by default
// (CinematicCam.AutoBarOnPlay) so takes are judged on a clean frame.
static bool sBarMode = false;                    // editor currently collapsed to the shooting bar
static bool sAutoBar = false;                    // the bar was entered automatically by Play (restore on stop)
static int sBarSizeFrames = 0;                   // frames remaining to force the window size after a mode switch
static unsigned int sSavedDockId = 0;            // dock node to return to when expanding (0 = was floating)
static float sSavedSize[2] = { 440.0f, 600.0f }; // floating window size to restore on expand
static int sPendingDock = 0;                     // one-shot: 1 = undock into the bar, 2 = re-dock on expand

static void EnterBarMode(bool automatic) {
    sBarMode = true;
    sAutoBar = automatic;
    sPendingDock = 1; // float free of any dock so the game reclaims the screen
    sBarSizeFrames = 2;
    CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.BarMode"), 1); // persisted: reopen as the bar next session
    CVarSave();
}
static void ExitBarMode() {
    sBarMode = false;
    sAutoBar = false;
    sPendingDock = 2; // return to the saved dock (or restore the floating size)
    sBarSizeFrames = 2;
    CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.BarMode"), 0);
    CVarSave();
}

// ---------------------------------------------------------------------------
// Path state
// ---------------------------------------------------------------------------
static std::vector<CineKeyframe> sKeyframes;
static int sNextId = 1;
static std::vector<int> sIds; // parallel to sKeyframes, stable identity for selection across re-sorts
static int sSelectedId = -1;
static std::vector<int> sSelection; // all selected keyframe ids (sSelectedId is the "primary" of these)
static bool sPlaying = false;
static bool sPreview = false;
static bool sLoop = false;
static int sLoopMode = 0;             // 0 = forward (wrap back to start), 1 = ping-pong (reverse each pass)
static int sPlayDir = 1;              // current ping-pong direction (+1 forward, -1 reverse)
static bool sLoopMarkerFrame = false; // true for the one frame the loop restarts (drives the GIF loop marker)
static float sLoopReturnTime = 2.0f;  // seconds to glide from the last keyframe back to the first when looping
static float sPlayhead = 0.0f;        // seconds
static float sPlaySpeed = 1.0f;
static char sFilename[64] = "path1";
static int sPathEntrance = -1;      // entrance (scene + spawn) bound to the loaded path, or -1 = none
static bool sBindLocation = true;   // capture the current location into the file when saving
static bool sTeleportOnLoad = true; // warp to the bound location when loading a path
static char sSpectateName[64] = ""; // display name of the spectated actor
static bool sHookRegistered = false;

// --- Parameter automation tracks -----------------------------------------------------------------------------
// A track holds keyframes for one exposed parameter on its own sub-timeline. To add another keyframable
// parameter: declare a CineParamTrack, add it to AllTrackDefs() with an apply hook, and place a
// DrawParamKeyNav() inline next to that parameter's own widget. The registry then wires it into the dope sheet,
// curve editor, save/load, playback and undo automatically.
struct CineParamTrack {
    const char* id;                 // stable key for save/load
    int interp;                     // CineTrackInterp (step for discrete params)
    bool enabled;                   // when true, the track drives its parameter during playback/preview
    std::vector<CineParamKey> keys; // sorted ascending by time
};

// Green screen: 0 = off, 1 = green, 2 = blue (matches the CinematicCam.GreenScreen CVar). Stepped (discrete).
static CineParamTrack sGreenScreenTrack = { "greenScreen", CINE_TRACK_STEP, false, {} };
static int sGreenScreenOverride = -1; // value forced by the track this frame, or -1 = none (CVar applies)

// A key's effective interpolation mode (per-key, falling back to the track default when unset).
static int KeyInterp(const CineParamTrack& t, int i) {
    int m = t.keys[i].interp;
    return (m < 0) ? t.interp : m;
}

// --- Bezier handle helpers --------------------------------------------------------------------------------
// Default (auto-smooth) tangent handles for key i: a Catmull-Rom-style slope, handle length 1/3 of the adjacent
// segment in time. Used to seed handles when a key is switched to Bezier (so the shape doesn't jump) and as a
// fallback when a key's handles are unset.
static void DefaultBezierHandles(const CineParamTrack& t, int i, float& outT, float& outV, float& inT, float& inV) {
    int n = (int)t.keys.size();
    const CineParamKey& k = t.keys[i];
    float prevT = (i > 0) ? t.keys[i - 1].time : k.time;
    float prevV = (i > 0) ? t.keys[i - 1].value : k.value;
    float nextT = (i < n - 1) ? t.keys[i + 1].time : k.time;
    float nextV = (i < n - 1) ? t.keys[i + 1].value : k.value;
    float span = nextT - prevT;
    float slope = (span > 1e-5f) ? (nextV - prevV) / span : 0.0f;
    float segNext = (i < n - 1) ? (t.keys[i + 1].time - k.time) : (k.time - prevT);
    float segPrev = (i > 0) ? (k.time - t.keys[i - 1].time) : (nextT - k.time);
    outT = segNext / 3.0f;
    outV = slope * outT;
    inT = -segPrev / 3.0f;
    inV = slope * inT;
}
// Effective handles for a key (stored if set, else default-derived).
static void GetBezierHandles(const CineParamTrack& t, int i, float& outT, float& outV, float& inT, float& inV) {
    const CineParamKey& k = t.keys[i];
    if (k.hasHandles) {
        outT = k.hOutT;
        outV = k.hOutV;
        inT = k.hInT;
        inV = k.hInV;
    } else {
        DefaultBezierHandles(t, i, outT, outV, inT, inV);
    }
}
// Solve for the Bezier parameter u in [0,1] where the curve's TIME coordinate equals `x`. With the control points'
// time coordinates kept inside [x0,x3] (we clamp the handles), time is monotonic in u, so bisection is robust.
static float BezierSolveU(float x, float x0, float x1, float x2, float x3) {
    float lo = 0.0f, hi = 1.0f;
    for (int it = 0; it < 26; it++) {
        float u = 0.5f * (lo + hi);
        float omu = 1.0f - u;
        float bx = omu * omu * omu * x0 + 3.0f * omu * omu * u * x1 + 3.0f * omu * u * u * x2 + u * u * u * x3;
        if (bx < x) {
            lo = u;
        } else {
            hi = u;
        }
    }
    return 0.5f * (lo + hi);
}
static float BezierEval1(float u, float p0, float p1, float p2, float p3) {
    float omu = 1.0f - u;
    return omu * omu * omu * p0 + 3.0f * omu * omu * u * p1 + 3.0f * omu * u * u * p2 + u * u * u * p3;
}
// Evaluate a Bezier segment between keys a (index i) and b (index i+1) at absolute time `time`.
static float EvalBezierSegment(const CineParamTrack& t, int i, float time) {
    const CineParamKey& a = t.keys[i];
    const CineParamKey& b = t.keys[i + 1];
    float segDur = b.time - a.time;
    float aOutT, aOutV, aInT, aInV, bOutT, bOutV, bInT, bInV;
    GetBezierHandles(t, i, aOutT, aOutV, aInT, aInV);
    GetBezierHandles(t, i + 1, bOutT, bOutV, bInT, bInV);
    // Keep the control points' time inside the segment so time stays monotonic (a proper function of time).
    float x1 = a.time + std::min(std::max(aOutT, 0.0f), segDur);
    float x2 = b.time + std::max(std::min(bInT, 0.0f), -segDur);
    float u = BezierSolveU(time, a.time, x1, x2, b.time);
    return BezierEval1(u, a.value, a.value + aOutV, b.value + bInV, b.value);
}

// Evaluate the segment LEAVING key i (keys[i] -> keys[i+1]) at absolute `time` (assumed within the segment),
// honoring that key's interpolation: step (hold), linear, smooth (uniform Catmull-Rom through the neighbors) or
// Bezier. Split out of EvalParamTrack so callers that already know the segment (the curve-editor's per-segment
// render subsampling) don't re-search the key list for every sample.
static float EvalTrackSegment(const CineParamTrack& t, size_t i, float time) {
    const CineParamKey& a = t.keys[i];
    const CineParamKey& b = t.keys[i + 1];
    float d = b.time - a.time;
    float s = (d > 1e-5f) ? (time - a.time) / d : 0.0f;
    int mode = KeyInterp(t, (int)i);
    if (mode == CINE_TRACK_STEP) {
        return a.value;
    }
    if (mode == CINE_TRACK_BEZIER) {
        return EvalBezierSegment(t, (int)i, time);
    }
    if (mode == CINE_TRACK_SMOOTH) {
        float p0 = (i > 0) ? t.keys[i - 1].value : a.value;
        float p1 = a.value;
        float p2 = b.value;
        float p3 = (i + 2 < t.keys.size()) ? t.keys[i + 2].value : b.value;
        float s2 = s * s, s3 = s2 * s;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * s + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * s2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * s3);
    }
    return a.value + (b.value - a.value) * s; // linear
}

// Evaluate a track at a time. Returns false (no value) when the track is disabled or empty. Clamps before the
// first / after the last key. The segment uses the LEFT key's interpolation.
static bool EvalParamTrack(const CineParamTrack& t, float time, float& out) {
    if (!t.enabled || t.keys.empty()) {
        return false;
    }
    if (time <= t.keys.front().time) {
        out = t.keys.front().value;
        return true;
    }
    if (time >= t.keys.back().time) {
        out = t.keys.back().value;
        return true;
    }
    size_t i = 0;
    while (i + 1 < t.keys.size() && t.keys[i + 1].time <= time) {
        i++;
    }
    out = EvalTrackSegment(t, i, time);
    return true;
}

// Seed key i's handles from the auto-smooth default and mark them explicit (called when switching a key to Bezier
// so its shape starts matching the previous Smooth curve, then becomes hand-editable).
static void SeedBezierHandles(CineParamKey& k, const CineParamTrack& t, int i) {
    DefaultBezierHandles(t, i, k.hOutT, k.hOutV, k.hInT, k.hInV);
    k.hasHandles = 1;
}

static int sNextParamKeyId = 1; // monotonic; assigns CineParamKey::id so multi-selection survives re-sorting

// Insert (or overwrite a near-coincident) key, keeping the track sorted by time. New keys inherit the track's
// default interpolation (interp = -1) and a fresh stable id. Returns the id of the added/updated key.
static int TrackAddKey(CineParamTrack& t, float time, float value) {
    for (CineParamKey& k : t.keys) {
        if (std::fabs(k.time - time) < 1e-3f) {
            k.value = value;
            return k.id;
        }
    }
    int id = sNextParamKeyId++;
    t.keys.push_back({ time, value, -1, id });
    std::sort(t.keys.begin(), t.keys.end(),
              [](const CineParamKey& a, const CineParamKey& b) { return a.time < b.time; });
    return id;
}

// --- Parameter-key multi-selection (parity with the camera-keyframe selection). A selected key is identified by
// (track, id) so it survives retiming / re-sorting. Shared by the curve editor and the dope sheet. ---
struct ParamKeyRef {
    CineParamTrack* track;
    int id;
};
static std::vector<ParamKeyRef> sParamSel; // selected parameter keys, across tracks (last entry = most recent)

static int ParamKeyIndexById(const CineParamTrack* t, int id) {
    for (int i = 0; i < (int)t->keys.size(); i++) {
        if (t->keys[i].id == id) {
            return i;
        }
    }
    return -1;
}
static bool IsParamKeySel(const CineParamTrack* t, int id) {
    for (const ParamKeyRef& r : sParamSel) {
        if (r.track == t && r.id == id) {
            return true;
        }
    }
    return false;
}
static int ParamSelCountInTrack(const CineParamTrack* t) {
    int n = 0;
    for (const ParamKeyRef& r : sParamSel) {
        if (r.track == t) {
            n++;
        }
    }
    return n;
}
static void ParamSelClear() {
    sParamSel.clear();
}
static void ParamSelOnly(CineParamTrack* t, int id) {
    sParamSel.clear();
    sParamSel.push_back({ t, id });
}
static void ParamSelToggle(CineParamTrack* t, int id) {
    for (size_t i = 0; i < sParamSel.size(); i++) {
        if (sParamSel[i].track == t && sParamSel[i].id == id) {
            sParamSel.erase(sParamSel.begin() + i);
            return;
        }
    }
    sParamSel.push_back({ t, id });
}
// Drop selected refs whose key no longer exists (after deletes / loads).
static void PruneParamSel() {
    sParamSel.erase(std::remove_if(sParamSel.begin(), sParamSel.end(),
                                   [](const ParamKeyRef& r) { return ParamKeyIndexById(r.track, r.id) < 0; }),
                    sParamSel.end());
}
// Erase every selected parameter key (across all their tracks), then clear the selection. Caller does PushUndo.
static void DeleteSelectedParamKeys() {
    for (const ParamKeyRef& r : sParamSel) {
        int idx = ParamKeyIndexById(r.track, r.id); // re-find by id each time (earlier erases shift indices)
        if (idx >= 0) {
            r.track->keys.erase(r.track->keys.begin() + idx);
        }
    }
    sParamSel.clear();
}

// Bridge for z_play.c: the green screen mode to actually render this frame - the track's value while a cinematic
// drives it, otherwise the manual CVar (the editor dropdown).
extern "C" int CinematicCam_GetGreenScreen(void) {
    if (sGreenScreenOverride >= 0) {
        return sGreenScreenOverride;
    }
    return CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.GreenScreen"), 0);
}

// Time of day: 0..65535 (0 = midnight). Continuous, so it interpolates (linear ramps between keys).
static CineParamTrack sTodTrack = { "timeOfDay", CINE_TRACK_LINEAR, false, {} };

// Discrete-value palette for dope-sheet coloring (index = value).
static const ImU32 kGsPalette[3] = { IM_COL32(150, 150, 150, 255), IM_COL32(40, 200, 90, 255),
                                     IM_COL32(60, 120, 230, 255) };

// Camera shake intensity: a multiplier on the shake amplitudes (0 = still, 1 = the sliders' value, up to 2x).
static CineParamTrack sShakeTrack = { "shake", CINE_TRACK_LINEAR, false, {} };
static float sShakeIntensity = 1.0f; // runtime multiplier applied to the shake amps this frame

// Hide HUD (0 = show, 1 = hide), to reveal/hide the HUD over a shot. Stepped (discrete).
static CineParamTrack sHudTrack = { "hideHud", CINE_TRACK_STEP, false, {} };
static int sHudHideOverride = -1; // 0/1 forced by the track this frame, or -1 = none (the HideHud setting applies)
static const ImU32 kHudPalette[2] = { IM_COL32(90, 90, 95, 255), IM_COL32(230, 170, 60, 255) };

// The shared movable aim target (used by "look at target" keyframes and the aim override). Declared here so the
// target-animation tracks below can drive it; its UI lives in the aim-override / per-keyframe sections.
static float sAimOverridePoint[3] = { 0.0f, 0.0f, 0.0f };

// Aim target position (X/Y/Z), continuous. When enabled, the movable target follows these curves over the shot.
static CineParamTrack sTargetXTrack = { "targetX", CINE_TRACK_LINEAR, false, {} };
static CineParamTrack sTargetYTrack = { "targetY", CINE_TRACK_LINEAR, false, {} };
static CineParamTrack sTargetZTrack = { "targetZ", CINE_TRACK_LINEAR, false, {} };

// Camera roll / FOV as their OWN automation tracks: independent keys on their own sub-timeline, fully editable
// in the curve editor (retime, per-key interpolation, Bezier handles). When enabled they OVERRIDE the camera
// keyframes' interpolated roll / FOV during playback; the keyframes keep their values for when the track is off.
static CineParamTrack sRollTrack = { "camRoll", CINE_TRACK_SMOOTH, false, {} };
static CineParamTrack sFovTrack = { "camFov", CINE_TRACK_SMOOTH, false, {} };
static float sRollTrackVal = 0.0f;
static int sRollTrackOn = 0; // value forced by the track this frame (reset each tick)
static float sFovTrackVal = 0.0f;
static int sFovTrackOn = 0;

// Letterbox amount (0..0.45): animate the cinematic bars in/out over a shot. Overrides the manual setting
// while driving (and draws the bars even if the Letterbox checkbox is off).
static CineParamTrack sLetterboxTrack = { "letterbox", CINE_TRACK_LINEAR, false, {} };
static float sLetterboxOverride = -1.0f;

// Per-track apply hooks, run during playback/preview with the track's value at the playhead.
static void ApplyGreenScreen(float v) {
    sGreenScreenOverride = (int)(v + 0.5f);
}
static void ApplyTimeOfDay(float v) {
    CinematicCam_SetDayTime((int)(v + 0.5f));
}
static void ApplyShakeIntensity(float v) {
    sShakeIntensity = v;
}
static void ApplyHud(float v) {
    sHudHideOverride = (v > 0.5f) ? 1 : 0;
}
static void ApplyTargetX(float v) {
    sAimOverridePoint[0] = v;
}
static void ApplyTargetY(float v) {
    sAimOverridePoint[1] = v;
}
static void ApplyTargetZ(float v) {
    sAimOverridePoint[2] = v;
}
static void ApplyCamRoll(float v) {
    sRollTrackVal = v;
    sRollTrackOn = 1;
}
static void ApplyCamFov(float v) {
    sFovTrackVal = v;
    sFovTrackOn = 1;
}
static void ApplyLetterbox(float v) {
    sLetterboxOverride = std::min(std::max(v, 0.0f), 0.45f);
}

// Registry of every keyframable parameter. Listing a parameter here makes it appear on the dope sheet and flow
// through save/load, playback and undo automatically; its inline keyframe control sits next to its own widget.
struct TrackDef {
    CineParamTrack* track;
    const char* name;     // dope-sheet lane label
    bool continuous;      // ramp (continuous) vs stepped (discrete) rendering
    float vmin, vmax;     // value range, for the continuous lane ramp
    const ImU32* palette; // discrete value colors (null when continuous)
    int palCount;
    void (*apply)(float v); // applied to the live parameter during playback
};
static const std::vector<TrackDef>& AllTrackDefs() {
    static const std::vector<TrackDef> defs = {
        { &sGreenScreenTrack, "Green scr", false, 0.0f, 0.0f, kGsPalette, 3, &ApplyGreenScreen },
        { &sTodTrack, "Time of day", true, 0.0f, 65535.0f, nullptr, 0, &ApplyTimeOfDay },
        { &sShakeTrack, "Shake", true, 0.0f, 2.0f, nullptr, 0, &ApplyShakeIntensity },
        { &sHudTrack, "Hide HUD", false, 0.0f, 0.0f, kHudPalette, 2, &ApplyHud },
        { &sTargetXTrack, "Target X", true, 0.0f, 0.0f, nullptr, 0, &ApplyTargetX }, // 0,0 = auto-range
        { &sTargetYTrack, "Target Y", true, 0.0f, 0.0f, nullptr, 0, &ApplyTargetY },
        { &sTargetZTrack, "Target Z", true, 0.0f, 0.0f, nullptr, 0, &ApplyTargetZ },
        { &sRollTrack, "Cam roll", true, 0.0f, 0.0f, nullptr, 0, &ApplyCamRoll }, // auto-range: barrel rolls expand
        { &sFovTrack, "Cam FOV", true, 1.0f, 120.0f, nullptr, 0, &ApplyCamFov },
        { &sLetterboxTrack, "Letterbox", true, 0.0f, 0.45f, nullptr, 0, &ApplyLetterbox },
    };
    return defs;
}

static int sEaseMode = 1;        // playback timing easing: 0 none, 1 in/out, 2 in, 3 out
static float sEaseAmount = 0.5f; // 0 = linear, 1 = full ease
static float sPlayU = 0.0f;      // linear play progress 0..1, eased into the playhead

// Camera shake / handheld: smooth pseudo-noise applied to the played-back pose for organic motion.
static bool sShakeEnabled = false;
static float sShakePosAmp = 3.0f;   // world units of positional jitter
static float sShakeRotAmp = 0.6f;   // degrees of aim jitter
static float sShakeFreq = 6.0f;     // wobbles per second
static bool sShakeOnPreview = true; // also shake while scrubbing/previewing (not just Play)

// Path-level aim override: when set, every keyframe aims at this target instead of its own.
static int sAimOverride = 0; // 0 none, 1 Link, 2 point, 3 actor, 4 rail (follow the travel direction)
// sAimOverridePoint (the movable target) is declared earlier, next to its animation tracks.
static int sAimOverrideActorId = 0;
static void* sAimOverrideActorPtr = nullptr;

// Path follow: translate the whole played-back path so it tracks a moving target (Link or an actor). The eye
// and look-at are offset by how far the target has moved from where the path was built around it.
static int sFollowMode = 0; // 0 off, 1 Link, 2 actor
static int sFollowActorId = 0;
static void* sFollowActorPtr = nullptr;
static float sFollowOrigin[3] = { 0.0f, 0.0f, 0.0f }; // target position the path was authored around

static bool sRecording = false; // recording the live freecam into keyframes
static float sRecordTime = 0.0f;
static float sRecordLast = 0.0f;
static float sRecordInterval = 0.2f; // seconds between recorded keyframes

static CineKeyframe sClipboard; // copied keyframe
static bool sClipboardValid = false;
static bool sShowPath = true;    // draw the spline + markers in the world while the editor is open
static bool sShowFields = false; // show numeric position/rotation fields for the selected keyframe

// Transform gizmo state for the selected keyframe.
enum GizmoMode { GIZMO_MOVE = 0, GIZMO_ROTATE = 1, GIZMO_BEND = 2 };
static int sGizmoMode = GIZMO_MOVE;
// Which side of the keyframe the Bend gizmo edits: 0 = both (mirrored / rigid), 1 = out only (the curve
// leaving toward the next keyframe), 2 = in only (the curve arriving from the previous one). Editing a single
// side "breaks" the handle so the path can turn a shaped corner through the keyframe.
static int sBendSide = 0;
static int sDragKfId = -1; // keyframe id currently being manipulated by the gizmo, or -1
static int sDragKind = 0;  // 0 = translate, 1 = rotate (snapshot of mode at grab time)
static int sDragAxis = -1; // which axis/ring: 0=X/yaw, 1=Y/pitch, 2=Z/roll
static float sRotPrevAngle = 0.0f;
static float sDragPitchAxis[3] = { 1.0f, 0.0f, 0.0f }; // pitch rotation axis, captured at drag start (stable)
static int sTargetDragId = -1;                         // keyframe id whose look-at-point target is being dragged, or -1
static int sTargetDragAxis = -1;                       // which world axis (0/1/2) of the target is being dragged

// Undo / redo history of the whole keyframe list plus the automation tracks.
struct PathSnapshot {
    std::vector<CineKeyframe> kf;
    std::vector<int> ids;
    int selectedId;
    std::vector<int> selection;
    std::vector<std::vector<CineParamKey>> tracks; // keys of every automation track, in AllTrackDefs() order
    std::vector<char> trackOn;                     // each track's enabled flag (parallel to `tracks`)
};
static std::vector<PathSnapshot> sUndo;
static std::vector<PathSnapshot> sRedo;

static PathSnapshot MakeSnapshot() {
    PathSnapshot s{ sKeyframes, sIds, sSelectedId, sSelection, {}, {} };
    for (const TrackDef& d : AllTrackDefs()) {
        s.tracks.push_back(d.track->keys);
        s.trackOn.push_back(d.track->enabled ? 1 : 0);
    }
    return s;
}
static void RestoreSnapshot(const PathSnapshot& s) {
    sKeyframes = s.kf;
    sIds = s.ids;
    sSelectedId = s.selectedId;
    sSelection = s.selection;
    const std::vector<TrackDef>& defs = AllTrackDefs();
    for (size_t i = 0; i < defs.size() && i < s.tracks.size(); i++) {
        defs[i].track->keys = s.tracks[i];
        if (i < s.trackOn.size()) {
            defs[i].track->enabled = s.trackOn[i] != 0;
        }
    }
}

// Assumed game logic tick rate; playback advances this many seconds per OnCameraState call.
static const float kTickSeconds = 1.0f / 20.0f;

// --- Precision aids: axis lock + grid snap ("magnet") ------------------------------------------------------
// Shared by BOTH editable graphs (the timeline and the curve editor) on purpose: the same two keys, the same
// grid steps, the same toggle. Two graphs that snapped by different rules would be two things to learn and two
// places to fix.
//
enum CineAxisLock { kAxisFree = 0, kAxisTime = 1, kAxisValue = 2 };

// Hold Q during a drag and it is constrained to ONE axis: whichever way it has travelled furthest at the
// moment Q goes down. That axis then stays put until Q is released, so the constraint cannot flicker between
// the two while you keep moving.
//
// The comparison is in PIXELS, never in units: the two axes share no scale whatsoever (a roll track is
// hundreds of degrees across a handful of seconds), so a units comparison would pick the same axis every
// time regardless of what the hand did.
//
// Why Q and not a modifier: Ctrl / Shift / Alt are all read at grab time in these graphs (multi-select,
// ripple, break-handle). And why not the mnemonic letters - X, H, T, V-for-value are wanted here, but X, C,
// E, R, Z, T, G, F, H and W/A/S/D are SoH's DEFAULT keyboard buttons, so holding one in the moment before
// the mouse goes down would fire it at Link. Q is bound to nothing, in this window or in the game.
static int AxisLockUpdate(int& latch, float dxPx, float dyPx) {
    if (ImGui::GetIO().WantTextInput || !ImGui::IsKeyDown(ImGuiKey_Q)) {
        latch = kAxisFree;
        return kAxisFree;
    }
    if (latch == kAxisFree) {
        float ax = std::fabs(dxPx), ay = std::fabs(dyPx);
        if (std::max(ax, ay) < 3.0f) {
            return kAxisFree; // hasn't moved far enough yet to say which way you meant
        }
        latch = (ax >= ay) ? kAxisTime : kAxisValue;
    }
    return latch;
}

static bool SnapEnabled() {
    return CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SnapGrid"), 0) != 0;
}

// A "nice" grid step for a visible span: 1, 2 or 5 times a power of ten, whichever gets closest to `want`
// divisions. Deriving it from the VISIBLE span (not the whole timeline) is what makes the magnet useful -
// zoom in and the grid refines with you, which is the whole point of snapping while working close.
static float NiceStep(float span, int want) {
    if (!(span > 0.0f) || want < 1) {
        return 0.0f;
    }
    float raw = span / (float)want;
    float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    float n = raw / mag;
    float m = (n < 1.5f) ? 1.0f : (n < 3.5f) ? 2.0f : (n < 7.5f) ? 5.0f : 10.0f;
    return m * mag;
}

// How many notches off the automatic step the user has asked for (- = finer, + = coarser). Persisted.
static int SnapLevel() {
    return std::min(std::max(CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SnapLevel"), 0), -6), 6);
}

// Move a step `notches` rungs along the 1-2-5 ladder (1, 2, 5, 10, 20, 50, ...). The grid stays on round
// numbers at every setting, which is the whole reason to have a grid: a step of span/17 would put the lines
// at values nobody would ever choose to type.
static float LadderStep(float base, int notches) {
    if (!(base > 0.0f)) {
        return base;
    }
    static const float kMult[3] = { 1.0f, 2.0f, 5.0f };
    float k = std::floor(std::log10(base));
    float m = base / std::pow(10.0f, k);
    int mi = (m < 1.5f) ? 0 : (m < 3.5f) ? 1 : 2;
    int idx = (int)k * 3 + mi + notches;
    int kk = (int)std::floor(idx / 3.0f); // floor division: idx can be negative
    return kMult[idx - kk * 3] * std::pow(10.0f, (float)kk);
}

// The time grid never goes finer than one tick: a tick is the smallest time the rest of the tool moves in
// (the , / . step buttons, playback), so a grid below it would snap to times nothing else can reach.
static float SnapTimeStep(float visibleSpan) {
    return std::max(LadderStep(NiceStep(visibleSpan, 10), SnapLevel()), kTickSeconds);
}

// The value grid has no equivalent floor - a value channel's units are whatever the channel means.
static float SnapValueStep(float visibleSpan) {
    return LadderStep(NiceStep(visibleSpan, 8), SnapLevel());
}

static float SnapTo(float v, float step) {
    return (step > 1e-9f) ? std::round(v / step) * step : v;
}

// The Snap cluster, drawn by BOTH graphs from this one function so they cannot drift apart: the toggle, the
// two ladder buttons, and the step they currently land on. `vStep <= 0` means this graph snaps time only.
// `id` disambiguates the ImGui ids - two SmallButtons with the same label in one window collide, which is
// exactly the "conflicting ID" assert.
static void SnapToolbarUI(const char* id, float tStep, float vStep) {
    const bool on = SnapEnabled();
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "Snap: %s##%ssnap", on ? "on" : "off", id);
    if (ImGui::SmallButton(lbl)) {
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.SnapGrid"), on ? 0 : 1);
        CVarSave();
    }
    if (ImGui::IsItemHovered()) {
        if (vStep > 0.0f) {
            CineTooltip("Magnet: a key you DRAG lands on the grid. Selecting one never moves it.\n\n"
                        "The grid is drawn while this is on and refines as you zoom in; the - and + buttons "
                        "step it finer or coarser, always on round numbers.\n\n"
                        "Separately: hold Q while dragging to lock the drag to one axis - time or value, "
                        "whichever you were already moving along.");
        } else {
            CineTooltip("Magnet: a keyframe you DRAG lands on the time grid. Selecting one never moves it.\n\n"
                        "The grid refines as you zoom in and never goes finer than one tick; the - and + "
                        "buttons step it finer or coarser. Same setting as the curve editor's Snap, which also "
                        "snaps values.");
        }
    }
    if (!on) {
        return;
    }
    const int lvl = SnapLevel();
    auto ladder = [&](const char* txt, int delta, bool disabled, const char* tip) {
        char b[40];
        snprintf(b, sizeof(b), "%s##%ssnap%d", txt, id, delta);
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::BeginDisabled(disabled);
        if (ImGui::SmallButton(b)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.SnapLevel"), lvl + delta);
            CVarSave();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            CineTooltip("%s", tip);
        }
    };
    ladder("-", -1, lvl <= -6, "Finer grid (one step down the 1-2-5 ladder)");
    ladder("+", 1, lvl >= 6, "Coarser grid (one step up the 1-2-5 ladder)");
    ImGui::SameLine(0.0f, 5.0f);
    if (vStep > 0.0f) {
        CineHint("%g s x %g", tStep, vStep);
    } else {
        CineHint("%g s", tStep);
    }
}

static float TotalTime() {
    return sKeyframes.empty() ? 0.0f : sKeyframes.back().time;
}

// True when the path is treated as a cycle (forward loop with a return segment). Ping-pong is NOT cyclic:
// it bounces within [0, TotalTime()] instead of wrapping, so it samples with clamped ends.
static bool LoopCyclic() {
    return sLoop && sLoopMode == 0;
}

// Total timeline length including the loop-return segment when forward-looping.
static float EffectiveTotal() {
    float t = TotalTime();
    if (LoopCyclic() && sKeyframes.size() >= 2) {
        t += sLoopReturnTime;
    }
    return t;
}

// Dirty tracking: every mutating edit goes through PushUndo, so it doubles as the "unsaved changes" signal.
// sDirty drives the * marker (cleared by Save/Load); sDirtyForAutosave arms the next autosave (cleared by
// Save and by the autosave itself, so an idle editor doesn't rewrite an identical backup every minute).
static bool sDirty = false;
static bool sDirtyForAutosave = false;
static char sFileStatus[160] = ""; // last save/load/autosave result, shown in the Save/Load section
static double sLastAutosaveMs = 0.0;

static void PushUndo() {
    sUndo.push_back(MakeSnapshot());
    if (sUndo.size() > 64) {
        sUndo.erase(sUndo.begin());
    }
    sRedo.clear();
    sDirty = true;
    sDirtyForAutosave = true;
}

static void Undo() {
    if (sUndo.empty()) {
        return;
    }
    sRedo.push_back(MakeSnapshot());
    PathSnapshot s = sUndo.back();
    sUndo.pop_back();
    RestoreSnapshot(s);
}

static void Redo() {
    if (sRedo.empty()) {
        return;
    }
    sUndo.push_back(MakeSnapshot());
    PathSnapshot s = sRedo.back();
    sRedo.pop_back();
    RestoreSnapshot(s);
}

static int SelectedIndex() {
    for (size_t i = 0; i < sIds.size(); i++) {
        if (sIds[i] == sSelectedId) {
            return (int)i;
        }
    }
    return -1;
}

// --- Multi-selection helpers (sSelection holds ids; sSelectedId is the primary/last-clicked) ---
static bool IsSelected(int id) {
    return std::find(sSelection.begin(), sSelection.end(), id) != sSelection.end();
}
static int SelectionCount() {
    return (int)sSelection.size();
}
static void SelectOnly(int id) {
    sSelection.clear();
    if (id >= 0) {
        sSelection.push_back(id);
    }
    sSelectedId = id;
}
static void ToggleSelect(int id) {
    auto it = std::find(sSelection.begin(), sSelection.end(), id);
    if (it != sSelection.end()) {
        sSelection.erase(it);
        sSelectedId = sSelection.empty() ? -1 : sSelection.back();
    } else {
        sSelection.push_back(id);
        sSelectedId = id;
    }
}
// Drop ids that no longer exist (after deletes/loads) so selection stays valid.
static void PruneSelection() {
    sSelection.erase(std::remove_if(sSelection.begin(), sSelection.end(),
                                    [](int id) { return std::find(sIds.begin(), sIds.end(), id) == sIds.end(); }),
                     sSelection.end());
    if (std::find(sIds.begin(), sIds.end(), sSelectedId) == sIds.end()) {
        sSelectedId = sSelection.empty() ? -1 : sSelection.back();
    }
}

// Keep keyframes sorted by time, carrying their ids along.
static void SortByTime() {
    std::vector<size_t> order(sKeyframes.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [](size_t a, size_t b) { return sKeyframes[a].time < sKeyframes[b].time; });
    std::vector<CineKeyframe> kf(sKeyframes.size());
    std::vector<int> ids(sIds.size());
    for (size_t i = 0; i < order.size(); i++) {
        kf[i] = sKeyframes[order[i]];
        ids[i] = sIds[order[i]];
    }
    sKeyframes = std::move(kf);
    sIds = std::move(ids);
}

// Defined with the gizmo vector helpers below.
static float v3len(const float* a);
static void v3sub(const float* a, const float* b, float* o);
static void v3norm(float* a);
static float v3dot(const float* a, const float* b);
static void AutoTangentDir(int idx, float out[3]); // path travel direction at a keyframe (rail aim)

// Kochanek-Bartels (TCB) Hermite interpolation for one scalar component over a segment p1 -> p2.
// tcB = TCB params at p1 (segment source), tcC = TCB params at p2 (segment destination).
// With all params 0 this reduces exactly to Catmull-Rom.
static float Hermite1(float p1, float p2, float m0, float m1, float s) {
    float s2 = s * s;
    float s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * p1 + (s3 - 2.0f * s2 + s) * m0 + (-2.0f * s3 + 3.0f * s2) * p2 +
           (s3 - s2) * m1;
}

// Derivative of Hermite1 with respect to its parameter s (used to read the curve's tangent at a split point).
static float Hermite1Deriv(float p1, float p2, float m0, float m1, float s) {
    float s2 = s * s;
    return (6.0f * s2 - 6.0f * s) * p1 + (3.0f * s2 - 4.0f * s + 1.0f) * m0 + (-6.0f * s2 + 6.0f * s) * p2 +
           (3.0f * s2 - 2.0f * s) * m1;
}

// Quintic Hermite: like Hermite1 but the SECOND derivative is prescribed at both ends too (c0, c1), so the
// curve has value, slope and curvature control at each knot. The arc-length schedule uses it for exactly that
// reason: the slope is the camera's speed and the second derivative is its acceleration, which is what the
// speed graph's handles edit. Passing the cubic's own end accelerations (AutoAccel below) reproduces Hermite1
// to the last bit, so "no handle touched" costs nothing.
static float Hermite5(float p0, float p1, float m0, float m1, float c0, float c1, float s) {
    float s2 = s * s, s3 = s2 * s, s4 = s3 * s, s5 = s4 * s;
    return p0 * (1.0f - 10.0f * s3 + 15.0f * s4 - 6.0f * s5) + m0 * (s - 6.0f * s3 + 8.0f * s4 - 3.0f * s5) +
           c0 * 0.5f * (s2 - 3.0f * s3 + 3.0f * s4 - s5) + c1 * 0.5f * (s3 - 2.0f * s4 + s5) +
           m1 * (-4.0f * s3 + 7.0f * s4 - 3.0f * s5) + p1 * (10.0f * s3 - 15.0f * s4 + 6.0f * s5);
}
static float Hermite5Deriv(float p0, float p1, float m0, float m1, float c0, float c1, float s) {
    float s2 = s * s, s3 = s2 * s, s4 = s3 * s;
    return p0 * (-30.0f * s2 + 60.0f * s3 - 30.0f * s4) + m0 * (1.0f - 18.0f * s2 + 32.0f * s3 - 15.0f * s4) +
           c0 * 0.5f * (2.0f * s - 9.0f * s2 + 12.0f * s3 - 5.0f * s4) +
           c1 * 0.5f * (3.0f * s2 - 8.0f * s3 + 5.0f * s4) + m1 * (-12.0f * s2 + 28.0f * s3 - 15.0f * s4) +
           p1 * (30.0f * s2 - 60.0f * s3 + 30.0f * s4);
}

static float Hermite5Deriv2(float p0, float p1, float m0, float m1, float c0, float c1, float s) {
    float s2 = s * s, s3 = s2 * s;
    return p0 * (-60.0f * s + 180.0f * s2 - 120.0f * s3) + m0 * (-36.0f * s + 96.0f * s2 - 60.0f * s3) +
           c0 * 0.5f * (2.0f - 18.0f * s + 36.0f * s2 - 20.0f * s3) + c1 * 0.5f * (6.0f * s - 24.0f * s2 + 20.0f * s3) +
           m1 * (-24.0f * s + 84.0f * s2 - 60.0f * s3) + p1 * (60.0f * s - 180.0f * s2 + 120.0f * s3);
}

// Does the schedule ever run backwards on this segment? The speed is Hermite5's derivative, a quartic, so a
// negative lobe can hide entirely between evenly spaced samples - sampling alone would call an unusable curve
// fine. Sample coarsely to bracket the lowest point, then ternary-search inside that bracket (a quartic is
// unimodal across two adjacent sample intervals) so the answer is about the actual minimum.
static bool SpeedStaysForward(float p0, float p1, float m0, float m1, float c0, float c1) {
    const int kN = 32;
    float lowV = 1e30f;
    int lowI = 0;
    for (int s = 0; s <= kN; s++) {
        float v = Hermite5Deriv(p0, p1, m0, m1, c0, c1, (float)s / (float)kN);
        if (v < lowV) {
            lowV = v;
            lowI = s;
        }
    }
    if (lowV < 0.0f) {
        return false;
    }
    float lo = (float)std::max(lowI - 1, 0) / (float)kN;
    float hi = (float)std::min(lowI + 1, kN) / (float)kN;
    for (int it = 0; it < 24 && hi - lo > 1e-5f; it++) {
        float a = lo + (hi - lo) / 3.0f, b = hi - (hi - lo) / 3.0f;
        if (Hermite5Deriv(p0, p1, m0, m1, c0, c1, a) < Hermite5Deriv(p0, p1, m0, m1, c0, c1, b)) {
            hi = b;
        } else {
            lo = a;
        }
    }
    return Hermite5Deriv(p0, p1, m0, m1, c0, c1, 0.5f * (lo + hi)) >= 0.0f;
}

// True when a value curve stays between its two endpoints instead of bulging past one of them. The envelope
// guarantee in one line: the view may never point somewhere neither neighbouring keyframe frames. A falling
// curve is checked by flipping it, since "never turns back" is the same question either way round.
static bool AngleStaysMonotone(float p0, float p1, float m0, float m1, float c0, float c1) {
    if (p1 >= p0) {
        return SpeedStaysForward(p0, p1, m0, m1, c0, c1);
    }
    return SpeedStaysForward(-p0, -p1, -m0, -m1, -c0, -c1);
}

// The end accelerations the plain cubic already has, in the same units as Hermite5's c0/c1. Used as the
// last-resort fallback for a segment whose asked-for shape would run backwards.
static void AutoAccel(float p0, float p1, float m0, float m1, float* c0, float* c1) {
    float d = 6.0f * (p1 - p0);
    *c0 = d - 4.0f * m0 - 2.0f * m1;
    *c1 = -d + 2.0f * m0 + 4.0f * m1;
}

// Non-uniform Kochanek-Bartels tangents for the segment p1->p2 (mOut at p1, mIn at p2), given the three knot
// intervals t01,t12,t23 (the "parameter distances" between the four control points; chord^alpha for
// uniform/centripetal/chordal, or keyframe-time differences for velocity-continuous motion). tB/cB/bB are the
// Tension/Continuity/Bias at p1 (out-tangent), tC/cC/bC at p2 (in-tangent). With all TCB params 0 this is
// exactly non-uniform Catmull-Rom, so dialing TCB away from 0 changes the curve continuously in any
// parameterization (no jump to a different uniform formula). Tangents are scaled to the [0,1] Hermite param.
// noPrev / noNext: p0 / p3 is a duplicated clamp point (no real neighbor on that side), so its zero-length
// secant must not participate in the overshoot guard - otherwise every path would be forced to start/end at rest.
static void NuKbTangents(const float* p0, const float* p1, const float* p2, const float* p3, float t01, float t12,
                         float t23, float tB, float cB, float bB, float tC, float cC, float bC, float* mOut, float* mIn,
                         bool noPrev, bool noNext) {
    if (t01 < 1e-5f) {
        t01 = 1e-5f;
    }
    if (t12 < 1e-5f) {
        t12 = 1e-5f;
    }
    if (t23 < 1e-5f) {
        t23 = 1e-5f;
    }
    float wInO = t12 / (t01 + t12), wOutO = t01 / (t01 + t12);  // interval weights, out-tangent at p1
    float wMidI = t23 / (t12 + t23), wFarI = t12 / (t12 + t23); // interval weights, in-tangent at p2
    float lIn = 0.0f, lOut = 0.0f, lFar = 0.0f;                 // secant SPEEDS (vector norms), for the guard below
    for (int k = 0; k < 3; k++) {
        float sIn = (p1[k] - p0[k]) / t01; // one-sided secant velocities
        float sOut = (p2[k] - p1[k]) / t12;
        float sFar = (p3[k] - p2[k]) / t23;
        float vOut = (1.0f - tB) * (wInO * (1.0f + cB) * (1.0f + bB) * sIn + wOutO * (1.0f - cB) * (1.0f - bB) * sOut);
        float vIn = (1.0f - tC) * (wMidI * (1.0f - cC) * (1.0f + bC) * sOut + wFarI * (1.0f + cC) * (1.0f - bC) * sFar);
        mOut[k] = vOut * t12;
        mIn[k] = vIn * t12;
        lIn += sIn * sIn;
        lOut += sOut * sOut;
        lFar += sFar * sFar;
    }
    lIn = std::sqrt(lIn);
    lOut = std::sqrt(lOut);
    lFar = std::sqrt(lFar);
    // Overshoot guard (a vector Fritsch-Carlson): cap each tangent's LENGTH at 3x the smaller of its two
    // adjacent secant speeds, keeping its direction. A velocity-continuous spline otherwise sails THROUGH a
    // keyframe at whatever speed the neighbors imply - with two coincident keyframes that literally draws a
    // full loop out and back (tangents nonzero, chord zero). The cap makes a zero-length segment an exact
    // hold, eases motion to rest INTO a hold and out of it, and tames the swing into very short segments,
    // while leaving well-proportioned paths untouched (their tangents sit far below 3x the local secants).
    float capOut = 3.0f * (noPrev ? lOut : std::min(lIn, lOut)) * t12;
    float capIn = 3.0f * (noNext ? lOut : std::min(lOut, lFar)) * t12;
    float nOut = std::sqrt(mOut[0] * mOut[0] + mOut[1] * mOut[1] + mOut[2] * mOut[2]);
    float nIn = std::sqrt(mIn[0] * mIn[0] + mIn[1] * mIn[1] + mIn[2] * mIn[2]);
    if (nOut > capOut) {
        float f = (nOut > 1e-6f) ? capOut / nOut : 0.0f;
        mOut[0] *= f;
        mOut[1] *= f;
        mOut[2] *= f;
    }
    if (nIn > capIn) {
        float f = (nIn > 1e-6f) ? capIn / nIn : 0.0f;
        mIn[0] *= f;
        mIn[1] *= f;
        mIn[2] *= f;
    }
}

// The two Hermite endpoint slopes for a scalar segment p1->p2 (non-uniform Kochanek-Bartels), already scaled
// to the [0,1] segment parameter. Split out from NuKbScalar so callers that need to OVERRIDE a slope (the aim
// channels, which expose per-keyframe tangent modes) can start from the automatic value.
static void NuKbScalarSlopes(float p0, float p1, float p2, float p3, float t01, float t12, float t23, float tB,
                             float cB, float bB, float tC, float cC, float bC, bool monotone, float* mOut, float* mIn) {
    if (t01 < 1e-5f) {
        t01 = 1e-5f;
    }
    if (t12 < 1e-5f) {
        t12 = 1e-5f;
    }
    if (t23 < 1e-5f) {
        t23 = 1e-5f;
    }
    float wInO = t12 / (t01 + t12), wOutO = t01 / (t01 + t12);
    float wMidI = t23 / (t12 + t23), wFarI = t12 / (t12 + t23);
    float sIn = (p1 - p0) / t01, sOut = (p2 - p1) / t12, sFar = (p3 - p2) / t23;
    float vOut = (1.0f - tB) * (wInO * (1.0f + cB) * (1.0f + bB) * sIn + wOutO * (1.0f - cB) * (1.0f - bB) * sOut);
    float vIn = (1.0f - tC) * (wMidI * (1.0f - cC) * (1.0f + bC) * sOut + wFarI * (1.0f + cC) * (1.0f - bC) * sFar);

    // Monotone limiting (Fritsch-Carlson): clamp each endpoint slope to the local secants so a 1D channel never
    // overshoots or bleeds past its adjacent keyframes - a big roll spike on one keyframe stays in its two
    // neighboring segments instead of rippling two keyframes out on each side. At a local extreme (the secants
    // disagree) the slope goes to zero, which is what keeps the curve from bulging past the keyframe's value.
    if (monotone) {
        if (sIn * sOut <= 0.0f) {
            vOut = 0.0f;
        } else {
            float lim = 3.0f * std::min(std::fabs(sIn), std::fabs(sOut));
            vOut = std::min(std::max(vOut, -lim), lim);
        }
        if (sOut * sFar <= 0.0f) {
            vIn = 0.0f;
        } else {
            float lim = 3.0f * std::min(std::fabs(sOut), std::fabs(sFar));
            vIn = std::min(std::max(vIn, -lim), lim);
        }
    }
    *mOut = vOut * t12;
    *mIn = vIn * t12;
}

// Scalar form of the non-uniform Kochanek-Bartels evaluation (for the 1D channels: roll and FOV), so they
// interpolate as smoothly as the eye path - no stiff/quick swing when a turn and an aim change coincide.
static float NuKbScalar(float p0, float p1, float p2, float p3, float t01, float t12, float t23, float tB, float cB,
                        float bB, float tC, float cC, float bC, float s, bool monotone) {
    float mOut, mIn;
    NuKbScalarSlopes(p0, p1, p2, p3, t01, t12, t23, tB, cB, bB, tC, cC, bC, monotone, &mOut, &mIn);
    return Hermite1(p1, p2, mOut, mIn, s);
}

// How much faster than its surroundings a keyframe may be taken - as a multiple of the smaller of the two
// average rates it sits between. Governs both value schedules: the eye's distance-over-time and the aim's
// angle-over-time.
//
// The classic Fritsch-Carlson number here is 3, and that is correct for a CUBIC. Both schedules are quintics
// now, and a quintic asked to hold a smooth (continuous-curvature) shape through such a knot cannot: take a
// symmetric segment whose two ends both run at V times its average, with the curvature levelled off at both
// ends. Its mid-segment rate works out to 1.875 - 0.875*V times the average, which goes NEGATIVE past
// V = 2.14 - the camera would have to reverse mid-segment to still arrive on time, or the view swing back on
// itself. The curve then has no choice but to abandon smoothness exactly where it was pushed hardest, which
// is what put a visible kink in the handles of keyframes dragged to their limit (and, through the knock-on
// rescaling, in their neighbours' too). Capping at 2 keeps the whole reachable range inside what a smooth
// curve can actually do.
static const float kSpeedSlopeLimit = 2.0f;

// Endpoint slopes for one aim angle (yaw or pitch) over segment b->c, in [0,1]-parameter space. Authority
// order: automatic from the neighbours -> the keyframes' baked explicit rates (exOut/exIn, from Insert) ->
// the envelope clamp -> Hold framing.
//
// `noPrev` / `noNext` mean there is no meaningful neighbour on that side (a clamped path end, a locked
// tracked aim), so this segment continues at its OWN rate instead of pretending the view was standing still
// there - which is what used to make the aim stall at those keyframes.
static void AimAngleSlopes(float p0, float p1, float p2, float p3, float t01, float t12, float t23,
                           const CineKeyframe& b, const CineKeyframe& c, bool noPrev, bool noNext, float exOut,
                           float exIn, float* mOut, float* mIn) {
    if (noPrev) {
        p0 = p1 - (p2 - p1) * (t01 / std::max(t12, 1e-5f)); // one-sided: continue this segment's own rate
    }
    if (noNext) {
        p3 = p2 + (p2 - p1) * (t23 / std::max(t12, 1e-5f));
    }
    NuKbScalarSlopes(p0, p1, p2, p3, t01, t12, t23, b.tension, b.continuity, b.bias, c.tension, c.continuity, c.bias,
                     false, mOut, mIn);
    if (b.hasAimTanOut) {
        *mOut = exOut;
    }
    if (c.hasAimTanIn) {
        *mIn = exIn;
    }
    // No clamp here. The envelope is enforced once, on the finished curve, by AimEnvelopeScale below - see
    // the note there for why four slope rules became one measurement.
    if (b.aimHold) { // "hold framing" on either end parks the view's turn there
        *mOut = 0.0f;
    }
    if (c.aimHold) {
        *mIn = 0.0f;
    }
}

// Interpolate one component over a segment with the given knot intervals, honoring the source keyframe's mode.
static float InterpComp(float p0, float p1, float p2, float p3, const CineKeyframe& kSrc, const CineKeyframe& kDst,
                        float t01, float t12, float t23, float s) {
    if (kSrc.interp == CINE_INTERP_LINEAR) {
        return p1 + (p2 - p1) * s;
    }
    return NuKbScalar(p0, p1, p2, p3, t01, t12, t23, kSrc.tension, kSrc.continuity, kSrc.bias, kDst.tension,
                      kDst.continuity, kDst.bias, s, true);
}

// Duration (seconds) of the path segment that starts at keyframe index j and runs to the next one cyclically.
// The loop-return segment (last -> first) uses sLoopReturnTime. Clamped to a small positive value. Used as the
// knot intervals for time-based (velocity-continuous) interpolation.
static float SegDurAt(int j) {
    int n = (int)sKeyframes.size();
    if (n < 2) {
        return 1.0f;
    }
    float d;
    if (j >= n - 1) {
        d = LoopCyclic() ? sLoopReturnTime : (sKeyframes[n - 1].time - sKeyframes[n - 2].time);
    } else {
        d = sKeyframes[j + 1].time - sKeyframes[j].time;
    }
    return d < 1e-4f ? 1e-4f : d;
}

// The eye-path Hermite tangents (in [0,1] segment-parameter units) for the segment i1 -> i2, exactly as
// SampleAt evaluates it: parameterization knots, TCB, the overshoot guard, and the per-keyframe direction and
// speed overrides. Shared by SampleAt and the shape-preserving insert, which needs the tangents of a segment
// and its neighbors to pin the curve before a knot is added.
static void EyeSegmentTangents(int i1, int i2, float td[3], float ts[3]) {
    int n = (int)sKeyframes.size();
    int i0, i3;
    if (LoopCyclic()) {
        i0 = ((i1 - 1) % n + n) % n;
        i3 = (i2 + 1) % n;
    } else {
        i0 = std::max(0, i1 - 1);
        i3 = std::min(n - 1, i2 + 1);
    }
    const CineKeyframe& a = sKeyframes[i0];
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[i2];
    const CineKeyframe& d = sKeyframes[i3];
    // Centripetal parameterization: knots from the square root of the chord lengths - GEOMETRY ONLY. This is
    // the classic loop/cusp-free choice, and because time plays no part here, retiming keyframes can never
    // reshape the path. Timing is applied separately as an arc-length speed schedule (see SampleAt).
    auto chordKnot = [](const float* p, const float* q) {
        float e0 = q[0] - p[0], e1 = q[1] - p[1], e2 = q[2] - p[2];
        return std::sqrt(std::sqrt(e0 * e0 + e1 * e1 + e2 * e2)); // chord^0.5
    };
    float t12 = chordKnot(b.eye, c.eye);
    float t01 = (i0 != i1) ? chordKnot(a.eye, b.eye) : t12;
    float t23 = (i2 != i3) ? chordKnot(c.eye, d.eye) : t12;
    NuKbTangents(a.eye, b.eye, c.eye, d.eye, t01, t12, t23, b.tension, b.continuity, b.bias, c.tension, c.continuity,
                 c.bias, td, ts, i0 == i1, i2 == i3);
    if (b.hasTangent) {
        float m = v3len(td);
        td[0] = b.tangent[0] * m;
        td[1] = b.tangent[1] * m;
        td[2] = b.tangent[2] * m;
    }
    // The in side of c uses its broken in-direction when set, else the shared/mirrored tangent.
    if (c.hasTangentIn || c.hasTangent) {
        const float* dir = c.hasTangentIn ? c.tangentIn : c.tangent;
        float m = v3len(ts);
        ts[0] = dir[0] * m;
        ts[1] = dir[1] * m;
        ts[2] = dir[2] * m;
    }
    // Per-side tangent WEIGHTS scale the (guarded) automatic magnitude. Relative on purpose: when a keyframe
    // moves, the auto magnitude follows the new chords and the weighted tangent scales with it - no stale
    // absolute length bulging the curve.
    if (b.tanWOut > 0.0f) {
        td[0] *= b.tanWOut;
        td[1] *= b.tanWOut;
        td[2] *= b.tanWOut;
    }
    if (c.tanWIn > 0.0f) {
        ts[0] *= c.tanWIn;
        ts[1] *= c.tanWIn;
        ts[2] *= c.tanWIn;
    }
}

// A keyframe's effective look-at point this frame: the stored point for free/point aim, or Link's live
// position for player aim.
static void EffectiveAt(int idx, float out[3]) {
    // Path-level rail override: every keyframe faces along the path's travel direction.
    if (sAimOverride == 4) {
        const CineKeyframe& kk = sKeyframes[idx];
        float td[3];
        AutoTangentDir(idx, td);
        out[0] = kk.eye[0] + td[0] * 100.0f;
        out[1] = kk.eye[1] + td[1] * 100.0f;
        out[2] = kk.eye[2] + td[2] * 100.0f;
        return;
    }
    // Path-level override aims every keyframe at one target (great for fixing up recorded paths at once).
    if (sAimOverride == 1) {
        float p[3];
        if (CinematicCam_GetPlayerPos(p)) {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            return;
        }
    } else if (sAimOverride == 2) {
        out[0] = sAimOverridePoint[0];
        out[1] = sAimOverridePoint[1];
        out[2] = sAimOverridePoint[2];
        return;
    } else if (sAimOverride == 3) {
        float p[3];
        if (CinematicCam_ResolveActor(&sAimOverrideActorPtr, (short)sAimOverrideActorId, nullptr, p)) {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            return;
        }
    }

    CineKeyframe& k = sKeyframes[idx];
    if (k.aimMode == CINE_AIM_PLAYER) {
        float p[3];
        if (CinematicCam_GetPlayerPos(p)) {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            return;
        }
    } else if (k.aimMode == CINE_AIM_ACTOR) {
        float p[3];
        // Use the saved pick position as a hint so reload re-acquires the nearest matching actor (skip the hint
        // for old keyframes that never stored one - all-zero - to preserve the previous first-match behavior).
        bool hasHint = (k.aimActorPos[0] != 0.0f) || (k.aimActorPos[1] != 0.0f) || (k.aimActorPos[2] != 0.0f);
        if (CinematicCam_ResolveActor(&k.aimActorPtr, (short)k.aimActorId, hasHint ? k.aimActorPos : nullptr, p)) {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            return;
        }
    } else if (k.aimMode == CINE_AIM_TARGET) {
        out[0] = sAimOverridePoint[0]; // shared movable aim target
        out[1] = sAimOverridePoint[1];
        out[2] = sAimOverridePoint[2];
        return;
    } else if (k.aimMode == CINE_AIM_RAIL) {
        float td[3]; // face along the rail at this keyframe (blends with neighboring free/rail aims)
        AutoTangentDir(idx, td);
        out[0] = k.eye[0] + td[0] * 100.0f;
        out[1] = k.eye[1] + td[1] * 100.0f;
        out[2] = k.eye[2] + td[2] * 100.0f;
        return;
    }
    out[0] = k.at[0];
    out[1] = k.at[1];
    out[2] = k.at[2];
}

// Two keyframes "lock" onto a single live/shared aim source when both track the same thing (the same actor,
// both Link, both the movable target) - or whenever a path-level aim override is active. While locked, the
// camera looks straight at that live point across the whole segment instead of interpolating a look-at
// position, so a moving (or off-axis) actor/target stays perfectly centered between keyframes. Without this the
// look-at spline only touches the target AT each keyframe and bows away from it in between (pulled by the
// neighboring keyframes' tangents). Free/point keyframes are authored static look-at points, so they still blend.
static bool AimLockedBetween(const CineKeyframe& b, const CineKeyframe& c) {
    if (sAimOverride == 4) {
        return false; // rail override: the aim follows the travel direction, it doesn't hold a target
    }
    if (sAimOverride != 0) {
        return true;
    }
    if (b.aimMode != c.aimMode) {
        return false;
    }
    switch (b.aimMode) {
        case CINE_AIM_PLAYER:
        case CINE_AIM_TARGET:
            return true;
        case CINE_AIM_ACTOR:
            return b.aimActorId == c.aimActorId;
        default:
            return false;
    }
}

// --- WHERE vs WHEN: the arc-length speed schedule ---------------------------------------------------------
// The spatial path is pure geometry (centripetal knots in EyeSegmentTangents) - the timeline cannot reshape
// it. Timing is layered on top: each segment's arc length is measured, and a monotone C1 cubic through the
// keyframe (time, cumulative distance) knots maps time -> distance traveled (Fritsch-Carlson slopes: speed
// glides smoothly THROUGH keyframes with no hang/snap, never runs backwards, and every keyframe is reached
// at exactly its timeline time). The eye then rides the path to that distance. Retiming a keyframe therefore
// only redistributes speed. Cached; rebuilt when PathShapeHash changes.
static uint32_t PathShapeHash(); // defined below (also used by the overlay/curve-editor caches)

static const int kArcSteps = 24; // per-segment tessellation for the length tables
struct CineArcCache {
    uint32_t hash = 0;
    int segs = 0;             // spatial segments, including the loop-return one when cyclic
    std::vector<float> cum;   // per segment: kArcSteps+1 cumulative lengths (local, 0 .. segment length)
    std::vector<float> S;     // cumulative arc length at each knot (segs+1 entries)
    std::vector<float> slope; // ds/dt at each knot, monotone-limited (world units/sec)
    // Per-SEGMENT endpoint speeds (units/sec), read off the knot slopes above. Both segments meeting at a
    // keyframe therefore use the SAME speed there: the curve cannot jump at a knot, only bend.
    std::vector<float> mOut, mIn;
    // Per-SEGMENT endpoint accelerations (units/sec^2): the automatic (cubic) value, or the keyframe's own
    // handle where it has one, scaled back if that would drive the speed negative mid-segment.
    std::vector<float> aOut, aIn;
};
static CineArcCache sArc;

// Monotone (Fritsch-Carlson) knot slopes for a cumulative-quantity-over-time schedule (arc length for the
// eye, view angle for the aim). All secants are >= 0, so limiting each knot's slope to a small multiple of
// the smaller adjacent secant guarantees the quantity never runs backwards; a zero-length segment (a hold)
// forces slope 0 on both of its ends, so motion eases to rest into it and out of it. When cyclic, knot 0 and
// the last knot are the SAME keyframe: give them the same wrapped slope so the rate glides through the loop
// seam instead of popping to a new pace each lap.
static void BuildMonotoneSlopes(const std::vector<float>& S, int segs, std::vector<float>& slope) {
    bool cyc = LoopCyclic() && segs >= 2;
    for (int i = 0; i <= segs; i++) {
        float sigPrev, sigNext;
        if (i > 0) {
            sigPrev = (S[i] - S[i - 1]) / SegDurAt(i - 1);
        } else {
            sigPrev = cyc ? (S[segs] - S[segs - 1]) / SegDurAt(segs - 1) : -1.0f;
        }
        if (i < segs) {
            sigNext = (S[i + 1] - S[i]) / SegDurAt(i);
        } else {
            sigNext = cyc ? (S[1] - S[0]) / SegDurAt(0) : -1.0f;
        }
        float m;
        if (sigPrev < 0.0f) {
            m = sigNext; // first knot, non-loop: one-sided
        } else if (sigNext < 0.0f) {
            m = sigPrev; // last knot, non-loop: one-sided
        } else if (sigPrev < 1e-6f || sigNext < 1e-6f) {
            m = 0.0f; // bordering a hold: come to rest
        } else {
            m = 0.5f * (sigPrev + sigNext);
            float lim = kSpeedSlopeLimit * std::min(sigPrev, sigNext);
            m = std::min(m, lim);
        }
        slope[i] = std::max(m, 0.0f);
    }
}

// The fastest a keyframe may be taken without breaking exactness OR smoothness - see kSpeedSlopeLimit. Past
// it the schedule would have to run backwards somewhere in one of the two segments to still arrive on time.
// Shared by the schedule and by the graph, which draws it as the ceiling line - so when a drag stops, the
// reason is on screen instead of hidden in a clamp.
static float KnotSpeedCap(const std::vector<float>& S, int segs, int i) {
    bool cyc = LoopCyclic() && segs >= 2;
    float sigPrev = -1.0f, sigNext = -1.0f;
    if (i > 0) {
        sigPrev = (S[i] - S[i - 1]) / SegDurAt(i - 1);
    } else if (cyc) {
        sigPrev = (S[segs] - S[segs - 1]) / SegDurAt(segs - 1);
    }
    if (i < segs) {
        sigNext = (S[i + 1] - S[i]) / SegDurAt(i);
    } else if (cyc) {
        sigNext = (S[1] - S[0]) / SegDurAt(0);
    }
    float sig;
    if (sigPrev < 0.0f) {
        sig = sigNext;
    } else if (sigNext < 0.0f) {
        sig = sigPrev;
    } else {
        sig = std::min(sigPrev, sigNext);
    }
    return std::max(sig, 0.0f) * kSpeedSlopeLimit;
}

static void ArcRebuild() {
    int n = (int)sKeyframes.size();
    sArc.segs = (n >= 2) ? (LoopCyclic() ? n : n - 1) : 0;
    sArc.cum.assign((size_t)std::max(sArc.segs, 0) * (kArcSteps + 1), 0.0f);
    sArc.S.assign((size_t)std::max(sArc.segs, 0) + 1, 0.0f);
    sArc.slope.assign((size_t)std::max(sArc.segs, 0) + 1, 0.0f);
    for (int i = 0; i < sArc.segs; i++) {
        int i2 = (i + 1) % n;
        const CineKeyframe& b = sKeyframes[i];
        const CineKeyframe& c = sKeyframes[i2];
        float* cum = &sArc.cum[(size_t)i * (kArcSteps + 1)];
        float len = 0.0f;
        if (b.interp == CINE_INTERP_LINEAR) {
            float e0 = c.eye[0] - b.eye[0], e1 = c.eye[1] - b.eye[1], e2 = c.eye[2] - b.eye[2];
            float chord = std::sqrt(e0 * e0 + e1 * e1 + e2 * e2);
            for (int s = 0; s <= kArcSteps; s++) {
                cum[s] = chord * (float)s / (float)kArcSteps;
            }
            len = chord;
        } else {
            float td[3], ts[3];
            EyeSegmentTangents(i, i2, td, ts);
            float prev[3] = { b.eye[0], b.eye[1], b.eye[2] };
            cum[0] = 0.0f;
            for (int s = 1; s <= kArcSteps; s++) {
                float u = (float)s / (float)kArcSteps;
                float p[3];
                for (int k = 0; k < 3; k++) {
                    p[k] = Hermite1(b.eye[k], c.eye[k], td[k], ts[k], u);
                }
                float e0 = p[0] - prev[0], e1 = p[1] - prev[1], e2 = p[2] - prev[2];
                len += std::sqrt(e0 * e0 + e1 * e1 + e2 * e2);
                cum[s] = len;
                prev[0] = p[0];
                prev[1] = p[1];
                prev[2] = p[2];
            }
        }
        sArc.S[i + 1] = sArc.S[i] + len;
    }
    BuildMonotoneSlopes(sArc.S, sArc.segs, sArc.slope);
    // Substitute each keyframe's own speed into its KNOT, not into the two segments separately: the value is
    // shared by the segment arriving and the segment leaving, so the speed curve is continuous at every
    // keyframe by construction. Capped at what still arrives on time (automatic values already satisfy it, so
    // only a dragged point is ever limited - and it draws at the limited value, so the bound is visible).
    for (int i = 0; i <= sArc.segs && n > 0; i++) {
        float ex = sKeyframes[i % n].speedRate;
        if (ex >= 0.0f) {
            sArc.slope[i] = std::min(std::max(ex, 0.0f), KnotSpeedCap(sArc.S, sArc.segs, i));
        }
    }
    sArc.mOut.assign((size_t)std::max(sArc.segs, 0), 0.0f);
    sArc.mIn.assign((size_t)std::max(sArc.segs, 0), 0.0f);
    sArc.aOut.assign((size_t)std::max(sArc.segs, 0), 0.0f);
    sArc.aIn.assign((size_t)std::max(sArc.segs, 0), 0.0f);
    for (int i = 0; i < sArc.segs; i++) {
        sArc.mOut[i] = sArc.slope[i];
        sArc.mIn[i] = sArc.slope[i + 1];
    }
    // Automatic acceleration is picked PER KNOT and shared by both of its segments, which makes the schedule
    // C2 in time: acceleration is continuous through every keyframe, not just speed.
    //
    // A plain cubic is only C1 - each segment arrives at a keyframe with one acceleration and the next leaves
    // with another. That step is a jerk impulse at every single keyframe, the exact thing this whole system is
    // supposed to not do, and it is also why an untouched keyframe used to draw its two handles at different
    // angles: they were honestly reporting a broken curve. Averaging the two one-sided cubic accelerations
    // (weighted the standard non-uniform way, by the OPPOSITE interval) removes the step. Keyframe times,
    // positions and speeds are untouched, so the camera still arrives exactly on time.
    bool cyc = LoopCyclic() && sArc.segs >= 2;
    std::vector<float> knotA((size_t)std::max(sArc.segs, 0) + 1, 0.0f);
    auto cubicEnds = [&](int seg, float* aStart, float* aEnd) {
        float d = SegDurAt(seg);
        float c0, c1;
        AutoAccel(sArc.S[seg], sArc.S[seg + 1], sArc.mOut[seg] * d, sArc.mIn[seg] * d, &c0, &c1);
        float dd = d * d;  // SegDurAt floors d at 1e-4, so this cannot be zero and needs no floor of its own
        *aStart = c0 / dd; // physical u/s^2, so the two sides are comparable across different durations
        *aEnd = c1 / dd;
    };
    for (int i = 0; i <= sArc.segs; i++) {
        int segPrev = (i > 0) ? i - 1 : (cyc ? sArc.segs - 1 : -1);
        int segNext = (i < sArc.segs) ? i : (cyc ? 0 : -1);
        float s0, s1;
        if (segPrev >= 0 && segNext >= 0) {
            float aPrev, aNext, dummy;
            cubicEnds(segPrev, &dummy, &aPrev);
            cubicEnds(segNext, &aNext, &dummy);
            float hPrev = SegDurAt(segPrev), hNext = SegDurAt(segNext);
            float w = hPrev + hNext;
            knotA[i] = (w > 1e-5f) ? (aPrev * hNext + aNext * hPrev) / w : 0.5f * (aPrev + aNext);
        } else if (segPrev >= 0) {
            cubicEnds(segPrev, &s0, &s1);
            knotA[i] = s1; // path end: only one side exists, so there is nothing to reconcile
        } else if (segNext >= 0) {
            cubicEnds(segNext, &s0, &s1);
            knotA[i] = s0;
        }
    }
    // A curve can be asked for that would run backwards mid-segment - the camera reversing to make the
    // arithmetic work - either by the automatic values above or by a hand-rotated handle. It has to be pulled
    // back to something feasible, and HOW that is done decides whether the continuity above survives.
    //
    // Scaling each knot's single acceleration is the move: both of its segments read the same number, so a
    // knot stays continuous no matter how far it gets scaled back. (The obvious alternative - blending each
    // segment separately toward its own cubic - quietly reintroduces the step, because the cubic's
    // acceleration differs on the two sides of a knot. That is a jerk at a keyframe nobody touched, which is
    // the exact bug this rebuild exists to remove.) Halve the offending knots, re-check, repeat.
    std::vector<float> beta((size_t)std::max(sArc.segs, 0) + 1, 1.0f);
    auto endAccel = [&](int knot, bool leaving) {
        const CineKeyframe& k = sKeyframes[knot % n];
        if (leaving ? (k.hasAccelOut != 0) : (k.hasAccelIn != 0)) {
            return leaving ? k.speedAccelOut : k.speedAccelIn;
        }
        return beta[knot] * knotA[knot];
    };
    auto segOk = [&](int i) {
        float dur = SegDurAt(i);
        return SpeedStaysForward(sArc.S[i], sArc.S[i + 1], sArc.mOut[i] * dur, sArc.mIn[i] * dur,
                                 endAccel(i, true) * dur * dur, endAccel(i + 1, false) * dur * dur);
    };
    // Scale down ONLY the knots that are actually the problem, and by a continuous amount.
    //
    // Halving both ends of any unhappy segment - the obvious first idea - is wrong twice over. It spreads:
    // pulling a knot down changes its OTHER segment, which can then fail and pull ITS far knot down, so one
    // keyframe dragged to its limit visibly reshapes keyframes several places away. And it is discontinuous:
    // 1, 0.5, 0.25 are jumps, so sliding a speed slowly makes the curve snap through a sequence of shapes
    // instead of following the cursor. Bisecting each knot's own scale fixes both - the value moves smoothly
    // with the drag, and a knot only moves if one of ITS segments is unhappy.
    bool anyBad = false;
    for (int i = 0; i < sArc.segs && !anyBad; i++) {
        anyBad = !segOk(i);
    }
    for (int pass = 0; pass < 3 && anyBad; pass++) {
        for (int i = 0; i <= sArc.segs; i++) {
            bool autoK = (i < sArc.segs) ? (sKeyframes[i % n].hasAccelOut == 0) : true;
            if (i > 0) {
                autoK = autoK && sKeyframes[i % n].hasAccelIn == 0;
            }
            if (!autoK) {
                continue; // hand-set: not ours to scale
            }
            auto knotOk = [&]() {
                bool ok = (i >= sArc.segs) || segOk(i);
                if (ok && i > 0) {
                    ok = segOk(i - 1);
                }
                if (ok && i == 0 && cyc) {
                    ok = segOk(sArc.segs - 1);
                }
                return ok;
            };
            float keep = beta[i];
            if (knotOk()) {
                continue;
            }
            float lo = 0.0f, hi = keep; // 0 = level acceleration here, which the speed cap keeps reachable
            for (int it = 0; it < 12; it++) {
                float mid = 0.5f * (lo + hi);
                beta[i] = mid;
                if (cyc && (i == 0 || i == sArc.segs)) {
                    beta[0] = beta[sArc.segs] = mid; // same keyframe on both ends of a loop
                }
                if (knotOk()) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            beta[i] = lo;
            if (cyc && (i == 0 || i == sArc.segs)) {
                beta[0] = beta[sArc.segs] = lo;
            }
        }
        anyBad = false;
        for (int i = 0; i < sArc.segs && !anyBad; i++) {
            anyBad = !segOk(i);
        }
    }
    for (int i = 0; i < sArc.segs; i++) {
        float dur = SegDurAt(i);
        float a0 = endAccel(i, true) * dur * dur; // physical u/s^2 -> Hermite5's per-progress units
        float a1 = endAccel(i + 1, false) * dur * dur;
        if (!segOk(i)) {
            // Still impossible with both knots scaled, so a hand-set handle is the cause. Fall back on this
            // segment alone to the plain cubic, which the monotone slope limit already proved safe. This is
            // the one place acceleration can step at a keyframe - the user asked for a shape the camera
            // cannot take, and the handle draws where it actually ended up rather than where it was dropped.
            float c0, c1;
            AutoAccel(sArc.S[i], sArc.S[i + 1], sArc.mOut[i] * dur, sArc.mIn[i] * dur, &c0, &c1);
            auto feasible = [&](float f) {
                return SpeedStaysForward(sArc.S[i], sArc.S[i + 1], sArc.mOut[i] * dur, sArc.mIn[i] * dur,
                                         c0 + (a0 - c0) * f, c1 + (a1 - c1) * f);
            };
            float lo = 0.0f, hi = 1.0f; // f = 0 (the cubic) is always feasible; f = 1 is known infeasible here
            for (int it = 0; it < 14; it++) {
                float mid = 0.5f * (lo + hi);
                if (feasible(mid)) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            a0 = c0 + (a0 - c0) * lo;
            a1 = c1 + (a1 - c1) * lo;
        }
        sArc.aOut[i] = a0;
        sArc.aIn[i] = a1;
    }
}

static void ArcEnsure() {
    uint32_t h = PathShapeHash();
    int wantSegs =
        ((int)sKeyframes.size() >= 2) ? (LoopCyclic() ? (int)sKeyframes.size() : (int)sKeyframes.size() - 1) : 0;
    if (h != sArc.hash || wantSegs != sArc.segs) {
        ArcRebuild();
        sArc.hash = h;
    }
}

// Map the (eased) local time progress p in arc segment i1 to the geometric curve parameter u in [0,1].
static float ArcParamAtTime(int i1, float p) {
    if (i1 < 0 || i1 >= sArc.segs) {
        return p; // no schedule (degenerate path): fall back to raw progress
    }
    float dur = SegDurAt(i1);
    float d =
        Hermite5(sArc.S[i1], sArc.S[i1 + 1], sArc.mOut[i1] * dur, sArc.mIn[i1] * dur, sArc.aOut[i1], sArc.aIn[i1], p) -
        sArc.S[i1];
    const float* cum = &sArc.cum[(size_t)i1 * (kArcSteps + 1)];
    float len = cum[kArcSteps];
    if (len < 1e-5f) {
        return 0.0f; // zero-length segment: hold on the keyframe
    }
    d = std::min(std::max(d, 0.0f), len);
    int lo = 0, hi = kArcSteps; // invariant: cum[lo] <= d <= cum[hi]
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (cum[mid] <= d) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    float span = cum[hi] - cum[lo];
    float frac = (span > 1e-6f) ? (d - cum[lo]) / span : 0.0f;
    return ((float)lo + frac) / (float)kArcSteps;
}

// Eye position on arc segment i1 -> i2 at geometric curve parameter u (pure geometry, no schedule).
static void EyePosAt(int i1, int i2, float u, float* out) {
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[i2];
    if (b.interp == CINE_INTERP_LINEAR) {
        for (int k = 0; k < 3; k++) {
            out[k] = b.eye[k] + (c.eye[k] - b.eye[k]) * u;
        }
    } else {
        float td[3], ts[3];
        EyeSegmentTangents(i1, i2, td, ts);
        for (int k = 0; k < 3; k++) {
            out[k] = Hermite1(b.eye[k], c.eye[k], td[k], ts[k], u);
        }
    }
}

// THE envelope rule, and the only one: the view may never point somewhere neither of the two keyframes it is
// travelling between is looking. Returns how much of a segment's natural turn survives that - 1 when the
// curve was never going to leave anyway, less when it was, and only as much less as it takes.
//
// This replaced four separate rules that used to sit on the slopes: clamp the sign, limit the magnitude, use
// the SMALLER of the two neighbouring rates, and special-case zero. Those come from monotone-interpolation
// numerics, where they are a SUFFICIENT condition - conservative on purpose, and they fire whether or not
// there is anything to prevent. Measured on a real path they cost far more than they bought: at one keyframe
// they cut the view's turn rate by 45% to prevent 0.06 degrees of overshoot, and cut the pitch by 66% to
// prevent none at all - the curve there was already inside the envelope. That is what the hesitate-then-whip
// bump was made of.
//
// Testing the actual curve costs a few evaluations and gives back every special case for free: at a
// turnaround, any nonzero slope leaves the box, so it scales to zero on its own; against a keyframe with no
// turn at all, the box has no width, so the same thing happens. Nothing to write down, nothing to tune.
// s = 0 is a plain smoothstep between the two framings, which is always inside, so the search always lands.
// Per channel, deliberately: yaw and pitch are independent curves, and one scale covering both lets a tight
// pitch hold a wide pan back for no reason (measured: it cost the yaw 13% at a keyframe whose own curve was
// nowhere near the envelope).
static float AimEnvelopeScale(float v0, float v1, float mO, float mI) {
    auto inside = [&](float s) {
        float c0, c1;
        AutoAccel(v0, v1, mO * s, mI * s, &c0, &c1);
        return AngleStaysMonotone(v0, v1, mO * s, mI * s, c0, c1);
    };
    if (inside(1.0f)) {
        return 1.0f;
    }
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 12; i++) {
        float mid = 0.5f * (lo + hi);
        if (inside(mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// The aim's value curves for segment i1: endpoint yaw/pitch (radians, yaw unwrapped the short way round),
// look-at distances, and the four Hermite slopes with explicit bakes, the envelope scale and holds applied.
// Returns false when the segment has no direction to interpolate (degenerate look-at on an endpoint).
// Shared by evaluation (AimPointAt) and by Insert @ playhead's bake, so what gets baked is exactly what
// renders. `raw` skips the envelope scale, which is how the neighbour lookups avoid recursing forever.
static bool AimSegmentCurve(int i1, float* oyB, float* oyC, float* opB, float* opC, float* moY, float* miY, float* moP,
                            float* miP, float* olenB, float* olenC, bool raw = false) {
    int n = (int)sKeyframes.size();
    int i2 = (i1 + 1) % n;
    int i0, i3;
    if (LoopCyclic()) {
        i0 = ((i1 - 1) % n + n) % n;
        i3 = (i2 + 1) % n;
    } else {
        i0 = std::max(0, i1 - 1);
        i3 = std::min(n - 1, i2 + 1);
    }
    const CineKeyframe& a = sKeyframes[i0];
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[i2];
    const CineKeyframe& d = sKeyframes[i3];
    float at12 = SegDurAt(i1);
    float at01 = (i0 != i1) ? SegDurAt(i0) : at12;
    float at23 = (i2 != i3) ? SegDurAt(i2) : at12;
    float aA[3], aB[3], aC[3], aD[3];
    EffectiveAt(i0, aA);
    EffectiveAt(i1, aB);
    EffectiveAt(i2, aC);
    EffectiveAt(i3, aD);
    float dA[3], dB[3], dC[3], dD[3];
    v3sub(aB, b.eye, dB);
    v3sub(aC, c.eye, dC);
    float lenB = v3len(dB), lenC = v3len(dC);
    if (lenB < 1e-3f || lenC < 1e-3f) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        dB[k] /= lenB;
        dC[k] /= lenC;
    }
    v3sub(aA, a.eye, dA); // neighbours give the turn its tangent context; degenerate ones borrow the
    v3sub(aD, d.eye, dD); // segment's own endpoint direction
    if (v3len(dA) > 1e-3f) {
        v3norm(dA);
    } else {
        std::memcpy(dA, dB, sizeof(dA));
    }
    if (v3len(dD) > 1e-3f) {
        v3norm(dD);
    } else {
        std::memcpy(dD, dC, sizeof(dD));
    }
    // Angle space (yaw + pitch), never Cartesian blending: componentwise blending of unit vectors shortens
    // the horizontal part and INFLATES pitch mid-segment (two 4.8-deg framings measured 7.7 deg between).
    const float kPi = 3.14159265358979f;
    auto yawOf = [](const float* dv) { return std::atan2(dv[0], dv[2]); };
    auto pitchOf = [](const float* dv) { return std::asin(std::min(std::max(dv[1], -1.0f), 1.0f)); };
    auto unwrap = [&](float y, float ref) { // shortest way round from the reference
        while (y - ref > kPi) {
            y -= 2.0f * kPi;
        }
        while (y - ref < -kPi) {
            y += 2.0f * kPi;
        }
        return y;
    };
    float yB = yawOf(dB), pB = pitchOf(dB);
    float yC = unwrap(yawOf(dC), yB), pC = pitchOf(dC);
    float yA = unwrap(yawOf(dA), yB), pA = pitchOf(dA);
    float yD = unwrap(yawOf(dD), yC), pD = pitchOf(dD);
    // A clamped path end or a locked (tracked) neighbour carries no useful rate: one-sided treatment instead
    // of pretending the view was standing still there (which forced dead stops at exactly those keyframes).
    bool prevLocked = (i0 != i1) && AimLockedBetween(a, b);
    bool nextLocked = (i2 != i3) && AimLockedBetween(c, d);
    bool noPrev = (i0 == i1) || prevLocked;
    bool noNext = (i2 == i3) || nextLocked;
    // Stored explicit rates are degrees per unit of SEGMENT PROGRESS, not degrees per second, so they only
    // need the radian conversion here. That distinction is the whole point: a rate in deg/s is a promise about
    // wall-clock time, so retiming the path left the baked value fighting the new duration - the slope grew
    // with the segment, overshot, hit the envelope clamp, and came out as a jerk. Progress-relative rates
    // describe the SHAPE of the turn, which is what a bake is supposed to preserve; retiming then changes how
    // fast that shape is played and nothing else. (Same reasoning as the path's relative tangent weights.)
    const float kD2R = kPi / 180.0f;
    AimAngleSlopes(yA, yB, yC, yD, at01, at12, at23, b, c, noPrev, noNext, b.aimTanYawOut * kD2R, c.aimTanYawIn * kD2R,
                   moY, miY);
    AimAngleSlopes(pA, pB, pC, pD, at01, at12, at23, b, c, noPrev, noNext, b.aimTanPitchOut * kD2R,
                   c.aimTanPitchIn * kD2R, moP, miP);
    // A keyframe where the framing turns back on itself has exactly one rate both of its segments allow:
    // zero. Any other value points the view further than the keyframe does, in the direction it is about to
    // leave - straight out of the envelope. This is not a limiter with a number in it, it is the answer to
    // the constraint, and it has to be settled BEFORE the scale below: one scale serves a whole segment, so
    // an end that must go to zero would otherwise drag the far end of that segment down with it (which is
    // how the first attempt at this made the very keyframe it was fixing worse than the old clamp did).
    {
        float s01y = (yB - yA) / at01, s12y = (yC - yB) / at12, s23y = (yD - yC) / at23;
        float s01p = (pB - pA) / at01, s12p = (pC - pB) / at12, s23p = (pD - pC) / at23;
        if (noPrev) { // no real neighbour on that side: this segment's own rate continues, nothing turns back
            s01y = s12y;
            s01p = s12p;
        }
        if (noNext) {
            s23y = s12y;
            s23p = s12p;
        }
        if (s01y * s12y <= 0.0f) {
            *moY = 0.0f;
        }
        if (s12y * s23y <= 0.0f) {
            *miY = 0.0f;
        }
        if (s01p * s12p <= 0.0f) {
            *moP = 0.0f;
        }
        if (s12p * s23p <= 0.0f) {
            *miP = 0.0f;
        }
    }
    // The envelope, measured. Each keyframe's scale is the smaller of what its two segments can carry, so
    // both sides of a knot end up with the same number and the turn rate cannot step across a keyframe.
    // Every scale here is computed from its own segment's natural curve alone, which is what keeps this from
    // recursing outward along the whole path.
    if (!raw) {
        float sYThis = AimEnvelopeScale(yB, yC, *moY, *miY);
        float sPThis = AimEnvelopeScale(pB, pC, *moP, *miP);
        float sYPrev = 1.0f, sPPrev = 1.0f, sYNext = 1.0f, sPNext = 1.0f;
        float q0, q1, q2, q3, nmoY, nmiY, nmoP, nmiP, ql0, ql1;
        if (!noPrev && AimSegmentCurve(i0, &q0, &q1, &q2, &q3, &nmoY, &nmiY, &nmoP, &nmiP, &ql0, &ql1, true)) {
            sYPrev = AimEnvelopeScale(q0, q1, nmoY, nmiY);
            sPPrev = AimEnvelopeScale(q2, q3, nmoP, nmiP);
        }
        if (!noNext && AimSegmentCurve(i2, &q0, &q1, &q2, &q3, &nmoY, &nmiY, &nmoP, &nmiP, &ql0, &ql1, true)) {
            sYNext = AimEnvelopeScale(q0, q1, nmoY, nmiY);
            sPNext = AimEnvelopeScale(q2, q3, nmoP, nmiP);
        }
        *moY *= std::min(sYPrev, sYThis);
        *miY *= std::min(sYThis, sYNext);
        *moP *= std::min(sPPrev, sPThis);
        *miP *= std::min(sPThis, sPNext);
    }
    *oyB = yB;
    *oyC = yC;
    *opB = pB;
    *opC = pC;
    *olenB = lenB;
    *olenC = lenC;
    return true;
}

// Look-at point on arc segment i1 at aim-curve parameter pAim. eyePos / ueEye are the eye's position and
// geometric parameter at the same moment. Three rules total: locked tracking is exact, rail follows the
// travel direction, and everything else is the yaw/pitch value curves from AimSegmentCurve.
// The aim curve's end CURVATURES for segment i1, in Hermite5's per-progress units.
//
// Picked per KNOT and shared by both of its segments, the same way the eye's schedule does it, and for the
// same reason: a cubic angle curve is only C1, so the view arrives at a keyframe slowing its turn at one rate
// and leaves speeding up at another. That step is a jerk at every single keyframe - it is what "the aim feels
// jumpy and clunky" has been made of, and it was invisible because nothing ever drew the aim's curvature.
static void AimSegmentAccels(int i1, float yB, float yC, float pB, float pC, float moY, float miY, float moP, float miP,
                             float* aoY, float* aiY, float* aoP, float* aiP) {
    int n = (int)sKeyframes.size();
    int i2 = (i1 + 1) % n;
    bool cyc = LoopCyclic();
    float at12 = SegDurAt(i1);
    float dd = at12 * at12;
    // This segment's own cubic end curvatures, converted to per-second^2 so the two sides of a knot are
    // comparable even when their segments run at different lengths.
    float sy0, sy1, sp0, sp1;
    AutoAccel(yB, yC, moY, miY, &sy0, &sy1);
    AutoAccel(pB, pC, moP, miP, &sp0, &sp1);
    sy0 /= dd;
    sy1 /= dd;
    sp0 /= dd;
    sp1 /= dd;
    // A neighbouring segment's curvature at the shared knot, if that segment has an aim curve at all.
    auto neighbour = [&](int seg, bool wantEnd, float* oy, float* op) {
        float q0, q1, q2, q3, mo1, mi1, mo2, mi2, l0, l1;
        if (seg < 0 || !AimSegmentCurve(seg, &q0, &q1, &q2, &q3, &mo1, &mi1, &mo2, &mi2, &l0, &l1, true)) {
            return false; // raw: this only needs the neighbour's shape, and it keeps the lookup one level deep
        }
        float d = SegDurAt(seg), d2 = d * d;
        float ay0, ay1, ap0, ap1;
        AutoAccel(q0, q1, mo1, mi1, &ay0, &ay1);
        AutoAccel(q2, q3, mo2, mi2, &ap0, &ap1);
        *oy = (wantEnd ? ay1 : ay0) / d2;
        *op = (wantEnd ? ap1 : ap0) / d2;
        return true;
    };
    float kY0 = sy0, kP0 = sp0, kY1 = sy1, kP1 = sp1; // one-sided defaults (path ends, locked neighbours)
    int segPrev = (i1 > 0) ? i1 - 1 : (cyc ? n - 1 : -1);
    if (segPrev >= 0 && !AimLockedBetween(sKeyframes[segPrev], sKeyframes[i1])) {
        float ny, np;
        if (neighbour(segPrev, true, &ny, &np)) {
            float h0 = SegDurAt(segPrev), w = h0 + at12;
            kY0 = (w > 1e-5f) ? (ny * at12 + sy0 * h0) / w : 0.5f * (ny + sy0);
            kP0 = (w > 1e-5f) ? (np * at12 + sp0 * h0) / w : 0.5f * (np + sp0);
        }
    }
    int segNext = (i2 < n - 1) ? i2 : (cyc ? i2 % n : -1);
    if (segNext >= 0 && segNext != i1 && !AimLockedBetween(sKeyframes[i2], sKeyframes[(segNext + 1) % n])) {
        float ny, np;
        if (neighbour(segNext, false, &ny, &np)) {
            float h1 = SegDurAt(segNext), w = at12 + h1;
            kY1 = (w > 1e-5f) ? (ny * at12 + sy1 * h1) / w : 0.5f * (ny + sy1);
            kP1 = (w > 1e-5f) ? (np * at12 + sp1 * h1) / w : 0.5f * (np + sp1);
        }
    }
    float y0 = kY0 * dd, y1 = kY1 * dd, p0 = kP0 * dd, p1v = kP1 * dd; // back to per-progress
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[i2];
    // Baked values are progress-relative already, but they are stored in DEGREES like the rates, and every
    // angle in here is radians. Reading them raw made every baked curvature 57x too strong, which the
    // envelope then had to scale back hard - and since a keyframe's curvature is shared with the segment on
    // its other side, the wreckage showed up further from the inserted keyframe than the insert had touched.
    const float kD2Ra = 3.14159265f / 180.0f;
    if (b.hasAimAccOut) {
        y0 = b.aimAccYawOut * kD2Ra;
        p0 = b.aimAccPitchOut * kD2Ra;
    }
    if (c.hasAimAccIn) {
        y1 = c.aimAccYawIn * kD2Ra;
        p1v = c.aimAccPitchIn * kD2Ra;
    }
    // Curvature this strong would carry the view past one of the two keyframes it sits between - the one
    // thing the aim may never do. Scale it back just far enough. Level curvature (0) is always within the
    // envelope because the slopes were already limited to kSpeedSlopeLimit, which is exactly what that limit
    // is for, so this search always has somewhere safe to land.
    bool okY = AngleStaysMonotone(yB, yC, moY, miY, y0, y1);
    bool okP = AngleStaysMonotone(pB, pC, moP, miP, p0, p1v);
    if (!okY || !okP) {
        float lo = 0.0f, hi = 1.0f;
        for (int it = 0; it < 12; it++) {
            float f = 0.5f * (lo + hi);
            if (AngleStaysMonotone(yB, yC, moY, miY, y0 * f, y1 * f) &&
                AngleStaysMonotone(pB, pC, moP, miP, p0 * f, p1v * f)) {
                lo = f;
            } else {
                hi = f;
            }
        }
        y0 *= lo;
        y1 *= lo;
        p0 *= lo;
        p1v *= lo;
    }
    *aoY = y0;
    *aiY = y1;
    *aoP = p0;
    *aiP = p1v;
}

static void AimPointAt(int i1, float pAim, float ueEye, const float* eyePos, float* out) {
    int n = (int)sKeyframes.size();
    int i2 = (i1 + 1) % n;
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[i2];
    float aB[3], aC[3];
    EffectiveAt(i1, aB);
    EffectiveAt(i2, aC);
    bool aimLocked = AimLockedBetween(b, c);
    bool railMode = sAimOverride == 4 || (!aimLocked && b.aimMode == CINE_AIM_RAIL && c.aimMode == CINE_AIM_RAIL);
    if (aimLocked) { // both ends track the same live/shared source: stay dead on it
        for (int k = 0; k < 3; k++) {
            out[k] = aB[k];
        }
        return;
    }
    if (railMode) { // look straight along the actual travel direction, like a dolly
        float dir[3];
        if (b.interp == CINE_INTERP_LINEAR) {
            v3sub(c.eye, b.eye, dir);
        } else {
            float td[3], ts[3];
            EyeSegmentTangents(i1, i2, td, ts);
            for (int k = 0; k < 3; k++) {
                dir[k] = Hermite1Deriv(b.eye[k], c.eye[k], td[k], ts[k], ueEye);
            }
        }
        if (v3len(dir) < 1e-4f) { // stationary stretch: hold the keyframe's own rail direction
            AutoTangentDir(i1, dir);
        }
        v3norm(dir);
        for (int k = 0; k < 3; k++) {
            out[k] = eyePos[k] + dir[k] * 100.0f;
        }
        return;
    }
    float yB, yC, pB, pC, moY, miY, moP, miP, lenB, lenC;
    if (!AimSegmentCurve(i1, &yB, &yC, &pB, &pC, &moY, &miY, &moP, &miP, &lenB, &lenC)) {
        for (int k = 0; k < 3; k++) { // degenerate framing: nothing to interpolate in angle space
            out[k] = aB[k] + (aC[k] - aB[k]) * pAim;
        }
        return;
    }
    float yaw, pitch;
    if (b.interp == CINE_INTERP_LINEAR) {
        yaw = yB + (yC - yB) * pAim;
        pitch = pB + (pC - pB) * pAim;
    } else {
        float aoY, aiY, aoP, aiP;
        AimSegmentAccels(i1, yB, yC, pB, pC, moY, miY, moP, miP, &aoY, &aiY, &aoP, &aiP);
        yaw = Hermite5(yB, yC, moY, miY, aoY, aiY, pAim);
        pitch = Hermite5(pB, pC, moP, miP, aoP, aiP, pAim);
    }
    float cp = std::cos(pitch);
    float dir[3] = { std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp };
    float dist = lenB + (lenC - lenB) * pAim;
    for (int k = 0; k < 3; k++) {
        out[k] = eyePos[k] + dir[k] * dist;
    }
}

// Smooth pose at an absolute time along the timeline. When looping, the path is treated as cyclic: a
// loop-return segment connects the last keyframe back to the first, and tangents wrap around so the seam
// is as smooth as any other keyframe (no snap).
static CineKeyframe SampleAt(float time) {
    CineKeyframe out{};
    int n = (int)sKeyframes.size();
    if (n == 0) {
        return out;
    }
    if (n == 1) {
        return sKeyframes[0];
    }

    float lastT = sKeyframes[n - 1].time;
    int i1, i2;
    float lt;

    if (!LoopCyclic()) {
        if (time <= sKeyframes[0].time) {
            return sKeyframes[0];
        }
        if (time >= lastT) {
            return sKeyframes[n - 1];
        }
        int i = 0;
        while (i < n - 1 && time >= sKeyframes[i + 1].time) {
            i++;
        }
        float segDur = sKeyframes[i + 1].time - sKeyframes[i].time;
        lt = (segDur > 0.0001f) ? (time - sKeyframes[i].time) / segDur : 0.0f;
        i1 = i;
        i2 = i + 1;
    } else {
        float eff = lastT + sLoopReturnTime;
        if (eff <= 0.0001f) {
            return sKeyframes[0];
        }
        time = std::fmod(time, eff);
        if (time < 0.0f) {
            time += eff;
        }
        if (time < lastT) {
            int i = 0;
            while (i < n - 1 && time >= sKeyframes[i + 1].time) {
                i++;
            }
            float segDur = sKeyframes[i + 1].time - sKeyframes[i].time;
            lt = (segDur > 0.0001f) ? (time - sKeyframes[i].time) / segDur : 0.0f;
            i1 = i;
            i2 = i + 1;
        } else {
            // loop-return segment: last keyframe -> first keyframe
            lt = (sLoopReturnTime > 0.0001f) ? (time - lastT) / sLoopReturnTime : 0.0f;
            i1 = n - 1;
            i2 = 0;
        }
    }

    // Tangent neighbors: clamp at the ends normally, wrap around when looping.
    int i0, i3;
    if (LoopCyclic()) {
        i0 = ((i1 - 1) % n + n) % n;
        i3 = (i2 + 1) % n;
    } else {
        i0 = std::max(0, i1 - 1);
        i3 = std::min(n - 1, i2 + 1);
    }

    const CineKeyframe& a = sKeyframes[i0];
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[i2];
    const CineKeyframe& d = sKeyframes[i3];

    // (Per-keyframe timing ease used to reparametrize the segment here. It was removed once the speed curve
    // grew acceleration handles, which shape an ease directly and continuously. It also had a defect the
    // handles don't: applied as easeOut leaving and easeIn arriving, a keyframe eased on one side only made
    // the camera's actual speed JUMP as it crossed - the one thing a single speed per keyframe exists to
    // prevent. Global easing is unaffected; it reshapes playback as a whole, not individual keyframes.)

    // Eye (spatial path): WHERE comes from the geometric spline (EyeSegmentTangents: centripetal knots, TCB,
    // overshoot guard, per-side direction/length overrides); WHEN comes from the arc-length speed schedule,
    // which maps the eased time progress to a distance along the path and then to the curve parameter.
    ArcEnsure();
    float ue = ArcParamAtTime(i1, lt);
    EyePosAt(i1, i2, ue, out.eye);

    // Roll / FOV knot intervals: keyframe times, so these channels change at a smooth, continuous rate.
    float at12 = SegDurAt(i1);
    float at01 = (i0 != i1) ? SegDurAt(i0) : at12;
    float at23 = (i2 != i3) ? SegDurAt(i2) : at12;

    // Aim: ordinary value curves over the segment's eased time progress - the way every editing suite treats
    // orientation. (An earlier build paced the aim by its own ANGULAR arc length, the twin of the eye's
    // distance schedule. It was a workaround for artifacts that came from blending directions as Cartesian
    // vectors; interpolating yaw/pitch as angles removed those at the source, and the schedule only added
    // rate plateaus and jumps at keyframes where segments turn by very different amounts.)
    AimPointAt(i1, lt, ue, out.eye, out.at);

    // Roll / FOV ride the same clock as the aim (the segment's eased time progress), so every value channel
    // authored on the same two keyframes stays in step with every other.
    out.roll = InterpComp(a.roll, b.roll, c.roll, d.roll, b, c, at01, at12, at23, lt);
    out.fov = InterpComp(a.fov, b.fov, c.fov, d.fov, b, c, at01, at12, at23, lt);
    out.time = time;
    return out;
}

// --- Path-shape checksum (spline caching) -----------------------------------------------------------------
// A cheap FNV-style hash over everything that changes SampleAt's eye/roll/fov output for a given time: the
// keyframes' shape-relevant fields plus the global spline/loop settings. Consumers (the world-overlay polyline
// and the curve editor's camera-channel sampling) re-tessellate only when this changes, instead of running
// hundreds of full spline evaluations every frame. Deliberately EXCLUDES aim/look-at state: neither consumer
// reads the interpolated aim, and live-tracked targets would otherwise invalidate the cache every frame.
static uint32_t HashF32(uint32_t h, float v) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    h ^= b;
    h *= 16777619u;
    return h;
}
static uint32_t PathShapeHash() {
    uint32_t h = 2166136261u;
    h = HashF32(h, (float)sKeyframes.size());
    h = HashF32(h, sLoop ? 1.0f : 0.0f);
    h = HashF32(h, (float)sLoopMode);
    h = HashF32(h, sLoopReturnTime);
    for (const CineKeyframe& k : sKeyframes) {
        h = HashF32(h, k.time);
        h = HashF32(h, k.eye[0]);
        h = HashF32(h, k.eye[1]);
        h = HashF32(h, k.eye[2]);
        h = HashF32(h, k.roll);
        h = HashF32(h, k.fov);
        h = HashF32(h, (float)k.interp);
        h = HashF32(h, k.tension);
        h = HashF32(h, k.continuity);
        h = HashF32(h, k.bias);
        h = HashF32(h, (float)k.hasTangent);
        h = HashF32(h, k.tangent[0]);
        h = HashF32(h, k.tangent[1]);
        h = HashF32(h, k.tangent[2]);
        h = HashF32(h, (float)k.hasTangentIn);
        h = HashF32(h, k.tangentIn[0]);
        h = HashF32(h, k.tangentIn[1]);
        h = HashF32(h, k.tangentIn[2]);
        h = HashF32(h, k.tanWOut);
        h = HashF32(h, k.tanWIn);
        h = HashF32(h, (float)(k.hasAimAccIn * 8 + k.hasAimAccOut * 4 + k.hasAimTanIn * 2 + k.hasAimTanOut));
        h = HashF32(h, k.speedRate); // these drive the arc schedule, which this hash keys
        h = HashF32(h, (float)(k.hasAccelIn * 2 + k.hasAccelOut));
        h = HashF32(h, k.speedAccelIn);
        h = HashF32(h, k.speedAccelOut);
    }
    return h;
}

// Playback timing easing applied to the 0..1 progress.
static float ApplyEase(float u, int mode) {
    if (u < 0.0f) {
        u = 0.0f;
    }
    if (u > 1.0f) {
        u = 1.0f;
    }
    switch (mode) {
        case 1:
            return u * u * (3.0f - 2.0f * u); // smoothstep (ease in + out)
        case 2:
            return u * u; // ease in
        case 3:
            return 1.0f - (1.0f - u) * (1.0f - u); // ease out
        default:
            return u; // none
    }
}

// The eased progress the playhead actually uses (ApplyEase blended toward linear by sEaseAmount).
static float EasedProgress(float u) {
    float e = ApplyEase(u, sEaseMode);
    return u + (e - u) * sEaseAmount;
}

// Inverse of EasedProgress: given the eased progress (playhead / total), find the linear progress u that maps
// to it. The mapping is monotonic, so bisection converges. Used so Play resumes EXACTLY at the playhead
// instead of jumping (seeding the linear progress straight from the eased playhead double-applies the ease).
static float InvertEasedProgress(float target) {
    if (target <= 0.0f) {
        return 0.0f;
    }
    if (target >= 1.0f) {
        return 1.0f;
    }
    float lo = 0.0f, hi = 1.0f;
    for (int it = 0; it < 24; it++) {
        float mid = (lo + hi) * 0.5f;
        if (EasedProgress(mid) < target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (lo + hi) * 0.5f;
}

// Start/stop playback from the current playhead (shared by the Space shortcut and the shooting bar's Play).
static void TogglePlay() {
    if (sKeyframes.size() < 2) {
        return;
    }
    if (sPlaying) {
        sPlaying = false;
        return;
    }
    float pt = EffectiveTotal();
    if (sPlayhead >= pt) {
        sPlayhead = 0.0f;
    }
    sPlayU = InvertEasedProgress((pt > 0.0f) ? (sPlayhead / pt) : 0.0f); // resume exactly at the playhead
    sPlayDir = 1;
    if (sPlayhead == 0.0f && CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SyncIdleAnim"), 0)) {
        CinematicCam_SyncLinkIdleAnim(); // anchor Link's idle anim when starting from the top
    }
    sPlaying = true;
    sPreview = false;
    CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
}

// Smooth sum-of-sines pseudo-noise in roughly [-1, 1] for one shake channel (phase separates channels).
static float ShakeNoise(float t, float phase) {
    return std::sin(t * 1.00f + phase) * 0.55f + std::sin(t * 2.13f + phase * 1.7f) * 0.30f +
           std::sin(t * 4.31f + phase * 2.3f) * 0.15f;
}

// Seamlessly-looping variant: integer harmonics of the loop, so the value at u=0 and u=1 is identical.
static float ShakeNoiseLoop(float u, int h, float phase) {
    const float TAU = 6.2831853f;
    return std::sin(TAU * (float)h * u + phase) * 0.55f + std::sin(TAU * (float)(2 * h) * u + phase * 1.7f) * 0.30f +
           std::sin(TAU * (float)(4 * h) * u + phase * 2.3f) * 0.15f;
}

// Apply handheld shake to a pose. Driven by play time so it's deterministic (same wobble when you scrub). When
// the path forward-loops, the noise is made periodic over the loop length so the seam doesn't jump.
static void ApplyShake(float playTime, float* eye, float* at, float* roll) {
    float n[6];
    if (sLoop && sLoopMode == 0) {
        float L = EffectiveTotal();
        if (L < 1e-3f) {
            L = 1.0f;
        }
        float u = playTime / L;
        u -= std::floor(u);
        int h = (int)std::lround(sShakeFreq * L); // whole number of wobbles across the loop
        if (h < 1) {
            h = 1;
        }
        for (int i = 0; i < 6; i++) {
            n[i] = ShakeNoiseLoop(u, h, i * 10.0f);
        }
    } else {
        // Free-running (no loop, or ping-pong which is already continuous because the playhead reflects).
        float t = playTime * sShakeFreq;
        for (int i = 0; i < 6; i++) {
            n[i] = ShakeNoise(t, i * 10.0f);
        }
    }
    // Scale by the (optionally keyframed) shake intensity multiplier.
    float posAmp = sShakePosAmp * sShakeIntensity;
    float rotAmp = sShakeRotAmp * sShakeIntensity;
    float px = n[0] * posAmp;
    float py = n[1] * posAmp;
    float pz = n[2] * posAmp;
    // Translate eye and look-at together so the aim direction is preserved...
    eye[0] += px;
    eye[1] += py;
    eye[2] += pz;
    at[0] += px;
    at[1] += py;
    at[2] += pz;
    // ...then add a little angular wobble by nudging the look-at perpendicular, scaled to the aim distance.
    float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1.0f) {
        dist = 1.0f;
    }
    float k = dist * std::tan(rotAmp * 3.14159265f / 180.0f);
    at[0] += n[3] * k;
    at[1] += n[4] * k;
    *roll += n[5] * rotAmp * 0.5f;
}

// Live position of the follow target (Link or actor). Returns false if follow is off or unavailable.
static bool FollowCenter(float* out) {
    if (sFollowMode == 1) {
        return CinematicCam_GetPlayerPos(out) != 0;
    }
    if (sFollowMode == 2) {
        return CinematicCam_ResolveActor(&sFollowActorPtr, (short)sFollowActorId, nullptr, out) != 0;
    }
    return false;
}

// Append a keyframe capturing the current live freecam pose at time t (used by recording).
static void RecordKeyframe(float t) {
    CineKeyframe kf{};
    CinematicCam_GetPose(kf.eye, kf.at, &kf.roll, &kf.fov);
    if (kf.fov < 1.0f) {
        kf.fov = 60.0f;
    }
    kf.time = t;
    sKeyframes.push_back(kf);
    sIds.push_back(sNextId++);
}

// Runs every game frame (OnCameraState hook), just before Camera_Update.
static void PlaybackTick() {
    static bool wasActive = false;
    sLoopMarkerFrame = false; // only true for the single frame a loop restarts (set below)

    // Perf probe: this hook fires once per frame, so the gap to the previous call is the whole frame period
    // (logic + render + present). Splitting it against our own measured draw tells us if a hitch is us or engine.
    sPerfOn = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.PerfDiag"), 0) != 0;
    if (sPerfOn) {
        double now = CineNowMs();
        if (sPerfTickPrev > 0.0) {
            sPerfFrameMs = now - sPerfTickPrev;
            if (now - sPerfPeakAt > 2000.0) { // refresh the rolling peak window every couple of seconds
                sPerfPeakMs = sPerfFrameMs;
                sPerfPeakAt = now;
            } else if (sPerfFrameMs > sPerfPeakMs) {
                sPerfPeakMs = sPerfFrameMs;
            }
            if (sPerfFrameMs > 60.0) { // ~below 16 fps for one frame: a visible hitch worth recording
                sPerfSpikeCount++;
                double engine = sPerfFrameMs - sPerfDrawMs - sPerfUpdateMs;
                SPDLOG_WARN("[CinePerf] SPIKE {:.0f}ms (draw {:.1f} [overlay {:.1f}] | update {:.1f} | engine/gpu "
                            "{:.1f})  play={} preview={} rec={} kf={} overlay={} freecam={} follow={} aimOv={}",
                            sPerfFrameMs, sPerfDrawMs, sPerfOverlayMs, sPerfUpdateMs, engine, (int)sPlaying,
                            (int)sPreview, (int)sRecording, (int)sKeyframes.size(), (int)sShowPath,
                            CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0), sFollowMode, sAimOverride);
            }
        }
        sPerfTickPrev = now;
    } else {
        sPerfTickPrev = 0.0;
    }

    // If the user left camera mode (toggled it off), stop any playback/preview/recording so the normal game
    // camera returns to Link. Without this, a still-running (e.g. looping) cinematic keeps gCineCamPlaybackActive
    // set, which keeps the camera dispatch active even though "Enabled" is off - so the view never returns.
    if (!CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0)) {
        sPlaying = false;
        sPreview = false;
        sRecording = false;
    }

    // Recording: lay down keyframes from the live freecam at a fixed interval.
    if (sRecording) {
        sRecordTime += kTickSeconds;
        if (sRecordTime - sRecordLast >= sRecordInterval) {
            RecordKeyframe(sRecordTime);
            sRecordLast = sRecordTime;
        }
    }

    if (sPlaying) {
        if (sKeyframes.size() < 2) {
            sPlaying = false;
        } else {
            float total = EffectiveTotal();
            float denom = (total > 0.0f) ? total : 1.0f;
            float step = (kTickSeconds * sPlaySpeed) / denom;
            if (sLoop && sLoopMode == 1) {
                // Ping-pong: advance in the current direction and reflect off each end.
                sPlayU += step * (float)sPlayDir;
                if (sPlayU >= 1.0f) {
                    sPlayU = 1.0f - (sPlayU - 1.0f);
                    sPlayDir = -1;
                } else if (sPlayU <= 0.0f) {
                    sPlayU = -sPlayU;
                    sPlayDir = 1;
                    sLoopMarkerFrame = true; // back to the start of a ping-pong cycle
                }
                if (sPlayU < 0.0f) {
                    sPlayU = 0.0f;
                }
                if (sPlayU > 1.0f) {
                    sPlayU = 1.0f;
                }
            } else {
                sPlayU += step;
                if (sPlayU >= 1.0f) {
                    if (sLoop) {
                        sPlayU = std::fmod(sPlayU, 1.0f);
                        sLoopMarkerFrame = true; // wrapped back to the start of a forward loop
                    } else {
                        sPlayU = 1.0f;
                        sPlaying = false;
                    }
                }
            }
            sPlayhead = EasedProgress(sPlayU) * total;
        }
    }

    // Cinematic: re-anchor Link's idle (breathing/head-bob) animation to the start of each loop, so a looping
    // GIF whose length is a whole number of idle cycles stays perfectly seamless.
    if (sLoopMarkerFrame && CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SyncIdleAnim"), 0)) {
        CinematicCam_SyncLinkIdleAnim();
    }

    // Pushing the movement stick while previewing (and not letting Link drive) drops back to manual flying.
    if (sPreview && !sPlaying && !CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.PlaybackControlsLink"), 0) &&
        CinematicCam_GetMoveStickActive()) {
        sPreview = false;
    }

    bool active = (sPlaying || sPreview) && !sKeyframes.empty();

    // Parameter automation: while a cinematic drives the view, each enabled track applies its value at the
    // playhead to the live parameter. Reset the per-frame overrides first so a non-driving track falls back to
    // its manual setting (green screen / HUD) or neutral value (shake intensity 1.0). Done before the pose block
    // so shake intensity is current this frame.
    sGreenScreenOverride = -1;
    sShakeIntensity = 1.0f;
    sHudHideOverride = -1;
    sRollTrackOn = 0;
    sFovTrackOn = 0;
    sLetterboxOverride = -1.0f;
    for (const TrackDef& d : AllTrackDefs()) {
        if (active && d.track->enabled) {
            float v;
            if (EvalParamTrack(*d.track, sPlayhead, v)) {
                d.apply(v);
            }
        }
    }

    if (active) {
        CineKeyframe s = SampleAt(sPlayhead);
        // Path follow: shift the whole rig by how far the tracked target has moved since authoring.
        if (sFollowMode != 0) {
            float c[3];
            if (FollowCenter(c)) {
                float dx = c[0] - sFollowOrigin[0], dy = c[1] - sFollowOrigin[1], dz = c[2] - sFollowOrigin[2];
                s.eye[0] += dx;
                s.eye[1] += dy;
                s.eye[2] += dz;
                s.at[0] += dx;
                s.at[1] += dy;
                s.at[2] += dz;
            }
        }
        // Roll / FOV automation tracks override the keyframes' interpolated values (applied before shake so
        // the shake's roll jitter still layers on top).
        if (sRollTrackOn) {
            s.roll = sRollTrackVal;
        }
        if (sFovTrackOn) {
            s.fov = std::min(std::max(sFovTrackVal, 1.0f), 170.0f);
        }
        if ((sShakeEnabled && (sPlaying || sShakeOnPreview)) || sShakeTrack.enabled) {
            ApplyShake(sPlayhead, s.eye, s.at, &s.roll); // intensity scales the amps (keyframable)
        }
        CinematicCam_SetPlayback(1, s.eye, s.at, s.roll, s.fov);
        wasActive = true;
    } else if (wasActive) {
        float z[3] = { 0.0f, 0.0f, 0.0f };
        CinematicCam_SetPlayback(0, z, z, 0.0f, 0.0f);
        wasActive = false;
    }

    // Hide the HUD while the cinematic camera is active (reuses SoH's NoUI state). Only clear what we set,
    // so an independently-enabled "no UI" isn't disturbed. A keyframed HUD track overrides the manual setting.
    static bool sWeHidHud = false;
    bool camActive = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0) || active;
    bool hideHud = (sHudHideOverride >= 0) ? (sHudHideOverride != 0)
                                           : (CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.HideHud"), 1) != 0);
    if (camActive && hideHud) {
        GameInteractor::State::NoUIActive = 1;
        sWeHidHud = true;
    } else if (sWeHidHud) {
        GameInteractor::State::NoUIActive = 0;
        sWeHidHud = false;
    }
}

// ---------------------------------------------------------------------------
// Editing actions
// ---------------------------------------------------------------------------
static bool FreeCamEnabled() {
    return CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0) != 0;
}

static void AddKeyframe() {
    PushUndo();
    CineKeyframe kf{};
    CinematicCam_GetPose(kf.eye, kf.at, &kf.roll, &kf.fov);
    kf.time = sKeyframes.empty() ? 0.0f : sKeyframes.back().time + 2.0f;
    sKeyframes.push_back(kf);
    sIds.push_back(sNextId);
    SelectOnly(sNextId);
    sNextId++;
}

static void UpdateSelected() {
    int idx = SelectedIndex();
    if (idx < 0) {
        return;
    }
    PushUndo();
    float time = sKeyframes[idx].time; // keep timing, refresh the pose
    CinematicCam_GetPose(sKeyframes[idx].eye, sKeyframes[idx].at, &sKeyframes[idx].roll, &sKeyframes[idx].fov);
    sKeyframes[idx].time = time;
}

static void DeleteSelected() {
    int idx = SelectedIndex();
    if (idx < 0) {
        return;
    }
    PushUndo();
    sKeyframes.erase(sKeyframes.begin() + idx);
    sIds.erase(sIds.begin() + idx);
    SelectOnly(sIds.empty() ? -1 : sIds[std::min((size_t)idx, sIds.size() - 1)]);
}

static void ClearPath() {
    PushUndo();
    sKeyframes.clear();
    sIds.clear();
    SelectOnly(-1);
    sPlayhead = 0.0f;
    sPlaying = false;
    sPreview = false;
    sFollowMode = 0; // follow is path-level runtime state; clear it with the path
    for (const TrackDef& d : AllTrackDefs()) {
        d.track->keys.clear();
        d.track->enabled = false;
    }
    ParamSelClear();
}

static void CopySelected() {
    int idx = SelectedIndex();
    if (idx < 0) {
        return;
    }
    sClipboard = sKeyframes[idx];
    sClipboardValid = true;
}

// Add a new keyframe at the playhead, either from the clipboard (paste) or sampled from the existing
// path / live freecam (insert).
static void AddKeyframeAtPlayhead(const CineKeyframe& kf, bool pushUndo = true) {
    if (pushUndo) {
        PushUndo();
    }
    CineKeyframe k = kf;
    k.time = sPlayhead;
    k.aimActorPtr = nullptr; // runtime pointer is not copied
    sKeyframes.push_back(k);
    sIds.push_back(sNextId);
    SelectOnly(sNextId);
    sNextId++;
    SortByTime();
}

static void PasteAtPlayhead() {
    if (!sClipboardValid) {
        return;
    }
    AddKeyframeAtPlayhead(sClipboard);
}

static void InsertAtPlayhead() {
    CineKeyframe kf{};
    if (sKeyframes.size() < 2) {
        CinematicCam_GetPose(kf.eye, kf.at, &kf.roll, &kf.fov);
        AddKeyframeAtPlayhead(kf);
        return;
    }
    kf = SampleAt(sPlayhead); // a control point on the existing curve
    PushUndo();               // one undo step covers BOTH the insert and the tangent baking on the neighbors below

    // Shape-preserving split, in two phases. Adding a knot re-spaces the neighbors' automatic (centripetal)
    // tangents, so a plain insert would nudge the curve near it. Phase 1 (before the insert): read the current
    // curve's tangents - the four neighbor sides that will re-space, plus the curve direction/derivative at the
    // split point. Phase 2 (after the insert): re-measure each side's automatic magnitude on the NEW arrangement
    // and store the ratio wanted/auto as a per-side WEIGHT. Exact at insert time, and because weights are
    // relative, later moves rescale the tangents naturally instead of leaving stale absolute lengths behind.
    struct SideWant {
        float len = -1.0f; // wanted absolute tangent length (<0 = leave automatic)
        float dir[3] = { 0.0f, 0.0f, 0.0f };
    };
    SideWant wBIn, wBOut, wKIn, wKOut, wCIn, wCOut;
    auto setWant = [](SideWant& w, const float* m, float len) {
        float ml = v3len(m);
        if (ml < 1e-5f || len < 1e-5f) {
            return; // zero tangent (a hold): leave automatic, a hold pins itself
        }
        w.len = len;
        w.dir[0] = m[0] / ml;
        w.dir[1] = m[1] / ml;
        w.dir[2] = m[2] / ml;
    };

    // Phase 1: read everything from the CURRENT curve.
    int n = (int)sKeyframes.size();
    float t = sPlayhead;
    bool splitOk = false, linearSeg = false, hasPrev = false, hasNext = false;
    int bId = -1, cId = -1;
    // Aim bake: captured rates (deg/s) as {yawIn, yawOut, pitchIn, pitchOut} for b, the new key, and c. Once
    // written as explicit values, nobody re-derives slopes from the new arrangement - so the insert cannot
    // reshape the aim curve.
    bool aimBake = false;
    // Whether the OUTER sides were actually measured off their own segment. Without this the fallback value
    // (a copy of the near side) would be pinned as if it had been measured, reshaping a segment the insert
    // never touched.
    bool haveBIn = false, haveCOut = false;
    float aimB[4] = {}, aimK[4] = {}, aimC[4] = {};
    float accB[4] = {}, accK[4] = {}, accC[4] = {}; // matching curvatures, same {YawIn,YawOut,PitchIn,PitchOut}
    // Speed bake: same idea for the pacing. The schedule derives its knot speeds from the (time, distance)
    // knots, so a new knot re-derives the whole profile - the camera would visibly change speed around an
    // insert even though the path and aim were preserved. Freezing the three keyframes' current speeds keeps
    // the motion identical. (b's leaving speed and c's arriving speed are unchanged by an exact split; the
    // new key takes the schedule's instantaneous speed at the split.)
    bool speedBake = false, accelBake = false;
    float spB = -1.0f, spK = -1.0f, spC = -1.0f;
    float acB = 0.0f, acK = 0.0f, acC = 0.0f; // matching accelerations (u/s^2), so the ease shape survives too
    if (t > sKeyframes[0].time + 1e-4f && t < sKeyframes[n - 1].time - 1e-4f) {
        int i1 = 0;
        while (i1 < n - 1 && t >= sKeyframes[i1 + 1].time) {
            i1++;
        }
        int i2 = i1 + 1;
        CineKeyframe& b = sKeyframes[i1];
        CineKeyframe& c = sKeyframes[i2];
        float D = c.time - b.time;
        // The GEOMETRIC split parameter: where on the segment's curve the camera visibly is at this playhead
        // (the same speed-schedule mapping SampleAt used to place kf.eye).
        float u = 0.0f;
        if (D > 1e-3f) {
            ArcEnsure();
            u = ArcParamAtTime(i1, (t - b.time) / D);
        }
        // Degenerate split (the curve point essentially ON a keyframe): skip pinning - a nonzero weight baked
        // onto a near-coincident pair would fight the hold the overshoot guard provides.
        splitOk = D > 1e-3f && u > 1e-3f && u < 1.0f - 1e-3f;
        if (splitOk && i1 < sArc.segs) { // capture the pacing before the insert re-derives it
            float pe = (t - b.time) / D;
            float dur = SegDurAt(i1);
            speedBake = true;
            accelBake = true;
            spB = sArc.mOut[i1];
            spC = sArc.mIn[i1];
            spK = std::max(Hermite5Deriv(sArc.S[i1], sArc.S[i1 + 1], sArc.mOut[i1] * dur, sArc.mIn[i1] * dur,
                                         sArc.aOut[i1], sArc.aIn[i1], pe),
                           0.0f) /
                  D;
            // The accelerations too, or the split would keep the speeds and lose the curvature between them.
            acB = sArc.aOut[i1] / (dur * dur);
            acC = sArc.aIn[i1] / (dur * dur);
            acK = Hermite5Deriv2(sArc.S[i1], sArc.S[i1 + 1], sArc.mOut[i1] * dur, sArc.mIn[i1] * dur, sArc.aOut[i1],
                                 sArc.aIn[i1], pe) /
                  (dur * dur);
        }
        linearSeg = b.interp == CINE_INTERP_LINEAR;
        hasPrev = splitOk && ((i1 > 0) || LoopCyclic());
        hasNext = splitOk && ((i2 < n - 1) || LoopCyclic());
        if (splitOk) {
            bId = sIds[i1];
            cId = sIds[i2];
            float prevTd[3], prevTs[3], nextTd[3], nextTs[3];
            if (hasPrev) {
                int p1 = (i1 > 0) ? i1 - 1 : n - 1; // wraps only when looping
                EyeSegmentTangents(p1, i1, prevTd, prevTs);
                setWant(wBIn, prevTs, v3len(prevTs)); // b's arriving side re-spaces against the new key
            }
            if (hasNext) {
                int n2 = (i2 < n - 1) ? i2 + 1 : 0;
                EyeSegmentTangents(i2, n2, nextTd, nextTs);
                setWant(wCOut, nextTd, v3len(nextTd)); // c's leaving side, same
            }
            if (linearSeg) {
                // A linear segment splits into two linear halves along the same line - keep it linear (without
                // this the new key's default Smooth mode would curve the second half).
                kf.interp = CINE_INTERP_LINEAR;
            } else {
                float td[3], ts[3];
                EyeSegmentTangents(i1, i2, td, ts);
                float dv[3];
                for (int k = 0; k < 3; k++) {
                    dv[k] = Hermite1Deriv(b.eye[k], c.eye[k], td[k], ts[k], u);
                }
                // The original cubic H(s) restricted to [0,u], as a cubic in the first half's own parameter,
                // needs endpoint tangents u*H'(0) and u*H'(u); the second half (1-u)*H'(u) and (1-u)*H'(1).
                setWant(wBOut, td, v3len(td) * u);
                setWant(wCIn, ts, v3len(ts) * (1.0f - u));
                float dvLen = v3len(dv);
                setWant(wKIn, dv, dvLen * u);
                setWant(wKOut, dv, dvLen * (1.0f - u));
                kf.interp = CINE_INTERP_SMOOTH;
            }
            // Aim: capture the current curve's rates at b, the split point, and c (skipped for locked/rail
            // segments - they don't interpolate an aim curve - and for linear ones, whose halves lerp
            // identically anyway).
            // Inherit the aim MODE when both ends agree (and the target with it): a keyframe inserted between
            // two shots of the same actor should keep watching that actor, not freeze into a free framing.
            if (b.aimMode == c.aimMode) {
                kf.aimMode = b.aimMode;
                if (b.aimMode == CINE_AIM_ACTOR && b.aimActorId == c.aimActorId) {
                    kf.aimActorId = b.aimActorId;
                    kf.aimActorPtr = b.aimActorPtr;
                    std::memcpy(kf.aimActorPos, b.aimActorPos, sizeof(kf.aimActorPos));
                } else if (b.aimMode == CINE_AIM_ACTOR) {
                    kf.aimMode = CINE_AIM_FREE; // different actors: no single target to inherit
                }
            }
            bool aimSegOk = !linearSeg && sAimOverride != 4 && !AimLockedBetween(b, c) &&
                            !(b.aimMode == CINE_AIM_RAIL && c.aimMode == CINE_AIM_RAIL);
            float yB2, yC2, pB2, pC2, moY, miY, moP, miP, lB2, lC2;
            if (aimSegOk && AimSegmentCurve(i1, &yB2, &yC2, &pB2, &pC2, &moY, &miY, &moP, &miP, &lB2, &lC2)) {
                const float kR2D = 180.0f / 3.14159265f;
                float pe = (t - b.time) / D;
                aimBake = true;
                // Measured off the curve the aim ACTUALLY evaluates - the quintic, with the same end
                // curvatures - so what gets frozen is what was on screen. (This read the cubic's derivative
                // for a moment after the aim went quintic, which would have quietly reshaped every insert.)
                float qoY, qiY, qoP, qiP;
                AimSegmentAccels(i1, yB2, yC2, pB2, pC2, moY, miY, moP, miP, &qoY, &qiY, &qoP, &qiP);
                // Rates are per unit of the OWNING segment's progress, and the split hands each side a
                // segment that is only a fraction of the original - so each rate is scaled by that fraction,
                // exactly as the path tangents are (v3len * u / * (1-u) above). Same chain rule, same reason.
                float fIn = std::max(pe, 1e-4f), fOut = std::max(1.0f - pe, 1e-4f);
                aimB[1] = kR2D * moY * fIn; // b's leaving side: now spans only the first sub-segment
                aimB[3] = kR2D * moP * fIn;
                aimC[0] = kR2D * miY * fOut; // c's arriving side: only the second
                aimC[2] = kR2D * miP * fOut;
                float krY = Hermite5Deriv(yB2, yC2, moY, miY, qoY, qiY, pe);
                float krP = Hermite5Deriv(pB2, pC2, moP, miP, qoP, qiP, pe);
                aimK[0] = kR2D * krY * fIn;
                aimK[1] = kR2D * krY * fOut;
                aimK[2] = kR2D * krP * fIn;
                aimK[3] = kR2D * krP * fOut;
                // Curvature too, or the split would hold the angles and rates and let the BEND between them
                // re-derive itself. Progress-relative like the rates, so it scales by the square of the split
                // fraction (a curvature is a rate of a rate).
                accB[1] = kR2D * qoY * fIn * fIn;
                accB[3] = kR2D * qoP * fIn * fIn;
                accC[0] = kR2D * qiY * fOut * fOut;
                accC[2] = kR2D * qiP * fOut * fOut;
                float kcY = Hermite5Deriv2(yB2, yC2, moY, miY, qoY, qiY, pe);
                float kcP = Hermite5Deriv2(pB2, pC2, moP, miP, qoP, qiP, pe);
                accK[0] = kR2D * kcY * fIn * fIn;
                accK[1] = kR2D * kcY * fOut * fOut;
                accK[2] = kR2D * kcP * fIn * fIn;
                accK[3] = kR2D * kcP * fOut * fOut;
                accB[0] = accB[1]; // the outer sides get the neighbour segments' values below, when they exist
                accB[2] = accB[3];
                accC[1] = accC[0];
                accC[3] = accC[2];
                // b's arriving side and c's leaving side belong to the NEIGHBOUR segments, whose auto slopes
                // also re-derive against the new key - freeze them at their current values too.
                aimB[0] = aimB[1]; // fallbacks when there is no usable neighbour segment
                aimB[2] = aimB[3];
                aimC[1] = aimC[0];
                aimC[3] = aimC[2];
                float q0, q1, q2, q3, nmoY, nmiY, nmoP, nmiP, ql0, ql1;
                if (hasPrev) {
                    int p1 = (i1 > 0) ? i1 - 1 : n - 1;
                    if (!AimLockedBetween(sKeyframes[p1], b) &&
                        AimSegmentCurve(p1, &q0, &q1, &q2, &q3, &nmoY, &nmiY, &nmoP, &nmiP, &ql0, &ql1)) {
                        aimB[0] = kR2D * nmiY; // this segment isn't split, so its progress is unchanged
                        aimB[2] = kR2D * nmiP;
                        float ry, riy, rp, rip;
                        AimSegmentAccels(p1, q0, q1, q2, q3, nmoY, nmiY, nmoP, nmiP, &ry, &riy, &rp, &rip);
                        accB[0] = kR2D * riy;
                        accB[2] = kR2D * rip;
                        haveBIn = true;
                    }
                }
                if (hasNext) {
                    if (!AimLockedBetween(c, sKeyframes[(i2 < n - 1) ? i2 + 1 : 0]) &&
                        AimSegmentCurve(i2, &q0, &q1, &q2, &q3, &nmoY, &nmiY, &nmoP, &nmiP, &ql0, &ql1)) {
                        aimC[1] = kR2D * nmoY;
                        aimC[3] = kR2D * nmoP;
                        float ry, riy, rp, rip;
                        AimSegmentAccels(i2, q0, q1, q2, q3, nmoY, nmiY, nmoP, nmiP, &ry, &riy, &rp, &rip);
                        accC[1] = kR2D * ry;
                        accC[3] = kR2D * rp;
                        haveCOut = true;
                    }
                }
            }
        }
    }

    // The insert itself (sorts and selects the new key).
    AddKeyframeAtPlayhead(kf, false); // undo was already pushed above
    if (!splitOk) {
        return;
    }

    // Phase 2: apply the wants as direction pins + RELATIVE weights measured against the new arrangement.
    int newIdx = SelectedIndex();
    int bIdx = -1, cIdx = -1;
    for (int i = 0; i < (int)sIds.size(); i++) {
        if (sIds[i] == bId) {
            bIdx = i;
        } else if (sIds[i] == cId) {
            cIdx = i;
        }
    }
    if (newIdx < 0 || bIdx < 0 || cIdx < 0) {
        return; // should not happen; leave the plain insert rather than pin the wrong keys
    }
    int n2 = (int)sKeyframes.size();
    CineKeyframe& B = sKeyframes[bIdx];
    CineKeyframe& C = sKeyframes[cIdx];
    CineKeyframe& K = sKeyframes[newIdx];
    // Directions first, weights cleared, so the auto magnitudes measured below are exactly what the eval will
    // scale by the weights.
    auto pinDir = [](int& has, float* dst, const SideWant& w) {
        if (w.len > 0.0f) {
            dst[0] = w.dir[0];
            dst[1] = w.dir[1];
            dst[2] = w.dir[2];
            has = 1;
        }
    };
    pinDir(B.hasTangentIn, B.tangentIn, wBIn);
    pinDir(B.hasTangent, B.tangent, wBOut);
    pinDir(K.hasTangentIn, K.tangentIn, wKIn);
    pinDir(K.hasTangent, K.tangent, wKOut);
    pinDir(C.hasTangentIn, C.tangentIn, wCIn);
    pinDir(C.hasTangent, C.tangent, wCOut);
    if (wBIn.len > 0.0f) {
        B.tanWIn = 0.0f;
    }
    if (wBOut.len > 0.0f) {
        B.tanWOut = 0.0f;
    }
    if (wCIn.len > 0.0f) {
        C.tanWIn = 0.0f;
    }
    if (wCOut.len > 0.0f) {
        C.tanWOut = 0.0f;
    }
    float aTd[3], aTs[3];
    auto weigh = [](float want, float autoLen) {
        if (want <= 1e-5f || autoLen <= 1e-5f) {
            return 0.0f;
        }
        // Clamped: where the overshoot guard capped the auto magnitude, an uncapped compensating weight could
        // be huge - exact today, but explosive the moment a later edit relaxes the guard. Slight inexactness
        // in already-degenerate spots beats a latent loop.
        return std::min(std::max(want / autoLen, 0.1f), 4.0f);
    };
    if (wBIn.len > 0.0f && bIdx - 1 >= 0) {
        EyeSegmentTangents(bIdx - 1, bIdx, aTd, aTs);
        B.tanWIn = weigh(wBIn.len, v3len(aTs));
    } else if (wBIn.len > 0.0f && LoopCyclic()) {
        EyeSegmentTangents(n2 - 1, 0, aTd, aTs); // bIdx == 0 while looping: the arriving segment is the return leg
        B.tanWIn = weigh(wBIn.len, v3len(aTs));
    }
    if (wBOut.len > 0.0f || wKIn.len > 0.0f) {
        EyeSegmentTangents(bIdx, newIdx, aTd, aTs);
        if (wBOut.len > 0.0f) {
            B.tanWOut = weigh(wBOut.len, v3len(aTd));
        }
        if (wKIn.len > 0.0f) {
            K.tanWIn = weigh(wKIn.len, v3len(aTs));
        }
    }
    if (wKOut.len > 0.0f || wCIn.len > 0.0f) {
        EyeSegmentTangents(newIdx, cIdx, aTd, aTs);
        if (wKOut.len > 0.0f) {
            K.tanWOut = weigh(wKOut.len, v3len(aTd));
        }
        if (wCIn.len > 0.0f) {
            C.tanWIn = weigh(wCIn.len, v3len(aTs));
        }
    }
    if (wCOut.len > 0.0f) {
        int nn = (cIdx < n2 - 1) ? cIdx + 1 : 0; // wraps only when looping (hasNext guaranteed a next segment)
        EyeSegmentTangents(cIdx, nn, aTd, aTs);
        C.tanWOut = weigh(wCOut.len, v3len(aTd));
    }
    // Speed bake: freeze the pacing at the three keyframes so the new knot can't re-derive the profile. Only
    // the sides that touch the split are pinned - b's arriving side and c's leaving side belong to segments
    // the insert didn't touch, and their automatic values are still correct.
    if (speedBake) {
        B.speedRate = spB;
        C.speedRate = spC;
        K.speedRate = spK;
        if (accelBake) {
            B.hasAccelOut = 1;
            B.speedAccelOut = acB;
            C.hasAccelIn = 1;
            C.speedAccelIn = acC;
            K.hasAccelIn = K.hasAccelOut = 1;
            K.speedAccelIn = acK;
            K.speedAccelOut = acK;
        }
    }
    // Aim bake: write the captured rates as explicit values on all three keyframes.
    if (aimBake) {
        // Pin only the sides that were measured. B's leaving side and C's arriving side face the split and
        // are always known; their outer sides belong to segments the insert did not touch, and are pinned
        // only when their own segment could be read.
        B.hasAimAccOut = B.hasAimTanOut = 1;
        C.hasAimAccIn = C.hasAimTanIn = 1;
        K.hasAimAccIn = K.hasAimAccOut = K.hasAimTanIn = K.hasAimTanOut = 1;
        B.hasAimAccIn = B.hasAimTanIn = haveBIn ? 1 : 0;
        C.hasAimAccOut = C.hasAimTanOut = haveCOut ? 1 : 0;
        B.aimAccYawIn = accB[0];
        B.aimAccYawOut = accB[1];
        B.aimAccPitchIn = accB[2];
        B.aimAccPitchOut = accB[3];
        C.aimAccYawIn = accC[0];
        C.aimAccYawOut = accC[1];
        C.aimAccPitchIn = accC[2];
        C.aimAccPitchOut = accC[3];
        K.aimAccYawIn = accK[0];
        K.aimAccYawOut = accK[1];
        K.aimAccPitchIn = accK[2];
        K.aimAccPitchOut = accK[3];
        B.aimTanYawIn = aimB[0];
        B.aimTanYawOut = aimB[1];
        B.aimTanPitchIn = aimB[2];
        B.aimTanPitchOut = aimB[3];
        K.aimTanYawIn = aimK[0];
        K.aimTanYawOut = aimK[1];
        K.aimTanPitchIn = aimK[2];
        K.aimTanPitchOut = aimK[3];
        C.aimTanYawIn = aimC[0];
        C.aimTanYawOut = aimC[1];
        C.aimTanPitchIn = aimC[2];
        C.aimTanPitchOut = aimC[3];
    }
}

// Serialize / restore a parameter track to the path file. Generic so future tracks reuse it.
static nlohmann::json TrackToJson(const CineParamTrack& t) {
    nlohmann::json keys = nlohmann::json::array();
    for (const CineParamKey& k : t.keys) {
        nlohmann::json kj = { { "time", k.time }, { "value", k.value }, { "interp", k.interp } };
        if (k.hasHandles) { // only persist explicit Bezier handles
            kj["h"] = { k.hOutT, k.hOutV, k.hInT, k.hInV };
            kj["hb"] = k.brokenHandles;
        }
        keys.push_back(kj);
    }
    return { { "enabled", t.enabled }, { "keys", keys } };
}

static void TrackFromJson(CineParamTrack& t, const nlohmann::json& j) {
    t.keys.clear();
    t.enabled = j.value("enabled", false);
    if (j.contains("keys")) {
        for (const auto& e : j["keys"]) {
            CineParamKey k{};
            k.time = e.value("time", 0.0f);
            k.value = e.value("value", 0.0f);
            k.interp = e.value("interp", -1);
            k.id = sNextParamKeyId++;
            if (e.contains("h") && e["h"].size() >= 4) {
                k.hOutT = e["h"][0];
                k.hOutV = e["h"][1];
                k.hInT = e["h"][2];
                k.hInV = e["h"][3];
                k.hasHandles = 1;
                k.brokenHandles = e.value("hb", 0);
            }
            t.keys.push_back(k);
        }
        std::sort(t.keys.begin(), t.keys.end(),
                  [](const CineParamKey& a, const CineParamKey& b) { return a.time < b.time; });
    }
}

// The saved-path format version, stamped into every file from now on.
//
// Up to this point the format changed freely and old files quietly lost whatever had been renamed - which was
// fine while the only paths in existence were throwaway test ones. It stops being fine the moment somebody
// else saves a shot they spent an evening on. From here: bump this when the meaning of a field changes, and
// either read the old form correctly or say plainly what was dropped. Version 0 (no field) means a file from
// before this line existed - those carry per-keyframe ease and deg/s aim rates, both of which mean something
// different now, so they load with those values left automatic rather than misinterpreted.
static const int kPathFormatVersion = 1;

// Write the whole path to cinematics/<base>.json. `bindEntrance` updates the path-bound location from the
// current scene (manual saves only - the autosave must not silently rebind or unbind it).
static void SavePathTo(const char* base, bool bindEntrance) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& k : sKeyframes) {
        arr.push_back({ { "time", k.time },
                        { "eye", { k.eye[0], k.eye[1], k.eye[2] } },
                        { "at", { k.at[0], k.at[1], k.at[2] } },
                        { "roll", k.roll },
                        { "fov", k.fov },
                        { "interp", k.interp },
                        { "tension", k.tension },
                        { "continuity", k.continuity },
                        { "bias", k.bias },
                        { "hasTangent", k.hasTangent },
                        { "tangent", { k.tangent[0], k.tangent[1], k.tangent[2] } },
                        { "hasTangentIn", k.hasTangentIn },
                        { "tangentIn", { k.tangentIn[0], k.tangentIn[1], k.tangentIn[2] } },
                        { "tanWOut", k.tanWOut },
                        { "tanWIn", k.tanWIn },
                        { "aimMode", k.aimMode },
                        { "aimHold", k.aimHold },
                        { "hasAimTanP", { k.hasAimTanIn, k.hasAimTanOut } },
                        { "aimTanP", { k.aimTanYawIn, k.aimTanYawOut, k.aimTanPitchIn, k.aimTanPitchOut } },
                        { "hasAimAcc", { k.hasAimAccIn, k.hasAimAccOut } },
                        { "aimAcc", { k.aimAccYawIn, k.aimAccYawOut, k.aimAccPitchIn, k.aimAccPitchOut } },
                        { "speed",
                          { k.speedRate, k.speedAccelIn, k.speedAccelOut, (float)k.hasAccelIn, (float)k.hasAccelOut,
                            (float)k.speedBroken } },
                        { "aimActorId", k.aimActorId },
                        { "aimActorPos", { k.aimActorPos[0], k.aimActorPos[1], k.aimActorPos[2] } } });
    }
    // Object wrapper carries path-level state (the shared aim target) alongside the keyframes.
    nlohmann::json j;
    j["keyframes"] = arr;
    j["target"] = { sAimOverridePoint[0], sAimOverridePoint[1], sAimOverridePoint[2] };
    // Optionally bind the current location (entrance = scene + spawn) so loading the path warps you back here.
    if (bindEntrance) {
        sPathEntrance = sBindLocation ? CinematicCam_GetCurrentEntrance() : -1;
    }
    j["entrance"] = sPathEntrance;
    j["formatVersion"] = kPathFormatVersion;
    // Playback settings that define how the path MOVES, not just where: without these, loading a path in a fresh
    // session could play back visibly differently than authored (the spline parameterization even changes the
    // curve shape). Old files without this block simply keep the session's current settings.
    j["playback"] = { { "loop", sLoop },
                      { "loopMode", sLoopMode },
                      { "loopReturn", sLoopReturnTime },
                      { "easeMode", sEaseMode },
                      { "easeAmount", sEaseAmount },
                      { "speed", sPlaySpeed },
                      { "aimOverride", sAimOverride },
                      { "aimOverrideActorId", sAimOverrideActorId },
                      { "shakeOn", sShakeEnabled },
                      { "shakePosAmp", sShakePosAmp },
                      { "shakeRotAmp", sShakeRotAmp },
                      { "shakeFreq", sShakeFreq } };
    // Parameter automation tracks (each keyed by its stable id).
    for (const TrackDef& d : AllTrackDefs()) {
        j["tracks"][d.track->id] = TrackToJson(*d.track);
    }
    std::filesystem::create_directories("cinematics");
    std::ofstream f(std::string("cinematics/") + base + ".json");
    if (f.good()) {
        f << j.dump(2);
    }
}

static void SavePath() {
    SavePathTo(sFilename, true);
    sDirty = false;
    sDirtyForAutosave = false;
    time_t tt = time(nullptr);
    struct tm* lt = localtime(&tt);
    snprintf(sFileStatus, sizeof(sFileStatus), "Saved %s.json at %02d:%02d", sFilename, lt ? lt->tm_hour : 0,
             lt ? lt->tm_min : 0);
}

static void LoadPath() {
    std::ifstream f(std::string("cinematics/") + sFilename + ".json");
    if (!f.good()) {
        return;
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (...) { return; }
    ClearPath();
    // New files are an object { keyframes, target, entrance }; old files are a bare keyframe array (still supported).
    const nlohmann::json* arr = &j;
    sPathEntrance = -1;
    if (j.is_object()) {
        if (j.contains("keyframes")) {
            arr = &j["keyframes"];
        }
        if (j.contains("target") && j["target"].size() >= 3) {
            sAimOverridePoint[0] = j["target"][0];
            sAimOverridePoint[1] = j["target"][1];
            sAimOverridePoint[2] = j["target"][2];
        }
        sPathEntrance = j.value("entrance", -1);
        // Restore the authored playback settings (see SavePath). Defaults are the CURRENT session values, so old
        // files without this block change nothing.
        if (j.contains("playback")) {
            const nlohmann::json& p = j["playback"];
            sLoop = p.value("loop", sLoop);
            sLoopMode = p.value("loopMode", sLoopMode);
            sLoopReturnTime = p.value("loopReturn", sLoopReturnTime);
            sEaseMode = p.value("easeMode", sEaseMode);
            sEaseAmount = p.value("easeAmount", sEaseAmount);
            sPlaySpeed = p.value("speed", sPlaySpeed);
            sAimOverride = p.value("aimOverride", sAimOverride);
            sAimOverrideActorId = p.value("aimOverrideActorId", sAimOverrideActorId);
            sAimOverrideActorPtr = nullptr; // stale across sessions - re-resolved live by id
            sShakeEnabled = p.value("shakeOn", sShakeEnabled);
            sShakePosAmp = p.value("shakePosAmp", sShakePosAmp);
            sShakeRotAmp = p.value("shakeRotAmp", sShakeRotAmp);
            sShakeFreq = p.value("shakeFreq", sShakeFreq);
        }
        if (j.contains("tracks")) {
            for (const TrackDef& d : AllTrackDefs()) {
                if (j["tracks"].contains(d.track->id)) {
                    TrackFromJson(*d.track, j["tracks"][d.track->id]);
                }
            }
        }
    }
    for (auto& e : *arr) {
        CineKeyframe k{};
        k.time = e.value("time", 0.0f);
        k.eye[0] = e["eye"][0];
        k.eye[1] = e["eye"][1];
        k.eye[2] = e["eye"][2];
        k.at[0] = e["at"][0];
        k.at[1] = e["at"][1];
        k.at[2] = e["at"][2];
        k.roll = e.value("roll", 0.0f);
        k.fov = e.value("fov", 60.0f);
        k.interp = e.value("interp", 0);
        k.tension = e.value("tension", 0.0f);
        k.continuity = e.value("continuity", 0.0f);
        k.bias = e.value("bias", 0.0f);
        k.hasTangent = e.value("hasTangent", 0);
        if (e.contains("tangent")) {
            k.tangent[0] = e["tangent"][0];
            k.tangent[1] = e["tangent"][1];
            k.tangent[2] = e["tangent"][2];
        } else {
            k.tangent[0] = 0.0f;
            k.tangent[1] = 0.0f;
            k.tangent[2] = 1.0f;
        }
        k.hasTangentIn = e.value("hasTangentIn", 0);
        if (e.contains("tangentIn")) {
            k.tangentIn[0] = e["tangentIn"][0];
            k.tangentIn[1] = e["tangentIn"][1];
            k.tangentIn[2] = e["tangentIn"][2];
        } else { // older files: the in side mirrors the out tangent
            k.tangentIn[0] = k.tangent[0];
            k.tangentIn[1] = k.tangent[1];
            k.tangentIn[2] = k.tangent[2];
        }
        k.tanWOut = e.value("tanWOut", 0.0f);
        k.tanWIn = e.value("tanWIn", 0.0f);
        k.aimMode = e.value("aimMode", 0);
        // "Flat" on either side of the short-lived Aim in/out controls becomes the hold flag.
        k.aimHold = e.value("aimHold", (e.value("aimTanIn", 0) == 1 || e.value("aimTanOut", 0) == 1) ? 1 : 0);
        // Only the progress-relative form is read. Files written while these were deg/s carry the old
        // "aimTan" key, which is deliberately ignored: the numbers mean something different now, and falling
        // back to automatic rates gives those paths a correct curve rather than a plausible wrong one.
        if (e.contains("hasAimTanP") && e["hasAimTanP"].is_array() && e["hasAimTanP"].size() >= 2) {
            k.hasAimTanIn = e["hasAimTanP"][0];
            k.hasAimTanOut = e["hasAimTanP"][1];
        }
        if (e.contains("aimTanP") && e["aimTanP"].size() >= 4) {
            k.aimTanYawIn = e["aimTanP"][0];
            k.aimTanYawOut = e["aimTanP"][1];
            k.aimTanPitchIn = e["aimTanP"][2];
            k.aimTanPitchOut = e["aimTanP"][3];
        }
        if (e.contains("hasAimAcc") && e["hasAimAcc"].is_array() && e["hasAimAcc"].size() >= 2) {
            k.hasAimAccIn = e["hasAimAcc"][0];
            k.hasAimAccOut = e["hasAimAcc"][1];
        }
        if (e.contains("aimAcc") && e["aimAcc"].size() >= 4) {
            k.aimAccYawIn = e["aimAcc"][0];
            k.aimAccYawOut = e["aimAcc"][1];
            k.aimAccPitchIn = e["aimAcc"][2];
            k.aimAccPitchOut = e["aimAcc"][3];
        }
        if (e.contains("speed") && e["speed"].is_array() && e["speed"].size() >= 6) {
            k.speedRate = e["speed"][0];
            k.speedAccelIn = e["speed"][1];
            k.speedAccelOut = e["speed"][2];
            k.hasAccelIn = (int)(float)e["speed"][3];
            k.hasAccelOut = (int)(float)e["speed"][4];
            k.speedBroken = (int)(float)e["speed"][5];
        } else if (e.contains("speedRate") && e["speedRate"].is_array() && e["speedRate"].size() >= 3) {
            // Paths saved while speed was per-side: take whichever side was set. The two can no longer differ,
            // so a file that had them apart collapses onto the leaving speed.
            float in = e["speedRate"][0], out = e["speedRate"][1];
            k.speedRate = (out >= 0.0f) ? out : in;
        }
        k.aimActorId = e.value("aimActorId", 0);
        k.aimActorPtr = nullptr;
        if (e.contains("aimActorPos") && e["aimActorPos"].size() >= 3) {
            k.aimActorPos[0] = e["aimActorPos"][0];
            k.aimActorPos[1] = e["aimActorPos"][1];
            k.aimActorPos[2] = e["aimActorPos"][2];
        } else {
            k.aimActorPos[0] = k.aimActorPos[1] = k.aimActorPos[2] = 0.0f;
        }
        // "easeIn"/"easeOut" in older files are deliberately not read - per-keyframe ease is gone, replaced by
        // the speed curve's acceleration handles.
        sKeyframes.push_back(k);
        sIds.push_back(sNextId++);
    }
    SortByTime();
    SelectOnly(sIds.empty() ? -1 : sIds[0]);
    sDirty = false;
    sDirtyForAutosave = false;
    // Say what happened, including what was dropped - a file that loads "fine" while silently discarding
    // shaping the author set is worse than one that admits it.
    int fileVer = j.value("formatVersion", 0);
    if (fileVer > kPathFormatVersion) {
        snprintf(sFileStatus, sizeof(sFileStatus),
                 "Loaded %s.json (%d keyframes) - saved by a NEWER build; "
                 "anything it added was ignored",
                 sFilename, (int)sKeyframes.size());
    } else if (fileVer < kPathFormatVersion) {
        snprintf(sFileStatus, sizeof(sFileStatus),
                 "Loaded %s.json (%d keyframes) - older format: per-keyframe ease and baked aim rates are "
                 "automatic now",
                 sFilename, (int)sKeyframes.size());
    } else {
        snprintf(sFileStatus, sizeof(sFileStatus), "Loaded %s.json (%d keyframes)", sFilename, (int)sKeyframes.size());
    }

    // If this path is bound to a location, warp there - unless we're already in that scene (avoids a needless
    // fade reload when re-loading a path in its home area).
    if (sTeleportOnLoad && sPathEntrance >= 0 && CinematicCam_GetCurrentEntrance() != sPathEntrance) {
        CinematicCam_WarpToEntrance(sPathEntrance);
    }
}

// List saved cinematic file base-names (without the .json extension) in the cinematics/ folder, sorted.
static std::vector<std::string> ListCinematics() {
    std::vector<std::string> out;
    std::error_code ec;
    if (std::filesystem::exists("cinematics", ec)) {
        for (auto& e : std::filesystem::directory_iterator("cinematics", ec)) {
            if (e.is_regular_file() && e.path().extension() == ".json") {
                out.push_back(e.path().stem().string());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Point sFilename at a saved file and load it.
static void LoadNamed(const std::string& name) {
    strncpy(sFilename, name.c_str(), sizeof(sFilename) - 1);
    sFilename[sizeof(sFilename) - 1] = '\0';
    LoadPath();
}

// ---------------------------------------------------------------------------
// In-world overlay
// ---------------------------------------------------------------------------
static bool WorldToScreen(const float* world, ImVec2& out) {
    float ndcX, ndcY;
    if (!CinematicCam_WorldToNdc((float*)world, &ndcX, &ndcY)) {
        return false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();

    // viewProjectionMtxF is built with the original 4:3 aspect (game logic / culling space), but the frame
    // is rendered at the viewport's real aspect. Correct X so markers track at any screen width.
    float aspect = (vp->Size.y > 0.0f) ? (vp->Size.x / vp->Size.y) : (4.0f / 3.0f);
    ndcX *= (4.0f / 3.0f) / aspect;

    // Map NDC into the main viewport's pixel rect (SoH uses multi-viewport ImGui, so the game viewport
    // is not necessarily at the screen origin).
    out.x = vp->Pos.x + (ndcX * 0.5f + 0.5f) * vp->Size.x;
    out.y = vp->Pos.y + (1.0f - (ndcY * 0.5f + 0.5f)) * vp->Size.y;
    return true;
}

// World position of a keyframe's aim handle (a point a short way along its look direction).
static void FacingHandleWorld(const CineKeyframe& k, float out[3]) {
    out[0] = k.eye[0] + (k.at[0] - k.eye[0]) * 0.4f;
    out[1] = k.eye[1] + (k.at[1] - k.eye[1]) * 0.4f;
    out[2] = k.eye[2] + (k.at[2] - k.eye[2]) * 0.4f;
}

// ---------------------------------------------------------------------------
// Small float[3] vector helpers + the transform gizmo
// ---------------------------------------------------------------------------
static void v3sub(const float* a, const float* b, float* o) {
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}
static void v3add(const float* a, const float* b, float* o) {
    o[0] = a[0] + b[0];
    o[1] = a[1] + b[1];
    o[2] = a[2] + b[2];
}
static void v3mad(const float* a, const float* d, float s, float* o) { // o = a + d*s
    o[0] = a[0] + d[0] * s;
    o[1] = a[1] + d[1] * s;
    o[2] = a[2] + d[2] * s;
}
static float v3dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void v3cross(const float* a, const float* b, float* o) {
    float x = a[1] * b[2] - a[2] * b[1];
    float y = a[2] * b[0] - a[0] * b[2];
    float z = a[0] * b[1] - a[1] * b[0];
    o[0] = x;
    o[1] = y;
    o[2] = z;
}
static float v3len(const float* a) {
    return std::sqrt(v3dot(a, a));
}
static void v3norm(float* a) {
    float l = v3len(a);
    if (l > 1e-6f) {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
}
// Rodrigues rotation of v around unit axis k by angle (radians).
static void v3rot(const float* v, const float* k, float ang, float* o) {
    float c = std::cos(ang);
    float s = std::sin(ang);
    float kv[3];
    v3cross(k, v, kv);
    float kd = v3dot(k, v);
    for (int i = 0; i < 3; i++) {
        o[i] = v[i] * c + kv[i] * s + k[i] * kd * (1.0f - c);
    }
}

// --- Path tools: smoothing + speed normalization -------------------------------------------------

// The keyframe-index range the path tools operate on: the span of the current multi-selection when 2+
// keyframes are selected, the whole path otherwise. Returns false if the path is too short.
static bool ToolRange(int& lo, int& hi) {
    int n = (int)sKeyframes.size();
    if (n < 2) {
        return false;
    }
    lo = 0;
    hi = n - 1;
    if (SelectionCount() >= 2) {
        int mn = n, mx = -1;
        for (int i = 0; i < n; i++) {
            if (IsSelected(sIds[i])) {
                mn = std::min(mn, i);
                mx = std::max(mx, i);
            }
        }
        if (mx > mn) {
            lo = mn;
            hi = mx;
        }
    }
    return hi > lo;
}

// Smooth WITHOUT moving any keyframe: reset the range (selection span, else whole path) to clean spline
// defaults so the centripetal parameterization can produce loop-free, even motion. Removes linear (sharp)
// Smooth path: make the range's motion CONTINUOUS - solve for the tangents of a C2 cubic (direction AND
// curvature smooth, the physical-dolly look; also what makes rail aim glide, since rail IS the tangent) and
// BAKE the result into the per-keyframe tangent storage. Keyframes never move; their tangents rotate and
// rescale. This is also the "release locality" moment: inside the range every keyframe shapes the whole
// stretch (that's what the solve does), while the range's end tangents are frozen at their current values so
// nothing outside it reshapes.
static void SmoothPath() {
    int lo, hi;
    if (!ToolRange(lo, hi)) {
        return;
    }
    int n = (int)sKeyframes.size();
    int m = hi - lo; // segments in range
    // Whole looping path: solve the PERIODIC spline - the loop-return segment is a segment like any other and
    // the seam becomes just another smooth knot. (A clamped solve left the seam with the old corner.)
    bool cyc = LoopCyclic() && lo == 0 && hi == n - 1 && n >= 3;
    int nk = cyc ? n : m + 1; // knots being solved
    int ns = cyc ? n : m;     // intervals (cyclic includes loop-return: last -> first)
    if (!cyc && m < 2) {
        return; // nothing to bend
    }
    PushUndo();
    for (int i = lo; i < (cyc ? n : hi); i++) {
        sKeyframes[i].interp = CINE_INTERP_SMOOTH; // a linear segment can't be part of a continuous curve
    }
    // Knot intervals: the same centripetal (chord^0.5) parameterization the evaluator uses.
    std::vector<float> h((size_t)ns);
    for (int i = 0; i < ns; i++) {
        const float* p = sKeyframes[lo + i].eye;
        const float* q = sKeyframes[(lo + i + 1) % n].eye;
        float e0 = q[0] - p[0], e1 = q[1] - p[1], e2 = q[2] - p[2];
        h[i] = std::max(std::sqrt(std::sqrt(e0 * e0 + e1 * e1 + e2 * e2)), 1e-3f);
    }
    // Solved knot velocities (dEye/dt in the global centripetal parameter), per axis. Sized to the path
    // rather than to a fixed buffer: these used to be 64-entry arrays with an early return above them, so on
    // any path longer than 64 keyframes Smooth path did nothing at all and said nothing about it.
    std::vector<std::array<float, 3>> M((size_t)nk);
    if (cyc) {
        // Periodic C2 system: every knot is interior, indices wrap. Solved as a dense system - it runs once
        // per button press, so an O(n^3) elimination is fine well past any realistic path length.
        std::vector<float> A((size_t)nk * (size_t)(nk + 1));
        auto at = [&](int r, int c) -> float& { return A[(size_t)r * (size_t)(nk + 1) + (size_t)c]; };
        for (int ax = 0; ax < 3; ax++) {
            std::fill(A.begin(), A.end(), 0.0f);
            for (int i = 0; i < nk; i++) {
                int ip = (i - 1 + nk) % nk, in2 = (i + 1) % nk;
                float hp = h[ip], hn = h[i];
                float Pm = sKeyframes[ip].eye[ax], P0 = sKeyframes[i].eye[ax], Pp = sKeyframes[in2].eye[ax];
                at(i, ip) += 1.0f / hp;
                at(i, i) += 2.0f * (1.0f / hp + 1.0f / hn);
                at(i, in2) += 1.0f / hn;
                at(i, nk) = 3.0f * ((P0 - Pm) / (hp * hp) + (Pp - P0) / (hn * hn));
            }
            for (int col = 0; col < nk; col++) { // Gaussian elimination with partial pivoting
                int piv = col;
                for (int r = col + 1; r < nk; r++) {
                    if (std::fabs(at(r, col)) > std::fabs(at(piv, col))) {
                        piv = r;
                    }
                }
                for (int cc = 0; cc <= nk; cc++) {
                    std::swap(at(col, cc), at(piv, cc));
                }
                if (std::fabs(at(col, col)) < 1e-9f) {
                    continue;
                }
                for (int r = 0; r < nk; r++) {
                    if (r == col) {
                        continue;
                    }
                    float w = at(r, col) / at(col, col);
                    for (int cc = col; cc <= nk; cc++) {
                        at(r, cc) -= w * at(col, cc);
                    }
                }
            }
            for (int i = 0; i < nk; i++) {
                M[i][ax] = (std::fabs(at(i, i)) > 1e-9f) ? at(i, nk) / at(i, i) : 0.0f;
            }
        }
    } else {
        // Open range: clamped solve (Thomas algorithm) - the range's CURRENT end tangents are the boundary
        // conditions, so nothing outside the range reshapes.
        float tdLo[3], tsLo[3], tdHi[3], tsHi[3];
        EyeSegmentTangents(lo, lo + 1, tdLo, tsLo);
        EyeSegmentTangents(hi - 1, hi, tdHi, tsHi);
        std::vector<float> diagA((size_t)nk), diagB((size_t)nk), diagC((size_t)nk), rhs((size_t)nk);
        for (int ax = 0; ax < 3; ax++) {
            diagB[0] = 1.0f;
            diagC[0] = 0.0f;
            rhs[0] = tdLo[ax] / h[0];
            for (int i = 1; i < nk - 1; i++) {
                float hp = h[i - 1], hn = h[i];
                float Pm = sKeyframes[lo + i - 1].eye[ax], P0 = sKeyframes[lo + i].eye[ax],
                      Pp = sKeyframes[lo + i + 1].eye[ax];
                diagA[i] = 1.0f / hp;
                diagB[i] = 2.0f * (1.0f / hp + 1.0f / hn);
                diagC[i] = 1.0f / hn;
                rhs[i] = 3.0f * ((P0 - Pm) / (hp * hp) + (Pp - P0) / (hn * hn));
            }
            diagA[nk - 1] = 0.0f;
            diagB[nk - 1] = 1.0f;
            rhs[nk - 1] = tsHi[ax] / h[ns - 1];
            for (int i = 1; i < nk; i++) { // forward elimination (diagA[nk-1] is 0: last row untouched)
                float w = diagA[i] / diagB[i - 1];
                diagB[i] -= w * diagC[i - 1];
                rhs[i] -= w * rhs[i - 1];
            }
            M[nk - 1][ax] = rhs[nk - 1] / diagB[nk - 1];
            for (int i = nk - 2; i >= 0; i--) {
                M[i][ax] = (rhs[i] - diagC[i] * M[i + 1][ax]) / diagB[i];
            }
        }
    }
    // Bake phase 1: write the solved DIRECTIONS (weights cleared so the magnitude measurement below is
    // unweighted). Cyclic solves bake every knot; open solves bake the interior only (ends are frozen).
    int bake0 = cyc ? 0 : 1, bake1 = cyc ? nk - 1 : nk - 2;
    for (int i = bake0; i <= bake1; i++) {
        CineKeyframe& k = sKeyframes[lo + i];
        float dir[3] = { M[i][0], M[i][1], M[i][2] };
        if (v3len(dir) < 1e-5f) {
            continue; // stationary knot: leave it as authored
        }
        v3norm(dir);
        k.hasTangent = 1;
        k.hasTangentIn = 0; // one direction through the knot - that is the continuity
        std::memcpy(k.tangent, dir, sizeof(k.tangent));
        k.tanWOut = 0.0f;
        k.tanWIn = 0.0f;
    }
    // Bake phase 2: weights = solved magnitude / automatic magnitude, per side (the storage is relative so
    // later keyframe moves rescale naturally - same scheme as Insert @ playhead).
    for (int i = bake0; i <= bake1; i++) {
        CineKeyframe& k = sKeyframes[lo + i];
        if (!k.hasTangent) {
            continue;
        }
        int gi = lo + i;
        int prevSeg = (gi - 1 + n) % n;   // segment arriving at this knot (wraps only when cyclic)
        float hOut = h[i % ns];           // interval of the segment leaving this knot
        float hIn = h[(i - 1 + ns) % ns]; // interval of the segment arriving
        float td[3], ts[3];
        EyeSegmentTangents(gi, (gi + 1) % n, td, ts);
        float autoOut = v3len(td);
        EyeSegmentTangents(prevSeg, gi, td, ts);
        float autoIn = v3len(ts);
        float mag = std::sqrt(M[i][0] * M[i][0] + M[i][1] * M[i][1] + M[i][2] * M[i][2]);
        if (autoOut > 1e-5f) {
            k.tanWOut = std::min(std::max(mag * hOut / autoOut, 0.1f), 4.0f);
        }
        if (autoIn > 1e-5f) {
            k.tanWIn = std::min(std::max(mag * hIn / autoIn, 0.1f), 4.0f);
        }
    }
}

// Arc length of the spline between keyframe i and i+1, straight off the schedule's own length table.
//
// It used to measure by walking SampleAt over the segment's time span - which samples the path at whatever
// pace the schedule currently runs, so a fast stretch got fewer effective samples and measured SHORT. The
// measurement therefore depended on the very timing it was about to rewrite: normalizing changed the times,
// which changed the next measurement, so one pass never landed and a second click moved things again. The arc
// table is built in curve-parameter space and is genuinely time-independent, which makes normalize a single
// exact pass.
static float SegmentArcLength(int i) {
    ArcEnsure();
    if (i >= 0 && i + 1 < (int)sArc.S.size()) {
        return sArc.S[i + 1] - sArc.S[i];
    }
    float d[3] = { sKeyframes[i + 1].eye[0] - sKeyframes[i].eye[0], sKeyframes[i + 1].eye[1] - sKeyframes[i].eye[1],
                   sKeyframes[i + 1].eye[2] - sKeyframes[i].eye[2] };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// Re-time keyframes so each segment's duration is ~proportional to its physical path length: the camera then
// covers the range at a near-constant speed. Operates on the selection span when 2+ keyframes are selected,
// else the whole path; the range's first/last times (and everything outside it) are preserved. One pass is
// exact now - the geometry no longer depends on the times, so re-timing cannot change the arc lengths.
//
// Floor each segment: two physically close keyframes have a tiny arc length, so pure proportionality gives
// them a near-zero duration -> a velocity spike as the camera jumps between them. Each segment gets at least
// a small share of the average, which keeps the motion smooth right where it used to struggle.
static void NormalizeSpeed() {
    int lo, hi;
    if (!ToolRange(lo, hi) || hi - lo < 2) {
        return; // need at least 3 keyframes in the range (2 segments) for re-timing to mean anything
    }
    float base = sKeyframes[lo].time;
    float totalTime = sKeyframes[hi].time - base;
    if (totalTime <= 1e-4f) {
        return;
    }
    int segs = hi - lo;
    std::vector<float> seg(segs);
    std::vector<float> origT(segs + 1); // times BEFORE re-timing (hold durations are read from these while
    for (int i = 0; i <= segs; i++) {   // the loop below is already rewriting earlier keyframes)
        origT[i] = sKeyframes[lo + i].time;
    }
    float totalLen = 0.0f, holdTime = 0.0f;
    for (int i = 0; i < segs; i++) {
        seg[i] = SegmentArcLength(lo + i);
        totalLen += seg[i];
        if (seg[i] <= 1e-3f) { // a deliberate hold (coincident keyframes): its authored pause is kept as-is
            holdTime += origT[i + 1] - origT[i];
        }
    }
    float moveTime = totalTime - holdTime;
    if (totalLen <= 1e-4f || moveTime <= 1e-3f) {
        return;
    }
    PushUndo();
    // Near-pure proportionality over the MOVING segments. This used to carry a 15% per-segment floor -
    // compensation for the old time-knot spline's velocity spikes - but under the arc-length engine any extra
    // time handed to a short segment IS a speed difference, which made "normalized" paths still feel uneven.
    // The tiny floor that remains only guards against degenerate weights.
    const float kFloor = 0.02f;
    float avgLen = totalLen / (float)segs;
    std::vector<float> w(segs);
    float wsum = 0.0f;
    for (int i = 0; i < segs; i++) {
        w[i] = (seg[i] <= 1e-3f) ? 0.0f : seg[i] + kFloor * avgLen;
        wsum += w[i];
    }
    if (wsum <= 1e-6f) {
        return;
    }
    float t = base;
    for (int i = 1; i <= segs; i++) {
        float dur = (seg[i - 1] <= 1e-3f) ? (origT[i] - origT[i - 1]) : moveTime * (w[i - 1] / wsum);
        t += dur;
        if (i < segs) {
            sKeyframes[lo + i].time = t;
        }
    }
    // Even speed is exactly what an explicit speed handle contradicts - clear them across the range, or the
    // schedule would keep forcing the old per-keyframe rates and the path would still play unevenly.
    for (int i = lo; i <= hi; i++) {
        sKeyframes[i].speedRate = -1.0f;
        sKeyframes[i].hasAccelIn = 0;
        sKeyframes[i].hasAccelOut = 0;
        sKeyframes[i].speedAccelIn = 0.0f;
        sKeyframes[i].speedAccelOut = 0.0f;
        sKeyframes[i].speedBroken = 0;
    }
    // A cyclic path's return leg is a real segment carrying real distance: give it the same speed as the rest,
    // otherwise the seam plays at whatever the old return time happened to be (fast or crawling).
    if (LoopCyclic() && lo == 0 && hi == (int)sKeyframes.size() - 1) {
        ArcEnsure(); // geometry is time-independent, so the cached lengths survive the re-timing above
        float speed = totalLen / moveTime;
        if (sArc.segs >= 1 && speed > 1e-4f) {
            float retLen = sArc.S[sArc.segs] - sArc.S[sArc.segs - 1]; // the return leg is the last arc segment
            if (retLen > 1e-3f) {
                sLoopReturnTime = retLen / speed;
            }
        }
    }
}

// Compress/expand the keyframes [lo..hi] to a new span, anchored at lo: every interval scales by the same
// factor, so the group's internal rhythm is intact while its total duration changes. Keyframes outside the
// range keep their absolute times (expanding far enough to overlap later keys re-sorts; the timeline shows it).
static void ScaleSelection(int lo, int hi, float newDur) {
    int n = (int)sKeyframes.size();
    if (lo < 0 || hi <= lo || hi >= n || newDur <= 1e-3f) {
        return;
    }
    float old = sKeyframes[hi].time - sKeyframes[lo].time;
    if (old <= 1e-4f) {
        return;
    }
    PushUndo();
    float s = newDur / old;
    float base = sKeyframes[lo].time;
    for (int i = lo + 1; i <= hi; i++) {
        sKeyframes[i].time = base + (sKeyframes[i].time - base) * s;
    }
    // Absolute speeds follow the stretch (see SetTotalDuration) - but only for keyframes STRICTLY inside the
    // range. A speed belongs to the keyframe, shared by the segment either side of it, and the range's two
    // end keyframes each keep one segment that wasn't scaled at all. Rescaling those would be right for the
    // inner side and wrong for the outer one, so the outer segment would replay at a pace nobody asked for
    // (and the value would then hit the ceiling and visibly snap). Left alone, they stay true for the side
    // that didn't move.
    for (int i = lo + 1; i < hi; i++) {
        if (sKeyframes[i].speedRate >= 0.0f) {
            sKeyframes[i].speedRate /= s;
        }
        sKeyframes[i].speedAccelIn /= s * s;
        sKeyframes[i].speedAccelOut /= s * s;
    }
    SortByTime();
}

// Rescale all keyframe times so the path lasts `newTotal` seconds (keeps the first keyframe's time and the
// relative spacing). The inverse of reading the duration off the timeline.
static void SetTotalDuration(float newTotal) {
    int n = (int)sKeyframes.size();
    if (n < 2 || newTotal <= 1e-4f) {
        return;
    }
    float base = sKeyframes[0].time;
    float old = sKeyframes[n - 1].time - base;
    if (old <= 1e-4f) {
        return;
    }
    PushUndo();
    float s = newTotal / old;
    for (auto& k : sKeyframes) {
        k.time = base + (k.time - base) * s;
    }
    // The loop-return leg is part of the path's duration, so it scales with it. Leaving it fixed meant
    // stretching a looping path changed its pacing everywhere EXCEPT the seam, which then only caught up when
    // you happened to hit Normalize - the retime appeared to happen in two goes.
    if (LoopCyclic()) {
        sLoopReturnTime *= s;
    }
    // Hand-set speeds and accelerations are absolute (u/s, u/s^2), so stretching the path in time has to
    // scale them or they'd contradict the new duration, get clamped, and come out as a jerk. Playing the same
    // motion over twice the time is exactly half the speed and a quarter of the acceleration.
    for (auto& k : sKeyframes) {
        if (k.speedRate >= 0.0f) {
            k.speedRate /= s;
        }
        k.speedAccelIn /= s * s;
        k.speedAccelOut /= s * s;
    }
}

// Build a fresh circular/arc path of `count` keyframes orbiting `center` at `radius`/`height`, each aiming at
// the center. Replaces the current path. A full 360 orbit enables looping. If followMode != 0 the path is set
// to track that target (Link=1 / actor=2) so the orbit follows it as it moves.
static void GenerateOrbit(const float* center, float radius, float height, int count, float arcDeg, float startDeg,
                          float duration, int followMode, int followActorId, void* followActorPtr) {
    if (count < 2) {
        count = 2;
    }
    PushUndo();
    sKeyframes.clear();
    sIds.clear();
    SelectOnly(-1);
    bool full = arcDeg >= 359.9f;
    for (int i = 0; i < count; i++) {
        float frac = full ? (float)i / (float)count : (float)i / (float)(count - 1);
        float ang = (startDeg + arcDeg * frac) * (3.14159265f / 180.0f);
        CineKeyframe k{};
        k.eye[0] = center[0] + std::cos(ang) * radius;
        k.eye[1] = center[1] + height;
        k.eye[2] = center[2] + std::sin(ang) * radius;
        k.fov = 60.0f;
        k.aimMode = CINE_AIM_POINT; // aim at the (baked) center; follow translates eye + at together
        k.at[0] = center[0];
        k.at[1] = center[1];
        k.at[2] = center[2];
        k.time = duration * frac;
        sKeyframes.push_back(k);
        sIds.push_back(sNextId++);
    }
    SortByTime();
    SelectOnly(sIds.empty() ? -1 : sIds[0]);
    // Set up (or clear) path follow so the whole orbit tracks the moving target.
    sFollowMode = followMode;
    sFollowActorId = followActorId;
    sFollowActorPtr = followActorPtr;
    sFollowOrigin[0] = center[0];
    sFollowOrigin[1] = center[1];
    sFollowOrigin[2] = center[2];
    if (full) {
        sLoop = true;
        sLoopMode = 0;
        sLoopReturnTime = duration / (float)count; // even spacing across the wrap-around segment
    }
    // A generated orbit should BE a smooth ring from the start - run the continuity solve immediately (the
    // full-circle case gets the periodic solve, so the wrap-around seam is as smooth as any other knot).
    if (count >= 3) {
        SmoothPath();
    }
}

// World size that projects to roughly targetPx pixels near a point (keeps the gizmo a constant screen size).
static float GizmoScale(const float* eye, float targetPx) {
    ImVec2 s0;
    if (!WorldToScreen(eye, s0)) {
        return 0.0f;
    }
    const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    float total = 0.0f;
    int n = 0;
    for (int a = 0; a < 3; a++) {
        float probe[3];
        v3mad(eye, axes[a], 10.0f, probe);
        ImVec2 s1;
        if (WorldToScreen(probe, s1)) {
            float dx = s1.x - s0.x;
            float dy = s1.y - s0.y;
            total += std::sqrt(dx * dx + dy * dy) / 10.0f;
            n++;
        }
    }
    if (n == 0 || total <= 1e-4f) {
        return 0.0f;
    }
    return targetPx / (total / n);
}

// Distance from point p to segment ab (all ImVec2).
static float DistToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = p.x - a.x, wy = p.y - a.y;
    float len2 = vx * vx + vy * vy;
    float t = (len2 > 1e-6f) ? ((wx * vx + wy * vy) / len2) : 0.0f;
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    float cx = a.x + vx * t, cy = a.y + vy * t;
    float dx = p.x - cx, dy = p.y - cy;
    return std::sqrt(dx * dx + dy * dy);
}

static const float kPi = 3.14159265f;

// Read a keyframe's orientation as yaw/pitch (degrees) plus the eye->at distance.
static void GetYawPitch(const CineKeyframe& k, float& yawDeg, float& pitchDeg, float& dist) {
    float dx = k.at[0] - k.eye[0];
    float dy = k.at[1] - k.eye[1];
    float dz = k.at[2] - k.eye[2];
    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1.0f) {
        dist = 1.0f;
    }
    float horiz = std::sqrt(dx * dx + dz * dz);
    yawDeg = std::atan2(dx, dz) * 180.0f / kPi;
    pitchDeg = std::atan2(dy, horiz) * 180.0f / kPi;
}

// Write a keyframe's orientation from yaw/pitch (degrees), preserving the eye->at distance. Pitch is
// clamped shy of vertical so the look direction can never cross straight up/down (avoids gimbal flips).
static void SetYawPitch(CineKeyframe& k, float yawDeg, float pitchDeg, float dist) {
    // Only used by the numeric fields, where yaw/pitch is an Euler decomposition; cap just shy of vertical
    // to avoid the yaw singularity. The gizmo rotates the look vector directly and has full range.
    if (pitchDeg > 89.9f) {
        pitchDeg = 89.9f;
    }
    if (pitchDeg < -89.9f) {
        pitchDeg = -89.9f;
    }
    float y = yawDeg * kPi / 180.0f;
    float p = pitchDeg * kPi / 180.0f;
    float ch = std::cos(p) * dist;
    k.at[0] = k.eye[0] + ch * std::sin(y);
    k.at[1] = k.eye[1] + dist * std::sin(p);
    k.at[2] = k.eye[2] + ch * std::cos(y);
}

// Build the local rotation axes (yaw/pitch/roll) for a keyframe.
static void RotationAxes(const CineKeyframe& k, float yawAxis[3], float pitchAxis[3], float rollAxis[3]) {
    float fwd[3];
    v3sub(k.at, k.eye, fwd);
    v3norm(fwd);
    float worldUp[3] = { 0.0f, 1.0f, 0.0f };
    float right[3];
    v3cross(worldUp, fwd, right);
    if (v3len(right) < 1e-3f) {
        float altUp[3] = { 0.0f, 0.0f, 1.0f };
        v3cross(altUp, fwd, right);
    }
    v3norm(right);
    yawAxis[0] = 0.0f;
    yawAxis[1] = 1.0f;
    yawAxis[2] = 0.0f; // yaw around world up
    pitchAxis[0] = right[0];
    pitchAxis[1] = right[1];
    pitchAxis[2] = right[2]; // pitch around camera right
    rollAxis[0] = fwd[0];
    rollAxis[1] = fwd[1];
    rollAxis[2] = fwd[2]; // roll around view direction
}

static const ImU32 kMoveCol[3] = { IM_COL32(235, 80, 80, 255), IM_COL32(90, 220, 90, 255),
                                   IM_COL32(90, 150, 255, 255) }; // X, Y, Z
static const ImU32 kRotCol[3] = { IM_COL32(90, 220, 90, 255), IM_COL32(235, 80, 80, 255),
                                  IM_COL32(90, 150, 255, 255) };                                // yaw, pitch, roll
static const ImU32 kBendCol[2] = { IM_COL32(230, 130, 255, 255), IM_COL32(255, 170, 60, 255) }; // bend rings

static void RingBasis(const float* k, float u[3], float v[3]); // defined below

// The natural Catmull-Rom tangent direction of the spatial path at keyframe idx.
static void AutoTangentDir(int idx, float out[3]) {
    int n = (int)sKeyframes.size();
    int prev = idx - 1, next = idx + 1;
    if (sLoop) {
        prev = (idx - 1 + n) % n;
        next = (idx + 1) % n;
    } else {
        if (prev < 0) {
            prev = idx;
        }
        if (next >= n) {
            next = idx;
        }
    }
    v3sub(sKeyframes[next].eye, sKeyframes[prev].eye, out);
    if (v3len(out) < 1e-4f) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 1.0f;
    }
    v3norm(out);
}

// The OUT-side tangent direction in use at keyframe idx (custom if set, otherwise the automatic one).
static void BendTangentDir(int idx, float out[3]) {
    if (sKeyframes[idx].hasTangent) {
        out[0] = sKeyframes[idx].tangent[0];
        out[1] = sKeyframes[idx].tangent[1];
        out[2] = sKeyframes[idx].tangent[2];
        v3norm(out);
    } else {
        AutoTangentDir(idx, out);
    }
}

// The IN-side (arriving) tangent direction in use at keyframe idx: the broken in-direction if set, otherwise
// it mirrors the out side.
static void BendTangentDirIn(int idx, float out[3]) {
    if (sKeyframes[idx].hasTangentIn) {
        out[0] = sKeyframes[idx].tangentIn[0];
        out[1] = sKeyframes[idx].tangentIn[1];
        out[2] = sKeyframes[idx].tangentIn[2];
        v3norm(out);
    } else {
        BendTangentDir(idx, out);
    }
}

// The two ring axes used by Bend mode (rotate the tangent direction).
static void BendAxes(const float* tdir, float yawAxis[3], float pitchAxis[3]) {
    yawAxis[0] = 0.0f;
    yawAxis[1] = 1.0f;
    yawAxis[2] = 0.0f;
    v3cross(yawAxis, tdir, pitchAxis);
    if (v3len(pitchAxis) < 1e-3f) {
        pitchAxis[0] = 1.0f;
        pitchAxis[1] = 0.0f;
        pitchAxis[2] = 0.0f;
    }
    v3norm(pitchAxis);
}

// Draw a world-space ring (circle in the plane perpendicular to axis) as a screen polyline.
static void DrawRing(ImDrawList* dl, const float* center, const float* axis, float R, ImU32 col, float thick) {
    float u[3], v[3];
    RingBasis(axis, u, v);
    const int SEG = 48;
    ImVec2 prev;
    bool pv = false;
    for (int s = 0; s <= SEG; s++) {
        float t = (float)s / SEG * 6.2831853f;
        float cs = std::cos(t), sn = std::sin(t);
        float p[3] = { center[0] + (u[0] * cs + v[0] * sn) * R, center[1] + (u[1] * cs + v[1] * sn) * R,
                       center[2] + (u[2] * cs + v[2] * sn) * R };
        ImVec2 sp;
        if (WorldToScreen(p, sp)) {
            if (pv) {
                dl->AddLine(prev, sp, col, thick);
            }
            prev = sp;
            pv = true;
        } else {
            pv = false;
        }
    }
}

// Minimum screen distance from m to a world-space ring (for hit-testing).
static float RingHitDist(const float* center, const float* axis, float R, ImVec2 m) {
    float u[3], v[3];
    RingBasis(axis, u, v);
    const int SEG = 48;
    float best = 1e9f;
    for (int s = 0; s < SEG; s++) {
        float t = (float)s / SEG * 6.2831853f;
        float cs = std::cos(t), sn = std::sin(t);
        float p[3] = { center[0] + (u[0] * cs + v[0] * sn) * R, center[1] + (u[1] * cs + v[1] * sn) * R,
                       center[2] + (u[2] * cs + v[2] * sn) * R };
        ImVec2 sp;
        if (!WorldToScreen(p, sp)) {
            continue;
        }
        float dx = sp.x - m.x, dy = sp.y - m.y;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < best) {
            best = d;
        }
    }
    return best;
}

// Compute the perpendicular ring basis (u, v) for a rotation axis.
static void RingBasis(const float* k, float u[3], float v[3]) {
    float ref[3] = { (std::fabs(k[0]) > 0.9f) ? 0.0f : 1.0f, (std::fabs(k[0]) > 0.9f) ? 1.0f : 0.0f, 0.0f };
    v3cross(ref, k, u);
    v3norm(u);
    v3cross(k, u, v);
}

// Draw the move axes or rotate rings for one keyframe.
static void DrawGizmo(ImDrawList* dl, int idx) {
    const CineKeyframe& k = sKeyframes[idx];
    ImVec2 origin;
    if (!WorldToScreen(k.eye, origin)) {
        return;
    }

    if (sGizmoMode == GIZMO_MOVE) {
        float L = GizmoScale(k.eye, 70.0f);
        if (L <= 0.0f) {
            return;
        }
        const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        for (int a = 0; a < 3; a++) {
            float end[3];
            v3mad(k.eye, axes[a], L, end);
            ImVec2 ep;
            if (!WorldToScreen(end, ep)) {
                continue;
            }
            bool hot = (sDragKfId == sIds[idx] && sDragKind == 0 && sDragAxis == a);
            ImU32 c = hot ? IM_COL32(255, 255, 120, 255) : kMoveCol[a];
            dl->AddLine(origin, ep, c, hot ? 3.5f : 2.5f);
            dl->AddCircleFilled(ep, 4.0f, c);
        }
    } else if (sGizmoMode == GIZMO_ROTATE) {
        float R = GizmoScale(k.eye, 60.0f);
        if (R <= 0.0f) {
            return;
        }
        float axesK[3][3];
        RotationAxes(k, axesK[0], axesK[1], axesK[2]);
        for (int a = 0; a < 3; a++) {
            if (a < 2 && k.aimMode != CINE_AIM_FREE) {
                continue; // only roll is editable when aiming at a point/Link
            }
            bool hot = (sDragKfId == sIds[idx] && sDragKind == 1 && sDragAxis == a);
            DrawRing(dl, k.eye, axesK[a], R, hot ? IM_COL32(255, 255, 120, 255) : kRotCol[a], hot ? 3.0f : 2.0f);
        }
    } else { // GIZMO_BEND: rotate the spatial tangent (bends the curve), independent of camera aim
        float R = GizmoScale(k.eye, 60.0f);
        float L = GizmoScale(k.eye, 70.0f);
        if (R <= 0.0f || L <= 0.0f) {
            return;
        }
        float tdir[3], idir[3];
        BendTangentDir(idx, tdir);   // out side: drawn forward (toward the next keyframe)
        BendTangentDirIn(idx, idir); // in side: drawn backward; diverges from the out line when broken
        // Handle length shows the side's WEIGHT (how far the curve bulges on that side); the dots are
        // directly draggable to steer each side.
        float fwOut = (k.tanWOut > 0.0f) ? std::min(std::max(k.tanWOut, 0.25f), 3.0f) : 1.0f;
        float fwIn = (k.tanWIn > 0.0f) ? std::min(std::max(k.tanWIn, 0.25f), 3.0f) : 1.0f;
        ImVec2 origin2, sp, sn;
        float hp[3], hn[3];
        v3mad(k.eye, tdir, L * fwOut, hp);
        v3mad(k.eye, idir, -L * fwIn, hn);
        bool editOut = (sBendSide != 2), editIn = (sBendSide != 1);
        bool hotO = (sDragKfId == sIds[idx] && sDragKind == 3 && sDragAxis == 0);
        bool hotI = (sDragKfId == sIds[idx] && sDragKind == 3 && sDragAxis == 1);
        if (WorldToScreen(k.eye, origin2)) {
            if (WorldToScreen(hp, sp)) { // out handle: magenta, dimmed when not being edited
                ImU32 c = hotO ? IM_COL32(255, 255, 120, 255)
                               : (editOut ? IM_COL32(230, 130, 255, 220) : IM_COL32(230, 130, 255, 90));
                dl->AddLine(origin2, sp, c, 2.0f);
                dl->AddCircleFilled(sp, hotO ? 6.0f : 4.5f, c);
            }
            if (WorldToScreen(hn, sn)) { // in handle: teal so a broken corner reads at a glance
                ImU32 c = hotI ? IM_COL32(255, 255, 120, 255)
                               : (editIn ? IM_COL32(120, 230, 210, 220) : IM_COL32(120, 230, 210, 90));
                dl->AddLine(origin2, sn, c, 2.0f);
                dl->AddCircleFilled(sn, hotI ? 6.0f : 4.5f, c);
            }
        }
        // Rings orient on the side being edited (the out side when editing both).
        float yawAxis[3], pitchAxis[3];
        BendAxes(sBendSide == 2 ? idir : tdir, yawAxis, pitchAxis);
        bool hot0 = (sDragKfId == sIds[idx] && sDragKind == 2 && sDragAxis == 0);
        bool hot1 = (sDragKfId == sIds[idx] && sDragKind == 2 && sDragAxis == 1);
        DrawRing(dl, k.eye, yawAxis, R, hot0 ? IM_COL32(255, 255, 120, 255) : kBendCol[0], hot0 ? 3.0f : 2.0f);
        DrawRing(dl, k.eye, pitchAxis, R, hot1 ? IM_COL32(255, 255, 120, 255) : kBendCol[1], hot1 ? 3.0f : 2.0f);
    }
}

// Hit-test the gizmo for keyframe idx against the mouse; begins a drag if a handle is grabbed.
static bool GizmoTryStart(int idx, ImVec2 m) {
    const CineKeyframe& k = sKeyframes[idx];
    ImVec2 origin;
    if (!WorldToScreen(k.eye, origin)) {
        return false;
    }
    if (sGizmoMode == GIZMO_MOVE) {
        float L = GizmoScale(k.eye, 70.0f);
        if (L <= 0.0f) {
            return false;
        }
        const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        int best = -1;
        float bestd = 9.0f;
        for (int a = 0; a < 3; a++) {
            float end[3];
            v3mad(k.eye, axes[a], L, end);
            ImVec2 ep;
            if (!WorldToScreen(end, ep)) {
                continue;
            }
            float d = DistToSegment(m, origin, ep);
            if (d < bestd) {
                bestd = d;
                best = a;
            }
        }
        if (best >= 0) {
            PushUndo();
            sDragKfId = sIds[idx];
            sDragKind = 0;
            sDragAxis = best;
            return true;
        }
    } else if (sGizmoMode == GIZMO_ROTATE) {
        float R = GizmoScale(k.eye, 60.0f);
        if (R <= 0.0f) {
            return false;
        }
        float axesK[3][3];
        RotationAxes(k, axesK[0], axesK[1], axesK[2]);
        int best = -1;
        float bestd = 8.0f;
        for (int a = 0; a < 3; a++) {
            if (a < 2 && k.aimMode != CINE_AIM_FREE) {
                continue; // yaw/pitch aim only meaningful in free aim; roll always allowed
            }
            float d = RingHitDist(k.eye, axesK[a], R, m);
            if (d < bestd) {
                bestd = d;
                best = a;
            }
        }
        if (best >= 0) {
            PushUndo();
            sDragKfId = sIds[idx];
            sDragKind = 1;
            sDragAxis = best;
            sRotPrevAngle = std::atan2(m.y - origin.y, m.x - origin.x);
            // Capture a stable pitch axis now; recomputing it each frame degenerates near vertical.
            sDragPitchAxis[0] = axesK[1][0];
            sDragPitchAxis[1] = axesK[1][1];
            sDragPitchAxis[2] = axesK[1][2];
            return true;
        }
    } else { // GIZMO_BEND
        float R = GizmoScale(k.eye, 60.0f);
        float L = GizmoScale(k.eye, 70.0f);
        if (R <= 0.0f || L <= 0.0f) {
            return false;
        }
        float tdir[3], idir[3];
        BendTangentDir(idx, tdir);
        BendTangentDirIn(idx, idir);
        // The handle DOTS grab with priority over the rings: dragging one steers that side directly.
        {
            float fwOut = (k.tanWOut > 0.0f) ? std::min(std::max(k.tanWOut, 0.25f), 3.0f) : 1.0f;
            float fwIn = (k.tanWIn > 0.0f) ? std::min(std::max(k.tanWIn, 0.25f), 3.0f) : 1.0f;
            float hp[3], hn[3];
            v3mad(k.eye, tdir, L * fwOut, hp);
            v3mad(k.eye, idir, -L * fwIn, hn);
            ImVec2 sp, sn;
            int dot = -1;
            if (WorldToScreen(hp, sp) && std::sqrt((sp.x - m.x) * (sp.x - m.x) + (sp.y - m.y) * (sp.y - m.y)) < 9.0f) {
                dot = 0;
            } else if (WorldToScreen(hn, sn) &&
                       std::sqrt((sn.x - m.x) * (sn.x - m.x) + (sn.y - m.y) * (sn.y - m.y)) < 9.0f) {
                dot = 1;
            }
            if (dot >= 0) {
                PushUndo();
                CineKeyframe& kf = sKeyframes[idx];
                // Grabbing one side breaks the handle: both sides get pinned at their current directions so
                // only the grabbed one moves.
                if (!kf.hasTangent) {
                    kf.tangent[0] = tdir[0];
                    kf.tangent[1] = tdir[1];
                    kf.tangent[2] = tdir[2];
                    kf.hasTangent = 1;
                }
                if (!kf.hasTangentIn) {
                    kf.tangentIn[0] = idir[0];
                    kf.tangentIn[1] = idir[1];
                    kf.tangentIn[2] = idir[2];
                    kf.hasTangentIn = 1;
                }
                sDragKfId = sIds[idx];
                sDragKind = 3;
                sDragAxis = dot; // 0 = out dot, 1 = in dot
                float pa[3], ya[3];
                BendAxes(dot == 0 ? tdir : idir, ya, pa);
                sDragPitchAxis[0] = pa[0]; // stable pitch axis, captured at grab (recomputing degenerates
                sDragPitchAxis[1] = pa[1]; // near vertical)
                sDragPitchAxis[2] = pa[2];
                return true;
            }
        }
        float yawAxis[3], pitchAxis[3];
        BendAxes(sBendSide == 2 ? idir : tdir, yawAxis, pitchAxis);
        int best = -1;
        float bestd = 8.0f;
        float d0 = RingHitDist(k.eye, yawAxis, R, m);
        float d1 = RingHitDist(k.eye, pitchAxis, R, m);
        if (d0 < bestd) {
            bestd = d0;
            best = 0;
        }
        if (d1 < bestd) {
            bestd = d1;
            best = 1;
        }
        if (best >= 0) {
            PushUndo();
            CineKeyframe& kf = sKeyframes[idx];
            // Lock in the current effective directions so rotation starts from what is on screen. Editing a
            // single side also freezes the OTHER side at its current direction - otherwise a mirrored/auto
            // opposite side would silently follow the drag.
            if (sBendSide != 2 && !kf.hasTangent) {
                kf.tangent[0] = tdir[0];
                kf.tangent[1] = tdir[1];
                kf.tangent[2] = tdir[2];
                kf.hasTangent = 1;
            }
            if (sBendSide == 1 && !kf.hasTangentIn) { // freeze the in side while bending out only
                kf.tangentIn[0] = idir[0];
                kf.tangentIn[1] = idir[1];
                kf.tangentIn[2] = idir[2];
                kf.hasTangentIn = 1;
            }
            if (sBendSide == 2) {     // bending the in side: it becomes broken, out side keeps its own state
                if (!kf.hasTangent) { // freeze the out side (currently auto) so it doesn't drift
                    kf.tangent[0] = tdir[0];
                    kf.tangent[1] = tdir[1];
                    kf.tangent[2] = tdir[2];
                    kf.hasTangent = 1;
                }
                if (!kf.hasTangentIn) {
                    kf.tangentIn[0] = idir[0];
                    kf.tangentIn[1] = idir[1];
                    kf.tangentIn[2] = idir[2];
                    kf.hasTangentIn = 1;
                }
            }
            sDragKfId = sIds[idx];
            sDragKind = 2;
            sDragAxis = best;
            sRotPrevAngle = std::atan2(m.y - origin.y, m.x - origin.x);
            sDragPitchAxis[0] = pitchAxis[0];
            sDragPitchAxis[1] = pitchAxis[1];
            sDragPitchAxis[2] = pitchAxis[2];
            return true;
        }
    }
    return false;
}

// Apply the active gizmo drag from this frame's mouse movement.
static void GizmoContinue() {
    int idx = -1;
    for (size_t i = 0; i < sIds.size(); i++) {
        if (sIds[i] == sDragKfId) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0 || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        sDragKfId = -1;
        sDragAxis = -1;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    CineKeyframe& k = sKeyframes[idx];

    if (sDragKind == 0) {
        ImVec2 origin, ep;
        if (!WorldToScreen(k.eye, origin)) {
            return;
        }
        float L = GizmoScale(k.eye, 70.0f);
        if (L <= 0.0f) {
            return;
        }
        float axis[3] = { 0, 0, 0 };
        axis[sDragAxis] = 1.0f;
        float end[3];
        v3mad(k.eye, axis, L, end);
        if (!WorldToScreen(end, ep)) {
            return;
        }
        float sx = ep.x - origin.x, sy = ep.y - origin.y;
        float slen = std::sqrt(sx * sx + sy * sy);
        if (slen < 1e-3f) {
            return;
        }
        float along = (io.MouseDelta.x * sx + io.MouseDelta.y * sy) / slen; // pixels along the axis
        float worldDelta = along * (L / slen);
        k.eye[sDragAxis] += worldDelta;
        if (k.aimMode == CINE_AIM_FREE) {
            k.at[sDragAxis] += worldDelta; // free aim: carry the look-at rigidly with the eye
        }
    } else if (sDragKind == 3) {
        // Bend handle dot: steer that side's direction so the dot follows the mouse. The screen response of a
        // yaw/pitch nudge depends on the view, so measure it numerically (probe both axes) and solve the 2x2
        // system for the rotation that closes the gap.
        float* dir = (sDragAxis == 0) ? k.tangent : k.tangentIn;
        float sign = (sDragAxis == 0) ? 1.0f : -1.0f;
        float L = GizmoScale(k.eye, 70.0f);
        if (L <= 0.0f) {
            return;
        }
        float w = (sDragAxis == 0) ? k.tanWOut : k.tanWIn;
        float fw = (w > 0.0f) ? std::min(std::max(w, 0.25f), 3.0f) : 1.0f;
        float reach = L * fw * sign;
        float up[3] = { 0.0f, 1.0f, 0.0f };
        const float eps = 0.02f;
        float hp[3], d1[3], d2[3], p1w[3], p2w[3];
        ImVec2 base, s1, s2;
        v3mad(k.eye, dir, reach, hp);
        v3rot(dir, up, eps, d1);
        v3rot(dir, sDragPitchAxis, eps, d2);
        v3mad(k.eye, d1, reach, p1w);
        v3mad(k.eye, d2, reach, p2w);
        if (!WorldToScreen(hp, base) || !WorldToScreen(p1w, s1) || !WorldToScreen(p2w, s2)) {
            return;
        }
        float j1x = (s1.x - base.x) / eps, j1y = (s1.y - base.y) / eps;
        float j2x = (s2.x - base.x) / eps, j2y = (s2.y - base.y) / eps;
        float ex = io.MousePos.x - base.x, ey = io.MousePos.y - base.y;
        float det = j1x * j2y - j1y * j2x;
        if (std::fabs(det) > 1e-3f) {
            float ya = std::min(std::max((ex * j2y - ey * j2x) / det, -0.2f), 0.2f); // clamp: stable convergence
            float pa = std::min(std::max((j1x * ey - j1y * ex) / det, -0.2f), 0.2f);
            float r1[3], r2[3];
            v3rot(dir, up, ya, r1);
            v3rot(r1, sDragPitchAxis, pa, r2);
            v3norm(r2);
            dir[0] = r2[0];
            dir[1] = r2[1];
            dir[2] = r2[2];
        }
    } else {
        ImVec2 origin;
        if (!WorldToScreen(k.eye, origin)) {
            return;
        }
        float ang = std::atan2(io.MousePos.y - origin.y, io.MousePos.x - origin.x);
        float d = ang - sRotPrevAngle;
        while (d > 3.14159265f) {
            d -= 6.2831853f;
        }
        while (d < -3.14159265f) {
            d += 6.2831853f;
        }
        sRotPrevAngle = ang;

        // Rotation axis: yaw around world up, pitch around the stable axis captured at drag start
        // (recomputing it each frame degenerates near vertical).
        float axis[3];
        if (sDragAxis == 0) {
            axis[0] = 0.0f;
            axis[1] = 1.0f;
            axis[2] = 0.0f;
        } else {
            axis[0] = sDragPitchAxis[0];
            axis[1] = sDragPitchAxis[1];
            axis[2] = sDragPitchAxis[2];
        }

        if (sDragKind == 2) {
            // Bend: rotate the spatial tangent(s) (reshapes the curve; no effect on aim). "Both" rotates the
            // pair rigidly (a broken corner keeps its angle); a single side leaves the other frozen.
            float rot[3];
            if (sBendSide != 2) {
                v3rot(k.tangent, axis, d, rot);
                v3norm(rot);
                k.tangent[0] = rot[0];
                k.tangent[1] = rot[1];
                k.tangent[2] = rot[2];
            }
            if (sBendSide != 1 && k.hasTangentIn) {
                v3rot(k.tangentIn, axis, d, rot);
                v3norm(rot);
                k.tangentIn[0] = rot[0];
                k.tangentIn[1] = rot[1];
                k.tangentIn[2] = rot[2];
            }
        } else if (sDragAxis == 2) {
            k.roll += d * (180.0f / kPi); // aim roll in degrees
        } else {
            // Aim: rotate the look vector directly (full pitch range, no clamp, no gimbal).
            float fwd[3];
            v3sub(k.at, k.eye, fwd);
            float rot[3];
            v3rot(fwd, axis, d, rot);
            k.at[0] = k.eye[0] + rot[0];
            k.at[1] = k.eye[1] + rot[1];
            k.at[2] = k.eye[2] + rot[2];
        }
    }
}

// Draw a 3-axis move gizmo at the look-at-point target.
static void DrawTargetGizmo(ImDrawList* dl, int idx) {
    if (sKeyframes[idx].aimMode != CINE_AIM_POINT) {
        return;
    }
    float* t = sKeyframes[idx].at;
    ImVec2 origin;
    if (!WorldToScreen(t, origin)) {
        return;
    }
    float L = GizmoScale(t, 60.0f);
    if (L <= 0.0f) {
        return;
    }
    const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int a = 0; a < 3; a++) {
        float end[3];
        v3mad(t, axes[a], L, end);
        ImVec2 ep;
        if (!WorldToScreen(end, ep)) {
            continue;
        }
        bool hot = (sTargetDragId == sIds[idx] && sTargetDragAxis == a);
        ImU32 c = hot ? IM_COL32(255, 255, 120, 255) : kMoveCol[a];
        dl->AddLine(origin, ep, c, hot ? 3.0f : 2.0f);
        dl->AddCircleFilled(ep, 3.5f, c);
    }
}

// Continue dragging the look-at-point target along its grabbed world axis.
static void TargetContinue() {
    int idx = -1;
    for (size_t i = 0; i < sIds.size(); i++) {
        if (sIds[i] == sTargetDragId) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0 || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        sTargetDragId = -1;
        sTargetDragAxis = -1;
        return;
    }
    float* t = sKeyframes[idx].at;
    ImVec2 origin, ep;
    if (!WorldToScreen(t, origin)) {
        return;
    }
    float L = GizmoScale(t, 60.0f);
    if (L <= 0.0f) {
        return;
    }
    float axis[3] = { 0, 0, 0 };
    axis[sTargetDragAxis] = 1.0f;
    float end[3];
    v3mad(t, axis, L, end);
    if (!WorldToScreen(end, ep)) {
        return;
    }
    float sx = ep.x - origin.x, sy = ep.y - origin.y;
    float slen = std::sqrt(sx * sx + sy * sy);
    if (slen < 1e-3f) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    float along = (io.MouseDelta.x * sx + io.MouseDelta.y * sy) / slen;
    t[sTargetDragAxis] += along * (L / slen);
}

// Grab the look-at-point target's gizmo axis under the mouse.
static bool TargetTryStart(int idx, ImVec2 m) {
    if (sKeyframes[idx].aimMode != CINE_AIM_POINT) {
        return false;
    }
    float* t = sKeyframes[idx].at;
    ImVec2 origin;
    if (!WorldToScreen(t, origin)) {
        return false;
    }
    float L = GizmoScale(t, 60.0f);
    if (L <= 0.0f) {
        return false;
    }
    const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    int best = -1;
    float bestd = 9.0f;
    for (int a = 0; a < 3; a++) {
        float end[3];
        v3mad(t, axes[a], L, end);
        ImVec2 ep;
        if (!WorldToScreen(end, ep)) {
            continue;
        }
        float d = DistToSegment(m, origin, ep);
        if (d < bestd) {
            bestd = d;
            best = a;
        }
    }
    if (best >= 0) {
        PushUndo();
        sTargetDragId = sIds[idx];
        sTargetDragAxis = best;
        return true;
    }
    return false;
}

// --- Movable aim target (the path-level "All look at point" override) ---------------------------
// A single world target you can place at the camera and drag with a 3-axis gizmo; when the path's aim
// override is set to "All look at point", the whole path aims at it.
static int sOvTargetDragAxis = -1; // which world axis (0/1/2) of the override target is being dragged

// The shared aim target is "in use" when the path-level override points at it, or any keyframe aims at it.
static bool TargetInUse() {
    if (sAimOverride == 2) {
        return true;
    }
    for (auto& k : sKeyframes) {
        if (k.aimMode == CINE_AIM_TARGET) {
            return true;
        }
    }
    return false;
}

static void DrawOverrideTarget(ImDrawList* dl) {
    if (!TargetInUse()) {
        return;
    }
    float* t = sAimOverridePoint;
    ImVec2 origin;
    if (!WorldToScreen(t, origin)) {
        return;
    }
    // Crosshair + label so it reads as a target even from far away.
    dl->AddLine(ImVec2(origin.x - 10, origin.y), ImVec2(origin.x + 10, origin.y), IM_COL32(255, 120, 60, 230), 1.5f);
    dl->AddLine(ImVec2(origin.x, origin.y - 10), ImVec2(origin.x, origin.y + 10), IM_COL32(255, 120, 60, 230), 1.5f);
    dl->AddCircle(origin, 12.0f, IM_COL32(255, 120, 60, 200), 0, 1.5f);
    dl->AddText(ImVec2(origin.x + 13, origin.y - 9), IM_COL32(255, 180, 120, 255), "target");

    float L = GizmoScale(t, 60.0f);
    if (L <= 0.0f) {
        return;
    }
    const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int a = 0; a < 3; a++) {
        float end[3];
        v3mad(t, axes[a], L, end);
        ImVec2 ep;
        if (!WorldToScreen(end, ep)) {
            continue;
        }
        bool hot = (sOvTargetDragAxis == a);
        ImU32 c = hot ? IM_COL32(255, 255, 120, 255) : kMoveCol[a];
        dl->AddLine(origin, ep, c, hot ? 3.0f : 2.0f);
        dl->AddCircleFilled(ep, 3.5f, c);
    }
}

static void OverrideTargetContinue() {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        sOvTargetDragAxis = -1;
        return;
    }
    float* t = sAimOverridePoint;
    ImVec2 origin, ep;
    if (!WorldToScreen(t, origin)) {
        return;
    }
    float L = GizmoScale(t, 60.0f);
    if (L <= 0.0f) {
        return;
    }
    float axis[3] = { 0, 0, 0 };
    axis[sOvTargetDragAxis] = 1.0f;
    float end[3];
    v3mad(t, axis, L, end);
    if (!WorldToScreen(end, ep)) {
        return;
    }
    float sx = ep.x - origin.x, sy = ep.y - origin.y;
    float slen = std::sqrt(sx * sx + sy * sy);
    if (slen < 1e-3f) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    float along = (io.MouseDelta.x * sx + io.MouseDelta.y * sy) / slen;
    t[sOvTargetDragAxis] += along * (L / slen);
}

static bool OverrideTargetTryStart(ImVec2 m) {
    if (!TargetInUse()) {
        return false;
    }
    float* t = sAimOverridePoint;
    ImVec2 origin;
    if (!WorldToScreen(t, origin)) {
        return false;
    }
    float L = GizmoScale(t, 60.0f);
    if (L <= 0.0f) {
        return false;
    }
    const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    int best = -1;
    float bestd = 9.0f;
    for (int a = 0; a < 3; a++) {
        float end[3];
        v3mad(t, axes[a], L, end);
        ImVec2 ep;
        if (!WorldToScreen(end, ep)) {
            continue;
        }
        float d = DistToSegment(m, origin, ep);
        if (d < bestd) {
            bestd = d;
            best = a;
        }
    }
    if (best >= 0) {
        sOvTargetDragAxis = best;
        return true;
    }
    return false;
}

// Place the override target a few units in front of the live free camera.
static void PlaceOverrideTargetAtCamera() {
    float eye[3], at[3], roll, fov;
    CinematicCam_GetPose(eye, at, &roll, &fov);
    float fwd[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (len > 1e-3f) {
        float s = 100.0f / len;
        sAimOverridePoint[0] = eye[0] + fwd[0] * s;
        sAimOverridePoint[1] = eye[1] + fwd[1] * s;
        sAimOverridePoint[2] = eye[2] + fwd[2] * s;
    } else {
        sAimOverridePoint[0] = eye[0];
        sAimOverridePoint[1] = eye[1];
        sAimOverridePoint[2] = eye[2];
    }
}

// Draw the spline, numbered keyframe markers, facing indicators and the playhead over the game view.
static void DrawWorldOverlay() {
    // Foreground draw list of the game's viewport so the overlay sits on top of the rendered frame.
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());

    DrawOverrideTarget(dl); // the movable aim target draws even before any keyframes exist

    if (sKeyframes.empty()) {
        return;
    }

    // Spline curve. Tessellation scales with the number of segments so bends stay smooth on long paths
    // (this is ONLY the drawn preview line - the camera samples the true curve continuously, see note below).
    if (sKeyframes.size() >= 2) {
        float total = EffectiveTotal();
        int steps = (int)sKeyframes.size() * 32;
        if (steps < 160) {
            steps = 160;
        }
        if (steps > 2400) {
            steps = 2400;
        }
        // The world-space polyline only changes when the path shape does, so it's cached behind PathShapeHash():
        // re-tessellating every frame was hundreds-to-thousands of full spline evaluations per frame (the single
        // biggest per-frame cost of the editor). Screen projection still runs per frame - the camera moves.
        static std::vector<float> sSplineCache; // xyz triplets, steps+1 points
        static uint32_t sSplineHash = 0;
        static int sSplineSteps = -1;
        uint32_t hsh = PathShapeHash();
        if (hsh != sSplineHash || steps != sSplineSteps) {
            sSplineHash = hsh;
            sSplineSteps = steps;
            sSplineCache.resize(((size_t)steps + 1) * 3);
            for (int i = 0; i <= steps; i++) {
                CineKeyframe s = SampleAt(total * (float)i / (float)steps);
                sSplineCache[(size_t)i * 3 + 0] = s.eye[0];
                sSplineCache[(size_t)i * 3 + 1] = s.eye[1];
                sSplineCache[(size_t)i * 3 + 2] = s.eye[2];
            }
        }
        ImVec2 prev;
        bool prevValid = false;
        for (int i = 0; i <= steps; i++) {
            ImVec2 sp;
            if (WorldToScreen(&sSplineCache[(size_t)i * 3], sp)) {
                if (prevValid) {
                    dl->AddLine(prev, sp, IM_COL32(255, 220, 40, 200), 2.0f);
                }
                prev = sp;
                prevValid = true;
            } else {
                prevValid = false;
            }
        }

        // Orientation ticks: the interpolated facing sampled along the path, so rotating a keyframe
        // visibly reshapes the camera orientation all along the curve (not just at the keyframe).
        const int ticks = 16;
        for (int i = 0; i <= ticks; i++) {
            CineKeyframe s = SampleAt(total * (float)i / (float)ticks);
            float tip[3] = { s.eye[0] + (s.at[0] - s.eye[0]) * 0.15f, s.eye[1] + (s.at[1] - s.eye[1]) * 0.15f,
                             s.eye[2] + (s.at[2] - s.eye[2]) * 0.15f };
            ImVec2 a, b;
            if (WorldToScreen(s.eye, a) && WorldToScreen(tip, b)) {
                dl->AddLine(a, b, IM_COL32(120, 200, 255, 130), 1.0f);
            }
        }
    }

    // Keyframe markers (numbered) with a short facing indicator.
    for (int i = 0; i < (int)sKeyframes.size(); i++) {
        ImVec2 sp;
        if (!WorldToScreen(sKeyframes[i].eye, sp)) {
            continue;
        }
        bool selected = sIds[i] == sSelectedId;
        ImU32 col = selected ? IM_COL32(80, 200, 255, 255) : IM_COL32(255, 160, 30, 255);
        float r = selected ? 7.0f : 5.0f;
        dl->AddCircleFilled(sp, r, col);
        dl->AddCircle(sp, r, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        char num[8];
        snprintf(num, sizeof(num), "%d", i + 1);
        dl->AddText(ImVec2(sp.x + 9.0f, sp.y - 9.0f), IM_COL32(255, 255, 255, 255), num);

        // Facing indicator: a short line toward the look-at point (drawn for every keyframe).
        float facing[3];
        FacingHandleWorld(sKeyframes[i], facing);
        ImVec2 fp;
        if (WorldToScreen(facing, fp)) {
            dl->AddLine(sp, fp, IM_COL32(120, 255, 120, 150), 1.5f);
        }
        // The selected keyframe gets the transform gizmo (move axes or rotate rings).
        if (selected) {
            DrawGizmo(dl, i);

            // Look-at target visualization: orange marker + line for a fixed point; a line to Link.
            float tgt[3];
            EffectiveAt(i, tgt);
            if (sKeyframes[i].aimMode != CINE_AIM_FREE) {
                ImVec2 tp;
                if (WorldToScreen(tgt, tp)) {
                    dl->AddLine(sp, tp, IM_COL32(255, 170, 60, 160), 1.5f);
                    if (sKeyframes[i].aimMode == CINE_AIM_POINT) {
                        dl->AddLine(ImVec2(tp.x - 9, tp.y), ImVec2(tp.x + 9, tp.y), IM_COL32(255, 255, 255, 220), 1.0f);
                        dl->AddLine(ImVec2(tp.x, tp.y - 9), ImVec2(tp.x, tp.y + 9), IM_COL32(255, 255, 255, 220), 1.0f);
                        DrawTargetGizmo(dl, i); // 3-axis move gizmo for the target
                    }
                }
            }
        }
    }

    // Playhead.
    if (sPlaying || sPreview) {
        CineKeyframe s = SampleAt(sPlayhead);
        ImVec2 sp;
        if (WorldToScreen(s.eye, sp)) {
            dl->AddCircleFilled(sp, 6.0f, IM_COL32(60, 255, 90, 255));
            dl->AddCircle(sp, 6.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        }
    }
}

// Mouse interaction with the overlay: click a keyframe to select it, grab the selected keyframe's gizmo
// to move (axis handles) or rotate (rings) it. Operates over the game view, menu open or not.
static void HandleOverlayInput() {
    ImGuiIO& io = ImGui::GetIO();

    // Continue an in-progress gizmo / target drag (ignores WantCaptureMouse so it survives passing over a window).
    if (sDragKfId >= 0) {
        GizmoContinue();
        return;
    }
    if (sTargetDragId >= 0) {
        TargetContinue();
        return;
    }
    if (sOvTargetDragAxis >= 0) {
        OverrideTargetContinue();
        return;
    }

    // Start a new interaction on click. Bail only if an ImGui widget is actively being used, so dragging
    // a slider or pressing a button in the editor doesn't also grab a world handle. (We deliberately do
    // NOT gate on WantCaptureMouse: the SoH menu is a fullscreen ImGui layer, so that would block every
    // click while the menu is open.)
    if (ImGui::IsAnyItemActive() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }

    ImVec2 m = io.MousePos;
    int sel = SelectedIndex();

    // Grab the movable aim target first, then the selected keyframe's look-at target, then its gizmo.
    if (OverrideTargetTryStart(m)) {
        return;
    }
    if (sel >= 0 && TargetTryStart(sel, m)) {
        return;
    }
    if (sel >= 0 && GizmoTryStart(sel, m)) {
        return;
    }

    // Otherwise, select whichever keyframe marker was clicked.
    for (int i = 0; i < (int)sKeyframes.size(); i++) {
        ImVec2 sp;
        if (WorldToScreen(sKeyframes[i].eye, sp)) {
            float dxp = sp.x - m.x;
            float dyp = sp.y - m.y;
            if (dxp * dxp + dyp * dyp <= 11.0f * 11.0f) {
                if (ImGui::GetIO().KeyCtrl) {
                    ToggleSelect(sIds[i]);
                } else {
                    SelectOnly(sIds[i]);
                }
                return;
            }
        }
    }
}

// Draw a distance-sorted actor list (nearest the camera first) with id + position so identical-named
// actors are distinguishable. Returns the picked index into buf, or -1.
// Self-contained actor picker for use inside a BeginPopup. The actor list is SNAPSHOTTED when the popup opens
// (and on Refresh) and sorted nearest-first once, so fast-moving actors don't reorder under the cursor (which
// made clicks land on the wrong actor / miss entirely). Fills *out and returns true when one is picked.
static bool DrawActorPicker(CineActorInfo* out) {
    static std::vector<CineActorInfo> snap;
    static char filter[32] = "";

    float eye[3];
    bool haveEye = CinematicCam_GetViewEye(eye) != 0;
    auto dist2 = [&](const CineActorInfo& a) {
        float dx = a.pos[0] - eye[0], dy = a.pos[1] - eye[1], dz = a.pos[2] - eye[2];
        return dx * dx + dy * dy + dz * dz;
    };

    bool refresh = ImGui::IsWindowAppearing(); // first frame the popup is shown
    ImGui::Text("%d actors%s", (int)snap.size(), haveEye ? " (nearest first)" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        refresh = true;
    }
    if (refresh) {
        static CineActorInfo tmp[512];
        int n = CinematicCam_EnumActors(tmp, 512);
        snap.assign(tmp, tmp + n);
        if (haveEye) {
            std::sort(snap.begin(), snap.end(),
                      [&](const CineActorInfo& a, const CineActorInfo& b) { return dist2(a) < dist2(b); });
        }
    }
    ImGui::InputTextWithHint("##actorfilter", "filter by name...", filter, sizeof(filter));
    CineHint("List frozen while open - press Refresh to re-read positions.");

    bool picked = false;
    ImGui::BeginChild("##actorlist", ImVec2(390, 320), true);
    for (size_t i = 0; i < snap.size(); i++) {
        const char* nm = snap[i].name ? snap[i].name : "?";
        if (filter[0]) {
            std::string h = nm, f = filter;
            std::transform(h.begin(), h.end(), h.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            if (h.find(f) == std::string::npos) {
                continue;
            }
        }
        float d = haveEye ? std::sqrt(dist2(snap[i])) : 0.0f;
        char lbl[128];
        snprintf(lbl, sizeof(lbl), "%s  (id %d)  %.0fu  @ %.0f, %.0f, %.0f##ap%zu", nm, snap[i].id, d, snap[i].pos[0],
                 snap[i].pos[1], snap[i].pos[2], i);
        if (ImGui::Selectable(lbl)) {
            *out = snap[i];
            picked = true;
        }
    }
    ImGui::EndChild();
    return picked;
}

// A timeline track: keyframe markers (drag to retime, click to select), a draggable playhead, and the
// loop-return region shaded. Replaces the plain slider.
static void DrawTimeline() {
    int n = (int)sKeyframes.size();

    // Interaction state (persists across frames). Declared up top so the view-duration logic can tell whether
    // an edit is in progress and avoid rescaling the ruler mid-drag.
    static bool sTlScrub = false;
    // 0 none, 1 move selection, 2 ripple (this kf + everything after), 3 scale selection
    static int sTlDragMode = 0;
    static float sTlPressX = 0.0f;       // screen x at press, for the shift click-vs-drag test
    static bool sTlClickClear = false;   // pressed empty space: clear the selection IF this stays a click
    static float sTlGrabTime0 = 0.0f;    // timeline time under the cursor when the drag began
    static float sTlGrabbedT0 = 0.0f;    // original time of the grabbed marker (ripple threshold / scale end)
    static float sTlScaleAnchorT = 0.0f; // scale drag: time of the selection's OPPOSITE end (the fixed point)
    static std::vector<int> sTlDragIds;  // snapshot of all ids at drag start...
    static std::vector<float> sTlDragT0; // ...and their original times
    static bool sTlNoDrag = false;       // true for a ctrl-click (toggle select, don't move)
    static float sTlViewDur = 0.0f;      // displayed ruler length in seconds (decoupled from content)
    static bool sTlAutoFit = true;       // keep the ruler fit to the path when not zoomed manually
    static int sTlTrackDrag = -1;        // automation lane whose key is being dragged (-1 = none)
    static int sTlKeyDrag = -1;          // key index within that track
    static float sTlKeyGrabT0 = 0.0f;    // that key's time when the drag began, and the cursor's - this lane
    static float sTlKeyGrabCur = 0.0f;   // moves by the cursor's DELTA like everything else (grabbing != editing)
    static bool sTlBand = false;         // Ctrl+drag on empty space: select every keyframe in the time range
    static float sTlBandX = 0.0f;        // its anchor, in pixels
    bool interacting = (sTlDragMode != 0) || sTlScrub || (sTlTrackDrag >= 0);

    float content = EffectiveTotal(); // actual path length (for labels + playhead clamp)
    if (content <= 0.0f) {
        content = 1.0f;
    }
    // Ruler length: fit-to-content with headroom when idle; frozen during a drag so markers don't slide
    // around under the cursor. Always grows to keep every marker on-screen.
    if (sTlAutoFit && !interacting) {
        sTlViewDur = content * 1.12f;
    }
    if (sTlViewDur < content) {
        sTlViewDur = content;
    }
    if (sTlViewDur < 0.25f) {
        sTlViewDur = 0.25f;
    }
    float view = sTlViewDur;

    // Dope-sheet lanes: the camera keyframes, plus one lane per ENABLED automation track, straight from the
    // registry - any keyframable parameter shows up here automatically once its track is enabled.
    std::vector<const TrackDef*> autoLanes;
    for (const TrackDef& d : AllTrackDefs()) {
        if (d.track->enabled) {
            autoLanes.push_back(&d);
        }
    }

    const float headerW = 88.0f;
    const float rulerH = 15.0f;
    const float laneH = 22.0f;
    int laneCount = 1 + (int)autoLanes.size();

    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, rulerH + laneH * laneCount + 6.0f);
    if (size.x < 80.0f) {
        size.x = 80.0f;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", size);
    ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 31, 255), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 95, 255), 4.0f);

    float axisX0 = p0.x + headerW;
    float axisW = size.x - headerW;
    if (axisW < 20.0f) {
        axisW = 20.0f;
    }
    float rulerY1 = p0.y + rulerH;
    auto timeToX = [&](float t) { return axisX0 + (t / view) * axisW; };
    auto xToTime = [&](float x) {
        float u = (x - axisX0) / axisW;
        if (u < 0.0f) {
            u = 0.0f;
        }
        if (u > 1.0f) {
            u = 1.0f;
        }
        return u * view;
    };
    auto laneTop = [&](int lane) { return rulerY1 + lane * laneH; };
    auto laneMid = [&](int lane) { return rulerY1 + lane * laneH + laneH * 0.5f; };

    // Header divider + ruler baseline.
    dl->AddLine(ImVec2(axisX0, p0.y), ImVec2(axisX0, p1.y), IM_COL32(70, 70, 75, 255), 1.0f);
    dl->AddLine(ImVec2(axisX0, rulerY1), ImVec2(p1.x, rulerY1), IM_COL32(70, 70, 75, 255), 1.0f);

    // Ruler ticks + faint vertical gridlines through every lane.
    {
        float rough = view / 8.0f;
        if (rough < 1e-4f) {
            rough = 1e-4f;
        }
        float mag = std::pow(10.0f, std::floor(std::log10(rough)));
        float r = rough / mag;
        float stepT = (r >= 5.0f) ? 5.0f * mag : (r >= 2.0f) ? 2.0f * mag : mag;
        for (float t = 0.0f; t <= view + 1e-4f; t += stepT) {
            float x = timeToX(t);
            dl->AddLine(ImVec2(x, rulerY1), ImVec2(x, p1.y), IM_COL32(46, 46, 50, 255), 1.0f);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%g", t);
            dl->AddText(ImVec2(x + 2.0f, p0.y + 1.0f), IM_COL32(140, 140, 145, 255), lbl);
        }
    }

    // Lane labels + separators.
    dl->AddText(ImVec2(p0.x + 6.0f, laneMid(0) - 7.0f), IM_COL32(215, 215, 220, 255), "Camera");
    for (int li = 0; li < (int)autoLanes.size(); li++) {
        dl->AddText(ImVec2(p0.x + 6.0f, laneMid(1 + li) - 7.0f), IM_COL32(215, 215, 220, 255), autoLanes[li]->name);
    }
    for (int lane = 1; lane < laneCount; lane++) {
        float y = laneTop(lane);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(45, 45, 48, 255), 1.0f);
    }

    // Loop-return tail shading (camera lane only).
    if (LoopCyclic() && n >= 2) {
        float lx = timeToX(TotalTime());
        float rx = timeToX(content);
        dl->AddRectFilled(ImVec2(lx, laneTop(0) + 1.0f), ImVec2(rx, laneTop(0) + laneH - 1.0f),
                          IM_COL32(80, 60, 30, 90), 0.0f);
    }

    // Camera keyframe markers.
    float camCy = laneMid(0);
    for (int i = 0; i < n; i++) {
        float kx = timeToX(sKeyframes[i].time);
        bool sel = IsSelected(sIds[i]);
        bool primary = sIds[i] == sSelectedId;
        ImU32 c = sel ? IM_COL32(80, 200, 255, 255) : IM_COL32(255, 160, 30, 255);
        dl->AddCircleFilled(ImVec2(kx, camCy), sel ? 6.0f : 5.0f, c);
        dl->AddCircle(ImVec2(kx, camCy), primary ? 8.0f : 6.0f, IM_COL32(255, 255, 255, primary ? 220 : 120), 0,
                      primary ? 2.0f : 1.0f);
    }

    // Automation-track keys. Discrete tracks draw value-colored squares (held value); continuous tracks draw a
    // connecting ramp with a dot at each key, the value mapped to the lane's height.
    for (int li = 0; li < (int)autoLanes.size(); li++) {
        const TrackDef* L = autoLanes[li];
        float lyTop = laneTop(1 + li) + 4.0f;
        float lyBot = laneTop(1 + li) + laneH - 4.0f;
        float ly = laneMid(1 + li);
        if (L->continuous) {
            // Fixed range when the track has one; otherwise auto-fit to the keys (e.g. unbounded positions).
            float lo = L->vmin, hi = L->vmax;
            if (!(hi > lo)) {
                lo = 1e30f;
                hi = -1e30f;
                for (const CineParamKey& k : L->track->keys) {
                    lo = std::min(lo, k.value);
                    hi = std::max(hi, k.value);
                }
                if (lo > hi) {
                    lo = 0.0f;
                    hi = 1.0f;
                }
                if (hi - lo < 1e-3f) {
                    lo -= 1.0f;
                    hi += 1.0f;
                }
            }
            float span = (hi > lo) ? (hi - lo) : 1.0f;
            auto valToY = [&](float v) {
                float u = (v - lo) / span;
                u = std::min(std::max(u, 0.0f), 1.0f);
                return lyBot - u * (lyBot - lyTop);
            };
            ImU32 line = IM_COL32(120, 200, 255, 220);
            for (size_t ki = 0; ki + 1 < L->track->keys.size(); ki++) {
                dl->AddLine(ImVec2(timeToX(L->track->keys[ki].time), valToY(L->track->keys[ki].value)),
                            ImVec2(timeToX(L->track->keys[ki + 1].time), valToY(L->track->keys[ki + 1].value)), line,
                            1.5f);
            }
            for (size_t ki = 0; ki < L->track->keys.size(); ki++) {
                ImVec2 c(timeToX(L->track->keys[ki].time), valToY(L->track->keys[ki].value));
                dl->AddCircleFilled(c, 3.5f, line);
                dl->AddCircle(c, 3.5f, IM_COL32(255, 255, 255, 150));
            }
        } else {
            for (size_t ki = 0; ki < L->track->keys.size(); ki++) {
                float kx = timeToX(L->track->keys[ki].time);
                int v = (int)(L->track->keys[ki].value + 0.5f);
                ImU32 c = (v >= 0 && v < L->palCount) ? L->palette[v] : IM_COL32(150, 150, 150, 255);
                dl->AddRectFilled(ImVec2(kx - 4.0f, ly - 6.0f), ImVec2(kx + 4.0f, ly + 6.0f), c, 2.0f);
                dl->AddRect(ImVec2(kx - 4.0f, ly - 6.0f), ImVec2(kx + 4.0f, ly + 6.0f), IM_COL32(255, 255, 255, 130),
                            2.0f);
            }
        }
    }

    // Playhead spanning all lanes.
    float px = timeToX(sPlayhead);
    dl->AddLine(ImVec2(px, rulerY1), ImVec2(px, p1.y), IM_COL32(60, 255, 90, 255), 2.0f);
    dl->AddTriangleFilled(ImVec2(px - 5.0f, rulerY1 + 1.0f), ImVec2(px + 5.0f, rulerY1 + 1.0f),
                          ImVec2(px, rulerY1 + 9.0f), IM_COL32(60, 255, 90, 255));

    // Interaction: in the Camera lane, click/drag markers (ctrl-click multi-select, shift-click range select,
    // shift-drag ripple, alt-drag an end of the selection to compress/expand it). In an
    // automation lane, drag a key to retime it (clamped between its neighbors so order stays stable). Empty lane
    // space or the ruler scrubs the playhead. Clicks in the header column are ignored.
    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x;
    float my = io.MousePos.y;
    auto laneAt = [&](float y) -> int {
        for (int lane = 0; lane < laneCount; lane++) {
            if (y >= laneTop(lane) && y < laneTop(lane) + laneH) {
                return lane;
            }
        }
        return -1;
    };
    if (ImGui::IsItemActivated()) {
        sTlNoDrag = false;
        sTlDragMode = 0;
        sTlScrub = false;
        sTlTrackDrag = -1;
        sTlKeyDrag = -1;
        int lane = (mx >= axisX0) ? laneAt(my) : -2; // -2 = header column, ignore
        if (lane == 0) {
            int hit = -1;
            float hitd = 8.0f;
            for (int i = 0; i < n; i++) {
                float d = std::fabs(timeToX(sKeyframes[i].time) - mx);
                if (d < hitd) {
                    hitd = d;
                    hit = i;
                }
            }
            if (hit >= 0) {
                int hitId = sIds[hit];
                auto beginDrag = [&](int mode) {
                    PushUndo();
                    sTlDragMode = mode;
                    sTlGrabTime0 = xToTime(mx);
                    sTlGrabbedT0 = sKeyframes[hit].time;
                    sTlDragIds = sIds;
                    sTlDragT0.resize(n);
                    for (int i = 0; i < n; i++) {
                        sTlDragT0[i] = sKeyframes[i].time;
                    }
                };
                if (io.KeyShift && io.KeyCtrl) {
                    // Ctrl+Shift+click: select every keyframe between the primary and this one. (Range select
                    // lives here, NOT on plain shift, so shift+drag stays the pure ripple it always was.)
                    int anchor = -1;
                    for (int i = 0; i < n; i++) {
                        if (sIds[i] == sSelectedId) {
                            anchor = i;
                            break;
                        }
                    }
                    if (anchor < 0) {
                        SelectOnly(hitId);
                    } else {
                        for (int i = std::min(anchor, hit); i <= std::max(anchor, hit); i++) {
                            if (!IsSelected(sIds[i])) {
                                sSelection.push_back(sIds[i]);
                            }
                        } // the primary stays the anchor, so further clicks extend from the same spot
                    }
                    sTlNoDrag = true;
                } else if (io.KeyShift) {
                    if (!IsSelected(hitId)) {
                        SelectOnly(hitId);
                    } else {
                        sSelectedId = hitId;
                    }
                    beginDrag(2); // ripple: this keyframe and everything after it, immediately
                } else if (io.KeyCtrl) {
                    ToggleSelect(hitId); // add/remove from the multi-selection, no drag
                    sTlNoDrag = true;
                } else if (io.KeyAlt && IsSelected(hitId) && SelectionCount() >= 2) {
                    // Alt-drag on the FIRST or LAST selected keyframe: compress/expand the selection about its
                    // opposite end (the group's internal rhythm is preserved; only its total duration changes).
                    float tMin = 1e30f, tMax = -1e30f;
                    for (int i = 0; i < n; i++) {
                        if (IsSelected(sIds[i])) {
                            tMin = std::min(tMin, sKeyframes[i].time);
                            tMax = std::max(tMax, sKeyframes[i].time);
                        }
                    }
                    float tHit = sKeyframes[hit].time;
                    bool atMin = std::fabs(tHit - tMin) < 1e-4f;
                    bool atMax = std::fabs(tHit - tMax) < 1e-4f;
                    if ((atMin || atMax) && tMax - tMin > 1e-3f) {
                        sSelectedId = hitId;
                        sTlScaleAnchorT = atMax ? tMin : tMax;
                        beginDrag(3);
                    } else {
                        sSelectedId = hitId; // alt on a middle key: plain group move
                        beginDrag(1);
                    }
                } else {
                    if (!IsSelected(hitId)) {
                        SelectOnly(hitId); // clicking an unselected marker selects just it
                    } else {
                        sSelectedId = hitId; // keep the group, make this the primary
                    }
                    beginDrag(1);
                }
            } else if (io.KeyCtrl) {
                // Ctrl+drag on empty space rubber-bands a time range. A plain drag here has to stay the
                // playhead scrub - that is the timeline's primary gesture and far too useful to spend - which
                // is why this graph needs the modifier and the curve editor, where empty space does nothing,
                // does not.
                sTlBand = true;
                sTlBandX = mx;
            } else {
                if (!io.KeyShift) { // a CLICK on empty space clears; a scrub drag must not
                    sTlClickClear = true;
                    sTlPressX = mx;
                }
                sTlScrub = true;
                sPreview = true;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            }
        } else if (lane > 0) {
            CineParamTrack* tr = autoLanes[lane - 1]->track;
            int hit = -1;
            float hitd = 8.0f;
            for (int ki = 0; ki < (int)tr->keys.size(); ki++) {
                float d = std::fabs(timeToX(tr->keys[ki].time) - mx);
                if (d < hitd) {
                    hitd = d;
                    hit = ki;
                }
            }
            if (hit >= 0) {
                PushUndo();
                sTlTrackDrag = lane - 1;
                sTlKeyDrag = hit;
                sTlKeyGrabT0 = tr->keys[hit].time;
                sTlKeyGrabCur = xToTime(mx);
            } else {
                if (!io.KeyCtrl && !io.KeyShift) {
                    sTlClickClear = true;
                    sTlPressX = mx;
                }
                sTlScrub = true;
                sPreview = true;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            }
        } else if (lane == -1) {
            if (!io.KeyCtrl && !io.KeyShift) { // ruler / empty area below the lanes also clears
                sTlClickClear = true;
                sTlPressX = mx;
            }
            sTlScrub = true;
            sPreview = true;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }
    // Same magnet as the curve editor, same step rule, read off THIS graph's visible span.
    float gridT = SnapTimeStep(view);
    bool snapOn = SnapEnabled();
    if (ImGui::IsItemActive() && sTlBand) {
        float a = std::min(sTlBandX, mx), b = std::max(sTlBandX, mx);
        if (b - a >= 2.5f) {
            dl->AddRectFilled(ImVec2(a, laneTop(0)), ImVec2(b, laneTop(1)), IM_COL32(255, 220, 80, 34));
            dl->AddRect(ImVec2(a, laneTop(0)), ImVec2(b, laneTop(1)), IM_COL32(255, 220, 80, 170));
            int inband = 0;
            for (int i = 0; i < n; i++) {
                float x = timeToX(sKeyframes[i].time);
                if (x >= a && x <= b) {
                    inband++;
                }
            }
            CineTooltip("%d keyframe%s", inband, inband == 1 ? "" : "s");
        }
    }
    if (ImGui::IsItemActive()) {
        if (sTlDragMode == 3) {
            // Scale the selection about its fixed end: the grabbed end follows the cursor, every selected
            // keyframe keeps its relative position within the group.
            // Delta-based like the move modes: the hit test accepts clicks up to 8px off the marker, so mapping
            // the end key straight to the cursor's absolute time would snap the group on the first drag frame.
            float endT = sTlGrabbedT0 + (xToTime(mx) - sTlGrabTime0);
            if (snapOn) { // snap the END you're pulling; the group's interior keeps its ratios
                endT = SnapTo(endT, gridT);
            }
            float denom = sTlGrabbedT0 - sTlScaleAnchorT;
            float scale = (std::fabs(denom) > 1e-4f) ? (endT - sTlScaleAnchorT) / denom : 1.0f;
            scale = std::min(std::max(scale, 0.02f), 50.0f); // no collapsing to a point, no flipping past the anchor
            // Cap the scale so no key would land before t=0 - clamping keys individually would pile them up at
            // 0 and silently destroy the group's internal ratios.
            for (size_t s = 0; s < sTlDragIds.size(); s++) {
                float off = sTlDragT0[s] - sTlScaleAnchorT;
                if (IsSelected(sTlDragIds[s]) && off < -1e-6f) {
                    scale = std::min(scale, sTlScaleAnchorT / -off);
                }
            }
            for (size_t s = 0; s < sTlDragIds.size(); s++) {
                if (!IsSelected(sTlDragIds[s])) {
                    continue;
                }
                float nt = std::max(sTlScaleAnchorT + (sTlDragT0[s] - sTlScaleAnchorT) * scale, 0.0f);
                for (int i = 0; i < (int)sIds.size(); i++) {
                    if (sIds[i] == sTlDragIds[s]) {
                        sKeyframes[i].time = nt;
                        break;
                    }
                }
            }
            SortByTime();
        } else if (sTlDragMode != 0) {
            float delta = xToTime(mx) - sTlGrabTime0;
            if (snapOn) { // the grabbed marker lands on the grid; the rest of the selection rides the same
                delta = SnapTo(sTlGrabbedT0 + delta, gridT) - sTlGrabbedT0; // delta, keeping its spacing exact
            }
            for (size_t s = 0; s < sTlDragIds.size(); s++) {
                bool affected = (sTlDragMode == 2) ? (sTlDragT0[s] >= sTlGrabbedT0 - 1e-4f) : IsSelected(sTlDragIds[s]);
                if (!affected) {
                    continue;
                }
                float nt = sTlDragT0[s] + delta;
                if (nt < 0.0f) {
                    nt = 0.0f;
                }
                for (int i = 0; i < (int)sIds.size(); i++) {
                    if (sIds[i] == sTlDragIds[s]) {
                        sKeyframes[i].time = nt;
                        break;
                    }
                }
            }
            SortByTime();
        } else if (sTlTrackDrag >= 0 && sTlTrackDrag < (int)autoLanes.size()) {
            CineParamTrack* tr = autoLanes[sTlTrackDrag]->track;
            if (sTlKeyDrag >= 0 && sTlKeyDrag < (int)tr->keys.size()) {
                // Clamp between neighbors so order (and the dragged index) stays valid - no resort needed.
                float lo = (sTlKeyDrag > 0) ? tr->keys[sTlKeyDrag - 1].time + 1e-3f : 0.0f;
                float hi = (sTlKeyDrag < (int)tr->keys.size() - 1) ? tr->keys[sTlKeyDrag + 1].time - 1e-3f : 1e9f;
                float nt = sTlKeyGrabT0 + (xToTime(mx) - sTlKeyGrabCur);
                if (snapOn) {
                    nt = SnapTo(nt, gridT);
                }
                nt = std::min(std::max(nt, lo), hi);
                tr->keys[sTlKeyDrag].time = nt;
            }
        } else if (sTlScrub) {
            sPlayhead = xToTime(mx);
            if (sPlayhead > content) {
                sPlayhead = content;
            }
            sPreview = true;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (sTlBand) { // Ctrl+drag on empty space: add every keyframe in the swept time range
            float a = std::min(sTlBandX, mx), b = std::max(sTlBandX, mx);
            if (b - a >= 2.5f) {
                for (int i = 0; i < n; i++) {
                    float x = timeToX(sKeyframes[i].time);
                    if (x >= a && x <= b && !IsSelected(sIds[i])) {
                        sSelection.push_back(sIds[i]);
                        if (sSelectedId < 0) {
                            sSelectedId = sIds[i];
                        }
                    }
                }
            }
            sTlBand = false;
        }
        // Empty-space press: only a real CLICK clears the selection - scrubbing the playhead must keep it.
        if (sTlClickClear && std::fabs(mx - sTlPressX) < 4.0f) {
            sSelection.clear();
            sSelectedId = -1;
        }
        sTlClickClear = false;
        sTlDragMode = 0;
        sTlScrub = false;
        sTlNoDrag = false;
        sTlTrackDrag = -1;
        sTlKeyDrag = -1;
        sTlBand = false;
    }

    float total = content;

    // Zoom controls for the ruler (manual zoom turns off auto-fit; Fit re-enables it).
    if (ImGui::SmallButton("-##tlzoom")) {
        sTlAutoFit = false;
        sTlViewDur *= 1.35f;
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Zoom out (show more time)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+##tlzoom")) {
        sTlAutoFit = false;
        sTlViewDur /= 1.35f;
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Zoom in (more precise dragging)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) {
        sTlAutoFit = true;
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Auto-fit the ruler to the path length");
    }
    ImGui::SameLine();
    // The same magnet cluster the curve editor draws, from the same function so the two cannot
    // drift apart - one setting, reachable from either graph, because the editor's other half is
    // often collapsed when you want it.
    SnapToolbarUI("tl", gridT, -1.0f);
    ImGui::SameLine();
    CineHint("|");
    ImGui::SameLine();

    // Step controls: prev keyframe, -1 tick, +1 tick, next keyframe.
    auto setPlayhead = [&](float t) {
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t > total) {
            t = total;
        }
        sPlayhead = t;
        sPreview = true;
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
    };
    if (ImGui::SmallButton("|<")) {
        float best = 0.0f;
        for (int i = 0; i < n; i++) {
            if (sKeyframes[i].time < sPlayhead - 1e-3f && sKeyframes[i].time > best) {
                best = sKeyframes[i].time;
            }
        }
        setPlayhead(best);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Jump to the previous keyframe");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("< tick")) {
        setPlayhead(sPlayhead - kTickSeconds);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Step back one tick (1/20 s)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("tick >")) {
        setPlayhead(sPlayhead + kTickSeconds);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Step forward one tick (1/20 s)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">|")) {
        float best = total;
        for (int i = 0; i < n; i++) {
            if (sKeyframes[i].time > sPlayhead + 1e-3f && sKeyframes[i].time < best) {
                best = sKeyframes[i].time;
            }
        }
        setPlayhead(best);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Jump to the next keyframe");
    }
    ImGui::SameLine();
    ImGui::Text("%.2fs / %.2fs%s", sPlayhead, total, sLoop ? " (loop)" : "");
    if (SelectionCount() > 1) {
        CineHint("%d keyframes selected - drag any to move them together, Alt-drag the first/last to "
                 "compress or expand the group.",
                 SelectionCount());
    } else {
        CineHint("Camera: drag = move, Ctrl+click = multi-select, Ctrl+Shift+click = select range, "
                 "Shift+drag = ripple. Track keys: drag to retime (add/remove via each parameter's keyframe "
                 "button or the curve editor). Ctrl+drag empty space = box-select a time range. Snap lands "
                 "every drag on the time grid.");
    }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
void CinematicCamPathWindow::InitElement() {
    if (!sHookRegistered) {
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnCameraState>(
            [](PlayState* play) { PlaybackTick(); });
        // Force the numeric locale to "C" so ImGui's Ctrl+click value entry (which parses via sscanf("%f", ...))
        // accepts a '.' decimal point. On comma-decimal locales sscanf otherwise stops at the dot, which made
        // typed decimals silently truncate to whole numbers. "C" numeric locale is what SoH's own config parsing
        // already expects, so this is consistent and safe.
        setlocale(LC_NUMERIC, "C");
        sHookRegistered = true;
    }
    // Bar mode is persisted: quitting while collapsed leaves the BAR's slim geometry in imgui.ini, so the next
    // session must come back up as the bar too - otherwise the full editor would open squeezed into that strip.
    if (CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.BarMode"), 0)) {
        sBarMode = true;
        sAutoBar = false;
        sBarSizeFrames = 2; // window geometry already restored by imgui.ini; just enforce the strip size
    }
}

// Inline Premiere-style keyframe control, placed next to a parameter's own widget. The first button toggles
// animation for the parameter (the "stopwatch"); when on, the navigator jumps to the previous key, adds OR
// removes a key at the playhead (with the current `value`), and jumps to the next. Returns true while the track
// drives the value, so the caller routes edits of the parameter into a keyframe at the playhead.
static bool DrawParamKeyNav(CineParamTrack& t, float value) {
    ImGui::PushID(t.id);
    auto jumpTo = [&](float tm) {
        sPlayhead = std::min(std::max(tm, 0.0f), EffectiveTotal());
        sPreview = true;
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
    };

    // Use the state from the START of the frame for push/pop so they stay balanced even though the button toggles
    // t.enabled below (pushing on the old value but popping on the new one was the PopStyleColor crash).
    bool wasEnabled = t.enabled;
    if (wasEnabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.66f, 0.40f, 1.0f));
    }
    if (ImGui::SmallButton(wasEnabled ? "Key on" : "Key")) {
        PushUndo(); // the enabled flag is part of the undo state (snapshotted with the keys)
        t.enabled = !t.enabled;
        if (t.enabled && t.keys.empty()) {
            TrackAddKey(t, sPlayhead, value); // seed a key so it holds the current value
        }
    }
    if (wasEnabled) {
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Keyframe this parameter (animate it over the timeline).");
    }
    if (!t.enabled) {
        ImGui::PopID();
        return false;
    }

    // Locate a key at the playhead and the nearest keys on each side.
    int atIdx = -1;
    float prevT = -1e9f, nextT = 1e9f;
    for (int i = 0; i < (int)t.keys.size(); i++) {
        float kt = t.keys[i].time;
        if (std::fabs(kt - sPlayhead) < 1e-3f) {
            atIdx = i;
        } else if (kt < sPlayhead && kt > prevT) {
            prevT = kt;
        } else if (kt > sPlayhead && kt < nextT) {
            nextT = kt;
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(prevT < -1e8f);
    if (ImGui::SmallButton("|<")) {
        jumpTo(prevT);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(atIdx >= 0 ? "-##key" : "+##key")) {
        PushUndo();
        if (atIdx >= 0) {
            t.keys.erase(t.keys.begin() + atIdx);
        } else {
            TrackAddKey(t, sPlayhead, value);
        }
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip(atIdx >= 0 ? "Remove keyframe at playhead" : "Add keyframe at playhead");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(nextT > 1e8f);
    if (ImGui::SmallButton(">|")) {
        jumpTo(nextT);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return true;
}

// Combined inline keyframe control for the 3-axis movable aim target (keys X/Y/Z together at the playhead).
static void DrawTargetKeyNav() {
    ImGui::PushID("targetkey");
    bool on = sTargetXTrack.enabled || sTargetYTrack.enabled || sTargetZTrack.enabled;
    auto jumpTo = [&](float tm) {
        sPlayhead = std::min(std::max(tm, 0.0f), EffectiveTotal());
        sPreview = true;
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
    };
    auto keyAll = [&]() {
        TrackAddKey(sTargetXTrack, sPlayhead, sAimOverridePoint[0]);
        TrackAddKey(sTargetYTrack, sPlayhead, sAimOverridePoint[1]);
        TrackAddKey(sTargetZTrack, sPlayhead, sAimOverridePoint[2]);
    };
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.66f, 0.40f, 1.0f));
    }
    if (ImGui::SmallButton(on ? "Key on" : "Key")) {
        bool turnOn = !on;
        sTargetXTrack.enabled = turnOn;
        sTargetYTrack.enabled = turnOn;
        sTargetZTrack.enabled = turnOn;
        if (turnOn) {
            PushUndo();
            keyAll();
        }
    }
    if (on) {
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Keyframe the target position (animate it over the timeline); edit per-axis in the Curve "
                    "editor.");
    }
    if (!on) {
        ImGui::PopID();
        return;
    }
    int atIdx = -1;
    float prevT = -1e9f, nextT = 1e9f;
    for (int i = 0; i < (int)sTargetXTrack.keys.size(); i++) {
        float kt = sTargetXTrack.keys[i].time;
        if (std::fabs(kt - sPlayhead) < 1e-3f) {
            atIdx = i;
        } else if (kt < sPlayhead && kt > prevT) {
            prevT = kt;
        } else if (kt > sPlayhead && kt < nextT) {
            nextT = kt;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(prevT < -1e8f);
    if (ImGui::SmallButton("|<")) {
        jumpTo(prevT);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(atIdx >= 0 ? "-##key" : "+##key")) {
        PushUndo();
        if (atIdx >= 0) {
            auto rm = [&](CineParamTrack& t) {
                for (int i = 0; i < (int)t.keys.size(); i++) {
                    if (std::fabs(t.keys[i].time - sPlayhead) < 1e-3f) {
                        t.keys.erase(t.keys.begin() + i);
                        break;
                    }
                }
            };
            rm(sTargetXTrack);
            rm(sTargetYTrack);
            rm(sTargetZTrack);
        } else {
            keyAll();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(nextT > 1e8f);
    if (ImGui::SmallButton(">|")) {
        jumpTo(nextT);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// Instantaneous eye speed (world units/s) at an absolute timeline time - read straight off the distance
// schedule (the exact derivative of what playback renders, not a finite difference). Keyframe ease in/out is
// folded in via the chain rule; the global playback ease is not (the graph shows the authored timeline). The
// aim's rate is measured from the sampled view direction instead - it has no schedule of its own any more.
static void ScheduleSpeedsAt(float time, float* outEye) {
    *outEye = 0.0f;
    int n = (int)sKeyframes.size();
    if (n < 2) {
        return;
    }
    float lastT = sKeyframes[n - 1].time;
    int i1;
    float p, D;
    if (!LoopCyclic()) {
        if (time <= sKeyframes[0].time || time >= lastT) {
            return; // parked on an endpoint
        }
        int i = 0;
        while (i < n - 1 && time >= sKeyframes[i + 1].time) {
            i++;
        }
        D = sKeyframes[i + 1].time - sKeyframes[i].time;
        if (D <= 0.0001f) {
            return;
        }
        p = (time - sKeyframes[i].time) / D;
        i1 = i;
    } else {
        float eff = lastT + sLoopReturnTime;
        if (eff <= 0.0001f) {
            return;
        }
        time = std::fmod(time, eff);
        if (time < 0.0f) {
            time += eff;
        }
        if (time < lastT) {
            int i = 0;
            while (i < n - 1 && time >= sKeyframes[i + 1].time) {
                i++;
            }
            D = sKeyframes[i + 1].time - sKeyframes[i].time;
            if (D <= 0.0001f) {
                return;
            }
            p = (time - sKeyframes[i].time) / D;
            i1 = i;
        } else {
            if (sLoopReturnTime <= 0.0001f) {
                return;
            }
            D = sLoopReturnTime;
            p = (time - lastT) / D;
            i1 = n - 1;
        }
    }
    const CineKeyframe& b = sKeyframes[i1];
    const CineKeyframe& c = sKeyframes[(i1 + 1) % n];
    float dur = SegDurAt(i1);
    if (i1 < sArc.segs) {
        *outEye = std::max(Hermite5Deriv(sArc.S[i1], sArc.S[i1 + 1], sArc.mOut[i1] * dur, sArc.mIn[i1] * dur,
                                         sArc.aOut[i1], sArc.aIn[i1], p),
                           0.0f) /
                  D;
    }
}

// Diagnostic graph channels. Two of them are RATES read straight off the motion schedules; the rest are the
// pose VALUES playback actually produces at each moment (sampled through SampleAt, so what you see is exactly
// what the camera does). Each is toggled separately - overlaying all seven at once tells you nothing.
struct CineSgChan {
    const char* name;
    ImU32 col;
    int bit;
    bool rate; // true: plotted 0..peak. false: plotted across its own min..max
    const char* unit;
    // Smallest range the plot will stretch over the graph's full height. Without it, a channel that is
    // essentially CONSTANT gets its last-digit float noise blown up to full scale and reads as violent
    // jitter that isn't in the camera at all.
    float minSpan;
};
static const CineSgChan kSgChans[] = {
    { "Speed", IM_COL32(255, 140, 50, 235), 1, true, "u/s", 25.0f },
    { "Aim", IM_COL32(80, 210, 255, 235), 2, true, "deg/s", 15.0f },
    { "Pitch", IM_COL32(130, 235, 130, 235), 4, false, "deg", 2.0f },
    { "Yaw", IM_COL32(235, 140, 235, 235), 8, false, "deg", 2.0f },
    { "X", IM_COL32(240, 110, 110, 235), 16, false, "", 5.0f },
    { "Y", IM_COL32(240, 225, 120, 235), 32, false, "", 5.0f },
    { "Z", IM_COL32(120, 160, 255, 235), 64, false, "", 5.0f },
};
static const int kSgCount = (int)(sizeof(kSgChans) / sizeof(kSgChans[0]));
static int SpeedGraphMask() {
    return CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SpeedGraphChans"), 1 | 2);
}

// Draw the enabled diagnostic channels over the visible time window. Every channel is normalized to its own
// range (printed in the legend), so shape is comparable across wildly different units. forceSpeedMax > 0
// pins the Speed channel's scale (used while a handle drag is live, so the graph can't rescale under the
// cursor); outSpeedMax reports the scale actually used, for the handles.
// Peak speed over the entire timeline, cached against the path hash. This is the Speed channel's scale, so
// that zooming or panning the graph never moves a handle: the picture changes, what it measures does not.
static float SpeedPeakWhole() {
    static uint32_t sCached = 0;
    static float sPeak = 0.0f;
    static bool sHave = false;
    uint32_t h = PathShapeHash();
    if (sHave && h == sCached) {
        return sPeak;
    }
    float total = EffectiveTotal();
    float peak = 0.0f;
    const int kN = 256;
    for (int i = 0; i <= kN; i++) {
        float s = 0.0f;
        ScheduleSpeedsAt(total * (float)i / (float)kN, &s);
        peak = std::max(peak, s);
    }
    sCached = h;
    sPeak = peak;
    sHave = true;
    return peak;
}

static void DrawSpeedGraphInto(ImDrawList* dl, float gx0, float gx1, float gy0, float gy1, float vt0, float vt1,
                               float forceSpeedMax, float* outSpeedMax) {
    if (outSpeedMax) {
        *outSpeedMax = 0.0f;
    }
    int mask = SpeedGraphMask();
    if (sKeyframes.size() < 2 || !(vt1 > vt0) || mask == 0) {
        return;
    }
    ArcEnsure();
    int N = (int)((gx1 - gx0) / 3.0f);
    N = std::min(std::max(N, 64), 512);
    std::vector<std::vector<float>> v((size_t)kSgCount, std::vector<float>((size_t)N + 1, 0.0f));
    // The aim rate is MEASURED from the sampled view direction (the aim has no schedule to read a derivative
    // off), so it needs the pose too.
    bool needPose = (mask & (2 | 4 | 8 | 16 | 32 | 64)) != 0;
    const float kRad2Deg = 57.2957795f;
    float prevYaw = 0.0f;
    bool haveYaw = false;
    float dt = (vt1 - vt0) / (float)N;
    std::vector<float> dirs((size_t)(N + 1) * 3, 0.0f);
    for (int i = 0; i <= N; i++) {
        float t = vt0 + (vt1 - vt0) * (float)i / (float)N;
        ScheduleSpeedsAt(t, &v[0][i]);
        if (!needPose) {
            continue;
        }
        CineKeyframe s = SampleAt(t);
        float d[3];
        v3sub(s.at, s.eye, d);
        if (v3len(d) > 1e-4f) {
            v3norm(d);
            float pitch = std::asin(std::min(std::max(d[1], -1.0f), 1.0f)) * kRad2Deg;
            float yaw = std::atan2(d[0], d[2]) * kRad2Deg;
            if (haveYaw) { // keep yaw continuous across the +-180 seam so the curve doesn't jump
                while (yaw - prevYaw > 180.0f) {
                    yaw -= 360.0f;
                }
                while (yaw - prevYaw < -180.0f) {
                    yaw += 360.0f;
                }
            }
            prevYaw = yaw;
            haveYaw = true;
            v[2][i] = pitch;
            v[3][i] = yaw;
            std::memcpy(&dirs[(size_t)i * 3], d, sizeof(d));
        } else if (i > 0) {
            v[2][i] = v[2][i - 1];
            v[3][i] = v[3][i - 1];
            std::memcpy(&dirs[(size_t)i * 3], &dirs[(size_t)(i - 1) * 3], sizeof(float) * 3);
        }
        v[4][i] = s.eye[0];
        v[5][i] = s.eye[1];
        v[6][i] = s.eye[2];
    }
    for (int i = 0; i <= N && needPose; i++) {
        // Turn rate over a FIXED interval (~1/30 s), not over the sample spacing: zoomed in, neighbouring
        // samples are microseconds apart and the division blew float noise up into a jittering line.
        int k = std::max(1, (int)std::ceil(0.033f / std::max(dt, 1e-5f) * 0.5f));
        int i0s = std::max(i - k, 0), i1s = std::min(i + k, N);
        float h = (float)(i1s - i0s) * dt;
        if (h > 1e-6f) {
            // atan2(|cross|, dot), not acos(dot): acos loses nearly all float precision for small angles
            // (dot ~ 0.9999999), which drew jitter on near-constant aims that the camera never had.
            const float* u = &dirs[(size_t)i0s * 3];
            const float* w = &dirs[(size_t)i1s * 3];
            float cx = u[1] * w[2] - u[2] * w[1], cy = u[2] * w[0] - u[0] * w[2], cz = u[0] * w[1] - u[1] * w[0];
            float cross = std::sqrt(cx * cx + cy * cy + cz * cz);
            v[1][i] = std::atan2(cross, v3dot(u, w)) * kRad2Deg / h;
        }
    }
    float labY = gy0 + 2.0f;
    for (int c = 0; c < kSgCount; c++) {
        if (!(mask & kSgChans[c].bit)) {
            continue;
        }
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i <= N; i++) {
            lo = std::min(lo, v[c][i]);
            hi = std::max(hi, v[c][i]);
        }
        char lab[80];
        if (c == 0) {
            // Speed fits the WHOLE timeline, not the visible window: it is the one channel you edit, and a
            // scale that changed every time you zoomed would move the handles and the ceiling out from under
            // the cursor. A drag freezes it outright.
            lo = 0.0f;
            hi = (forceSpeedMax > 0.0f) ? forceSpeedMax : SpeedPeakWhole();
        }
        bool flat = (hi - lo) < kSgChans[c].minSpan;
        if (kSgChans[c].rate) {
            lo = 0.0f;
            snprintf(lab, sizeof(lab), "%-5s peak %.1f %s", kSgChans[c].name, hi, kSgChans[c].unit);
        } else {
            snprintf(lab, sizeof(lab), "%-5s %.2f .. %.2f %s%s", kSgChans[c].name, lo, hi, kSgChans[c].unit,
                     flat ? "  (flat)" : "");
        }
        dl->AddText(ImVec2(gx0 + 4.0f, labY), kSgChans[c].col, lab);
        labY += 13.0f;
        // Never stretch a range smaller than the channel's minSpan across the graph - see minSpan above.
        if (flat) {
            float mid = 0.5f * (lo + hi);
            lo = mid - kSgChans[c].minSpan * 0.5f;
            hi = mid + kSgChans[c].minSpan * 0.5f;
            if (kSgChans[c].rate) {
                lo = 0.0f;
                hi = kSgChans[c].minSpan;
            }
        }
        float span = hi - lo;
        if (span < 1e-6f) {
            continue;
        }
        if (c == 0 && outSpeedMax) {
            *outSpeedMax = hi; // the scale the Speed handles must share
        }
        // Normalized value clamped to [0,1]: a sample must never draw outside the graph rect (the standalone
        // rect has no clip, and an escaping line painted over the timeline).
        auto yOf = [&](float val) {
            float u = std::min(std::max((val - lo) / span, 0.0f), 1.0f);
            return gy1 - u * (gy1 - gy0) * 0.92f;
        };
        ImVec2 prev(gx0, yOf(v[c][0]));
        for (int i = 1; i <= N; i++) {
            ImVec2 cur(gx0 + (gx1 - gx0) * (float)i / (float)N, yOf(v[c][i]));
            dl->AddLine(prev, cur, kSgChans[c].col, 1.5f);
            prev = cur;
        }
    }
}

// Editing the Speed curve, with the same grammar as every other curve in the editor:
//
//   the POINT on a keyframe is its speed      - drag it up or down to make the camera faster or slower there
//   the two HANDLES are Bezier tangents       - drag one to rotate the curve through the point, shaping the
//                                               ease into and out of the keyframe
//
// So the point moves the curve and the handles bend it, and breaking a pair (Alt-drag) gives a CORNER, not a
// step: the speed at a keyframe is one number, because two different speeds at the same instant is a jump the
// camera can only take as a hitch. Handles belong to selected keyframes only - drawing every pair at once
// buried the curve they were meant to describe.
//
// Everything draws at its EFFECTIVE value, so when exactness limits a drag you watch it stop against the
// ceiling line rather than wondering why the number disagrees with the picture.
static int sSpDragKnot = -1;    // keyframe index being dragged (-1 = none)
static int sSpDragSide = -1;    // -1 = the point itself, 0 = arriving handle, 1 = leaving handle
static float sSpDragMax = 0.0f; // frozen Speed scale for the duration of the drag
static bool sSpHot = false;     // a handle is hovered or dragged: the editor's own click handlers must yield
// Grab bookkeeping. Picking something up must not change it: the drag applies the cursor's OFFSET from where
// it grabbed, and nothing is written (nor pushed to undo) until the cursor has actually moved.
static ImVec2 sSpGrabOff(0.0f, 0.0f);
static bool sSpDragMoved = false;
static void SpeedHandlesUI(ImDrawList* dl, float gx0, float gx1, float gy0, float gy1, float vt0, float vt1,
                           float speedMax, bool hovered) {
    sSpHot = sSpDragKnot >= 0;
    int n = (int)sKeyframes.size();
    if (!(SpeedGraphMask() & 1) || n < 2 || speedMax <= 0.0f || !(vt1 > vt0)) {
        return;
    }
    ArcEnsure();
    ImGuiIO& io = ImGui::GetIO();
    const float kArm = 26.0f; // handle arm length in pixels, matching the curve editor's Bezier handles
    float scale = (sSpDragKnot >= 0 && sSpDragMax > 0.0f) ? sSpDragMax : speedMax;
    auto yOf = [&](float rate) { return gy1 - std::min(std::max(rate / scale, 0.0f), 1.0f) * (gy1 - gy0) * 0.92f; };
    auto rateOfY = [&](float y) { return std::max((gy1 - y) / ((gy1 - gy0) * 0.92f), 0.0f) * scale; };
    auto xOf = [&](float t) { return gx0 + ((t - vt0) / (vt1 - vt0)) * (gx1 - gx0); };
    float secPerPx = (vt1 - vt0) / std::max(gx1 - gx0, 1.0f);
    float pxPerRate = (gy1 - gy0) * 0.92f / std::max(scale, 1e-5f); // pixels per unit of speed
    // An acceleration, drawn: convert it to a screen slope, then walk kArm pixels ALONG that slope. dir is +1
    // for the leaving handle, -1 for the arriving one (which reaches back in time).
    auto ArmEnd = [&](ImVec2 knot, float accel, float dir) {
        float vx = dir, vy = -accel * secPerPx * pxPerRate * dir;
        float len = std::sqrt(vx * vx + vy * vy);
        return ImVec2(knot.x + vx / len * kArm, knot.y + vy / len * kArm);
    };
    // ...and back: the acceleration implied by a handle sitting at `hp`.
    auto AccelOfArm = [&](ImVec2 knot, ImVec2 hp, float dir) {
        // Floor the horizontal reach at a real fraction of the arm, not a few pixels. A tiny floor doesn't
        // saturate the angle, it AMPLIFIES it: drag the handle back past its own knot and dy keeps its full
        // size over a 4px dx, asking for an acceleration ~7x steeper than the arm is even pointing. The
        // schedule then refuses it, the handle springs back to where it was, and the drag looks broken while
        // having quietly marked the keyframe as hand-set. This bounds the steepest handle to ~70 degrees.
        float dx = std::max((hp.x - knot.x) * dir, kArm * 0.35f);
        float dy = (hp.y - knot.y) * dir;
        return -dy / dx / std::max(secPerPx * pxPerRate, 1e-9f);
    };
    // The speed at keyframe i, and the accelerations either side, exactly as the schedule ended up using them.
    auto stateAt = [&](int i, float* rate, float* aIn, float* aOut) {
        int segIn = (i > 0) ? i - 1 : (LoopCyclic() ? sArc.segs - 1 : -1);
        int segOut = (i < sArc.segs) ? i : -1;
        *rate = (segOut >= 0) ? sArc.mOut[segOut] : ((segIn >= 0) ? sArc.mIn[segIn] : 0.0f);
        // Stored per local progress; the handle is drawn in real seconds, so undo the segment's duration.
        float dIn = (segIn >= 0) ? SegDurAt(segIn) : 1.0f;
        float dOut = (segOut >= 0) ? SegDurAt(segOut) : 1.0f;
        *aIn = (segIn >= 0) ? sArc.aIn[segIn] / std::max(dIn * dIn, 1e-6f) : 0.0f;
        *aOut = (segOut >= 0) ? sArc.aOut[segOut] / std::max(dOut * dOut, 1e-6f) : 0.0f;
    };
    int hover = -1, hoverSide = -2;
    for (int i = 0; i < n; i++) {
        float t = sKeyframes[i].time;
        if (t < vt0 - 1e-4f || t > vt1 + 1e-4f) {
            continue;
        }
        float rate, aIn, aOut;
        stateAt(i, &rate, &aIn, &aOut);
        float x = xOf(t);
        ImVec2 knot(x, yOf(rate));
        bool showHandles = IsSelected(sIds[i]) || sSpDragKnot == i;
        if (showHandles) {
            // A path end has a segment on one side only; the missing side's handle would shape nothing, so it
            // isn't drawn rather than sitting there ignoring drags.
            bool haveIn = (i > 0) || LoopCyclic();
            bool haveOut = (i < sArc.segs);
            // Fixed-length arms: the acceleration sets the handle's ANGLE, and the arm keeps the same length
            // whatever that angle is. Drawing the tangent at a fixed horizontal reach instead made the arms
            // stretch and shrink as the slope changed, which read as the handles jumping around while you
            // dragged the point. Every curve editor draws them this way for exactly this reason.
            ImVec2 pIn = ArmEnd(knot, aIn, -1.0f);
            ImVec2 pOut = ArmEnd(knot, aOut, 1.0f);
            if (haveIn) {
                dl->AddLine(pIn, knot, IM_COL32(255, 140, 50, 150), 1.2f);
            }
            if (haveOut) {
                dl->AddLine(knot, pOut, IM_COL32(255, 140, 50, 150), 1.2f);
            }
            for (int sd = 0; sd < 2; sd++) {
                if (!(sd ? haveOut : haveIn)) {
                    continue;
                }
                ImVec2 hp = sd ? pOut : pIn;
                bool hot = (sSpDragKnot == i && sSpDragSide == sd);
                if (hovered && sSpDragKnot < 0 && std::fabs(io.MousePos.x - hp.x) < 7.0f &&
                    std::fabs(io.MousePos.y - hp.y) < 8.0f) {
                    hover = i;
                    hoverSide = sd;
                    hot = true;
                }
                // Hollow while automatic, solid once you've set it. An automatic handle re-derives itself
                // when the curve around it changes - including when you drag this keyframe's own speed - and
                // that is worth being able to see at a glance, since it's the difference between a handle
                // that follows the curve and one that commands it.
                bool manual = sd ? (sKeyframes[i].hasAccelOut != 0) : (sKeyframes[i].hasAccelIn != 0);
                if (manual) {
                    dl->AddCircleFilled(hp, hot ? 5.0f : 3.5f, IM_COL32(255, 175, 90, 235));
                } else {
                    dl->AddCircleFilled(hp, hot ? 5.0f : 3.5f, IM_COL32(40, 42, 48, 255));
                    dl->AddCircle(hp, hot ? 5.0f : 3.5f, IM_COL32(255, 175, 90, 200), 0, 1.4f);
                }
            }
        }
        bool hotKnot = (sSpDragKnot == i && sSpDragSide == -1);
        if (hovered && sSpDragKnot < 0 && hover < 0 && std::fabs(io.MousePos.x - knot.x) < 7.0f &&
            std::fabs(io.MousePos.y - knot.y) < 8.0f) {
            hover = i;
            hoverSide = -1;
            hotKnot = true;
        }
        dl->AddCircleFilled(knot, hotKnot ? 6.0f : 4.0f, IM_COL32(255, 140, 50, 255));
        if (sKeyframes[i].speedRate >= 0.0f) { // a speed you set, as opposed to one the schedule chose
            dl->AddCircle(knot, 7.5f, IM_COL32(255, 255, 255, 200));
        }
    }
    // The exactness ceiling for the keyframe in hand, drawn where it bites. The bound is a property of the
    // path (3x the slower of the two segments it sits between), so it holds still while the view moves. Shown
    // for a selected keyframe as well as a dragged one - waiting for the drag meant it only ever appeared
    // once you were already pushing against it.
    int capKnot = (sSpDragKnot >= 0) ? sSpDragKnot : ((hover >= 0) ? hover : -1);
    if (capKnot < 0) {
        for (int i = 0; i < n && capKnot < 0; i++) {
            if (IsSelected(sIds[i])) {
                capKnot = i;
            }
        }
    }
    if (capKnot >= 0 && capKnot < n) {
        float cap = KnotSpeedCap(sArc.S, sArc.segs, capKnot);
        if (cap > 0.0f && cap <= scale) {
            float cy = yOf(cap);
            for (float x = gx0; x < gx1; x += 9.0f) {
                dl->AddLine(ImVec2(x, cy), ImVec2(std::min(x + 5.0f, gx1), cy), IM_COL32(255, 90, 90, 170), 1.0f);
            }
        }
    }
    if (hover >= 0) {
        sSpHot = true;
        float rate, aIn, aOut;
        stateAt(hover, &rate, &aIn, &aOut);
        if (hoverSide == -1) {
            ImGui::SetTooltip("%.0f u/s at this keyframe - drag to set the speed here (the times never move), "
                              "right-click for automatic",
                              rate);
        } else {
            bool manual = hoverSide ? (sKeyframes[hover].hasAccelOut != 0) : (sKeyframes[hover].hasAccelIn != 0);
            ImGui::SetTooltip("%+.0f u/s^2 %s (%s) - drag to shape the ease through this keyframe, Alt-drag to "
                              "break the two handles apart",
                              hoverSide ? aOut : aIn, hoverSide ? "leaving" : "arriving",
                              manual ? "set by you" : "automatic: follows the curve until you drag it");
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            sSpDragKnot = hover;
            sSpDragSide = hoverSide;
            sSpDragMoved = false;
            // Freeze the scale as it is. It used to expand here to fit the ceiling, which squashed the whole
            // curve the instant you touched a point - a rescale nobody asked for, in response to a click that
            // wasn't meant to change anything. If the drag actually pushes into the top of the graph the scale
            // opens up then (below), when the extra room is the thing being asked for.
            sSpDragMax = speedMax;
            // Clicking a point selects its keyframe, like clicking it anywhere else in the editor - that is
            // also what makes its handles stay up after the mouse is released.
            if (hoverSide == -1) {
                if (io.KeyCtrl) {
                    ToggleSelect(sIds[hover]);
                } else if (!IsSelected(sIds[hover])) {
                    SelectOnly(sIds[hover]);
                } else {
                    sSelectedId = sIds[hover];
                }
            }
            // Remember where inside the dot the grab landed, so picking it up doesn't move it.
            float rate2, aIn2, aOut2;
            stateAt(hover, &rate2, &aIn2, &aOut2);
            ImVec2 knot(xOf(sKeyframes[hover].time), yOf(rate2));
            ImVec2 grabbed =
                (hoverSide == -1) ? knot : ArmEnd(knot, hoverSide ? aOut2 : aIn2, hoverSide ? 1.0f : -1.0f);
            sSpGrabOff = ImVec2(grabbed.x - io.MousePos.x, grabbed.y - io.MousePos.y);
            if (io.KeyAlt && hoverSide >= 0) {
                PushUndo();
                sSpDragMoved = true; // breaking the pair IS an edit, even if the cursor never moves
                sKeyframes[hover].speedBroken = 1;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            PushUndo();
            CineKeyframe& k = sKeyframes[hover];
            k.speedRate = -1.0f;
            k.hasAccelIn = k.hasAccelOut = 0;
            k.speedAccelIn = k.speedAccelOut = 0.0f;
            k.speedBroken = 0;
        }
    }
    if (sSpDragKnot >= 0) {
        if (sSpDragKnot < n && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 at(io.MousePos.x + sSpGrabOff.x, io.MousePos.y + sSpGrabOff.y);
            if (!sSpDragMoved) {
                // A click that never travelled is a selection, not an edit: nothing written, no undo entry.
                // The dead zone is what keeps a slightly shaky click from nudging the value.
                ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                if (std::fabs(dd.x) + std::fabs(dd.y) < 2.5f) {
                    return;
                }
                PushUndo();
                sSpDragMoved = true;
            }
            CineKeyframe& k = sKeyframes[sSpDragKnot];
            if (sSpDragSide == -1) {
                float want = rateOfY(at.y);
                float cap = KnotSpeedCap(sArc.S, sArc.segs, sSpDragKnot);
                // Only now, with the drag pressed against the top of the graph, is more headroom what you're
                // asking for - so open the scale up to the ceiling and let the point run to it.
                if (want > sSpDragMax * 0.98f && cap > sSpDragMax) {
                    sSpDragMax = cap * 1.06f;
                }
                // Clamp before storing, and against the bound shared by BOTH neighbouring segments: a value
                // the two sides would have to disagree about is exactly the jump this curve cannot have.
                k.speedRate = std::min(want, cap);
            } else {
                float rate, aIn, aOut;
                stateAt(sSpDragKnot, &rate, &aIn, &aOut);
                ImVec2 knot(xOf(sKeyframes[sSpDragKnot].time), yOf(rate));
                float a = AccelOfArm(knot, at, sSpDragSide ? 1.0f : -1.0f);
                if (k.speedBroken) {
                    if (sSpDragSide) {
                        k.hasAccelOut = 1;
                        k.speedAccelOut = a;
                    } else {
                        k.hasAccelIn = 1;
                        k.speedAccelIn = a;
                    }
                } else { // unbroken: one straight tangent through the point, so both sides take the same slope
                    k.hasAccelIn = k.hasAccelOut = 1;
                    k.speedAccelIn = k.speedAccelOut = a;
                }
            }
        } else {
            sSpDragKnot = -1;
            sSpDragSide = -1;
            sSpDragMoved = false;
        }
    }
}

// Curve editor: a value-over-time graph overlaying the enabled continuous parameter tracks - including the
// camera channels (Cam roll / Cam FOV), which are ordinary tracks with their own keys. Drag a point in 2D to
// retime + revalue it; right-click deletes; double-click empty space drops a key; per-key interpolation (Step /
// Linear / Smooth / Bezier with handles). Mouse wheel zooms the time axis, middle-drag pans, Fit resets.
static void DrawCurveEditor() {
    struct CurveChannel {
        const char* name;
        ImU32 col;
        CineParamTrack* track;
        float vmin, vmax; // fixed range (vmax > vmin) or 0,0 = auto-fit to the keys
    };
    static const ImU32 kChanCol[6] = { IM_COL32(120, 200, 255, 255), IM_COL32(255, 180, 90, 255),
                                       IM_COL32(150, 230, 120, 255), IM_COL32(230, 130, 230, 255),
                                       IM_COL32(240, 220, 90, 255),  IM_COL32(120, 230, 230, 255) };

    // Quick keyframe toggles for the camera channels - always offered so you can start animating roll / FOV
    // (they seed a key at the playhead holding the current interpolated value).
    {
        CineKeyframe cur{};
        if (sKeyframes.size() >= 2) {
            cur = SampleAt(sPlayhead);
        } else if (!sKeyframes.empty()) {
            cur = sKeyframes[0];
        } else {
            CinematicCam_GetPose(cur.eye, cur.at, &cur.roll, &cur.fov);
        }
        // Seed new keys from whatever is actually DRIVING right now: the track itself when it's on (adding a
        // key mid-curve must not step it), else the keyframes' interpolated value.
        float rollSeed = cur.roll, fovSeed = cur.fov, tv;
        if (sRollTrack.enabled && EvalParamTrack(sRollTrack, sPlayhead, tv)) {
            rollSeed = tv;
        }
        if (sFovTrack.enabled && EvalParamTrack(sFovTrack, sPlayhead, tv)) {
            fovSeed = tv;
        }
        CineHint("Camera:");
        ImGui::SameLine();
        ImGui::TextUnformatted("Roll");
        ImGui::SameLine();
        DrawParamKeyNav(sRollTrack, rollSeed);
        ImGui::SameLine();
        ImGui::TextUnformatted("  FOV");
        ImGui::SameLine();
        DrawParamKeyNav(sFovTrack, fovSeed);
        ImGui::SameLine();
        ImGui::TextUnformatted(" ");
        ImGui::SameLine();
        bool spd = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SpeedGraph"), 0) != 0;
        if (ImGui::Checkbox("Speed graph", &spd)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.SpeedGraph"), spd ? 1 : 0);
            CVarSave();
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Debugging overlay: what the camera actually does over the timeline. Speed and Aim "
                        "are rates read off the motion schedules; Pitch / Yaw / X / Y / Z are the values "
                        "playback produces. Toggle channels below - each is scaled to its own range, printed "
                        "in the legend.");
        }
        if (spd) { // per-channel toggles, so the graph shows only what you're diagnosing
            int mask = SpeedGraphMask();
            for (int c = 0; c < kSgCount; c++) {
                bool on = (mask & kSgChans[c].bit) != 0;
                ImGui::PushID(c);
                ImU32 col = on ? kSgChans[c].col : ((kSgChans[c].col & 0x00FFFFFF) | 0x66000000);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "%s%s", on ? "* " : "", kSgChans[c].name);
                if (ImGui::SmallButton(lbl)) {
                    CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.SpeedGraphChans"), mask ^ kSgChans[c].bit);
                    CVarSave();
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
                if (c < kSgCount - 1) {
                    ImGui::SameLine();
                }
            }
            if (SpeedGraphMask() & 1) { // the Speed channel is the editable one - say how, since it looks like a plot
                CineHint("Speed: drag a keyframe's point to set the speed there. Select keyframes to get their "
                         "ease handles; Alt-drag a handle to bend one side alone. Right-click a point for "
                         "automatic. The red line is the fastest that keyframe can be taken and still arrive on "
                         "time.");
            }
        }
    }
    bool showSpeed = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SpeedGraph"), 0) != 0;
    // The visible time window, declared up here because BOTH graphs use it: the channel editor below and the
    // standalone speed graph, which is what you get when no parameter track is enabled. The speed graph used
    // to be stuck at the full timeline in that case - you could only zoom by first keying something you didn't
    // want, which is a strange price for a closer look at the speed.
    static float sCvT0 = 0.0f, sCvT1 = -1.0f; // sCvT1 <= sCvT0 = the whole timeline

    std::vector<CurveChannel> curves;
    int pc = 0;
    for (const TrackDef& d : AllTrackDefs()) {
        if (d.continuous && d.track->enabled) {
            curves.push_back({ d.name, kChanCol[pc % 6], d.track, d.vmin, d.vmax });
            pc++;
        }
    }
    if (curves.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        CineHint("Keyframe a camera channel above (Roll / FOV) or enable a parameter track (Time of day, "
                 "Shake, Letterbox, Target X/Y/Z) to shape its curve here.");
        ImGui::PopTextWrapPos();
        if (showSpeed && sKeyframes.size() >= 2) { // the speed graph stands on its own - no tracks needed
            // Fixed slice (matched by the layout budget in DrawElement) so it never pushes the timeline
            // off-screen - taking all remaining height made the bottom region scroll.
            float sgH = std::min(std::max(ImGui::GetContentRegionAvail().y - 6.0f, 80.0f), 126.0f);
            ImVec2 sz(std::max(ImGui::GetContentRegionAvail().x, 80.0f), sgH);
            ImVec2 q0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##speedgraph", sz);
            ImVec2 q1(q0.x + sz.x, q0.y + sz.y);
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            sdl->AddRectFilled(q0, q1, IM_COL32(24, 24, 27, 255), 4.0f);
            sdl->AddRect(q0, q1, IM_COL32(90, 90, 95, 255), 4.0f);
            float tEnd = EffectiveTotal();
            if (tEnd < 0.001f) {
                tEnd = 1.0f;
            }
            float sgx0 = q0.x + 6.0f, sgx1 = q1.x - 6.0f;
            // The window is shared with the channel editor and outlives the path it was set on: shorten the
            // path (or delete keyframes) while zoomed and it can end up entirely past the end, drawing an
            // empty graph with no hint as to why. Clamp it back into the timeline, and drop to Fit if there's
            // nothing left of it.
            if (sCvT1 > sCvT0) {
                sCvT0 = std::min(std::max(sCvT0, 0.0f), tEnd);
                sCvT1 = std::min(std::max(sCvT1, 0.0f), tEnd);
                if (sCvT1 - sCvT0 < tEnd * 0.01f) {
                    sCvT0 = 0.0f;
                    sCvT1 = -1.0f;
                }
            }
            float vt0 = (sCvT1 > sCvT0) ? sCvT0 : 0.0f;
            float vt1 = (sCvT1 > sCvT0) ? sCvT1 : tEnd;
            // Same wheel-zoom / middle-drag-pan as the channel editor, on the same window state, so switching
            // between the two views keeps your place.
            ImGuiIO& sio = ImGui::GetIO();
            if (ImGui::IsItemHovered() && sio.MouseWheel != 0.0f && sSpDragKnot < 0) {
                float mt = vt0 + ((sio.MousePos.x - sgx0) / std::max(sgx1 - sgx0, 1.0f)) * (vt1 - vt0);
                mt = std::min(std::max(mt, 0.0f), tEnd);
                float fz = std::pow(0.8f, sio.MouseWheel); // wheel up = zoom in
                float nt0 = mt - (mt - vt0) * fz, nt1 = mt + (vt1 - mt) * fz;
                float minSpan = tEnd * 0.02f;
                if (nt1 - nt0 < minSpan) {
                    float c = (nt0 + nt1) * 0.5f;
                    nt0 = c - minSpan * 0.5f;
                    nt1 = c + minSpan * 0.5f;
                }
                nt0 = std::max(nt0, 0.0f);
                nt1 = std::min(nt1, tEnd);
                if (nt1 - nt0 >= tEnd * 0.999f) {
                    sCvT0 = 0.0f;
                    sCvT1 = -1.0f; // zoomed right out: back to the full timeline
                } else {
                    sCvT0 = nt0;
                    sCvT1 = nt1;
                }
                vt0 = (sCvT1 > sCvT0) ? sCvT0 : 0.0f;
                vt1 = (sCvT1 > sCvT0) ? sCvT1 : tEnd;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Middle) && sCvT1 > sCvT0) {
                float dt = -sio.MouseDelta.x * (vt1 - vt0) / std::max(sgx1 - sgx0, 1.0f);
                dt = std::min(std::max(dt, -vt0), tEnd - vt1);
                sCvT0 = vt0 + dt;
                sCvT1 = vt1 + dt;
                vt0 = sCvT0;
                vt1 = sCvT1;
            }
            sdl->PushClipRect(q0, q1, true); // belt and braces: nothing painted past the graph frame
            float spMax = 0.0f;
            DrawSpeedGraphInto(sdl, sgx0, sgx1, q0.y + 8.0f, q1.y - 8.0f, vt0, vt1,
                               (sSpDragKnot >= 0) ? sSpDragMax : 0.0f, &spMax);
            SpeedHandlesUI(sdl, sgx0, sgx1, q0.y + 8.0f, q1.y - 8.0f, vt0, vt1, spMax, ImGui::IsItemHovered());
            if (sPlayhead >= vt0 && sPlayhead <= vt1) {
                float sphx = sgx0 + ((sPlayhead - vt0) / std::max(vt1 - vt0, 1e-5f)) * (sgx1 - sgx0);
                sdl->AddLine(ImVec2(sphx, q0.y + 4.0f), ImVec2(sphx, q1.y - 4.0f), IM_COL32(60, 255, 90, 150), 1.5f);
            }
            sdl->PopClipRect();
            CineHint("Wheel: zoom time. Middle-drag: pan. Zoom right out to fit.");
        }
        return;
    }

    // The active (editable) channel is tracked by identity, so toggling another channel's visibility doesn't shift
    // which curve you're editing.
    static CineParamTrack* sActTrack = nullptr;
    static int drag = -1;
    static float sDragLo = 0.0f, sDragHi = 1.0f; // active channel's value range, frozen for the duration of a drag
    static bool sDragRange = false;
    int active = -1;
    for (int i = 0; i < (int)curves.size(); i++) {
        if (curves[i].track == sActTrack) {
            active = i;
            break;
        }
    }
    if (active < 0) { // the previously active channel is gone (disabled/hidden) - fall back
        active = 0;
        drag = -1;
    }

    // Legend: every visible channel; click one to make it the editable (bright) curve, the rest stay faded.
    // (Selection is shared per (track,id), so it persists across channels.)
    for (int i = 0; i < (int)curves.size(); i++) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(curves[i].col));
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s%s", (i == active) ? "* " : "", curves[i].name);
        if (ImGui::SmallButton(lbl)) {
            active = i;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
    sActTrack = curves[active].track; // remember the active channel's identity for next frame
    const CurveChannel& AL = curves[active];

    float total = EffectiveTotal();
    if (total < 0.001f) {
        total = 1.0f;
    }
    // Visible time window (zoom/pan state, clamped to the timeline).
    float vt0 = 0.0f, vt1 = total;
    if (sCvT1 > sCvT0) {
        vt0 = std::min(std::max(sCvT0, 0.0f), total);
        vt1 = std::min(std::max(sCvT1, vt0 + total * 0.01f), total);
        if (vt1 - vt0 < 1e-4f) { // the timeline shrank underneath the zoom window (deletes / shorter path
            sCvT0 = 0.0f;        // loaded): a zero-width window would divide time-to-pixel by zero
            sCvT1 = -1.0f;
            vt0 = 0.0f;
            vt1 = total;
        }
    }

    auto keyCount = [&](const CurveChannel& C) -> int { return (int)C.track->keys.size(); };
    auto keyTime = [&](const CurveChannel& C, int i) -> float { return C.track->keys[i].time; };
    auto keyValue = [&](const CurveChannel& C, int i) -> float { return C.track->keys[i].value; };
    // Display range for a channel: its fixed range if it has one (vmax > vmin), otherwise auto-fit to the keys
    // (with padding) - needed for unbounded values like positions.
    auto rangeOf = [&](const CurveChannel& C) -> std::pair<float, float> {
        if (C.vmax > C.vmin) {
            return { C.vmin, C.vmax };
        }
        float lo = 1e30f, hi = -1e30f;
        int n = keyCount(C);
        for (int i = 0; i < n; i++) {
            float v = keyValue(C, i);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (C.track == &sRollTrack) {   // floor the roll view to a full half-turn each way (barrel rolls past
            lo = std::min(lo, -180.0f); // +-180 still expand it; they're never clamped)
            hi = std::max(hi, 180.0f);
        }
        if (lo > hi) {
            lo = 0.0f;
            hi = 1.0f;
        }
        if (hi - lo < 1e-3f) {
            lo -= 1.0f;
            hi += 1.0f;
        }
        float pad = (hi - lo) * 0.1f;
        return { lo - pad, hi + pad };
    };
    std::pair<float, float> ar = rangeOf(AL);
    float vmin = ar.first, vmax = ar.second;
    // While dragging a point, freeze the active channel's value range (captured at grab time). This keeps the drag
    // sensitivity constant and stops the graph from shifting under the cursor as the dragged value sets a new
    // extreme - the auto-ranged position channels would otherwise run away.
    if (drag >= 0 && sDragRange) {
        vmin = sDragLo;
        vmax = sDragHi;
    }
    float vspan = (vmax > vmin) ? (vmax - vmin) : 1.0f;
    int activeN = keyCount(AL);
    PruneParamSel(); // drop selected param keys that no longer exist (deletes / loads)

    // Reserve a FIXED slice for the toolbar + the block under it. That block swaps between the primary key's
    // fields (one row) and the how-to hint (three wrapped lines) as the selection changes - and the height
    // difference was enough to add or remove the WINDOW's scrollbar, which changes the content width, which
    // rescales this graph horizontally. Clicking a key then moved it, because the pixel-to-time mapping had
    // shifted under the cursor between the grab frame and the next one. A constant footer height means the
    // scrollbar can't flip, so the graph can't rescale.
    const float kFootH = ImGui::GetTextLineHeightWithSpacing() * 5.0f + 10.0f;
    float graphH = ImGui::GetContentRegionAvail().y - kFootH;
    if (graphH < 80.0f) {
        graphH = 80.0f;
    }
    ImVec2 size(ImGui::GetContentRegionAvail().x, graphH);
    if (size.x < 80.0f) {
        size.x = 80.0f;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##curvegraph", size);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // wheel zooms the graph instead of scrolling the window
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 27, 255), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 95, 255), 4.0f);
    float gx0 = p0.x + 6.0f, gx1 = p1.x - 6.0f, gy0 = p0.y + 8.0f, gy1 = p1.y - 8.0f;
    auto timeToX = [&](float t) { return gx0 + ((t - vt0) / (vt1 - vt0)) * (gx1 - gx0); };
    auto xToTime = [&](float x) {
        float u = (x - gx0) / (gx1 - gx0);
        return vt0 + std::min(std::max(u, 0.0f), 1.0f) * (vt1 - vt0);
    };
    dl->PushClipRect(p0, p1, true); // zoomed-out keys/curves must not paint past the graph frame
    auto valToY = [&](float v, float lo, float hi) {
        float sp = (hi > lo) ? (hi - lo) : 1.0f;
        float u = std::min(std::max((v - lo) / sp, 0.0f), 1.0f);
        return gy1 - u * (gy1 - gy0);
    };
    auto yToValActive = [&](float y) {
        float u = std::min(std::max((gy1 - y) / (gy1 - gy0), 0.0f), 1.0f);
        return vmin + u * vspan;
    };
    // The magnet's grid, derived from the VISIBLE span so it refines as you zoom in. Computed here, next to
    // the rest of the view maths, so the lines you SEE and the positions a drag LANDS on are the same numbers.
    float gridT = SnapTimeStep(vt1 - vt0);
    float gridV = SnapValueStep(vspan);

    for (int i = 0; i <= 4; i++) {
        float yy = gy0 + (gy1 - gy0) * i / 4.0f;
        dl->AddLine(ImVec2(gx0, yy), ImVec2(gx1, yy), IM_COL32(44, 44, 48, 255));
    }
    if (SnapEnabled() && gridT > 1e-6f && gridV > 1e-9f) { // draw what you'll snap to - a magnet you
                                                           // can't see is a magnet you can't aim
        const ImU32 gcol = IM_COL32(70, 78, 92, 255);
        for (float t = std::ceil(vt0 / gridT) * gridT; t <= vt1 + 1e-4f; t += gridT) {
            float gxx = timeToX(t);
            dl->AddLine(ImVec2(gxx, gy0), ImVec2(gxx, gy1), gcol);
        }
        for (float v = std::ceil(vmin / gridV) * gridV; v <= vmax + 1e-4f; v += gridV) {
            float gyy = valToY(v, vmin, vmax);
            dl->AddLine(ImVec2(gx0, gyy), ImVec2(gx1, gyy), gcol);
        }
    }
    char lab[24]; // Y labels are the ACTIVE channel's range (the overlays are normalized to their own ranges).
    snprintf(lab, sizeof(lab), "%g", vmax);
    dl->AddText(ImVec2(gx0 + 2.0f, gy0 - 1.0f), IM_COL32(150, 150, 155, 255), lab);
    snprintf(lab, sizeof(lab), "%g", vmin);
    dl->AddText(ImVec2(gx0 + 2.0f, gy1 - 13.0f), IM_COL32(150, 150, 155, 255), lab);

    if (showSpeed) { // behind the value curves so it reads as context, not another editable channel
        float spMax = 0.0f;
        DrawSpeedGraphInto(dl, gx0, gx1, gy0, gy1, vt0, vt1, (sSpDragKnot >= 0) ? sSpDragMax : 0.0f, &spMax);
        SpeedHandlesUI(dl, gx0, gx1, gy0, gy1, vt0, vt1, spMax, ImGui::IsItemHovered());
    } else {
        sSpHot = false;
    }

    float phx = timeToX(std::min(sPlayhead, total));
    dl->AddLine(ImVec2(phx, gy0), ImVec2(phx, gy1), IM_COL32(60, 255, 90, 150), 1.5f);

    // Draw every channel PER SEGMENT so each interpolation type is exact: step = hold + vertical drop, linear =
    // a straight line, smooth/bezier = a polyline sub-sampled by the segment's on-screen width (so curves
    // between close keyframes stay smooth instead of collapsing to a line). Each channel is scaled to its own
    // range; the active one is bright, the rest are faded overlays.
    for (int ci = 0; ci < (int)curves.size(); ci++) {
        const CurveChannel& C = curves[ci];
        bool isAct = (ci == active);
        ImU32 col = isAct ? C.col : ((C.col & 0x00FFFFFF) | 0x55000000);
        float w = isAct ? 2.0f : 1.3f;
        std::pair<float, float> cr = rangeOf(C);
        float lo = cr.first, hi = cr.second;
        if (isAct) { // the active channel shares the (possibly drag-frozen) range used for its points and drag math
            lo = vmin;
            hi = vmax;
        }
        int nk = keyCount(C);
        if (nk == 0) {
            continue;
        }
        CineParamTrack* ct = C.track;
        // Flat "hold" extensions before the first and after the last key.
        dl->AddLine(ImVec2(gx0, valToY(ct->keys[0].value, lo, hi)),
                    ImVec2(timeToX(ct->keys[0].time), valToY(ct->keys[0].value, lo, hi)), col, w);
        dl->AddLine(ImVec2(timeToX(ct->keys[nk - 1].time), valToY(ct->keys[nk - 1].value, lo, hi)),
                    ImVec2(gx1, valToY(ct->keys[nk - 1].value, lo, hi)), col, w);
        for (int i = 0; i + 1 < nk; i++) {
            const CineParamKey& a = ct->keys[i];
            const CineParamKey& b = ct->keys[i + 1];
            float xa = timeToX(a.time), xb = timeToX(b.time);
            float ya = valToY(a.value, lo, hi), yb = valToY(b.value, lo, hi);
            int mode = KeyInterp(*ct, i);
            if (mode == CINE_TRACK_STEP) {
                dl->AddLine(ImVec2(xa, ya), ImVec2(xb, ya), col, w); // hold
                dl->AddLine(ImVec2(xb, ya), ImVec2(xb, yb), col, w); // vertical drop at the next key
            } else if (mode == CINE_TRACK_LINEAR) {
                dl->AddLine(ImVec2(xa, ya), ImVec2(xb, yb), col, w);
            } else {
                int sub = (int)((xb - xa) / 4.0f);
                if (sub < 2) {
                    sub = 2;
                }
                if (sub > 64) {
                    sub = 64;
                }
                ImVec2 prev(xa, ya);
                for (int sN = 1; sN <= sub; sN++) {
                    float tt = a.time + (b.time - a.time) * (float)sN / (float)sub;
                    float vv = EvalTrackSegment(*ct, (size_t)i, tt); // segment already known - no re-search
                    ImVec2 cur(timeToX(tt), valToY(vv, lo, hi));
                    dl->AddLine(prev, cur, col, w);
                    prev = cur;
                }
            }
        }
    }
    // Selection accessors for the active channel (the shared (track,id) selection).
    auto keyIdAt = [&](int i) -> int { return AL.track->keys[i].id; };
    auto isSelIdx = [&](int i) -> bool { return IsParamKeySel(AL.track, AL.track->keys[i].id); };
    // Primary (last-selected) key index in the active channel - drives the toolbar fields.
    int primIdx = -1;
    for (int s = (int)sParamSel.size() - 1; s >= 0; s--) {
        if (sParamSel[s].track == AL.track) {
            primIdx = ParamKeyIndexById(AL.track, sParamSel[s].id);
            break;
        }
    }

    // Active channel's key points. Selected = filled gold; the primary gets a brighter ring.
    for (int i = 0; i < activeN; i++) {
        ImVec2 c(timeToX(keyTime(AL, i)), valToY(keyValue(AL, i), vmin, vmax));
        bool sel = isSelIdx(i);
        bool prim = (i == primIdx);
        dl->AddCircleFilled(c, sel ? 5.5f : 4.0f, sel ? IM_COL32(255, 220, 80, 255) : AL.col);
        dl->AddCircle(c, prim ? 7.5f : (sel ? 6.5f : 5.0f), IM_COL32(255, 255, 255, sel ? 230 : 120));
    }

    // Bezier handles: for selected parameter keys that touch a Bezier segment, draw the in/out tangent handles
    // (a line from the key to a draggable endpoint). Collected so the click handler can grab them with priority.
    struct HandleHit {
        int keyId;
        int which; // 0 = out (segment leaving this key), 1 = in (segment arriving at this key)
        ImVec2 pos;
    };
    std::vector<HandleHit> handles;
    {
        CineParamTrack* tr = AL.track;
        for (int i = 0; i < activeN; i++) {
            if (!isSelIdx(i)) {
                continue;
            }
            bool showOut = (i < activeN - 1) && KeyInterp(*tr, i) == CINE_TRACK_BEZIER;
            bool showIn = (i > 0) && KeyInterp(*tr, i - 1) == CINE_TRACK_BEZIER;
            if (!showOut && !showIn) {
                continue;
            }
            float oT, oV, iT, iV;
            GetBezierHandles(*tr, i, oT, oV, iT, iV);
            ImVec2 kp(timeToX(tr->keys[i].time), valToY(tr->keys[i].value, vmin, vmax));
            if (showOut) {
                ImVec2 hp(timeToX(tr->keys[i].time + oT), valToY(tr->keys[i].value + oV, vmin, vmax));
                dl->AddLine(kp, hp, IM_COL32(170, 180, 200, 200), 1.0f);
                dl->AddCircleFilled(hp, 3.5f, IM_COL32(120, 220, 255, 255));
                handles.push_back({ tr->keys[i].id, 0, hp });
            }
            if (showIn) {
                ImVec2 hp(timeToX(tr->keys[i].time + iT), valToY(tr->keys[i].value + iV, vmin, vmax));
                dl->AddLine(kp, hp, IM_COL32(170, 180, 200, 200), 1.0f);
                dl->AddCircleFilled(hp, 3.5f, IM_COL32(120, 220, 255, 255));
                handles.push_back({ tr->keys[i].id, 1, hp });
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x, my = io.MousePos.y;
    auto nearestKey = [&]() {
        int hit = -1;
        float best = 11.0f;
        for (int i = 0; i < activeN; i++) {
            float dxp = timeToX(keyTime(AL, i)) - mx, dyp = valToY(keyValue(AL, i), vmin, vmax) - my;
            float dd = std::sqrt(dxp * dxp + dyp * dyp);
            if (dd < best) {
                best = dd;
                hit = i;
            }
        }
        return hit;
    };

    // Drag snapshot: the start time/value of every key being moved, so a multi-key drag stays rigid even as the
    // track re-sorts under it. Keys are tracked by stable id (param) or camera id, not index.
    static std::vector<int> sCdIds;
    static std::vector<float> sCdT0, sCdV0;
    static float sCdGrabT0 = 0.0f, sCdGrabV0 = 0.0f;
    // The GRABBED key's own start position. The magnet snaps this one key onto the grid and every other key in
    // the selection moves by the same delta - snapping each key on its own would pull the group's internal
    // spacing onto the gridlines and quietly destroy the rhythm you selected it for.
    static int sCdAnchorId = -1;
    static float sCdAnchorT0 = 0.0f, sCdAnchorV0 = 0.0f;
    static bool sCdMoved = false;   // this drag has passed the dead zone; until then nothing is written
    static ImVec2 sCdGrabPx(0, 0);  // graph rect at grab, to notice the view moving under the hand
    static int sCdAxis = kAxisFree; // axis-lock latch for the key drag
    static int sHAxis = kAxisFree;  // ...and for the handle drag (separate: they can't be active at once,
                                    // but a shared latch would carry one drag's choice into the next)
    static bool sCdRipple = false;
    static bool sCdBox = false;    // rubber-band select in progress (drag started on empty graph space)
    static ImVec2 sCdBoxA(0, 0);   // its anchor corner, in screen pixels
    static bool sCdBoxAdd = false; // Ctrl was held: add to the selection instead of replacing it
    static bool sHDrag = false;    // dragging a Bezier handle endpoint
    static bool sHMoved = false;   // ...and whether it has passed the dead zone (same rule as the key drag)
    static int sHKeyId = -1;       // param key id whose handle is being dragged
    static int sHWhich = 0;        // 0 = out handle, 1 = in handle
    // Where the grab landed relative to the thing grabbed. Everything draggable moves by the cursor's DELTA
    // from here, never to the cursor's absolute position: you can hit a 4px dot anywhere inside its ~10px
    // catch radius, and picking something up must never be the same as changing it.
    static float sHGrabDT = 0.0f, sHGrabDV = 0.0f;

    if (ImGui::IsItemActivated() && !sSpHot) { // a hovered/dragged Speed handle owns the click
        // A Bezier handle grab takes priority over selecting/moving keys.
        int hKey = -1, hWhich = 0;
        float hBest = 10.0f;
        for (const HandleHit& h : handles) {
            float dxp = h.pos.x - mx, dyp = h.pos.y - my;
            float dd = std::sqrt(dxp * dxp + dyp * dyp);
            if (dd < hBest) {
                hBest = dd;
                hKey = h.keyId;
                hWhich = h.which;
            }
        }
        if (hKey >= 0) {
            sHDrag = true;
            sHKeyId = hKey;
            sHWhich = hWhich;
            {
                int bi = ParamKeyIndexById(AL.track, hKey);
                if (bi >= 0) {
                    const CineParamKey& gk = AL.track->keys[bi];
                    float hT = (hWhich == 0) ? gk.hOutT : gk.hInT;
                    float hV = (hWhich == 0) ? gk.hOutV : gk.hInV;
                    sHGrabDT = (gk.time + hT) - xToTime(mx); // offset from cursor to the handle's own position
                    sHGrabDV = (gk.value + hV) - yToValActive(my);
                }
            }
            if (io.KeyAlt) { // Alt breaks the pair apart; without it the two handles stay mirrored
                int bi = ParamKeyIndexById(AL.track, hKey);
                if (bi >= 0) {
                    AL.track->keys[bi].brokenHandles = 1;
                }
            }
            sDragLo = vmin; // freeze the value range so the handle's value-to-pixel scale stays constant
            sDragHi = vmax;
            sDragRange = true;
            // Breaking the pair IS an edit even if the cursor never moves, so that case takes its undo step
            // now; otherwise the undo waits for the dead zone, like every other drag here.
            sHMoved = io.KeyAlt;
            if (sHMoved) {
                PushUndo();
            }
            drag = -1;
        } else {
            int hit = nearestKey();
            if (hit < 0) {
                // Empty space starts a rubber band. It only becomes a selection once the cursor has actually
                // travelled; a press that never moves is still a click, and a click on empty space clears
                // (that decision is made on RELEASE, below, so a band that starts on empty space and lands on
                // nothing behaves the same as the click it looks like).
                sCdBox = true;
                sCdBoxA = ImVec2(mx, my);
                sCdBoxAdd = io.KeyCtrl;
                drag = -1;
            } else if (io.KeyCtrl) { // Ctrl-click toggles this key in/out of the selection, no drag (matches the
                                     // timeline)
                ParamSelToggle(AL.track, keyIdAt(hit));
                drag = -1;
            } else {
                if (!isSelIdx(hit)) { // clicking an unselected point selects only it; a selected one keeps the group
                    ParamSelOnly(AL.track, keyIdAt(hit));
                }
                // Begin a drag of the whole selection (Shift ripples this key + all later ones). Nothing is
                // written and no undo step is pushed until the cursor passes the dead zone below: picking a
                // key up must never be the same as editing it. With the magnet on that is not a nicety -
                // without it, merely clicking a key would snap it onto the nearest gridline.
                sCdMoved = false;
                sCdGrabPx = ImVec2(gx0, gy0);
                sDragLo = vmin;
                sDragHi = vmax;
                sDragRange = true;
                sCdRipple = io.KeyShift;
                sCdGrabT0 = xToTime(mx); // the CURSOR at grab, not the key: the first frame's delta is zero
                sCdGrabV0 = yToValActive(my);
                sCdAnchorId = keyIdAt(hit); // the KEY the grid snaps; the rest ride its delta
                sCdAnchorT0 = keyTime(AL, hit);
                sCdAnchorV0 = keyValue(AL, hit);
                sCdIds.clear();
                sCdT0.clear();
                sCdV0.clear();
                for (int i = 0; i < activeN; i++) {
                    bool take = sCdRipple ? (keyTime(AL, i) >= sCdGrabT0 - 1e-4f) : isSelIdx(i);
                    if (take) {
                        sCdIds.push_back(keyIdAt(i));
                        sCdT0.push_back(keyTime(AL, i));
                        sCdV0.push_back(keyValue(AL, i));
                    }
                }
                drag = hit;
            }
        }
    }
    // Bezier handle drag: move the grabbed in/out handle in (time, value), clamped so time stays inside the
    // adjacent segment (keeps the curve a proper function of time).
    if (ImGui::IsItemActive() && sHDrag && !sHMoved) {
        ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        if (std::fabs(dd.x) + std::fabs(dd.y) >= 2.5f) {
            PushUndo();
            sHMoved = true;
        }
    }
    if (ImGui::IsItemActive() && sHDrag && sHMoved) {
        int idx = ParamKeyIndexById(AL.track, sHKeyId);
        if (idx >= 0) {
            CineParamKey& k = AL.track->keys[idx];
            if (!k.hasHandles) {
                SeedBezierHandles(k, *AL.track, idx);
            }
            // Handles live in (seconds, value) but are SEEN in pixels, and the two axes are scaled nothing
            // like each other - a roll track spans hundreds of degrees across a handful of seconds. Every
            // length and direction below is therefore computed in pixels and converted back, which is the
            // only way an arm that looks 30px long stays 30px long when it swings.
            float pxT = (gx1 - gx0) / std::max(vt1 - vt0, 1e-5f); // pixels per second
            float pxV = (gy1 - gy0) / std::max(vspan, 1e-5f);     // pixels per value unit
            // Shorten by SCALING the whole arm, never by truncating its time reach: cutting one component
            // alone rotates the handle, and the handle's angle is the curve's shape. This is what made a
            // near-flat handle fling itself sideways as the segment length changed.
            auto clampArm = [](float& dt, float& dv, float maxT) {
                if (maxT > 1e-6f && dt > maxT) {
                    dv *= maxT / dt;
                    dt = maxT;
                }
            };
            float segPrev = (idx > 0) ? (k.time - AL.track->keys[idx - 1].time) : 1e9f;
            float segNext = (idx < activeN - 1) ? (AL.track->keys[idx + 1].time - k.time) : 1e9f;
            float nt = (xToTime(mx) + sHGrabDT) - k.time;
            float nv = (yToValActive(my) + sHGrabDV) - k.value;
            // Handles honour the axis lock but NOT the magnet: a handle is a relative arm, so quantizing its
            // tip to an absolute grid would change both its length and its angle - and the angle is the shape.
            ImVec2 hDragPx = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            int hAxis = AxisLockUpdate(sHAxis, hDragPx.x, hDragPx.y);
            if (hAxis == kAxisTime) {
                nv = (sHWhich == 0) ? k.hOutV : k.hInV;
            } else if (hAxis == kAxisValue) {
                nt = (sHWhich == 0) ? k.hOutT : k.hInT;
            }
            if (sHWhich == 0) { // out handle: reaches forward, toward the next key
                float dt = std::max(nt, 0.0f), dv = nv;
                clampArm(dt, dv, segNext);
                k.hOutT = dt;
                k.hOutV = dv;
            } else { // in handle: reaches back, toward the previous key (stored negative)
                float dt = std::max(-nt, 0.0f), dv = -nv;
                clampArm(dt, dv, segPrev);
                k.hInT = -dt;
                k.hInV = -dv;
            }
            // Unless the pair is broken (Alt-drag), the opposite handle mirrors this one's DIRECTION while
            // keeping its own arm length - so the curve passes through the key smoothly, which is what you
            // want almost every time. Alt while grabbing breaks the pair and each side moves alone.
            if (!k.brokenHandles) {
                float dt = (sHWhich == 0) ? k.hOutT : -k.hInT;
                float dv = (sHWhich == 0) ? k.hOutV : -k.hInV;
                float dxp = dt * pxT, dyp = dv * pxV;
                float lenPx = std::sqrt(dxp * dxp + dyp * dyp);
                if (lenPx > 1e-4f) {
                    float oT = (sHWhich == 0) ? -k.hInT : k.hOutT;
                    float oV = (sHWhich == 0) ? -k.hInV : k.hOutV;
                    float oxp = oT * pxT, oyp = oV * pxV;
                    float oLenPx = std::sqrt(oxp * oxp + oyp * oyp);
                    if (oLenPx < 1e-4f) {
                        oLenPx = lenPx;
                    }
                    float mt = (dxp / lenPx * oLenPx) / pxT; // opposite arm: this direction, its own length
                    float mv = (dyp / lenPx * oLenPx) / pxV;
                    if (sHWhich == 0) { // mirror onto the in side
                        clampArm(mt, mv, segPrev);
                        k.hInT = -mt;
                        k.hInV = -mv;
                    } else { // mirror onto the out side
                        clampArm(mt, mv, segNext);
                        k.hOutT = mt;
                        k.hOutV = mv;
                    }
                }
            }
        }
    }
    // A click that never travelled is a selection, not an edit: nothing is written and no undo step is pushed
    // until the cursor leaves this dead zone. With the magnet on that is not a nicety - without it, merely
    // clicking a key would snap it onto the nearest gridline.
    if (ImGui::IsItemActive() && drag >= 0 && !sCdMoved) {
        ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        if (std::fabs(dd.x) + std::fabs(dd.y) >= 2.5f) {
            PushUndo();
            sCdMoved = true;
        }
    }
    if (ImGui::IsItemActive() && drag >= 0 && sCdMoved) {
        // If the graph itself moved or resized since the grab (a section above expanded, the scrollbar
        // flipped), the cursor is over a different time than it was. Re-anchor rather than reading the view's
        // movement as the hand's.
        if (std::fabs(sCdGrabPx.x - gx0) > 0.5f || std::fabs(sCdGrabPx.y - gy0) > 0.5f) {
            sCdGrabPx = ImVec2(gx0, gy0);
            sCdGrabT0 = xToTime(mx);
            sCdGrabV0 = yToValActive(my);
            for (size_t k = 0; k < sCdIds.size(); k++) {
                int idx = ParamKeyIndexById(AL.track, sCdIds[k]);
                if (idx >= 0) {
                    sCdT0[k] = AL.track->keys[idx].time;
                    sCdV0[k] = AL.track->keys[idx].value;
                    if (sCdIds[k] == sCdAnchorId) {
                        sCdAnchorT0 = sCdT0[k];
                        sCdAnchorV0 = sCdV0[k];
                    }
                }
            }
        }
        float dtime = xToTime(mx) - sCdGrabT0;
        float dval = yToValActive(my) - sCdGrabV0;
        // Precision aids, in this order: the lock removes an axis, then the magnet quantizes what's left.
        // (Snapping first and locking after would let the magnet move an axis you asked to hold still.)
        ImVec2 dragPx = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        int axis = AxisLockUpdate(sCdAxis, dragPx.x, dragPx.y);
        if (axis == kAxisTime) {
            dval = 0.0f;
        } else if (axis == kAxisValue) {
            dtime = 0.0f;
        }
        if (SnapEnabled()) {
            if (axis != kAxisValue) {
                dtime = SnapTo(sCdAnchorT0 + dtime, gridT) - sCdAnchorT0;
            }
            if (axis != kAxisTime) {
                dval = SnapTo(sCdAnchorV0 + dval, gridV) - sCdAnchorV0;
            }
        }
        for (size_t k = 0; k < sCdIds.size(); k++) {
            int idx = ParamKeyIndexById(AL.track, sCdIds[k]);
            if (idx < 0) {
                continue;
            }
            AL.track->keys[idx].time = std::min(std::max(sCdT0[k] + dtime, 0.0f), total);
            if (!sCdRipple) { // ripple moves time only; a plain move also carries the value
                AL.track->keys[idx].value = std::min(std::max(sCdV0[k] + dval, sDragLo), sDragHi);
            }
        }
        std::sort(AL.track->keys.begin(), AL.track->keys.end(),
                  [](const CineParamKey& a, const CineParamKey& b) { return a.time < b.time; });
        // Live readout at the cursor - the grabbed key's exact numbers, no squinting at the graph.
        const char* cons = (axis == kAxisTime)    ? "   [locked: time]"
                           : (axis == kAxisValue) ? "   [locked: value]"
                           : SnapEnabled()        ? "   [snap]"
                                                  : "";
        if (sCdRipple) {
            CineTooltip("ripple  %+.2fs%s", dtime, cons);
        } else {
            CineTooltip("t %.2fs   v %.2f%s", std::min(std::max(sCdAnchorT0 + dtime, 0.0f), total),
                        std::min(std::max(sCdAnchorV0 + dval, sDragLo), sDragHi), cons);
        }
        // Show the axis you're held to, straight through the key being dragged.
        if (axis != kAxisFree) {
            ImVec2 kp(timeToX(std::min(std::max(sCdAnchorT0 + dtime, 0.0f), total)),
                      valToY(std::min(std::max(sCdAnchorV0 + dval, sDragLo), sDragHi), vmin, vmax));
            ImU32 axCol = IM_COL32(255, 220, 80, 160);
            if (axis == kAxisTime) {
                dl->AddLine(ImVec2(gx0, kp.y), ImVec2(gx1, kp.y), axCol, 1.0f);
            } else {
                dl->AddLine(ImVec2(kp.x, gy0), ImVec2(kp.x, gy1), axCol, 1.0f);
            }
        }
    }
    // Rubber band: drag on empty graph space to select every key of the ACTIVE channel inside the rectangle
    // (that is the only channel whose points are drawn as hittable dots, so it is the only one you could be
    // aiming at). Ctrl adds to the selection instead of replacing it.
    if (ImGui::IsItemActive() && sCdBox) {
        ImVec2 a(std::min(sCdBoxA.x, mx), std::min(sCdBoxA.y, my));
        ImVec2 b(std::max(sCdBoxA.x, mx), std::max(sCdBoxA.y, my));
        if (b.x - a.x + (b.y - a.y) >= 2.5f) {
            dl->AddRectFilled(a, b, IM_COL32(255, 220, 80, 30));
            dl->AddRect(a, b, IM_COL32(255, 220, 80, 160));
            int inside = 0;
            for (int i = 0; i < activeN; i++) {
                float px = timeToX(keyTime(AL, i)), py = valToY(keyValue(AL, i), vmin, vmax);
                if (px >= a.x && px <= b.x && py >= a.y && py <= b.y) {
                    inside++;
                }
            }
            CineTooltip("%d key%s", inside, inside == 1 ? "" : "s");
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (sCdBox) {
            ImVec2 a(std::min(sCdBoxA.x, mx), std::min(sCdBoxA.y, my));
            ImVec2 b(std::max(sCdBoxA.x, mx), std::max(sCdBoxA.y, my));
            bool dragged = (b.x - a.x) + (b.y - a.y) >= 2.5f;
            if (!dragged) {
                // Never travelled: this was a click on empty space, which clears (unless Ctrl was held, which
                // has always meant "leave my selection alone").
                if (!sCdBoxAdd) {
                    ParamSelClear();
                    sSelection.clear();
                    sSelectedId = -1;
                }
            } else {
                if (!sCdBoxAdd) {
                    ParamSelClear();
                }
                for (int i = 0; i < activeN; i++) {
                    float px = timeToX(keyTime(AL, i)), py = valToY(keyValue(AL, i), vmin, vmax);
                    if (px >= a.x && px <= b.x && py >= a.y && py <= b.y && !isSelIdx(i)) {
                        ParamSelToggle(AL.track, keyIdAt(i));
                    }
                }
            }
            sCdBox = false;
        }
        drag = -1;
        sHDrag = false;
        sHMoved = false;
        sDragRange = false;
        sCdMoved = false;
        sCdAnchorId = -1;
        sCdAxis = kAxisFree; // a Q held across two drags must not carry the first one's axis into the second
        sHAxis = kAxisFree;
    }
    // Time-axis zoom (mouse wheel, centered on the cursor) and pan (middle-drag). Zooming right out snaps back
    // to the full timeline.
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f && drag < 0 && !sHDrag) {
        float mt = xToTime(mx);
        float fz = std::pow(0.8f, io.MouseWheel); // wheel up = zoom in
        float nt0 = mt - (mt - vt0) * fz;
        float nt1 = mt + (vt1 - mt) * fz;
        float minSpan = total * 0.02f;
        if (nt1 - nt0 < minSpan) {
            float c = (nt0 + nt1) * 0.5f;
            nt0 = c - minSpan * 0.5f;
            nt1 = c + minSpan * 0.5f;
        }
        nt0 = std::max(nt0, 0.0f);
        nt1 = std::min(nt1, total);
        if (nt1 - nt0 >= total * 0.999f) {
            sCvT0 = 0.0f;
            sCvT1 = -1.0f; // back to the full timeline
        } else {
            sCvT0 = nt0;
            sCvT1 = nt1;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Middle) && sCvT1 > sCvT0) {
        float dt = -io.MouseDelta.x * (vt1 - vt0) / std::max(gx1 - gx0, 1.0f);
        dt = std::min(std::max(dt, -vt0), total - vt1); // keep the window inside the timeline
        sCvT0 = vt0 + dt;
        sCvT1 = vt1 + dt;
    }
    // Double-click empty graph space drops a key right there ("empty" also means clear of Bezier handle
    // endpoints - those sit away from their key, and double-clicking one must stay a handle grab).
    auto nearAnyHandle = [&]() {
        for (const HandleHit& h : handles) {
            float dxp = h.pos.x - mx, dyp = h.pos.y - my;
            if (dxp * dxp + dyp * dyp < 100.0f) {
                return true;
            }
        }
        return false;
    };
    if (ImGui::IsItemHovered() && !sSpHot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearestKey() < 0 &&
        !nearAnyHandle()) {
        PushUndo();
        float nt = std::min(std::max(xToTime(mx), 0.0f), total);
        float nv = std::min(std::max(yToValActive(my), vmin), vmax);
        if (SnapEnabled()) { // a key you place with the magnet on should land on the grid, not near it
            nt = std::min(std::max(SnapTo(nt, gridT), 0.0f), total);
            nv = std::min(std::max(SnapTo(nv, gridV), vmin), vmax);
        }
        ParamSelOnly(AL.track, TrackAddKey(*AL.track, nt, nv));
    }
    // Right-click deletes: the whole selection when you right-click a selected point, otherwise just the one
    // under the cursor. primIdx was computed BEFORE this erase, so it must be invalidated - the toolbar below
    // would otherwise index past the shrunken key list (this was a crash).
    if (ImGui::IsItemHovered() && !sSpHot && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hit = nearestKey();
        if (hit >= 0) {
            PushUndo();
            if (isSelIdx(hit)) {
                DeleteSelectedParamKeys();
            } else {
                AL.track->keys.erase(AL.track->keys.begin() + hit);
                PruneParamSel();
            }
            primIdx = -1;
        }
    }
    dl->PopClipRect();

    // Toolbar + primary-key row, inside a child of FIXED height. The block below swaps between the key's
    // fields and the how-to hint as the selection changes; letting it change the window's total height was
    // what flipped the scrollbar and rescaled the graph mid-click (see kFootH above).
    ImGui::BeginChild("##cvfoot", ImVec2(0.0f, kFootH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (ImGui::SmallButton("Add key at playhead")) {
            float v;
            if (!EvalParamTrack(*AL.track, sPlayhead, v)) {
                v = (vmin + vmax) * 0.5f;
            }
            PushUndo();
            ParamSelOnly(AL.track, TrackAddKey(*AL.track, sPlayhead, v)); // select the new key
        }
        ImGui::SameLine();
        SnapToolbarUI("cv", gridT, gridV);
        ImGui::SameLine();
        if (sCvT1 > sCvT0) {
            if (ImGui::SmallButton("Fit##cvzoom")) {
                sCvT0 = 0.0f;
                sCvT1 = -1.0f;
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("Reset the time zoom to the whole timeline (wheel = zoom, middle-drag = pan).");
            }
            ImGui::SameLine();
        }
        int nsel = ParamSelCountInTrack(AL.track);
        if (nsel > 1) {
            ImGui::Text("%d selected", nsel);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete sel")) {
                PushUndo();
                DeleteSelectedParamKeys();
                primIdx = -1; // computed before the erase - stale (same crash as the right-click delete)
            }
            ImGui::SameLine();
        }
    }
    // Bound against the LIVE key list, not the activeN captured before this frame's deletes.
    if (primIdx >= 0 && primIdx < (int)AL.track->keys.size()) {
        {
            CineParamTrack* tr = AL.track;
            const char* im[] = { "Step", "Linear", "Smooth", "Bezier" };
            int mode = (tr->keys[primIdx].interp < 0) ? tr->interp : tr->keys[primIdx].interp;
            // Set a key's interp, seeding default handles when switching to Bezier so the shape doesn't jump.
            auto applyMode = [&](int idx, int m) {
                tr->keys[idx].interp = m;
                if (m == CINE_TRACK_BEZIER && !tr->keys[idx].hasHandles) {
                    SeedBezierHandles(tr->keys[idx], *tr, idx);
                }
            };
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##ckinterp", &mode, im, 4)) {
                PushUndo(); // interp changes are undoable like any other key edit
                // apply the per-key spline type to every selected key in this track (or just the primary)
                if (ParamSelCountInTrack(tr) > 1) {
                    for (const ParamKeyRef& r : sParamSel) {
                        if (r.track == tr) {
                            int idx = ParamKeyIndexById(tr, r.id);
                            if (idx >= 0) {
                                applyMode(idx, mode);
                            }
                        }
                    }
                } else {
                    applyMode(primIdx, mode);
                }
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("Spline type for the segment after this key: Step / Linear / Smooth / Bezier. "
                            "Bezier shows draggable tangent handles on the selected key. Applies to all "
                            "selected keys in this channel.");
            }
            ImGui::SameLine();
            float kt = tr->keys[primIdx].time, kv = tr->keys[primIdx].value;
            float lo = (primIdx > 0) ? tr->keys[primIdx - 1].time + 1e-3f : 0.0f;
            float hi = (primIdx < (int)tr->keys.size() - 1) ? tr->keys[primIdx + 1].time - 1e-3f : total;
            ImGui::SetNextItemWidth(70.0f);
            bool ktCh = ImGui::InputFloat("t##ck", &kt, 0.0f, 0.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo(); // snapshot pre-edit so the whole typed edit is one undo step (inspector convention)
            }
            if (ktCh) {
                tr->keys[primIdx].time = std::min(std::max(kt, lo), hi);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            bool kvCh = ImGui::InputFloat("val##ck", &kv, 0.0f, 0.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (kvCh) {
                tr->keys[primIdx].value = std::min(std::max(kv, vmin), vmax);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete##ck")) {
                PushUndo();
                tr->keys.erase(tr->keys.begin() + primIdx);
                PruneParamSel();
            }
        }
    } else {
        ImGui::PushTextWrapPos(0.0f);
        CineHint("drag = move, Shift+drag = ripple, Ctrl+click = multi, drag empty space = box-select.\n"
                 "double-click / right-click = add / delete.  wheel = zoom, middle-drag = pan.\n"
                 "Hold Q to lock an axis; Snap lands drags on the grid.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

// Keyboard shortcuts (active while the editor window - full or bar - is focused and not typing into a field).
static void HandleEditorShortcuts() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    auto goTo = [&](float t) {
        sPlayhead = std::min(std::max(t, 0.0f), EffectiveTotal());
        sPreview = true;
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
    };
    auto selectAdjacent = [&](int dir) {
        if (sIds.empty()) {
            return;
        }
        int idx = SelectedIndex();
        idx = (idx < 0) ? 0 : std::min(std::max(idx + dir, 0), (int)sIds.size() - 1);
        SelectOnly(sIds[idx]);
        goTo(sKeyframes[idx].time); // jump the playhead to it so the camera frames the selection
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        TogglePlay();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Comma)) {
        goTo(sPlayhead - kTickSeconds);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Period)) {
        goTo(sPlayhead + kTickSeconds);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
        selectAdjacent(-1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
        selectAdjacent(1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_K) && FreeCamEnabled()) {
        AddKeyframe();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        PruneParamSel();          // stale refs (keys removed while the curve editor was collapsed) must not eat the Del
        if (!sParamSel.empty()) { // parameter keys first: if any are selected in the curve editor, Del means those
            PushUndo();
            DeleteSelectedParamKeys();
        } else if (SelectedIndex() >= 0) {
            DeleteSelected();
        }
    }
    // (No Esc shortcut: Esc is the game's menu key, so it can't double as clear-selection. Clicking empty
    // timeline/graph space clears instead.)
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) { // select every camera keyframe (group drags/ripples)
        sSelection.assign(sIds.begin(), sIds.end());
        if (!sIds.empty() && SelectedIndex() < 0) {
            sSelectedId = sIds[0];
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        Undo();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        Redo();
    }
}

// The compact "shooting bar": one slim transport strip replacing the whole editor while flying/framing on a
// single monitor. The world overlay (spline, markers, gizmos) and every keyboard shortcut stay active - this is
// the mode you LIVE in; expand back to the full editor for curve / parameter / file work.
static void DrawShootingBar() {
    if (ImGui::SmallButton("Expand")) {
        ExitBarMode();
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Back to the full editor (timeline, curves, parameters, files).");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sKeyframes.size() < 2);
    if (ImGui::SmallButton(sPlaying ? "Stop" : "Play")) {
        TogglePlay();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    float total = EffectiveTotal();
    auto scrubTo = [&](float t) {
        sPlayhead = std::min(std::max(t, 0.0f), total);
        sPreview = true;
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
    };
    if (ImGui::SmallButton("|<")) {
        float best = 0.0f;
        for (const CineKeyframe& k : sKeyframes) {
            if (k.time < sPlayhead - 1e-3f && k.time > best) {
                best = k.time;
            }
        }
        scrubTo(best);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 250.0f, 120.0f));
    float ph = sPlayhead;
    if (ImGui::SliderFloat("##barscrub", &ph, 0.0f, std::max(total, 0.01f), "%.2fs",
                           ImGuiSliderFlags_NoRoundToFormat)) {
        scrubTo(ph);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">|")) {
        float best = total;
        for (const CineKeyframe& k : sKeyframes) {
            if (k.time > sPlayhead + 1e-3f && k.time < best) {
                best = k.time;
            }
        }
        scrubTo(best);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!FreeCamEnabled());
    if (ImGui::SmallButton("+KF")) {
        AddKeyframe();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        CineTooltip("Add a keyframe at the current camera pose (K).");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(SelectedIndex() < 0 || !FreeCamEnabled());
    if (ImGui::SmallButton("Upd")) {
        UpdateSelected();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        CineTooltip("Update the selected keyframe to the current camera pose.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sUndo.empty());
    if (ImGui::SmallButton("Undo")) {
        Undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImVec4 col = sPlaying ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                          : (sPreview ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextColored(col, "%d kf", (int)sKeyframes.size());
}

void CinematicCamPathWindow::DrawElement() {
    double cinePerfT0 = sPerfOn ? CineNowMs() : 0.0; // perf probe: time the whole editor draw (see CinePerf notes)
    if (sPerfOn) {
        // Draw heartbeat: this only runs when ImGui::Begin returned true. Measure how long the window went WITHOUT
        // redrawing (i.e. how long Begin was returning false / Draw was skipped) - that is the visual "freeze".
        double now = CineNowMs();
        double stale = (sLastDrawTimeMs > 0.0) ? now - sLastDrawTimeMs : 0.0;
        if (stale > 250.0) { // resumed after >0.25s without a redraw: a real freeze, now ended
            if (stale > sWorstDrawStaleMs) {
                sWorstDrawStaleMs = stale;
            }
            SPDLOG_WARN("[CineDraw] window resumed after {:.0f}ms without redraw ({} frames skipped) - DrawElement/"
                        "Begin was skipped while UpdateElement kept running (so the rest of the UI was alive)",
                        stale, (long long)(sUpdateCalls - sLastDrawAtUpdate));
        }
        sLastDrawTimeMs = now;
        sLastDrawAtUpdate = sUpdateCalls;
    }
    sDrawCalls++;

    // Shooting bar: remember the state to return to while in the full editor, and render the bar instead of
    // everything else when collapsed. The capture must NOT run during the expand transition (sBarSizeFrames /
    // sPendingDock still active) - on the first expanded frame the window still has the bar's size, and saving
    // that would make the restore a no-op (the window would stay bar-sized forever).
    if (!sBarMode && sBarSizeFrames == 0 && sPendingDock == 0) {
        sSavedDockId = (unsigned int)ImGui::GetWindowDockID();
        if (!ImGui::IsWindowDocked()) {
            ImVec2 ws = ImGui::GetWindowSize();
            sSavedSize[0] = ws.x;
            sSavedSize[1] = ws.y;
        }
    }
    if (sBarMode) {
        if (sBarSizeFrames > 0) { // shrink to a slim strip (width fixed, height auto-fit)
            ImGui::SetWindowSize(ImVec2(660.0f, 0.0f));
            sBarSizeFrames--;
        }
        DrawShootingBar();
        if (sShowPath) { // the world overlay + gizmos are the whole point of shooting mode
            HandleOverlayInput();
            double ovT0 = sPerfOn ? CineNowMs() : 0.0;
            DrawWorldOverlay();
            if (sPerfOn) {
                sPerfOverlayMs = CineNowMs() - ovT0;
            }
        } else if (sPerfOn) {
            sPerfOverlayMs = 0.0;
        }
        HandleEditorShortcuts();
        if (sPerfOn) {
            sPerfDrawMs = CineNowMs() - cinePerfT0;
        }
        return;
    }
    if (sBarSizeFrames > 0) { // just expanded: restore the saved size unless a dock took over the sizing
        // (re-docking is attempted in UpdateElement; if the old dock node no longer exists - it is destroyed
        // when its last window undocks - the window stays floating and gets its saved floating size back).
        if (!ImGui::IsWindowDocked()) {
            ImGui::SetWindowSize(ImVec2(sSavedSize[0], sSavedSize[1]));
        }
        sBarSizeFrames--;
    }

    // Status strip: always shows the current mode at a glance.
    {
        char st[96];
        ImVec4 col;
        if (sRecording) {
            snprintf(st, sizeof(st), "REC  %.1fs  (%d keyframes)", sRecordTime, (int)sKeyframes.size());
            col = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        } else if (sPlaying) {
            snprintf(st, sizeof(st), "PLAYING  %.2f / %.2fs%s", sPlayhead, EffectiveTotal(),
                     sLoop ? (sLoopMode == 1 ? "  (ping-pong)" : "  (loop)") : "");
            col = ImVec4(0.4f, 1.0f, 0.5f, 1.0f);
        } else if (sPreview) {
            snprintf(st, sizeof(st), "PREVIEW  %.2fs", sPlayhead);
            col = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        } else if (FreeCamEnabled()) {
            int s = SelectedIndex();
            if (s >= 0) {
                snprintf(st, sizeof(st), "FREE CAMERA  -  editing keyframe %d / %d", s + 1, (int)sKeyframes.size());
            } else {
                snprintf(st, sizeof(st), "FREE CAMERA  -  %d keyframes", (int)sKeyframes.size());
            }
            col = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
        } else {
            snprintf(st, sizeof(st), "IDLE  -  enable the free camera to begin");
            col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        }
        ImGui::TextColored(col, "%s", st);
    }
    if (ImGui::CollapsingHeader("Help / shortcuts")) {
        ImGui::TextWrapped("Fly the free camera to a shot and Add Keyframe (or Record a live flight). Build a few, "
                           "then Play to glide through them. Click a marker in the world or on the timeline to "
                           "select it; drag to reposition / retime.");
        ImGui::BulletText("Sticks: left = move, right = look. Buttons (rebindable in Dev Tools > Cinematic Cam):");
        ImGui::Indent();
        ImGui::TextUnformatted("Boost = Modifier 2, Precision = L, Ascend = R, Descend = Z, FOV = D-pad "
                               "up/down, Roll = D-pad left/right, Toggle = Modifier 1.");
        CineHint("Modifier 1 and 2 are yours to assign: Settings > Controller Configuration > your port > "
                 "Modifier Buttons. Map M1 to Select/Back and M2 to your right bumper.");
        ImGui::Unindent();
        ImGui::BulletText("Timeline: drag a marker to move; Ctrl+click = multi-select; Ctrl+Shift+click = "
                          "select range; Shift+drag = ripple (push this + later); Alt+drag a selection's end "
                          "= compress/expand it. -/+/Fit zoom the ruler.");
        ImGui::BulletText("Curve editor: drag a point to retime + revalue it; double-click empty space to add "
                          "one, right-click to delete. Ctrl+click = multi-select, Shift+drag = ripple. Wheel "
                          "zooms time, middle-drag pans.");
        ImGui::BulletText("Precision (both graphs): hold Q while dragging to lock the drag to one axis, and "
                          "Snap to land every drag on the grid (the grid refines as you zoom in).");
        ImGui::BulletText("Keyboard (this window focused): Space = play/stop, , / . = step a tick, "
                          "[ / ] = prev/next keyframe, K = add keyframe, Del = delete (curve-editor keys first), "
                          "Ctrl+A = select all, Ctrl+Z / Ctrl+Y = undo/redo.");
    }
    ImGui::Separator();

    bool enabled = FreeCamEnabled();
    if (ImGui::Checkbox("Enable Free Camera", &enabled)) {
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), enabled);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Detached free camera. Left stick: move; right stick: look; plus rebindable "
                    "ascend/descend/boost/FOV/roll. Bindings are in Dev Tools > Cinematic Cam > Controls.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show path in world", &sShowPath);
    if (ImGui::IsItemHovered()) {
        CineTooltip("Draw the spline, keyframe markers, and the editing gizmo over the game view.");
    }
    ImGui::SameLine();
    {
        bool diag = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.PerfDiag"), 0) != 0;
        if (ImGui::Checkbox("Perf diag", &diag)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.PerfDiag"), diag);
            CVarSave();
            sPerfSpikeCount = 0;
            sPerfPeakMs = 0.0;
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Diagnose the freeze: shows a live frame-time HUD (top-right) splitting each frame into "
                        "our editor draw vs engine/GPU, and logs every hitch over 60 ms to the SoH log "
                        "(soh.log) with the current state.");
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Minimize to bar")) {
        EnterBarMode(false);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Collapse the editor to a slim transport bar so the game keeps the screen. World "
                    "gizmos and keyboard shortcuts stay active.");
    }
    ImGui::SameLine();
    CineHint("|");
    ImGui::SameLine();
    {
        bool autoBar = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.AutoBarOnPlay"), 1) != 0;
        if (ImGui::Checkbox("Bar on Play", &autoBar)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.AutoBarOnPlay"), autoBar);
            CVarSave(); // raw CVarSet does NOT persist on its own - without this the setting resets on quit/crash
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Minimize to the bar automatically while playing (judge the take on a clean frame) "
                        "and expand back when playback stops.");
        }
        ImGui::SameLine();
        float alpha = CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.BgAlpha"), 1.0f);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::SliderFloat("##cineBgAlpha", &alpha, 0.35f, 1.0f, "opacity %.2f")) {
            CVarSetFloat(CVAR_ENHANCEMENT("CinematicCam.BgAlpha"), alpha);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CVarSave(); // persist once when the drag ends (raw CVarSet alone is lost on quit)
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Editor background opacity - lower it so the game reads through the window. "
                        "Most effective while the window floats over the game view.");
        }
    }

    if (sShowPath) {
        HandleOverlayInput();
        double ovT0 = sPerfOn ? CineNowMs() : 0.0;
        DrawWorldOverlay();
        if (sPerfOn) {
            sPerfOverlayMs = CineNowMs() - ovT0;
        }
    } else if (sPerfOn) {
        sPerfOverlayMs = 0.0;
    }

    HandleEditorShortcuts();

    // Phase 1 layout: a left controls column, a right column (inspector + playback + library), and the dope-sheet
    // timeline pinned across the bottom. Sized from the remaining space so the timeline always stays in view.
    ImVec2 cineAvail = ImGui::GetContentRegionAvail();
    int cineLanes = 1; // camera lane + one per enabled automation track
    for (const TrackDef& d : AllTrackDefs()) {
        if (d.track->enabled) {
            cineLanes++;
        }
    }
    static bool sCurveEditorOpen = false; // remembered from last frame to size the bottom region
    // Reserve the bottom region for everything it actually holds - the dope-sheet timeline, the separator + the
    // "Curve editor" collapsing header, and (when open) the curve editor itself - then give the top the remainder.
    // The earlier estimate under-budgeted the header, so the bottom child scrolled and clipped it behind the edge.
    float cineTimelineH = (15.0f + 22.0f * cineLanes + 6.0f) + 80.0f; // dope-sheet canvas + toolbar + hint row
    float cineHeaderH = 34.0f;                                        // separator + "Curve editor" header + spacing
    // With no curve to show (no continuous track enabled) the open editor is just the camera key-row + a hint,
    // so only reserve that much instead of leaving a large empty region.
    bool cineHaveCurves = false;
    for (const TrackDef& d : AllTrackDefs()) {
        if (d.continuous && d.track->enabled) {
            cineHaveCurves = true;
            break;
        }
    }
    // key row + legend + adaptive graph + toolbar (84 = row + hint when there is no curve). With no curve but
    // the speed graph on, the standalone graph needs its own slice budgeted or the bottom region scrolls and
    // the timeline and the graph can't be on screen at the same time.
    bool cineSpeedGraph = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SpeedGraph"), 0) != 0;
    float cineCurveH = cineHaveCurves ? 280.0f : (cineSpeedGraph ? 84.0f + 158.0f : 84.0f);
    float cineWantBottom = cineTimelineH + cineHeaderH + (sCurveEditorOpen ? cineCurveH : 0.0f);
    float cineMinTop = 160.0f;
    // Give the bottom exactly what it wants; only when the window is too short to fit both do we cap the top at its
    // minimum and let the bottom take the (smaller) remainder.
    float cineTopH = cineAvail.y - cineWantBottom - 8.0f;
    if (cineTopH < cineMinTop) {
        cineTopH = cineMinTop;
    }
    float cineBottomH = cineAvail.y - cineTopH - 8.0f;
    if (cineBottomH < 60.0f) {
        cineBottomH = 60.0f;
    }
    float cineLeftW = cineAvail.x * 0.42f;
    if (cineLeftW < 240.0f) {
        cineLeftW = 240.0f;
    }
    ImGui::BeginChild("##cineLeft", ImVec2(cineLeftW, cineTopH), true);

    // Actor POV spectate: lock the camera to an actor's viewpoint, live.
    if (ImGui::CollapsingHeader("Actor POV (spectate)")) {
        bool spec = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SpectateEnabled"), 0);
        if (ImGui::Checkbox("Spectate actor POV", &spec)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.SpectateEnabled"), spec);
            if (spec) {
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            }
        }
        ImGui::Text("Actor: %s", sSpectateName[0] ? sSpectateName : "(none picked)");
        if (ImGui::Button("Pick actor##spectate")) {
            ImGui::OpenPopup("Pick spectate actor");
        }
        if (ImGui::BeginPopup("Pick spectate actor")) {
            CineActorInfo pick;
            if (DrawActorPicker(&pick)) {
                CinematicCam_SetSpectateActor(pick.ptr, pick.id);
                strncpy(sSpectateName, pick.name ? pick.name : "?", sizeof(sSpectateName) - 1);
                sSpectateName[sizeof(sSpectateName) - 1] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        bool useFocus = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SpectateUseFocus"), 1);
        if (ImGui::Checkbox("Track head / focus point", &useFocus)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.SpectateUseFocus"), useFocus);
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Ride the actor's animated focus point (its head) so head bob/turn shows in the POV. "
                        "Falls back to the actor's base position for actors that don't set a focus point. "
                        "With this on, you'll usually want a lower Eye height.");
        }
        float h = CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.SpectateHeight"), 40.0f);
        if (ImGui::SliderFloat("Eye height", &h, -100.0f, 200.0f, "%.2f", ImGuiSliderFlags_NoRoundToFormat)) {
            CVarSetFloat(CVAR_ENHANCEMENT("CinematicCam.SpectateHeight"), h);
        }
        CineHint("Locks the camera to the actor's viewpoint (aimed along its facing). "
                 "The world keeps running so you see what it sees.");
    }

    // Follow actor: the FREE camera rides along with a moving actor (keeps its offset); you still fly + aim.
    if (ImGui::CollapsingHeader("Follow actor (free camera)")) {
        int followId = CinematicCam_GetFreecamFollowId();
        if (followId != 0) {
            const char* nm = CinematicCam_ActorName(followId);
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "Following: %s (id %d)", nm ? nm : "?", followId);
            ImGui::SameLine();
            if (ImGui::Button("Stop##follow")) {
                CinematicCam_SetFreecamFollow(nullptr, 0);
            }
        } else {
            CineHint("Not following. Pick an actor to attach the free camera to it.");
        }
        if (ImGui::Button("Pick actor##follow")) {
            ImGui::OpenPopup("Pick follow actor");
        }
        if (ImGui::BeginPopup("Pick follow actor")) {
            CineActorInfo pick;
            if (DrawActorPicker(&pick)) {
                CinematicCam_SetFreecamFollow(pick.ptr, pick.id);
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        bool ctrlLink = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.FollowControlsLink"), 0);
        if (ImGui::Checkbox("Control Link (camera auto-aims at Link)", &ctrlLink)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.FollowControlsLink"), ctrlLink);
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("The best of both: the controller plays Link normally while the camera rides the "
                        "followed actor and automatically keeps Link in frame. You give up manual camera "
                        "control during the take (it's hands-off). Turn off to fly/aim the camera yourself.");
        }
        if (ctrlLink) {
            CineHint("Hands-off camera: follows the actor + auto-aims at Link. You play Link normally; "
                     "the world runs (this overrides Freeze World).");
        } else {
            CineHint("The camera keeps its position relative to the actor as it moves - fly to set the "
                     "offset (e.g. behind it) and aim wherever you like. Needs the world unfrozen.");
        }
    }

    // Area teleporter: jump to any major location to set up a shot without flying there.
    if (ImGui::CollapsingHeader("Teleport to area")) {
        int tn = CinematicCam_GetTeleportCount();
        static int sTpSel = 0;
        if (sTpSel >= tn) {
            sTpSel = 0;
        }
        const char* curName = CinematicCam_GetTeleportName(sTpSel);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Destination", curName ? curName : "?")) {
            for (int i = 0; i < tn; i++) {
                const char* nm = CinematicCam_GetTeleportName(i);
                if (nm && ImGui::Selectable(nm, i == sTpSel)) {
                    sTpSel = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Go")) {
            CinematicCam_TeleportTo(sTpSel);
        }
        CineHint("Fades to the chosen area (spawn point 0). Only works while in-game.");
    }

    // Sky & time: freeze the sky for clean loops and scrub the time of day directly.
    if (ImGui::CollapsingHeader("Sky & time")) {
        // Keyframable parameters carry an inline [Key | |< +/- >| ] control. When the track is on, editing the
        // parameter writes a key at the playhead (and previews it); otherwise it drives the live/manual value.
        bool gsAuto = sGreenScreenTrack.enabled;
        int greenScreen = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.GreenScreen"), 0);
        if (gsAuto) {
            float v;
            if (EvalParamTrack(sGreenScreenTrack, sPlayhead, v)) {
                greenScreen = (int)(v + 0.5f);
            }
        }
        ImGui::SetNextItemWidth(180.0f);
        const char* gsModes[] = { "Off", "Green (#00B140)", "Blue (#0047BB)" };
        if (ImGui::Combo("Green screen", &greenScreen, gsModes, 3)) {
            if (gsAuto) {
                PushUndo();
                TrackAddKey(sGreenScreenTrack, sPlayhead, (float)greenScreen);
                sPreview = true;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            } else {
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.GreenScreen"), greenScreen);
            }
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Replace the sky with a solid chroma-key color (and hide the sun/moon/sky glow) so you "
                        "can key it out when compositing. Scene geometry still renders over it.");
        }
        ImGui::SameLine();
        DrawParamKeyNav(sGreenScreenTrack, (float)greenScreen);

        bool freezeSky = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.FreezeSky"), 0);
        if (ImGui::Checkbox("Freeze sky & time", &freezeSky)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.FreezeSky"), freezeSky);
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Stop cloud drift and time-of-day progression so looping clips/GIFs line up.");
        }

        int liveDt = CinematicCam_GetDayTime();
        if (liveDt < 0) {
            CineHint("Time of day available in-game only.");
        } else {
            bool todAuto = sTodTrack.enabled;
            int dt = liveDt;
            if (todAuto) {
                float v;
                if (EvalParamTrack(sTodTrack, sPlayhead, v)) {
                    dt = (int)(v + 0.5f);
                }
            }
            auto setTod = [&](int value) {
                if (todAuto) {
                    PushUndo();
                    TrackAddKey(sTodTrack, sPlayhead, (float)value);
                    sPreview = true;
                    CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
                } else {
                    CinematicCam_SetDayTime(value);
                }
            };
            int mins = (int)(dt * (24.0f * 60.0f) / 65536.0f);
            char label[16];
            snprintf(label, sizeof(label), "%02d:%02d", (mins / 60) % 24, mins % 60);
            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::SliderInt("Time of day", &dt, 0, 0xFFFF, label)) {
                setTod(dt);
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("Set the sun/moon position and lighting. Keyframe it for a sunrise/sunset across a "
                            "shot, or turn on Freeze to hold it.");
            }
            ImGui::SameLine();
            DrawParamKeyNav(sTodTrack, (float)dt);
            if (ImGui::SmallButton("Noon")) {
                setTod(0x8000);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Sunset")) {
                setTod(0xC000);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Night")) {
                setTod(0x0000);
            }
        }
    }

    if (ImGui::CollapsingHeader("Pose Link")) {
        int yaw = CinematicCam_GetLinkYaw();
        if (yaw < 0) {
            CineHint("Available in-game only.");
        } else {
            const float kPi = 3.14159265358979f;
            const float radius = 58.0f;
            ImVec2 size(radius * 2.0f + 6.0f, radius * 2.0f + 6.0f);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##linkdial", size);
            bool dialActive = ImGui::IsItemActive();
            ImVec2 center(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 colRing = ImGui::GetColorU32(ImGuiCol_FrameBg);
            ImU32 colBorder = ImGui::GetColorU32(ImGuiCol_Border);
            ImU32 colNeedle = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
            ImU32 colTick = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddCircleFilled(center, radius, colRing, 48);
            dl->AddCircle(center, radius, colBorder, 48, 1.5f);
            // Cardinal ticks (visual reference only - the dial sets an absolute heading).
            for (int i = 0; i < 4; i++) {
                float a = i * (kPi * 0.5f);
                ImVec2 t0(center.x + sinf(a) * (radius - 8.0f), center.y - cosf(a) * (radius - 8.0f));
                ImVec2 t1(center.x + sinf(a) * radius, center.y - cosf(a) * radius);
                dl->AddLine(t0, t1, colTick, 1.5f);
            }

            // While dragging, point Link toward the cursor (clockwise from straight up = 0 deg).
            int newYaw = yaw;
            if (dialActive) {
                ImVec2 m = ImGui::GetIO().MousePos;
                float dx = m.x - center.x, dy = m.y - center.y;
                if (dx != 0.0f || dy != 0.0f) {
                    float deg = atan2f(dx, -dy) * (180.0f / kPi);
                    newYaw = (int)floorf(deg + 0.5f);
                }
            }
            newYaw %= 360;
            if (newYaw < 0) {
                newYaw += 360;
            }
            float rad = newYaw * (kPi / 180.0f);
            ImVec2 tip(center.x + sinf(rad) * (radius - 6.0f), center.y - cosf(rad) * (radius - 6.0f));
            dl->AddLine(center, tip, colNeedle, 2.5f);
            dl->AddCircleFilled(tip, 4.0f, colNeedle);
            dl->AddCircleFilled(center, 3.0f, colBorder);
            if (dialActive && newYaw != yaw) {
                CinematicCam_SetLinkYaw(newYaw);
                yaw = newYaw;
            }

            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(120.0f);
            int yIn = yaw;
            if (ImGui::InputInt("degrees", &yIn)) {
                CinematicCam_SetLinkYaw(yIn);
            }
            if (ImGui::Button("Face camera")) {
                CinematicCam_FaceLinkToCamera(0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Face away")) {
                CinematicCam_FaceLinkToCamera(1);
            }
            ImGui::EndGroup();
            CineHint("Drag the dial or type degrees to aim Link. Best while he stands idle.");
        }
    }

    ImGui::SeparatorText("Keyframes");
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Button("Add Keyframe")) {
        AddKeyframe();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(SelectedIndex() < 0 || !enabled);
    if (ImGui::Button("Update Selected")) {
        UpdateSelected();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(SelectedIndex() < 0);
    if (ImGui::Button("Delete")) {
        DeleteSelected();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        ClearPath();
    }

    // Copy / paste / insert at the playhead.
    ImGui::BeginDisabled(SelectedIndex() < 0);
    if (ImGui::Button("Copy")) {
        CopySelected();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!sClipboardValid);
    if (ImGui::Button("Paste @ playhead")) {
        PasteAtPlayhead();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !sClipboardValid) {
        CineTooltip("Copy a keyframe first.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Insert @ playhead")) {
        InsertAtPlayhead();
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("Add a keyframe at the current playhead time (on the existing curve, or the live "
                    "freecam pose if there's no path yet).");
    }

    // Record the live freecam motion into keyframes.
    if (!sRecording) {
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button("Record")) {
            PushUndo();
            sKeyframes.clear();
            sIds.clear();
            SelectOnly(-1);
            sPlayhead = 0.0f;
            sPlaying = false;
            sPreview = false;
            sRecording = true;
            sRecordTime = 0.0f;
            sRecordLast = 0.0f;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            RecordKeyframe(0.0f); // first keyframe at t=0
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !enabled) {
            CineTooltip("Enable the free camera first.");
        }
    } else {
        if (ImGui::Button("Stop recording")) {
            sRecording = false;
            SelectOnly(sIds.empty() ? -1 : sIds[0]);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::SliderFloat("Rec interval", &sRecordInterval, 0.05f, 1.0f, "%.2fs");
    if (sRecording) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "REC %.1fs (%d)", sRecordTime, (int)sKeyframes.size());
    }

    ImGui::BeginDisabled(sUndo.empty());
    if (ImGui::Button("Undo")) {
        Undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sRedo.empty());
    if (ImGui::Button("Redo")) {
        Redo();
    }
    ImGui::EndDisabled();

    // Path tools: smooth out jitter, normalize speed, retime, and generate orbits.
    if (ImGui::CollapsingHeader("Path tools", ImGuiTreeNodeFlags_DefaultOpen)) {
        CineHint("Shape is set only by keyframe positions; the timeline only sets speed.");
        if (ImGui::IsItemHovered()) {
            CineTooltip("The path's shape depends purely on WHERE the keyframes are - retiming them on the "
                        "timeline never bends the curve, it only redistributes how fast the camera travels. "
                        "Speed glides smoothly through keyframes (no slow-down/hang at each one).");
        }
        ImGui::BeginDisabled((int)sKeyframes.size() < 2);
        if (ImGui::Button("Smooth path")) {
            SmoothPath();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            CineTooltip("Bend the keyframes' tangents so the whole path (or the selected range) moves as ONE "
                        "continuous curve - direction and curvature smooth, no corner at any keyframe. Never "
                        "moves a keyframe; the range's ends are frozen so nothing outside it changes. This "
                        "also removes rail-aim flicks, since rail follows the curve's direction.");
        }

        ImGui::BeginDisabled((int)sKeyframes.size() < 3);
        if (ImGui::Button("Normalize speed")) {
            NormalizeSpeed();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            CineTooltip("Re-time the keyframes so the camera moves at a constant speed: keyframes spread out "
                        "over long path stretches and pack together over short ones. Keeps total duration. "
                        "With 3+ keyframes selected (Ctrl+click), only that range is re-timed (its ends stay "
                        "put).");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled((int)sKeyframes.size() < 2);
        // With 2+ keyframes selected this field scales the SELECTION: type a new duration for that span and
        // its keyframes compress/expand proportionally about the first one (later keyframes stay put).
        // Otherwise it rescales the whole path.
        bool selScale = SelectionCount() >= 2;
        int scLo = 0, scHi = 0;
        if (selScale) {
            selScale = ToolRange(scLo, scHi) && !(scLo == 0 && scHi == (int)sKeyframes.size() - 1);
        }
        float dur = selScale ? (sKeyframes[scHi].time - sKeyframes[scLo].time) : TotalTime();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat(selScale ? "Selection (s)" : "Total (s)", &dur, 0.0f, 0.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) { // commit on Enter / focus loss, not on every keystroke
            if (selScale) {
                ScaleSelection(scLo, scHi, dur);
            } else {
                SetTotalDuration(dur);
            }
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip(selScale ? "New duration for the SELECTED span: its keyframes compress or expand "
                                   "proportionally about the first selected one. Keyframes after the span keep "
                                   "their times."
                                 : "Rescale the whole path to last this many seconds (keeps relative spacing). "
                                   "Select 2+ keyframes to compress/expand just that group instead.");
        }
        ImGui::EndDisabled();

        // Auto-orbit: its own collapsible (collapsed by default) so it isn't always taking up space.
        if (ImGui::CollapsingHeader("Auto-orbit")) {
            static float oRadius = 200.0f, oHeight = 80.0f, oArc = 360.0f, oDur = 8.0f;
            static int oCount = 8, oCenter = 0; // center: 0 Link, 1 actor, 2 target, 3 in front of camera
            static bool oFollow = true;
            static int oActorId = 0;
            static void* oActorPtr = nullptr;
            static char oActorName[64] = "";
            const char* centers[] = { "Link", "Actor", "Target", "Camera" };
            ImGui::SetNextItemWidth(110.0f);
            ImGui::Combo("Around", &oCenter, centers, 4);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderInt("Points", &oCount, 3, 32);
            if (oCenter == 1) { // actor: pick which one
                ImGui::Text("Actor: %s (id %d)", oActorName[0] ? oActorName : "(none)", oActorId);
                ImGui::SameLine();
                if (ImGui::Button("Pick##orbitactor")) {
                    ImGui::OpenPopup("Pick orbit actor");
                }
                if (ImGui::BeginPopup("Pick orbit actor")) {
                    CineActorInfo pick;
                    if (DrawActorPicker(&pick)) {
                        oActorId = pick.id;
                        oActorPtr = pick.ptr;
                        strncpy(oActorName, pick.name ? pick.name : "?", sizeof(oActorName) - 1);
                        oActorName[sizeof(oActorName) - 1] = '\0';
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("Radius", &oRadius, 1.0f, 10.0f, 8000.0f, "%.2f", ImGuiSliderFlags_NoRoundToFormat);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("Height", &oHeight, 1.0f, -1000.0f, 2000.0f, "%.2f", ImGuiSliderFlags_NoRoundToFormat);
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Arc", &oArc, 30.0f, 360.0f, "%.2f deg", ImGuiSliderFlags_NoRoundToFormat);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Seconds", &oDur, 1.0f, 60.0f, "%.1f");
            if (oCenter == 0 || oCenter == 1) {
                ImGui::Checkbox("Follow it as it moves", &oFollow);
                if (ImGui::IsItemHovered()) {
                    CineTooltip("The whole orbit tracks the target's movement during playback, keeping it framed.");
                }
            }
            if (ImGui::Button("Generate orbit")) {
                float c[3];
                bool have = false;
                int followMode = 0;
                int followId = 0;
                void* followPtr = nullptr;
                if (oCenter == 0) {
                    have = CinematicCam_GetPlayerPos(c) != 0;
                    followMode = oFollow ? 1 : 0;
                } else if (oCenter == 1) {
                    have = CinematicCam_ResolveActor(&oActorPtr, (short)oActorId, nullptr, c) != 0;
                    followMode = oFollow ? 2 : 0;
                    followId = oActorId;
                    followPtr = oActorPtr;
                } else if (oCenter == 2) {
                    c[0] = sAimOverridePoint[0];
                    c[1] = sAimOverridePoint[1];
                    c[2] = sAimOverridePoint[2];
                    have = true;
                } else {
                    float eye[3], at[3], roll, fov;
                    CinematicCam_GetPose(eye, at, &roll, &fov);
                    float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
                    float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                    float s = (l > 1e-3f) ? oRadius / l : 0.0f;
                    c[0] = eye[0] + f[0] * s;
                    c[1] = eye[1] + f[1] * s;
                    c[2] = eye[2] + f[2] * s;
                    have = true;
                }
                if (have) {
                    GenerateOrbit(c, oRadius, oHeight, oCount, oArc, 0.0f, oDur, followMode, followId, followPtr);
                }
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("Replace the path with a circle/arc of keyframes around the chosen center, each "
                            "aimed at it. A full 360 arc turns on looping. Great for establishing shots.");
            }
        } // Auto-orbit collapsible
    }

    // Keyframe list
    ImGui::Text("Keyframes: %d", (int)sKeyframes.size());
    if (SelectionCount() > 1) {
        ImGui::SameLine();
        CineHint("(%d selected)", SelectionCount());
    }
    ImGui::BeginChild("##kflist", ImVec2(0, 160), true);
    // Clipper: only build widgets for visible rows (keeps long/recorded paths responsive).
    ImGuiListClipper clipper;
    clipper.Begin((int)sKeyframes.size());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            ImGui::PushID(i);
            char label[64];
            bool kfPinned = sKeyframes[i].hasTangent || sKeyframes[i].hasTangentIn || sKeyframes[i].tanWOut > 0.0f ||
                            sKeyframes[i].tanWIn > 0.0f;
            snprintf(label, sizeof(label), "#%d   t=%.2fs   %s%s", i + 1, sKeyframes[i].time,
                     sKeyframes[i].interp == CINE_INTERP_LINEAR ? "[Linear]" : "",
                     kfPinned ? "*" : ""); // * = custom/pinned tangents (see Bend gizmo / Reset tangent)
            if (ImGui::Selectable(label, IsSelected(sIds[i]))) {
                if (ImGui::GetIO().KeyCtrl) {
                    ToggleSelect(sIds[i]); // ctrl-click extends the selection
                } else {
                    SelectOnly(sIds[i]);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // End the controls column; begin the right column (selected-keyframe inspector + playback + save/load).
    ImGui::EndChild(); // ##cineLeft
    ImGui::SameLine();
    ImGui::BeginChild("##cineRight", ImVec2(0, cineTopH), true);

    int sel = SelectedIndex();
    if (SelectionCount() > 1) {
        ImGui::SeparatorText("Selected keyframes");
        CineHint("%d keyframes selected. Per-keyframe fields are hidden while multiple are selected - "
                 "drag on the timeline to move them together, or Ctrl+click to narrow the selection.",
                 SelectionCount());
    } else if (sel >= 0) {
        ImGui::SeparatorText("Selected keyframe");

        // World gizmo for this keyframe (only meaningful while the in-world path is shown).
        if (sShowPath) {
            ImGui::TextUnformatted("Gizmo:");
            ImGui::SameLine();
            ImGui::RadioButton("Move", &sGizmoMode, GIZMO_MOVE);
            ImGui::SameLine();
            ImGui::RadioButton("Rotate (aim)", &sGizmoMode, GIZMO_ROTATE);
            ImGui::SameLine();
            ImGui::RadioButton("Bend (path)", &sGizmoMode, GIZMO_BEND);
            if (sGizmoMode == GIZMO_MOVE) {
                CineHint("Drag the red/green/blue axes in the world to move this keyframe.");
            } else if (sGizmoMode == GIZMO_ROTATE) {
                CineHint("Drag the rings to aim the camera: green=yaw, red=pitch, blue=roll.");
            } else {
                CineHint("Drag the rings to bend the path's curve through this point (no effect on aim).");
                ImGui::TextUnformatted("Side:");
                ImGui::SameLine();
                ImGui::RadioButton("Both", &sBendSide, 0);
                bool sideHov = ImGui::IsItemHovered();
                ImGui::SameLine();
                ImGui::RadioButton("Out##bendside", &sBendSide, 1);
                sideHov = sideHov || ImGui::IsItemHovered();
                ImGui::SameLine();
                ImGui::RadioButton("In##bendside", &sBendSide, 2);
                sideHov = sideHov || ImGui::IsItemHovered();
                if (sideHov || sBendSide != 0) {
                    CineHint("Out = the curve leaving toward the next keyframe (magenta handle); In = the "
                             "curve arriving from the previous one (teal). Editing one side breaks the "
                             "handle so the path can turn a corner here; Both keeps/rotates them rigidly.");
                }
                // Side weights: how far the curve bulges on each side of this keyframe (a scalable Bezier
                // handle, the "tension per side" control). 1.0 = the automatic length; relative, so it stays
                // sensible when the keyframe moves. Drag the handle DOTS in the world to steer direction.
                {
                    CineKeyframe& kfb = sKeyframes[sel];
                    float wo = (kfb.tanWOut > 0.0f) ? kfb.tanWOut : 1.0f;
                    float wi = (kfb.tanWIn > 0.0f) ? kfb.tanWIn : 1.0f;
                    // Neutral (1.0) stores as 0 = automatic, so merely touching a slider doesn't pin the key.
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::SliderFloat("Out weight", &wo, 0.2f, 3.0f, "%.2f");
                    if (ImGui::IsItemActivated()) {
                        PushUndo();
                    }
                    if (ImGui::IsItemActive() || ImGui::IsItemDeactivatedAfterEdit()) {
                        kfb.tanWOut = (std::fabs(wo - 1.0f) < 0.01f) ? 0.0f : wo;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::SliderFloat("In weight", &wi, 0.2f, 3.0f, "%.2f");
                    if (ImGui::IsItemActivated()) {
                        PushUndo();
                    }
                    if (ImGui::IsItemActive() || ImGui::IsItemDeactivatedAfterEdit()) {
                        kfb.tanWIn = (std::fabs(wi - 1.0f) < 0.01f) ? 0.0f : wi;
                    }
                    if (ImGui::IsItemHovered()) {
                        CineTooltip("How strongly each side of this keyframe shapes the curve (handle length). "
                                    "Works with or without a custom direction.");
                    }
                }
                bool pinned = sKeyframes[sel].hasTangent || sKeyframes[sel].hasTangentIn ||
                              sKeyframes[sel].tanWOut > 0.0f || sKeyframes[sel].tanWIn > 0.0f;
                if (pinned && ImGui::SmallButton("Reset tangent")) {
                    PushUndo();
                    sKeyframes[sel].hasTangent = 0;
                    sKeyframes[sel].hasTangentIn = 0;
                    sKeyframes[sel].tanWOut = 0.0f;
                    sKeyframes[sel].tanWIn = 0.0f;
                }
            }
        } else {
            CineHint("Enable 'Show path in world' to use the move/rotate/bend gizmo.");
        }

        float t = sKeyframes[sel].time;
        bool changed = ImGui::InputFloat("Keyframe time (s)", &t, 0.1f, 1.0f, "%.2f");
        if (ImGui::IsItemActivated()) {
            PushUndo(); // snapshot pre-edit state so the whole edit is one undo step
        }
        if (changed) {
            if (t < 0.0f) {
                t = 0.0f;
            }
            sKeyframes[sel].time = t;
            SortByTime();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Jump to##kf")) {
            // Park the playhead here and preview so the free camera sits on this keyframe for editing.
            sPlayhead = sKeyframes[sel].time;
            sPreview = true;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }

        // Per-keyframe interpolation (shapes the segment leaving this keyframe toward the next).
        const char* interpModes[] = { "Smooth (spline)", "Linear" };
        int mode = sKeyframes[sel].interp;
        if (ImGui::Combo("Interpolation", &mode, interpModes, 2)) {
            PushUndo();
            sKeyframes[sel].interp = mode;
        }
        if (sKeyframes[sel].interp == CINE_INTERP_SMOOTH) {
            float tens = sKeyframes[sel].tension;
            ImGui::SliderFloat("Tension", &tens, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("Higher = tighter/straighter through the keyframe; lower = rounder, wider arcs.");
            }
            sKeyframes[sel].tension = tens;

            float cont = sKeyframes[sel].continuity;
            ImGui::SliderFloat("Continuity", &cont, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("0 = smooth pass-through; away from 0 sharpens the corner at the keyframe.");
            }
            sKeyframes[sel].continuity = cont;

            float bias = sKeyframes[sel].bias;
            ImGui::SliderFloat("Bias", &bias, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("Lean the curve toward the previous (+) or the next (-) keyframe (overshoot/undershoot).");
            }
            sKeyframes[sel].bias = bias;

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                PushUndo();
                sKeyframes[sel].tension = 0.0f;
                sKeyframes[sel].continuity = 0.0f;
                sKeyframes[sel].bias = 0.0f;
            }
        }

        CineHint("Ease: shape it on the Speed curve - tick 'Speed graph' at the top of the Curve editor, then "
                 "drag this keyframe's point for how fast the camera is here, and its handles for how it gets "
                 "there.");

        // Aim mode: how this keyframe's camera is oriented.
        const char* aimModes[] = { "Free orientation", "Look at point",  "Look at Link",
                                   "Look at actor",    "Look at target", "Follow path (rail)" };
        int am = sKeyframes[sel].aimMode;
        if (ImGui::Combo("Aim", &am, aimModes, 6)) {
            PushUndo();
            sKeyframes[sel].aimMode = am;
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Follow path (rail): the camera looks straight along its direction of travel, like a "
                        "dolly on a rail - always parallel to the spline. Roll and FOV still apply.");
        }
        {
            bool hold = sKeyframes[sel].aimHold != 0;
            if (ImGui::Checkbox("Hold framing here", &hold)) {
                PushUndo();
                sKeyframes[sel].aimHold = hold ? 1 : 0;
            }
            if (ImGui::IsItemHovered()) {
                CineTooltip("The view's turn comes to rest ON this keyframe and builds up again leaving it, "
                            "so the framing parks for a beat while the camera keeps travelling. Off, the view "
                            "flows straight through without stopping.");
            }
            if (sKeyframes[sel].hasAimTanIn || sKeyframes[sel].hasAimTanOut) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset aim rates")) {
                    PushUndo();
                    sKeyframes[sel].hasAimTanIn = sKeyframes[sel].hasAimTanOut = 0;
                    sKeyframes[sel].hasAimAccIn = sKeyframes[sel].hasAimAccOut = 0;
                }
                if (ImGui::IsItemHovered()) {
                    CineTooltip("This keyframe carries baked aim rates (from Insert @ playhead), frozen so "
                                "the insert couldn't reshape the aim. Reset returns them to automatic.");
                }
            }
        }
        if (sKeyframes[sel].aimMode == CINE_AIM_POINT) {
            float tgt[3] = { sKeyframes[sel].at[0], sKeyframes[sel].at[1], sKeyframes[sel].at[2] };
            bool tch = ImGui::InputFloat3("Target", tgt, "%.1f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (tch) {
                sKeyframes[sel].at[0] = tgt[0];
                sKeyframes[sel].at[1] = tgt[1];
                sKeyframes[sel].at[2] = tgt[2];
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("To Link")) {
                float p[3];
                if (CinematicCam_GetPlayerPos(p)) {
                    PushUndo();
                    sKeyframes[sel].at[0] = p[0];
                    sKeyframes[sel].at[1] = p[1];
                    sKeyframes[sel].at[2] = p[2];
                }
            }
            CineHint("Drag the orange crosshair in the world to place the target.");
        } else if (sKeyframes[sel].aimMode == CINE_AIM_PLAYER) {
            CineHint("Tracks Link's position (live during playback).");
        } else if (sKeyframes[sel].aimMode == CINE_AIM_ACTOR) {
            // Cheap id->name lookup (no per-frame enumeration of all actors).
            const char* curName =
                sKeyframes[sel].aimActorId ? CinematicCam_ActorName(sKeyframes[sel].aimActorId) : "(pick one)";
            ImGui::Text("Target: %s (id %d)", curName ? curName : "?", sKeyframes[sel].aimActorId);
            if (ImGui::Button("Pick actor...")) {
                ImGui::OpenPopup("Pick actor");
            }
            if (ImGui::BeginPopup("Pick actor")) {
                CineActorInfo pick;
                if (DrawActorPicker(&pick)) {
                    PushUndo();
                    sKeyframes[sel].aimActorId = pick.id;
                    sKeyframes[sel].aimActorPtr = pick.ptr;
                    sKeyframes[sel].aimActorPos[0] = pick.pos[0]; // hint for nearest-actor re-acquire on reload
                    sKeyframes[sel].aimActorPos[1] = pick.pos[1];
                    sKeyframes[sel].aimActorPos[2] = pick.pos[2];
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            CineHint("Tracks the actor live. Saved by id (re-acquired on load).");
        } else if (sKeyframes[sel].aimMode == CINE_AIM_TARGET) {
            bool tgtAuto = sTargetXTrack.enabled;
            float tgt[3] = { sAimOverridePoint[0], sAimOverridePoint[1], sAimOverridePoint[2] };
            if (tgtAuto) {
                float v;
                if (EvalParamTrack(sTargetXTrack, sPlayhead, v)) {
                    tgt[0] = v;
                }
                if (EvalParamTrack(sTargetYTrack, sPlayhead, v)) {
                    tgt[1] = v;
                }
                if (EvalParamTrack(sTargetZTrack, sPlayhead, v)) {
                    tgt[2] = v;
                }
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputFloat3("##kftgt", tgt, "%.0f")) {
                sAimOverridePoint[0] = tgt[0];
                sAimOverridePoint[1] = tgt[1];
                sAimOverridePoint[2] = tgt[2];
                if (tgtAuto) {
                    PushUndo();
                    TrackAddKey(sTargetXTrack, sPlayhead, tgt[0]);
                    TrackAddKey(sTargetYTrack, sPlayhead, tgt[1]);
                    TrackAddKey(sTargetZTrack, sPlayhead, tgt[2]);
                    sPreview = true;
                    CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Place at camera")) {
                PlaceOverrideTargetAtCamera();
            }
            ImGui::SameLine();
            DrawTargetKeyNav();
            CineHint("Aims at the shared movable target (one point for the whole path). Keyframe it to "
                     "animate it; edit each axis in the Curve editor (Target X/Y/Z).");
        }

        // Numeric fields: type exact position/orientation values for the selected keyframe.
        ImGui::Checkbox("Numeric fields", &sShowFields);
        if (sShowFields) {
            CineKeyframe& kf = sKeyframes[sel];

            // Per-axis rows (not InputFloat3) so each gets -/+ step buttons like yaw/pitch/roll below.
            static const char* kPosAxis[3] = { "Pos X", "Pos Y", "Pos Z" };
            for (int ax = 0; ax < 3; ax++) {
                float v = kf.eye[ax];
                ImGui::InputFloat(kPosAxis[ax], &v, 1.0f, 25.0f, "%.1f");
                if (ImGui::IsItemActivated()) {
                    PushUndo();
                }
                if (v != kf.eye[ax]) {
                    kf.at[ax] += v - kf.eye[ax]; // move the look-at rigidly with the eye
                    kf.eye[ax] = v;
                }
            }

            float yaw, pitch, dist;
            GetYawPitch(kf, yaw, pitch, dist);
            float yaw0 = yaw, pitch0 = pitch;
            ImGui::BeginDisabled(kf.aimMode != CINE_AIM_FREE); // yaw/pitch are target-driven otherwise
            ImGui::InputFloat("Yaw", &yaw, 1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            ImGui::InputFloat("Pitch", &pitch, 1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            ImGui::EndDisabled();
            if (yaw != yaw0 || pitch != pitch0) {
                SetYawPitch(kf, yaw, pitch, dist);
            }

            float roll = kf.roll;
            ImGui::InputFloat("Roll", &roll, 1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            kf.roll = roll;

            float fov = kf.fov;
            ImGui::InputFloat("FOV", &fov, 1.0f, 5.0f, "%.1f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (fov < 1.0f) {
                fov = 1.0f;
            }
            if (fov > 170.0f) {
                fov = 170.0f;
            }
            kf.fov = fov;

            if (ImGui::SmallButton("Reset Roll")) {
                PushUndo();
                kf.roll = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Level Pitch")) {
                PushUndo();
                float y2, p2, d2;
                GetYawPitch(kf, y2, p2, d2);
                SetYawPitch(kf, y2, 0.0f, d2);
            }
        }
    }

    ImGui::SeparatorText("Playback");

    float total = TotalTime();
    ImGui::BeginDisabled(sKeyframes.size() < 2);
    if (sPlaying) {
        if (ImGui::Button("Stop")) {
            sPlaying = false;
        }
    } else {
        if (ImGui::Button("Play")) {
            float pt = EffectiveTotal();
            if (sPlayhead >= pt) {
                sPlayhead = 0.0f;
            }
            sPlayU = InvertEasedProgress((pt > 0.0f) ? (sPlayhead / pt) : 0.0f); // resume exactly at the playhead
            sPlayDir = 1;
            if (sPlayhead == 0.0f && CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SyncIdleAnim"), 0)) {
                CinematicCam_SyncLinkIdleAnim(); // anchor Link's idle anim when starting from the top
            }
            sPlaying = true;
            sPreview = false;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &sLoop);
    ImGui::SameLine();
    if (ImGui::Checkbox("Preview", &sPreview)) {
        if (sPreview) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }

    bool controlLink = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.PlaybackControlsLink"), 0);
    if (ImGui::Checkbox("Control Link during playback", &controlLink)) {
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.PlaybackControlsLink"), controlLink);
    }
    if (ImGui::IsItemHovered()) {
        CineTooltip("While a path plays, the controller moves Link and the world keeps running "
                    "(the camera follows the path). Pairs well with 'Look at Link'.");
    }

    // Path follow status: when set (e.g. by an orbit), the whole path tracks a moving target.
    if (sFollowMode != 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "Following: %s", sFollowMode == 1 ? "Link" : "actor");
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop following")) {
            sFollowMode = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Re-anchor here")) {
            // Reset the origin to the target's current position (so the path centers on it from now).
            float c[3];
            int prev = sFollowMode;
            if (FollowCenter(c)) {
                sFollowOrigin[0] = c[0];
                sFollowOrigin[1] = c[1];
                sFollowOrigin[2] = c[2];
            }
            sFollowMode = prev;
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Recenter the follow offset on the target's current position.");
        }
    }

    if (sLoop) {
        ImGui::SetNextItemWidth(160.0f);
        const char* loopModes[] = { "Forward (wrap)", "Ping-pong (reverse)" };
        ImGui::Combo("Loop style", &sLoopMode, loopModes, 2);
        if (ImGui::IsItemHovered()) {
            CineTooltip("Forward: glide from the last keyframe back to the first and repeat. "
                        "Ping-pong: play to the end, then play back in reverse, and repeat.");
        }
        if (sLoopMode == 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Return (s)", &sLoopReturnTime, 0.25f, 10.0f, "%.2fs");
            if (sLoopReturnTime < 0.0f) {
                sLoopReturnTime = 0.0f;
            }
        }
        bool loopMark = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.LoopStartMarker"), 0);
        if (ImGui::Checkbox("Loop-start marker", &loopMark)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.LoopStartMarker"), loopMark);
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Flash a magenta square in the top-left for the single frame each loop restarts. "
                        "Lets you find the exact loop boundary in a recording and trim there (off by "
                        "default - it's only an editing aid, delete that frame in post).");
        }
    }

    ImGui::SliderFloat("Speed", &sPlaySpeed, 0.1f, 4.0f, "%.2fx");
    const char* easeModes[] = { "None", "Ease in/out", "Ease in", "Ease out" };
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Easing", &sEaseMode, easeModes, 4);
    if (ImGui::IsItemHovered()) {
        CineTooltip("Playback timing: accelerate/decelerate the whole move instead of moving at a "
                    "constant rate. Affects Play only, not scrubbing.");
    }
    if (sEaseMode != 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Amount##ease", &sEaseAmount, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            CineTooltip("How strong the easing is (0 = linear, 1 = full).");
        }
    }

    // Camera shake / handheld: organic jitter layered on top of playback.
    ImGui::Checkbox("Camera shake", &sShakeEnabled);
    if (ImGui::IsItemHovered()) {
        CineTooltip("Add smooth handheld-style jitter to the moving camera. Deterministic, so it looks the "
                    "same every time you scrub or replay.");
    }
    {
        // Shake intensity is a keyframable multiplier on the amps below (ramp calm -> shaky over a shot).
        ImGui::TextUnformatted("Intensity");
        ImGui::SameLine();
        float shVal = 1.0f;
        if (sShakeTrack.enabled) {
            float v;
            if (EvalParamTrack(sShakeTrack, sPlayhead, v)) {
                shVal = v;
            }
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::SliderFloat("##shakeInt", &shVal, 0.0f, 2.0f, "%.2fx", ImGuiSliderFlags_NoRoundToFormat)) {
                PushUndo();
                TrackAddKey(sShakeTrack, sPlayhead, shVal);
                sPreview = true;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            }
            ImGui::SameLine();
        }
        DrawParamKeyNav(sShakeTrack, shVal);
    }
    if (sShakeEnabled) {
        ImGui::SetNextItemWidth(130.0f);
        ImGui::SliderFloat("Position##shake", &sShakePosAmp, 0.0f, 30.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        ImGui::SliderFloat("Angle##shake", &sShakeRotAmp, 0.0f, 5.0f, "%.2f deg");
        ImGui::SetNextItemWidth(130.0f);
        ImGui::SliderFloat("Frequency##shake", &sShakeFreq, 0.5f, 20.0f, "%.1f Hz");
        ImGui::SameLine();
        ImGui::Checkbox("In preview too", &sShakeOnPreview);
    }

    // Hide HUD (keyframable): reveal or hide the HUD over a shot. Manual checkbox drives the CVar; with the track
    // on it edits a key at the playhead instead.
    {
        bool hudAuto = sHudTrack.enabled;
        bool hide = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.HideHud"), 1) != 0;
        if (hudAuto) {
            float v;
            if (EvalParamTrack(sHudTrack, sPlayhead, v)) {
                hide = v > 0.5f;
            }
        }
        if (ImGui::Checkbox("Hide HUD", &hide)) {
            if (hudAuto) {
                PushUndo();
                TrackAddKey(sHudTrack, sPlayhead, hide ? 1.0f : 0.0f);
                sPreview = true;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            } else {
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.HideHud"), hide);
            }
        }
        ImGui::SameLine();
        DrawParamKeyNav(sHudTrack, hide ? 1.0f : 0.0f);
    }

    // Path-level aim override: aim every keyframe at one target (fixes up recorded paths at once).
    const char* aimOv[] = { "Per-keyframe (off)", "All look at Link", "All look at target", "All look at actor",
                            "All follow path (rail)" };
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Aim override", &sAimOverride, aimOv, 5);
    if (ImGui::IsItemHovered()) {
        CineTooltip("Ignore each keyframe's own aim and point the whole path at one target - or, with rail, "
                    "straight along the travel direction like a dolly. Handy for re-aiming a recorded flight "
                    "in one step.");
    }
    if (sAimOverride == 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat3("##aimovpt", sAimOverridePoint, "%.0f");
        ImGui::SameLine();
        if (ImGui::Button("Place at camera")) {
            PlaceOverrideTargetAtCamera();
        }
        ImGui::SameLine();
        DrawTargetKeyNav();
        if (ImGui::IsItemHovered()) {
            CineTooltip("Drop the aim target in front of the free camera. With 'Show path in world' on you can "
                        "then drag its red/green/blue handles to reposition it, and the whole path aims at it.");
        }
        CineHint("Movable aim target: the whole path looks at this point. Drag its gizmo in the world.");
    } else if (sAimOverride == 3) {
        ImGui::SameLine();
        if (ImGui::Button("Pick##aimov")) {
            ImGui::OpenPopup("Pick override actor");
        }
        if (ImGui::BeginPopup("Pick override actor")) {
            CineActorInfo pick;
            if (DrawActorPicker(&pick)) {
                sAimOverrideActorId = pick.id;
                sAimOverrideActorPtr = pick.ptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::Text("id %d", sAimOverrideActorId);
    }

    if (ImGui::CollapsingHeader("Save / Load", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("Name", sFilename, sizeof(sFilename));
        if (ImGui::Button("Save")) {
            std::string p = std::string("cinematics/") + sFilename + ".json";
            if (std::filesystem::exists(p)) {
                ImGui::OpenPopup("Overwrite path?");
            } else {
                SavePath();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            LoadPath();
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            ImGui::OpenPopup("Cinematic files");
        }
        if (ImGui::IsItemHovered()) {
            CineTooltip("Browse, load, or delete saved cinematics in the cinematics/ folder.");
        }
        ImGui::SameLine();
        CineHint("(cinematics/<name>.json)");
        if (sDirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "*");
            if (ImGui::IsItemHovered()) {
                CineTooltip("Unsaved changes (autosaved to cinematics/<name>_autosave.json once a minute).");
            }
        }
        if (sFileStatus[0]) {
            CineHint("%s", sFileStatus);
        }

        // Path-bound location: tie the current scene/spawn to the path so loading warps you straight back.
        ImGui::Checkbox("Bind location to path", &sBindLocation);
        if (ImGui::IsItemHovered()) {
            CineTooltip("When saving, remember the current scene/spawn so loading this path warps you here.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Teleport on load", &sTeleportOnLoad);
        if (ImGui::IsItemHovered()) {
            CineTooltip("When loading a path with a bound location, fade-warp to it (skipped if already there).");
        }
        if (sPathEntrance >= 0) {
            bool here = (CinematicCam_GetCurrentEntrance() == sPathEntrance);
            CineHint("Bound location: entrance %d%s", sPathEntrance, here ? "  (you are here)" : "");
        } else {
            CineHint("Bound location: none");
        }

        if (ImGui::BeginPopup("Cinematic files")) {
            CineHint("Saved cinematics  (* = current)");
            ImGui::Separator();
            // Snapshot the directory listing when the popup opens (re-reading + sorting it every frame was a
            // per-frame disk hit); refreshed after a delete so the list stays truthful.
            static std::vector<std::string> sFileList;
            if (ImGui::IsWindowAppearing()) {
                sFileList = ListCinematics();
            }
            if (sFileList.empty()) {
                CineHint("(none yet - Save one first)");
            }
            static std::string sConfirmDelete; // name awaiting delete confirmation, or empty
            bool closePopup = false;
            ImGui::BeginChild("##cinelist", ImVec2(320, 240), false);
            for (auto& name : sFileList) {
                ImGui::PushID(name.c_str());
                // Buttons first (a full-width Selectable would otherwise swallow their clicks).
                if (ImGui::SmallButton("Load")) {
                    LoadNamed(name);
                    closePopup = true;
                }
                ImGui::SameLine();
                if (sConfirmDelete == name) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "delete?");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("yes")) {
                        std::error_code ec;
                        std::filesystem::remove(std::string("cinematics/") + name + ".json", ec);
                        sConfirmDelete.clear();
                        sFileList = ListCinematics(); // refresh: iterators into the old list are done this frame
                        ImGui::PopID();
                        break; // the list we're iterating changed; redraw next frame
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("no")) {
                        sConfirmDelete.clear();
                    }
                } else {
                    if (ImGui::SmallButton("X")) {
                        sConfirmDelete = name;
                    }
                    ImGui::SameLine();
                    bool isCurrent = (name == sFilename);
                    ImGui::TextUnformatted((name + (isCurrent ? "  *" : "")).c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (closePopup) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Overwrite path?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("\"%s.json\" already exists. Overwrite it?", sFilename);
            if (ImGui::Button("Overwrite")) {
                SavePath();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    } // end of the "Save / Load" collapsing section

    ImGui::EndChild(); // ##cineRight

    // Dope-sheet timeline pinned across the bottom, with the collapsible curve editor docked beneath it.
    ImGui::BeginChild("##cineTimeline", ImVec2(0, cineBottomH), true);
    DrawTimeline();
    ImGui::Separator();
    sCurveEditorOpen = ImGui::CollapsingHeader("Curve editor");
    if (sCurveEditorOpen) {
        DrawCurveEditor();
    }
    ImGui::EndChild();

    if (sPerfOn) {
        sPerfDrawMs = CineNowMs() - cinePerfT0;
    }
}

// Live perf HUD (top-right), shown whenever CinematicCam.PerfDiag is on - even when the cinematic camera is idle,
// since the freeze "happens all the time". Splits each frame into our editor draw vs the engine/GPU remainder.
static void DrawPerfHud() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    double engine = sPerfFrameMs - sPerfDrawMs - sPerfUpdateMs;
    if (engine < 0.0) {
        engine = 0.0;
    }
    double fps = (sPerfFrameMs > 0.001) ? 1000.0 / sPerfFrameMs : 0.0;
    char l1[160], l2[224], l3[200];
    snprintf(l1, sizeof(l1), "CINE PERF  frame %.1fms (%.0f fps)  peak(2s) %.0fms  spikes %d", sPerfFrameMs, fps,
             sPerfPeakMs, sPerfSpikeCount);
    snprintf(l2, sizeof(l2), "draw %.1f [overlay %.1f]  update %.1f  engine/gpu %.1f   kf %d %s%s%s", sPerfDrawMs,
             sPerfOverlayMs, sPerfUpdateMs, engine, (int)sKeyframes.size(),
             sPlaying ? "PLAY " : (sPreview ? "PREVIEW " : ""), sRecording ? "REC " : "", sShowPath ? "overlay" : "");
    // Heartbeat: drawStale = ms since the window content last redrew. Climbs during a freeze (= Begin skipped);
    // worst = the longest freeze seen. draws/updates show whether DrawElement is keeping pace with the frame loop.
    snprintf(l3, sizeof(l3), "drawStale %.0fms (worst %.0fms)   draws %llu / updates %llu", sDrawStaleMs,
             sWorstDrawStaleMs, (unsigned long long)sDrawCalls, (unsigned long long)sUpdateCalls);
    float w = 580.0f;
    ImVec2 p(vp->Pos.x + vp->Size.x - w - 12.0f, vp->Pos.y + 12.0f);
    dl->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 4.0f), ImVec2(p.x + w, p.y + 56.0f), IM_COL32(0, 0, 0, 160), 4.0f);
    ImU32 col = (sPerfFrameMs > 40.0) ? IM_COL32(255, 140, 120, 255) : IM_COL32(150, 235, 150, 255);
    dl->AddText(p, col, l1);
    dl->AddText(ImVec2(p.x, p.y + 18.0f), IM_COL32(220, 220, 225, 255), l2);
    ImU32 hcol = (sDrawStaleMs > 250.0) ? IM_COL32(255, 90, 90, 255) : IM_COL32(140, 200, 255, 255);
    dl->AddText(ImVec2(p.x, p.y + 36.0f), hcol, l3);
}

// Called every frame (even when the window is hidden): draw the cinematic letterbox bars and grid overlay.
void CinematicCamPathWindow::UpdateElement() {
    double upT0 = sPerfOn ? CineNowMs() : 0.0;
    sUpdateCalls++; // runs every frame regardless of the window's Begin (our external "is the window redrawing?" clock)
    if (sPerfOn) {
        if (sLastDrawTimeMs > 0.0) {
            sDrawStaleMs = CineNowMs() - sLastDrawTimeMs;
        }
        DrawPerfHud(); // live frame-time HUD, drawn even when the cinematic camera is idle
    }

    // Shooting-bar housekeeping: with "Bar on Play" enabled, Play collapses the editor to the bar and stopping
    // restores it - but only if the bar was entered automatically (a manual expand mid-playback sticks, and the
    // restore still runs even if the checkbox was turned off during playback).
    static bool sWasPlaying = false;
    if (sPlaying && !sWasPlaying && !sBarMode && CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.AutoBarOnPlay"), 1)) {
        EnterBarMode(true);
    } else if (!sPlaying && sWasPlaying && sAutoBar && sBarMode) {
        ExitBarMode();
    }
    sWasPlaying = sPlaying;

    // Autosave: back up edited-but-unsaved work to cinematics/_autosave.json once a minute. Skipped while a
    // take is playing/recording (no disk hit mid-shot); the first backup lands a minute after the first edit.
    if (sDirtyForAutosave && !sPlaying && !sRecording && sKeyframes.size() >= 2) {
        double now = CineNowMs();
        if (sLastAutosaveMs <= 0.0) {
            sLastAutosaveMs = now;
        } else if (now - sLastAutosaveMs > 60000.0) {
            char autoName[96]; // per-path backup slot: editing one path never overwrites another's autosave
            snprintf(autoName, sizeof(autoName), "%s_autosave", sFilename);
            SavePathTo(autoName, false);
            sDirtyForAutosave = false;
            sLastAutosaveMs = now;
            snprintf(sFileStatus, sizeof(sFileStatus), "Autosaved to %s.json (unsaved changes remain)", autoName);
        }
    }

    // Window styling that must land on OUR ImGui::Begin: Update() runs immediately before this window's Draw()
    // (no other Begin in between), and both are gated on visibility so nothing leaks onto another window.
    if (IsVisible()) {
        float alpha = CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.BgAlpha"), 1.0f);
        if (alpha < 0.999f) {
            ImGui::SetNextWindowBgAlpha(alpha);
        }
        if (sPendingDock == 1) { // entering the bar: float free so the game reclaims the dock space
            ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
            sPendingDock = 0;
        } else if (sPendingDock == 2) { // expanding: return to the dock the full editor lived in
            // Only if that dock node still exists - a node is destroyed when its last window undocks, and
            // SetNextWindowDockID with a dead id would re-"dock" the window into a phantom node that keeps the
            // bar's size. If it is gone, stay floating; DrawElement restores the saved floating size.
            if (sSavedDockId != 0 && ImGui::DockBuilderGetNode((ImGuiID)sSavedDockId) != nullptr) {
                ImGui::SetNextWindowDockID((ImGuiID)sSavedDockId, ImGuiCond_Always);
            }
            sPendingDock = 0;
        }
    }

    bool camActive = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0) || sPlaying || sPreview;
    if (!camActive) {
        if (sPerfOn) {
            sPerfUpdateMs = CineNowMs() - upT0;
        }
        return;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);

    // Letterbox bars. The Letterbox automation track overrides the manual setting while it drives (and shows
    // the bars even with the checkbox off, so the amount can be animated from zero).
    if (CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Letterbox"), 0) || sLetterboxOverride >= 0.0f) {
        float amount = (sLetterboxOverride >= 0.0f)
                           ? sLetterboxOverride
                           : CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.LetterboxAmount"), 0.12f);
        if (amount > 0.0f) {
            float barH = vp->Size.y * amount;
            dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + barH), IM_COL32(0, 0, 0, 255));
            dl->AddRectFilled(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - barH),
                              ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 255));
        }
    }

    // Composition grid (0=off, 1=3x3, 2=4x4, 3=5x5).
    int gi = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Grid"), 0);
    if (gi > 0) {
        int div = gi + 2; // 1->3, 2->4, 3->5
        ImU32 col = IM_COL32(255, 255, 255, 70);
        for (int i = 1; i < div; i++) {
            float x = vp->Pos.x + vp->Size.x * (float)i / (float)div;
            float y = vp->Pos.y + vp->Size.y * (float)i / (float)div;
            dl->AddLine(ImVec2(x, vp->Pos.y), ImVec2(x, vp->Pos.y + vp->Size.y), col, 1.0f);
            dl->AddLine(ImVec2(vp->Pos.x, y), ImVec2(vp->Pos.x + vp->Size.x, y), col, 1.0f);
        }
    }

    // Live readout: state + FOV / roll / move speed in the corner while flying (toggle in the menu).
    if (CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.ShowReadout"), 1)) {
        float eye[3], at[3], roll, fov;
        CinematicCam_GetPose(eye, at, &roll, &fov);
        float spd = CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.MoveSpeed"), 30.0f);
        const char* mode = sRecording ? "REC" : (sPlaying ? "PLAY" : (sPreview ? "PREVIEW" : "FREE CAM"));
        char buf[128];
        snprintf(buf, sizeof(buf), "%s   FOV %.0f   Roll %.0f   Speed %.0f", mode, fov, roll, spd);
        float barOff = 0.0f;
        if (CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Letterbox"), 0)) {
            barOff = vp->Size.y * CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.LetterboxAmount"), 0.12f);
        }
        ImVec2 tp(vp->Pos.x + 16.0f, vp->Pos.y + barOff + 12.0f);
        dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), buf); // shadow for readability
        dl->AddText(tp, IM_COL32(255, 255, 255, 210), buf);
    }

    // Loop-start marker: a solid magenta square shown for the single frame the loop restarts, so the loop
    // boundary is findable in a recorded clip. Opt-in; the user trims that frame in post.
    if (sLoopMarkerFrame && CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.LoopStartMarker"), 0)) {
        ImVec2 a(vp->Pos.x + 8.0f, vp->Pos.y + 8.0f);
        ImVec2 b(a.x + 40.0f, a.y + 40.0f);
        dl->AddRectFilled(a, b, IM_COL32(255, 0, 255, 255));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
    }

    if (sPerfOn) {
        sPerfUpdateMs = CineNowMs() - upT0;
    }
}
