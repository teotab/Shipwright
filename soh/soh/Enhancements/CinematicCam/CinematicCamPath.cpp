#include "CinematicCamPath.h"

#include <imgui.h>
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

// Evaluate a track at a time. Returns false (no value) when the track is disabled or empty. Clamps before the
// first / after the last key. The segment uses the LEFT key's interpolation: step (hold), linear, or smooth
// (uniform Catmull-Rom through the neighbors).
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
    const CineParamKey& a = t.keys[i];
    const CineParamKey& b = t.keys[i + 1];
    float d = b.time - a.time;
    float s = (d > 1e-5f) ? (time - a.time) / d : 0.0f;
    int mode = KeyInterp(t, (int)i);
    if (mode == CINE_TRACK_STEP) {
        out = a.value;
    } else if (mode == CINE_TRACK_SMOOTH) {
        float p0 = (i > 0) ? t.keys[i - 1].value : a.value;
        float p1 = a.value;
        float p2 = b.value;
        float p3 = (i + 2 < t.keys.size()) ? t.keys[i + 2].value : b.value;
        float s2 = s * s, s3 = s2 * s;
        out = 0.5f * ((2.0f * p1) + (-p0 + p2) * s + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * s2 +
                      (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * s3);
    } else {
        out = a.value + (b.value - a.value) * s; // linear
    }
    return true;
}

// Insert (or overwrite a near-coincident) key, keeping the track sorted by time. New keys inherit the track's
// default interpolation (interp = -1).
static void TrackAddKey(CineParamTrack& t, float time, float value) {
    for (CineParamKey& k : t.keys) {
        if (std::fabs(k.time - time) < 1e-3f) {
            k.value = value;
            return;
        }
    }
    t.keys.push_back({ time, value, -1 });
    std::sort(t.keys.begin(), t.keys.end(),
              [](const CineParamKey& a, const CineParamKey& b) { return a.time < b.time; });
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
static int sAimOverride = 0; // 0 none, 1 Link, 2 point, 3 actor
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
};
static std::vector<PathSnapshot> sUndo;
static std::vector<PathSnapshot> sRedo;

static PathSnapshot MakeSnapshot() {
    PathSnapshot s{ sKeyframes, sIds, sSelectedId, sSelection, {} };
    for (const TrackDef& d : AllTrackDefs()) {
        s.tracks.push_back(d.track->keys);
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
    }
}

// Assumed game logic tick rate; playback advances this many seconds per OnCameraState call.
static const float kTickSeconds = 1.0f / 20.0f;

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

static void PushUndo() {
    sUndo.push_back(MakeSnapshot());
    if (sUndo.size() > 64) {
        sUndo.erase(sUndo.begin());
    }
    sRedo.clear();
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

// Endpoint slopes for one aim angle (yaw or pitch) over segment b->c, in [0,1]-parameter space. Authority
// order: automatic from the neighbours -> the keyframes' baked explicit rates (exOut/exIn, from Insert) ->
// the envelope clamp -> Hold framing.
//
// The limiting here is deliberately gentler than the roll/FOV rule above, and that difference IS the fix for
// the aim stalling at keyframes: `noPrev` / `noNext` mean there is no meaningful neighbour on that side (a
// clamped path end, a locked tracked aim), so this segment continues at its OWN rate instead of pretending
// the view was standing still there. The clamp keeps both slopes pointing the way this segment actually
// turns, no steeper than 3x its own rate: monotone, so the view can never swing past either keyframe's
// framing. (For exact bakes the clamp is a no-op - a monotone cubic's slopes already satisfy it.)
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
    if (b.hasAimTan) {
        *mOut = exOut;
    }
    if (c.hasAimTan) {
        *mIn = exIn;
    }
    float secant = p2 - p1; // this segment's straight-line slope, in [0,1] parameter space
    float lim = 3.0f * std::fabs(secant);
    if (secant >= 0.0f) {
        *mOut = std::min(std::max(*mOut, 0.0f), lim);
        *mIn = std::min(std::max(*mIn, 0.0f), lim);
    } else {
        *mOut = std::min(std::max(*mOut, -lim), 0.0f);
        *mIn = std::min(std::max(*mIn, -lim), 0.0f);
    }
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
    // Per-SEGMENT endpoint speeds (units/sec): the knot slopes above, with each keyframe's explicit rate
    // substituted where it has one, clamped to 3x this segment's own average (the bound that keeps distance
    // monotone in time - i.e. that keeps the camera arriving exactly on its keyframes).
    std::vector<float> mOut, mIn;
};
static CineArcCache sArc;

// Monotone (Fritsch-Carlson) knot slopes for a cumulative-quantity-over-time schedule (arc length for the
// eye, view angle for the aim). All secants are >= 0, so limiting each knot's slope to 3x the smaller
// adjacent secant guarantees the quantity never runs backwards; a zero-length segment (a hold) forces slope 0
// on both of its ends, so motion eases to rest into it and out of it. When cyclic, knot 0 and the last knot
// are the SAME keyframe: give them the same wrapped slope so the rate glides through the loop seam instead of
// popping to a new pace each lap.
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
            float lim = 3.0f * std::min(sigPrev, sigNext);
            m = std::min(m, lim);
        }
        slope[i] = std::max(m, 0.0f);
    }
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
    // Per-segment endpoint speeds: start from the automatic knot slopes, substitute each keyframe's explicit
    // rate for the side it governs, then clamp to 3x THIS segment's own average speed. The clamp is what keeps
    // the camera arriving exactly on the next keyframe; automatic values already satisfy it, so only dragged
    // handles are ever limited (and the handle draws at the limited value, so the graph shows the bound).
    sArc.mOut.assign((size_t)std::max(sArc.segs, 0), 0.0f);
    sArc.mIn.assign((size_t)std::max(sArc.segs, 0), 0.0f);
    for (int i = 0; i < sArc.segs; i++) {
        float sig = (sArc.S[i + 1] - sArc.S[i]) / SegDurAt(i);
        float cap = 3.0f * sig;
        float exOut = (n > 0) ? sKeyframes[i % n].speedRateOut : -1.0f;
        float exIn = (n > 0) ? sKeyframes[(i + 1) % n].speedRateIn : -1.0f;
        float mo = (exOut >= 0.0f) ? exOut : sArc.slope[i];
        float mi = (exIn >= 0.0f) ? exIn : sArc.slope[i + 1];
        sArc.mOut[i] = std::min(std::max(mo, 0.0f), cap);
        sArc.mIn[i] = std::min(std::max(mi, 0.0f), cap);
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
    float d = Hermite1(sArc.S[i1], sArc.S[i1 + 1], sArc.mOut[i1] * dur, sArc.mIn[i1] * dur, p) - sArc.S[i1];
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

// The aim's value curves for segment i1: endpoint yaw/pitch (radians, yaw unwrapped the short way round),
// look-at distances, and the four Hermite slopes with explicit bakes, the envelope clamp and holds applied.
// Returns false when the segment has no direction to interpolate (degenerate look-at on an endpoint).
// Shared by evaluation (AimPointAt) and by Insert @ playhead's bake, so what gets baked is exactly what
// renders.
static bool AimSegmentCurve(int i1, float* oyB, float* oyC, float* opB, float* opC, float* moY, float* miY, float* moP,
                            float* miP, float* olenB, float* olenC) {
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
    const float kD2R = kPi / 180.0f; // stored explicit rates are deg/s; slopes are per [0,1] segment param
    AimAngleSlopes(yA, yB, yC, yD, at01, at12, at23, b, c, noPrev, noNext, b.aimTanYawOut * kD2R * at12,
                   c.aimTanYawIn * kD2R * at12, moY, miY);
    AimAngleSlopes(pA, pB, pC, pD, at01, at12, at23, b, c, noPrev, noNext, b.aimTanPitchOut * kD2R * at12,
                   c.aimTanPitchIn * kD2R * at12, moP, miP);
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
        yaw = Hermite1(yB, yC, moY, miY, pAim);
        pitch = Hermite1(pB, pC, moP, miP, pAim);
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

    // Per-keyframe timing ease: reparametrize the segment so the camera slows leaving b (b.easeOut) and/or
    // slows arriving at c (c.easeIn). A cubic with adjustable start/end slopes; slope 0 = fully eased (hold).
    if (b.easeOut > 0.0f || c.easeIn > 0.0f) {
        float m0 = 1.0f - std::min(std::max(b.easeOut, 0.0f), 1.0f);
        float m1 = 1.0f - std::min(std::max(c.easeIn, 0.0f), 1.0f);
        lt = Hermite1(0.0f, 1.0f, m0, m1, lt);
        lt = std::min(std::max(lt, 0.0f), 1.0f);
    }

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
        h = HashF32(h, k.easeIn);
        h = HashF32(h, k.easeOut);
        h = HashF32(h, k.speedRateIn); // these drive the arc schedule, which this hash keys
        h = HashF32(h, k.speedRateOut);
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
static void AddKeyframeAtPlayhead(const CineKeyframe& kf) {
    PushUndo();
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
    if (sKeyframes.size() >= 2) {
        kf = SampleAt(sPlayhead); // a control point on the existing curve (shape preserved)
    } else {
        CinematicCam_GetPose(kf.eye, kf.at, &kf.roll, &kf.fov);
    }
    AddKeyframeAtPlayhead(kf);
}

// Serialize / restore a parameter track to the path file. Generic so future tracks reuse it.
static nlohmann::json TrackToJson(const CineParamTrack& t) {
    nlohmann::json keys = nlohmann::json::array();
    for (const CineParamKey& k : t.keys) {
        keys.push_back({ { "time", k.time }, { "value", k.value }, { "interp", k.interp } });
    }
    return { { "enabled", t.enabled }, { "keys", keys } };
}

static void TrackFromJson(CineParamTrack& t, const nlohmann::json& j) {
    t.keys.clear();
    t.enabled = j.value("enabled", false);
    if (j.contains("keys")) {
        for (const auto& e : j["keys"]) {
            t.keys.push_back({ e.value("time", 0.0f), e.value("value", 0.0f), e.value("interp", -1) });
        }
        std::sort(t.keys.begin(), t.keys.end(),
                  [](const CineParamKey& a, const CineParamKey& b) { return a.time < b.time; });
    }
}

static void SavePath() {
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
                        { "hasAimTan", k.hasAimTan },
                        { "aimTan", { k.aimTanYawIn, k.aimTanYawOut, k.aimTanPitchIn, k.aimTanPitchOut } },
                        { "speedRate", { k.speedRateIn, k.speedRateOut, (float)k.speedBroken } },
                        { "aimActorId", k.aimActorId },
                        { "aimActorPos", { k.aimActorPos[0], k.aimActorPos[1], k.aimActorPos[2] } },
                        { "easeIn", k.easeIn },
                        { "easeOut", k.easeOut } });
    }
    // Object wrapper carries path-level state (the shared aim target) alongside the keyframes.
    nlohmann::json j;
    j["keyframes"] = arr;
    j["target"] = { sAimOverridePoint[0], sAimOverridePoint[1], sAimOverridePoint[2] };
    // Optionally bind the current location (entrance = scene + spawn) so loading the path warps you back here.
    sPathEntrance = sBindLocation ? CinematicCam_GetCurrentEntrance() : -1;
    j["entrance"] = sPathEntrance;
    // Parameter automation tracks (each keyed by its stable id).
    for (const TrackDef& d : AllTrackDefs()) {
        j["tracks"][d.track->id] = TrackToJson(*d.track);
    }
    std::filesystem::create_directories("cinematics");
    std::ofstream f(std::string("cinematics/") + sFilename + ".json");
    if (f.good()) {
        f << j.dump(2);
    }
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
        k.hasAimTan = e.value("hasAimTan", 0);
        if (e.contains("aimTan") && e["aimTan"].size() >= 4) {
            k.aimTanYawIn = e["aimTan"][0];
            k.aimTanYawOut = e["aimTan"][1];
            k.aimTanPitchIn = e["aimTan"][2];
            k.aimTanPitchOut = e["aimTan"][3];
        }
        if (e.contains("speedRate") && e["speedRate"].is_array() && e["speedRate"].size() >= 3) {
            k.speedRateIn = e["speedRate"][0];
            k.speedRateOut = e["speedRate"][1];
            k.speedBroken = (int)(float)e["speedRate"][2];
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
        k.easeIn = e.value("easeIn", 0.0f);
        k.easeOut = e.value("easeOut", 0.0f);
        sKeyframes.push_back(k);
        sIds.push_back(sNextId++);
    }
    SortByTime();
    SelectOnly(sIds.empty() ? -1 : sIds[0]);

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
    if ((!cyc && m < 2) || nk > 64) {
        return; // nothing to bend, or beyond the solver's buffer
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
    float M[3][64]; // solved knot velocities (dEye/dt in the global centripetal parameter), per axis
    if (cyc) {
        // Periodic C2 system: every knot is interior, indices wrap. Solved as a small dense system (n <= 64,
        // runs once per button press - no need for a specialised cyclic solver).
        for (int ax = 0; ax < 3; ax++) {
            static float A[64][65];
            std::memset(A, 0, sizeof(A));
            for (int i = 0; i < nk; i++) {
                int ip = (i - 1 + nk) % nk, in2 = (i + 1) % nk;
                float hp = h[ip], hn = h[i];
                float Pm = sKeyframes[ip].eye[ax], P0 = sKeyframes[i].eye[ax], Pp = sKeyframes[in2].eye[ax];
                A[i][ip] += 1.0f / hp;
                A[i][i] += 2.0f * (1.0f / hp + 1.0f / hn);
                A[i][in2] += 1.0f / hn;
                A[i][nk] = 3.0f * ((P0 - Pm) / (hp * hp) + (Pp - P0) / (hn * hn));
            }
            for (int col = 0; col < nk; col++) { // Gaussian elimination with partial pivoting
                int piv = col;
                for (int r = col + 1; r < nk; r++) {
                    if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) {
                        piv = r;
                    }
                }
                for (int cc = 0; cc <= nk; cc++) {
                    std::swap(A[col][cc], A[piv][cc]);
                }
                if (std::fabs(A[col][col]) < 1e-9f) {
                    continue;
                }
                for (int r = 0; r < nk; r++) {
                    if (r == col) {
                        continue;
                    }
                    float w = A[r][col] / A[col][col];
                    for (int cc = col; cc <= nk; cc++) {
                        A[r][cc] -= w * A[col][cc];
                    }
                }
            }
            for (int i = 0; i < nk; i++) {
                M[ax][i] = (std::fabs(A[i][i]) > 1e-9f) ? A[i][nk] / A[i][i] : 0.0f;
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
            M[ax][nk - 1] = rhs[nk - 1] / diagB[nk - 1];
            for (int i = nk - 2; i >= 0; i--) {
                M[ax][i] = (rhs[i] - diagC[i] * M[ax][i + 1]) / diagB[i];
            }
        }
    }
    // Bake phase 1: write the solved DIRECTIONS (weights cleared so the magnitude measurement below is
    // unweighted). Cyclic solves bake every knot; open solves bake the interior only (ends are frozen).
    int bake0 = cyc ? 0 : 1, bake1 = cyc ? nk - 1 : nk - 2;
    for (int i = bake0; i <= bake1; i++) {
        CineKeyframe& k = sKeyframes[lo + i];
        float dir[3] = { M[0][i], M[1][i], M[2][i] };
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
        float mag = std::sqrt(M[0][i] * M[0][i] + M[1][i] * M[1][i] + M[2][i] * M[2][i]);
        if (autoOut > 1e-5f) {
            k.tanWOut = std::min(std::max(mag * hOut / autoOut, 0.1f), 4.0f);
        }
        if (autoIn > 1e-5f) {
            k.tanWIn = std::min(std::max(mag * hIn / autoIn, 0.1f), 4.0f);
        }
    }
}

// Arc length of the spline between keyframe i and i+1 (geometry is independent of timing, so we sample by
// the segment's current time span). Used by speed normalization.
static float SegmentArcLength(int i) {
    const int kSub = 24;
    float t0 = sKeyframes[i].time, t1 = sKeyframes[i + 1].time;
    if (t1 - t0 < 1e-4f) {
        float d[3] = { sKeyframes[i + 1].eye[0] - sKeyframes[i].eye[0], sKeyframes[i + 1].eye[1] - sKeyframes[i].eye[1],
                       sKeyframes[i + 1].eye[2] - sKeyframes[i].eye[2] };
        return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    float len = 0.0f;
    CineKeyframe prev = SampleAt(t0);
    for (int s = 1; s <= kSub; s++) {
        float t = t0 + (t1 - t0) * (float)s / (float)kSub;
        CineKeyframe cur = SampleAt(t);
        float d[3] = { cur.eye[0] - prev.eye[0], cur.eye[1] - prev.eye[1], cur.eye[2] - prev.eye[2] };
        len += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        prev = cur;
    }
    return len;
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
        sKeyframes[i].speedRateIn = -1.0f;
        sKeyframes[i].speedRateOut = -1.0f;
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
        ImVec2 prev;
        bool prevValid = false;
        for (int i = 0; i <= steps; i++) {
            CineKeyframe s = SampleAt(total * (float)i / (float)steps);
            ImVec2 sp;
            if (WorldToScreen(s.eye, sp)) {
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
    ImGui::TextDisabled("List frozen while open - press Refresh to re-read positions.");

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
    static int sTlDragMode = 0;          // 0 none, 1 move selection, 2 ripple (this kf + everything after)
    static float sTlGrabTime0 = 0.0f;    // timeline time under the cursor when the drag began
    static float sTlGrabbedT0 = 0.0f;    // original time of the grabbed marker (ripple threshold)
    static std::vector<int> sTlDragIds;  // snapshot of all ids at drag start...
    static std::vector<float> sTlDragT0; // ...and their original times
    static bool sTlNoDrag = false;       // true for a ctrl-click (toggle select, don't move)
    static float sTlViewDur = 0.0f;      // displayed ruler length in seconds (decoupled from content)
    static bool sTlAutoFit = true;       // keep the ruler fit to the path when not zoomed manually
    static int sTlTrackDrag = -1;        // automation lane whose key is being dragged (-1 = none)
    static int sTlKeyDrag = -1;          // key index within that track
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

    // Interaction: in the Camera lane, click/drag markers (ctrl-click multi-select, shift-drag ripple). In an
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
                if (io.KeyCtrl) {
                    ToggleSelect(hitId); // add/remove from the multi-selection, no drag
                    sTlNoDrag = true;
                } else {
                    if (!IsSelected(hitId)) {
                        SelectOnly(hitId); // clicking an unselected marker selects just it
                    } else {
                        sSelectedId = hitId; // keep the group, make this the primary
                    }
                    PushUndo();
                    sTlDragMode = io.KeyShift ? 2 : 1;
                    sTlGrabTime0 = xToTime(mx);
                    sTlGrabbedT0 = sKeyframes[hit].time;
                    sTlDragIds = sIds;
                    sTlDragT0.resize(n);
                    for (int i = 0; i < n; i++) {
                        sTlDragT0[i] = sKeyframes[i].time;
                    }
                }
            } else {
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
            } else {
                sTlScrub = true;
                sPreview = true;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            }
        } else if (lane == -1) {
            sTlScrub = true; // ruler / empty area below the lanes
            sPreview = true;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }
    if (ImGui::IsItemActive()) {
        if (sTlDragMode != 0) {
            float delta = xToTime(mx) - sTlGrabTime0;
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
                float nt = xToTime(mx);
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
        sTlDragMode = 0;
        sTlScrub = false;
        sTlNoDrag = false;
        sTlTrackDrag = -1;
        sTlKeyDrag = -1;
    }

    float total = content;

    // Zoom controls for the ruler (manual zoom turns off auto-fit; Fit re-enables it).
    if (ImGui::SmallButton("-##tlzoom")) {
        sTlAutoFit = false;
        sTlViewDur *= 1.35f;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Zoom out (show more time)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+##tlzoom")) {
        sTlAutoFit = false;
        sTlViewDur /= 1.35f;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Zoom in (more precise dragging)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) {
        sTlAutoFit = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto-fit the ruler to the path length");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
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
        ImGui::SetTooltip("Jump to the previous keyframe");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("< tick")) {
        setPlayhead(sPlayhead - kTickSeconds);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Step back one tick (1/20 s)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("tick >")) {
        setPlayhead(sPlayhead + kTickSeconds);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Step forward one tick (1/20 s)");
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
        ImGui::SetTooltip("Jump to the next keyframe");
    }
    ImGui::SameLine();
    ImGui::Text("%.2fs / %.2fs%s", sPlayhead, total, sLoop ? " (loop)" : "");
    if (SelectionCount() > 1) {
        ImGui::TextDisabled("%d keyframes selected - drag any to move them together.", SelectionCount());
    } else {
        ImGui::TextDisabled("Camera: drag = move, Ctrl+click = multi-select, Shift+drag = ripple. Track keys: drag to "
                            "retime (add/remove via each parameter's keyframe button or the curve editor).");
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
        t.enabled = !t.enabled;
        if (t.enabled && t.keys.empty()) {
            PushUndo();
            TrackAddKey(t, sPlayhead, value); // seed a key so it holds the current value
        }
    }
    if (wasEnabled) {
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keyframe this parameter (animate it over the timeline).");
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
        ImGui::SetTooltip(atIdx >= 0 ? "Remove keyframe at playhead" : "Add keyframe at playhead");
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

// Curve editor: a value-over-time graph overlaying the enabled continuous parameter tracks (time of day, shake,
// target X/Y/Z) and any camera channels (Eye / Look-at / Roll / FOV) switched on via the "Camera:" chips. Drag a
// parameter point in 2D to retime + revalue it (right-click to delete; "Add key at playhead" drops one); camera
// points are locked in time to their keyframe, so dragging only changes the value. The dope sheet handles
// retiming/overview; this panel is where you shape the value.
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
        ImGui::SetTooltip("Keyframe the target position (animate it over the timeline); edit per-axis in the Curve "
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

// --- Camera channels (curve editor) -----------------------------------------------------------------------
// The curve editor can graph the SCALAR camera values - Roll and FOV - as value-over-time channels, read from the
// camera keyframes and LOCKED in time to their keyframe (you retime/add/remove them on the main timeline); only
// the value is editable here. Eye / Look-at are deliberately NOT channels: positions belong to the world gizmos,
// and graphing them just clutters the editor. Labeled "Cam:" to stay distinct from the "Target X/Y/Z" tracks.
enum CamChan { CAM_ROLL = 0, CAM_FOV, CAM_CHAN_COUNT };
static const char* kCamChanName[CAM_CHAN_COUNT] = { "Cam: Roll", "Cam: FOV" };
static const char* kCamChanShort[CAM_CHAN_COUNT] = { "Roll", "FOV" };
static const ImU32 kCamPalette[CAM_CHAN_COUNT] = { IM_COL32(205, 140, 240, 255), IM_COL32(150, 235, 130, 255) };
static float CamChanGet(const CineKeyframe& k, int c) {
    return (c == CAM_FOV) ? k.fov : k.roll;
}
static void CamChanSet(CineKeyframe& k, int c, float v) {
    if (c == CAM_FOV) {
        k.fov = v;
    } else {
        k.roll = v;
    }
}

static void DrawCurveEditor() {
    // A channel is either a parameter automation track (free keys on its own sub-timeline) or a CAMERA channel
    // (Eye / Look-at / Roll / FOV) read straight from the camera keyframes. Camera points are locked in time to
    // their keyframe - you retime/add/remove them on the main timeline; here only their VALUE is editable, letting
    // you fine-tune a position or FOV against the curve. Both kinds overlay together, each scaled to its own range.
    struct CurveChannel {
        const char* name;
        ImU32 col;
        bool isCam;
        CineParamTrack* track; // parameter channel (null for camera channels)
        int camChan;           // CamChan index (camera channels; -1 otherwise)
        float vmin, vmax;      // fixed range (vmax > vmin) or 0,0 = auto-fit to the keys
    };
    static const ImU32 kChanCol[6] = { IM_COL32(120, 200, 255, 255), IM_COL32(255, 180, 90, 255),
                                       IM_COL32(150, 230, 120, 255), IM_COL32(230, 130, 230, 255),
                                       IM_COL32(240, 220, 90, 255),  IM_COL32(120, 230, 230, 255) };
    static bool sCamShow[CAM_CHAN_COUNT] = { false, false };

    // Camera-channel visibility chips - always offered (even with no parameter track enabled) so you can pull a
    // camera curve up on its own. A lit chip is overlaid; click its legend entry below to make it editable.
    ImGui::TextDisabled("Camera:");
    ImGui::SameLine();
    for (int c = 0; c < CAM_CHAN_COUNT; c++) {
        ImGui::PushID(2000 + c);
        bool on = sCamShow[c];
        ImGui::PushStyleColor(ImGuiCol_Text,
                              on ? ImGui::ColorConvertU32ToFloat4(kCamPalette[c]) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        if (ImGui::SmallButton(kCamChanShort[c])) {
            sCamShow[c] = !sCamShow[c];
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (c != CAM_CHAN_COUNT - 1) {
            ImGui::SameLine();
        }
    }

    std::vector<CurveChannel> curves;
    int pc = 0;
    for (const TrackDef& d : AllTrackDefs()) {
        if (d.continuous && d.track->enabled) {
            curves.push_back({ d.name, kChanCol[pc % 6], false, d.track, -1, d.vmin, d.vmax });
            pc++;
        }
    }
    for (int c = 0; c < CAM_CHAN_COUNT; c++) {
        if (sCamShow[c] && !sKeyframes.empty()) {
            // FOV is genuinely bounded, so a fixed range gives the graph height a sensible, precise scale. Roll is
            // left AUTO-fit (range floored to +-180 in rangeOf) so the dial reads naturally but barrel rolls past a
            // half-turn still expand the view instead of clamping.
            float cvmin = 0.0f, cvmax = 0.0f;
            if (c == CAM_FOV) {
                cvmin = 1.0f;
                cvmax = 120.0f;
            }
            curves.push_back({ kCamChanName[c], kCamPalette[c], true, nullptr, c, cvmin, cvmax });
        }
    }
    if (curves.empty()) {
        ImGui::TextDisabled("Enable a continuous parameter (Time of day, Shake, Target X/Y/Z) or switch on a camera "
                            "channel above to shape its curve.");
        return;
    }

    // The active (editable) channel is tracked by identity, so toggling another channel's visibility doesn't shift
    // which curve you're editing.
    static bool sActIsCam = false;
    static CineParamTrack* sActTrack = nullptr;
    static int sActCam = -1;
    static int keySel = -1;
    static int drag = -1;
    static float sDragLo = 0.0f, sDragHi = 1.0f; // active channel's value range, frozen for the duration of a drag
    static bool sDragRange = false;
    int active = -1;
    for (int i = 0; i < (int)curves.size(); i++) {
        if (curves[i].isCam == sActIsCam && (sActIsCam ? curves[i].camChan == sActCam : curves[i].track == sActTrack)) {
            active = i;
            break;
        }
    }
    if (active < 0) { // the previously active channel is gone (disabled/hidden) - fall back and reset selection
        active = 0;
        keySel = -1;
        drag = -1;
    }

    // Legend: every visible channel; click one to make it the editable (bright) curve, the rest stay faded.
    for (int i = 0; i < (int)curves.size(); i++) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(curves[i].col));
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s%s", (i == active) ? "* " : "", curves[i].name);
        if (ImGui::SmallButton(lbl)) {
            active = i;
            keySel = -1;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
    { // remember the active channel's identity for next frame
        const CurveChannel& A = curves[active];
        sActIsCam = A.isCam;
        sActTrack = A.track;
        sActCam = A.camChan;
    }
    const CurveChannel& AL = curves[active];

    float total = EffectiveTotal();
    if (total < 0.001f) {
        total = 1.0f;
    }

    // Generic key accessors spanning both channel kinds.
    auto keyCount = [&](const CurveChannel& C) -> int {
        return C.isCam ? (int)sKeyframes.size() : (int)C.track->keys.size();
    };
    auto keyTime = [&](const CurveChannel& C, int i) -> float {
        return C.isCam ? sKeyframes[i].time : C.track->keys[i].time;
    };
    auto keyValue = [&](const CurveChannel& C, int i) -> float {
        return C.isCam ? CamChanGet(sKeyframes[i], C.camChan) : C.track->keys[i].value;
    };
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
        if (C.isCam && C.camChan == CAM_ROLL) { // floor the roll view to a full half-turn each way (barrel rolls
            lo = std::min(lo, -180.0f);         // past +-180 still expand it; they're never clamped)
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
    if (keySel >= activeN) {
        keySel = -1;
    }

    float graphH = ImGui::GetContentRegionAvail().y - 30.0f; // leave a row for the toolbar below
    if (graphH < 80.0f) {
        graphH = 80.0f;
    }
    ImVec2 size(ImGui::GetContentRegionAvail().x, graphH);
    if (size.x < 80.0f) {
        size.x = 80.0f;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##curvegraph", size);
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 27, 255), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 95, 255), 4.0f);
    float gx0 = p0.x + 6.0f, gx1 = p1.x - 6.0f, gy0 = p0.y + 8.0f, gy1 = p1.y - 8.0f;
    auto timeToX = [&](float t) { return gx0 + (t / total) * (gx1 - gx0); };
    auto xToTime = [&](float x) {
        float u = (x - gx0) / (gx1 - gx0);
        return std::min(std::max(u, 0.0f), 1.0f) * total;
    };
    auto valToY = [&](float v, float lo, float hi) {
        float sp = (hi > lo) ? (hi - lo) : 1.0f;
        float u = std::min(std::max((v - lo) / sp, 0.0f), 1.0f);
        return gy1 - u * (gy1 - gy0);
    };
    auto yToValActive = [&](float y) {
        float u = std::min(std::max((gy1 - y) / (gy1 - gy0), 0.0f), 1.0f);
        return vmin + u * vspan;
    };

    for (int i = 0; i <= 4; i++) {
        float yy = gy0 + (gy1 - gy0) * i / 4.0f;
        dl->AddLine(ImVec2(gx0, yy), ImVec2(gx1, yy), IM_COL32(44, 44, 48, 255));
    }
    char lab[24]; // Y labels are the ACTIVE channel's range (the overlays are normalized to their own ranges).
    snprintf(lab, sizeof(lab), "%g", vmax);
    dl->AddText(ImVec2(gx0 + 2.0f, gy0 - 1.0f), IM_COL32(150, 150, 155, 255), lab);
    snprintf(lab, sizeof(lab), "%g", vmin);
    dl->AddText(ImVec2(gx0 + 2.0f, gy1 - 13.0f), IM_COL32(150, 150, 155, 255), lab);

    float phx = timeToX(std::min(sPlayhead, total));
    dl->AddLine(ImVec2(phx, gy0), ImVec2(phx, gy1), IM_COL32(60, 255, 90, 150), 1.5f);

    // Camera channels share one pose sampling across the visible range (so the drawn curve matches what actually
    // plays back - ease and spline shaping included); computed once here and reused by every visible camera channel.
    bool anyCam = false;
    for (const CurveChannel& C : curves) {
        if (C.isCam) {
            anyCam = true;
            break;
        }
    }
    std::vector<float> camT;
    std::vector<CineKeyframe> camP;
    if (anyCam) {
        int ns = (int)((gx1 - gx0) / 3.0f);
        if (ns < 24) {
            ns = 24;
        }
        if (ns > 400) {
            ns = 400;
        }
        camT.reserve(ns + 1);
        camP.reserve(ns + 1);
        for (int s = 0; s <= ns; s++) {
            float tt = total * (float)s / (float)ns;
            camT.push_back(tt);
            camP.push_back(SampleAt(tt));
        }
    }

    // Draw every channel. Camera channels are a polyline sampled straight from the camera evaluation. Parameter
    // channels render PER SEGMENT so each interpolation type is exact: step = hold + vertical drop, linear = a
    // straight line, smooth = a polyline sub-sampled by the segment's on-screen width (so curves between close
    // keyframes stay smooth instead of collapsing to a line). Each channel is scaled to its own range; the active
    // one is bright, the rest are faded overlays.
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
        if (C.isCam) {
            ImVec2 prev(timeToX(camT[0]), valToY(CamChanGet(camP[0], C.camChan), lo, hi));
            for (size_t s = 1; s < camT.size(); s++) {
                ImVec2 cur(timeToX(camT[s]), valToY(CamChanGet(camP[s], C.camChan), lo, hi));
                dl->AddLine(prev, cur, col, w);
                prev = cur;
            }
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
                    float vv;
                    EvalParamTrack(*ct, tt, vv);
                    ImVec2 cur(timeToX(tt), valToY(vv, lo, hi));
                    dl->AddLine(prev, cur, col, w);
                    prev = cur;
                }
            }
        }
    }
    // Active channel's key points (draggable). Camera points are locked in time (vertical drag only).
    for (int i = 0; i < activeN; i++) {
        ImVec2 c(timeToX(keyTime(AL, i)), valToY(keyValue(AL, i), vmin, vmax));
        bool s = (i == keySel);
        dl->AddCircleFilled(c, s ? 5.5f : 4.0f, s ? IM_COL32(255, 220, 80, 255) : AL.col);
        dl->AddCircle(c, s ? 7.0f : 5.0f, IM_COL32(255, 255, 255, s ? 220 : 120));
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
    if (ImGui::IsItemActivated()) {
        int hit = nearestKey();
        if (hit >= 0) {
            keySel = hit;
            drag = hit;
            sDragLo = vmin; // snapshot the (still-live) range so this drag keeps a constant value-to-pixel scale
            sDragHi = vmax;
            sDragRange = true;
            PushUndo();
        } else {
            keySel = -1;
        }
    }
    if (ImGui::IsItemActive() && drag >= 0 && drag < activeN) {
        if (AL.isCam) {
            CamChanSet(sKeyframes[drag], AL.camChan, yToValActive(my)); // time stays locked to the keyframe
        } else {
            CineParamTrack* tr = AL.track;
            float lo = (drag > 0) ? tr->keys[drag - 1].time + 1e-3f : 0.0f;
            float hi = (drag < (int)tr->keys.size() - 1) ? tr->keys[drag + 1].time - 1e-3f : total;
            tr->keys[drag].time = std::min(std::max(xToTime(mx), lo), hi);
            tr->keys[drag].value = yToValActive(my);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        drag = -1;
        sDragRange = false;
    }
    // Right-click deletes a key - parameter channels only (camera keyframes are deleted on the main timeline).
    if (!AL.isCam && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hit = nearestKey();
        if (hit >= 0) {
            PushUndo();
            AL.track->keys.erase(AL.track->keys.begin() + hit);
            keySel = -1;
        }
    }

    // Toolbar. Parameter channels can add keys at the playhead and edit a selected key's interp / time / value /
    // delete. Camera channels are value-only here (their timing and existence live on the main timeline).
    if (!AL.isCam) {
        if (ImGui::SmallButton("Add key at playhead")) {
            float v;
            if (!EvalParamTrack(*AL.track, sPlayhead, v)) {
                v = (vmin + vmax) * 0.5f;
            }
            PushUndo();
            TrackAddKey(*AL.track, sPlayhead, v);
        }
        ImGui::SameLine();
    }
    if (keySel >= 0 && keySel < activeN) {
        if (AL.isCam) {
            ImGui::Text("%s  key %d", AL.name, keySel + 1);
            ImGui::SameLine();
            float kv = keyValue(AL, keySel);
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputFloat("val##cck", &kv, 0.0f, 0.0f, "%.3f")) {
                CamChanSet(sKeyframes[keySel], AL.camChan, kv);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("t=%.2f (locked - retime on the timeline)", keyTime(AL, keySel));
        } else {
            CineParamTrack* tr = AL.track;
            const char* im[] = { "Step", "Linear", "Smooth" };
            int mode = (tr->keys[keySel].interp < 0) ? tr->interp : tr->keys[keySel].interp;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##ckinterp", &mode, im, 3)) {
                tr->keys[keySel].interp = mode; // per-key spline type for the segment leaving this key
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Spline type for the segment after this key: Step / Linear / Smooth.");
            }
            ImGui::SameLine();
            float kt = tr->keys[keySel].time, kv = tr->keys[keySel].value;
            float lo = (keySel > 0) ? tr->keys[keySel - 1].time + 1e-3f : 0.0f;
            float hi = (keySel < (int)tr->keys.size() - 1) ? tr->keys[keySel + 1].time - 1e-3f : total;
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputFloat("t##ck", &kt, 0.0f, 0.0f, "%.2f")) {
                tr->keys[keySel].time = std::min(std::max(kt, lo), hi);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputFloat("val##ck", &kv, 0.0f, 0.0f, "%.2f")) {
                tr->keys[keySel].value = std::min(std::max(kv, vmin), vmax);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete##ck")) {
                PushUndo();
                tr->keys.erase(tr->keys.begin() + keySel);
                keySel = -1;
            }
        }
    } else {
        ImGui::TextDisabled("%s", AL.isCam ? "click point = select, drag = move value (time locked to keyframe)"
                                           : "click point = select, drag = move value/time, right-click = delete");
    }
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
        ImGui::TextUnformatted("Boost = RB, Precision = L, Ascend = R, Descend = Z, FOV = D-pad up/down, "
                               "Roll = D-pad left/right.");
        ImGui::Unindent();
        ImGui::BulletText("Timeline: drag a marker to move; Ctrl+click = multi-select; Shift+drag = ripple "
                          "(push this + later). -/+/Fit zoom the ruler.");
        ImGui::BulletText("Keyboard (this window focused): Space = play/stop, , / . = step a tick, "
                          "[ / ] = prev/next keyframe, K = add keyframe, Del = delete, Ctrl+Z / Ctrl+Y = undo/redo.");
    }
    ImGui::Separator();

    bool enabled = FreeCamEnabled();
    if (ImGui::Checkbox("Enable Free Camera", &enabled)) {
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), enabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Detached free camera. Left stick: move; right stick: look; plus rebindable "
                          "ascend/descend/boost/FOV/roll. Bindings are in Dev Tools > Cinematic Cam > Controls.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show path in world", &sShowPath);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw the spline, keyframe markers, and the editing gizmo over the game view.");
    }
    ImGui::SameLine();
    {
        bool diag = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.PerfDiag"), 0) != 0;
        if (ImGui::Checkbox("Perf diag", &diag)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.PerfDiag"), diag);
            sPerfSpikeCount = 0;
            sPerfPeakMs = 0.0;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Diagnose the freeze: shows a live frame-time HUD (top-right) splitting each frame into "
                              "our editor draw vs engine/GPU, and logs every hitch over 60 ms to the SoH log "
                              "(soh.log) with the current state.");
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

    // Keyboard shortcuts (only while this window is focused and not typing into a field).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
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
        if (ImGui::IsKeyPressed(ImGuiKey_Space) && sKeyframes.size() >= 2) {
            if (sPlaying) {
                sPlaying = false;
            } else {
                float pt = EffectiveTotal();
                if (sPlayhead >= pt) {
                    sPlayhead = 0.0f;
                }
                sPlayU = InvertEasedProgress((pt > 0.0f) ? (sPlayhead / pt) : 0.0f); // resume exactly at playhead
                sPlayDir = 1;
                if (sPlayhead == 0.0f && CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.SyncIdleAnim"), 0)) {
                    CinematicCam_SyncLinkIdleAnim(); // anchor Link's idle anim when starting from the top
                }
                sPlaying = true;
                sPreview = false;
                CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
            }
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
        if (ImGui::IsKeyPressed(ImGuiKey_K) && enabled) {
            AddKeyframe();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && SelectedIndex() >= 0) {
            DeleteSelected();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            Undo();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
            Redo();
        }
    }

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
    float cineCurveH = 280.0f;                                        // chips + legend + adaptive graph + toolbar
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
            ImGui::SetTooltip("Ride the actor's animated focus point (its head) so head bob/turn shows in the POV. "
                              "Falls back to the actor's base position for actors that don't set a focus point. "
                              "With this on, you'll usually want a lower Eye height.");
        }
        float h = CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.SpectateHeight"), 40.0f);
        if (ImGui::SliderFloat("Eye height", &h, -100.0f, 200.0f, "%.2f", ImGuiSliderFlags_NoRoundToFormat)) {
            CVarSetFloat(CVAR_ENHANCEMENT("CinematicCam.SpectateHeight"), h);
        }
        ImGui::TextDisabled("Locks the camera to the actor's viewpoint (aimed along its facing). "
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
            ImGui::TextDisabled("Not following. Pick an actor to attach the free camera to it.");
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
            ImGui::SetTooltip("The best of both: the controller plays Link normally while the camera rides the "
                              "followed actor and automatically keeps Link in frame. You give up manual camera "
                              "control during the take (it's hands-off). Turn off to fly/aim the camera yourself.");
        }
        if (ctrlLink) {
            ImGui::TextDisabled("Hands-off camera: follows the actor + auto-aims at Link. You play Link normally; "
                                "the world runs (this overrides Freeze World).");
        } else {
            ImGui::TextDisabled("The camera keeps its position relative to the actor as it moves - fly to set the "
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
        ImGui::TextDisabled("Fades to the chosen area (spawn point 0). Only works while in-game.");
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
            ImGui::SetTooltip("Replace the sky with a solid chroma-key color (and hide the sun/moon/sky glow) so you "
                              "can key it out when compositing. Scene geometry still renders over it.");
        }
        ImGui::SameLine();
        DrawParamKeyNav(sGreenScreenTrack, (float)greenScreen);

        bool freezeSky = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.FreezeSky"), 0);
        if (ImGui::Checkbox("Freeze sky & time", &freezeSky)) {
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.FreezeSky"), freezeSky);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stop cloud drift and time-of-day progression so looping clips/GIFs line up.");
        }

        int liveDt = CinematicCam_GetDayTime();
        if (liveDt < 0) {
            ImGui::TextDisabled("Time of day available in-game only.");
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
                ImGui::SetTooltip("Set the sun/moon position and lighting. Keyframe it for a sunrise/sunset across a "
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
            ImGui::TextDisabled("Available in-game only.");
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
            ImGui::TextDisabled("Drag the dial or type degrees to aim Link. Best while he stands idle.");
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
        ImGui::SetTooltip("Copy a keyframe first.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Insert @ playhead")) {
        InsertAtPlayhead();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add a keyframe at the current playhead time (on the existing curve, or the live "
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
            ImGui::SetTooltip("Enable the free camera first.");
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
                    ImGui::SetTooltip(
                        "The whole orbit tracks the target's movement during playback, keeping it framed.");
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
                ImGui::SetTooltip("Replace the path with a circle/arc of keyframes around the chosen center, each "
                                  "aimed at it. A full 360 arc turns on looping. Great for establishing shots.");
            }
        } // Auto-orbit collapsible
    }

    // Keyframe list
    ImGui::Text("Keyframes: %d", (int)sKeyframes.size());
    if (SelectionCount() > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d selected)", SelectionCount());
    }
    ImGui::BeginChild("##kflist", ImVec2(0, 160), true);
    // Clipper: only build widgets for visible rows (keeps long/recorded paths responsive).
    ImGuiListClipper clipper;
    clipper.Begin((int)sKeyframes.size());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            ImGui::PushID(i);
            char label[64];
            snprintf(label, sizeof(label), "#%d   t=%.2fs   %s", i + 1, sKeyframes[i].time,
                     sKeyframes[i].interp == CINE_INTERP_LINEAR ? "[Linear]" : "");
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
        ImGui::TextDisabled("%d keyframes selected. Per-keyframe fields are hidden while multiple are selected - "
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
                ImGui::SetTooltip("Higher = tighter/straighter through the keyframe; lower = rounder, wider arcs.");
            }
            sKeyframes[sel].tension = tens;

            float cont = sKeyframes[sel].continuity;
            ImGui::SliderFloat("Continuity", &cont, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("0 = smooth pass-through; away from 0 sharpens the corner at the keyframe.");
            }
            sKeyframes[sel].continuity = cont;

            float bias = sKeyframes[sel].bias;
            ImGui::SliderFloat("Bias", &bias, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Lean the curve toward the previous (+) or the next (-) keyframe (overshoot/undershoot).");
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

        // Per-keyframe timing ease: slow the camera arriving at / leaving this keyframe (set both high to
        // "hold" on it).
        float eIn = sKeyframes[sel].easeIn, eOut = sKeyframes[sel].easeOut;
        ImGui::SetNextItemWidth(130.0f);
        ImGui::SliderFloat("Ease in", &eIn, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemActivated()) {
            PushUndo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Decelerate as the camera arrives at this keyframe (slows the segment before it).");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        ImGui::SliderFloat("Ease out", &eOut, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemActivated()) {
            PushUndo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Accelerate gently as the camera leaves this keyframe (slows the segment after it). "
                              "Set Ease in + Ease out high to pause on this keyframe.");
        }
        sKeyframes[sel].easeIn = eIn;
        sKeyframes[sel].easeOut = eOut;
        if ((eIn > 0.0f || eOut > 0.0f)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear ease")) {
                PushUndo();
                sKeyframes[sel].easeIn = 0.0f;
                sKeyframes[sel].easeOut = 0.0f;
            }
        }

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
            if (sKeyframes[sel].hasAimTan) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset aim rates")) {
                    PushUndo();
                    sKeyframes[sel].hasAimTan = 0;
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
            ImGui::TextDisabled("Drag the orange crosshair in the world to place the target.");
        } else if (sKeyframes[sel].aimMode == CINE_AIM_PLAYER) {
            ImGui::TextDisabled("Tracks Link's position (live during playback).");
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
            ImGui::TextDisabled("Tracks the actor live. Saved by id (re-acquired on load).");
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
            ImGui::TextDisabled("Aims at the shared movable target (one point for the whole path). Keyframe it to "
                                "animate it; edit each axis in the Curve editor (Target X/Y/Z).");
        }

        // Numeric fields: type exact position/orientation values for the selected keyframe.
        ImGui::Checkbox("Numeric fields", &sShowFields);
        if (sShowFields) {
            CineKeyframe& kf = sKeyframes[sel];

            float pos[3] = { kf.eye[0], kf.eye[1], kf.eye[2] };
            bool posCh = ImGui::InputFloat3("Position", pos, "%.1f");
            if (ImGui::IsItemActivated()) {
                PushUndo();
            }
            if (posCh) {
                float dxp = pos[0] - kf.eye[0], dyp = pos[1] - kf.eye[1], dzp = pos[2] - kf.eye[2];
                kf.eye[0] = pos[0];
                kf.eye[1] = pos[1];
                kf.eye[2] = pos[2];
                kf.at[0] += dxp; // move the look-at rigidly with the eye
                kf.at[1] += dyp;
                kf.at[2] += dzp;
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
        ImGui::SetTooltip("While a path plays, the controller moves Link and the world keeps running "
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
            ImGui::SetTooltip("Recenter the follow offset on the target's current position.");
        }
    }

    if (sLoop) {
        ImGui::SetNextItemWidth(160.0f);
        const char* loopModes[] = { "Forward (wrap)", "Ping-pong (reverse)" };
        ImGui::Combo("Loop style", &sLoopMode, loopModes, 2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Forward: glide from the last keyframe back to the first and repeat. "
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
            ImGui::SetTooltip("Flash a magenta square in the top-left for the single frame each loop restarts. "
                              "Lets you find the exact loop boundary in a recording and trim there (off by "
                              "default - it's only an editing aid, delete that frame in post).");
        }
    }

    ImGui::SliderFloat("Speed", &sPlaySpeed, 0.1f, 4.0f, "%.2fx");
    const char* easeModes[] = { "None", "Ease in/out", "Ease in", "Ease out" };
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Easing", &sEaseMode, easeModes, 4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Playback timing: accelerate/decelerate the whole move instead of moving at a "
                          "constant rate. Affects Play only, not scrubbing.");
    }
    if (sEaseMode != 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Amount##ease", &sEaseAmount, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How strong the easing is (0 = linear, 1 = full).");
        }
    }

    // Camera shake / handheld: organic jitter layered on top of playback.
    ImGui::Checkbox("Camera shake", &sShakeEnabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add smooth handheld-style jitter to the moving camera. Deterministic, so it looks the "
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
            ImGui::SetTooltip("Drop the aim target in front of the free camera. With 'Show path in world' on you can "
                              "then drag its red/green/blue handles to reposition it, and the whole path aims at it.");
        }
        ImGui::TextDisabled("Movable aim target: the whole path looks at this point. Drag its gizmo in the world.");
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
            ImGui::SetTooltip("Browse, load, or delete saved cinematics in the cinematics/ folder.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(cinematics/<name>.json)");

        // Path-bound location: tie the current scene/spawn to the path so loading warps you straight back.
        ImGui::Checkbox("Bind location to path", &sBindLocation);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When saving, remember the current scene/spawn so loading this path warps you here.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Teleport on load", &sTeleportOnLoad);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When loading a path with a bound location, fade-warp to it (skipped if already there).");
        }
        if (sPathEntrance >= 0) {
            bool here = (CinematicCam_GetCurrentEntrance() == sPathEntrance);
            ImGui::TextDisabled("Bound location: entrance %d%s", sPathEntrance, here ? "  (you are here)" : "");
        } else {
            ImGui::TextDisabled("Bound location: none");
        }

        if (ImGui::BeginPopup("Cinematic files")) {
            ImGui::TextDisabled("Saved cinematics  (* = current)");
            ImGui::Separator();
            std::vector<std::string> files = ListCinematics();
            if (files.empty()) {
                ImGui::TextDisabled("(none yet - Save one first)");
            }
            static std::string sConfirmDelete; // name awaiting delete confirmation, or empty
            bool closePopup = false;
            ImGui::BeginChild("##cinelist", ImVec2(320, 240), false);
            for (auto& name : files) {
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

    bool camActive = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0) || sPlaying || sPreview;
    if (!camActive) {
        if (sPerfOn) {
            sPerfUpdateMs = CineNowMs() - upT0;
        }
        return;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);

    // Letterbox bars.
    if (CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Letterbox"), 0)) {
        float amount = CVarGetFloat(CVAR_ENHANCEMENT("CinematicCam.LetterboxAmount"), 0.12f);
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
