#include "CinematicCamPath.h"

#include <imgui.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

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

// ---------------------------------------------------------------------------
// Path state
// ---------------------------------------------------------------------------
static std::vector<CineKeyframe> sKeyframes;
static int sNextId = 1;
static std::vector<int> sIds; // parallel to sKeyframes, stable identity for selection across re-sorts
static int sSelectedId = -1;
static bool sPlaying = false;
static bool sPreview = false;
static bool sLoop = false;
static float sLoopReturnTime = 2.0f; // seconds to glide from the last keyframe back to the first when looping
static float sPlayhead = 0.0f;       // seconds
static float sPlaySpeed = 1.0f;
static char sFilename[64] = "path1";
static char sSpectateName[64] = ""; // display name of the spectated actor
static bool sHookRegistered = false;

static int sEaseMode = 1;        // playback timing easing: 0 none, 1 in/out, 2 in, 3 out
static float sEaseAmount = 0.5f; // 0 = linear, 1 = full ease
static float sPlayU = 0.0f;      // linear play progress 0..1, eased into the playhead

// Path-level aim override: when set, every keyframe aims at this target instead of its own.
static int sAimOverride = 0; // 0 none, 1 Link, 2 point, 3 actor
static float sAimOverridePoint[3] = { 0.0f, 0.0f, 0.0f };
static int sAimOverrideActorId = 0;
static void* sAimOverrideActorPtr = nullptr;

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
static int sDragKfId = -1; // keyframe id currently being manipulated by the gizmo, or -1
static int sDragKind = 0;  // 0 = translate, 1 = rotate (snapshot of mode at grab time)
static int sDragAxis = -1; // which axis/ring: 0=X/yaw, 1=Y/pitch, 2=Z/roll
static float sRotPrevAngle = 0.0f;
static float sDragPitchAxis[3] = { 1.0f, 0.0f, 0.0f }; // pitch rotation axis, captured at drag start (stable)
static int sTargetDragId = -1;                         // keyframe id whose look-at-point target is being dragged, or -1
static int sTargetDragAxis = -1;                       // which world axis (0/1/2) of the target is being dragged

// Undo / redo history of the whole keyframe list.
struct PathSnapshot {
    std::vector<CineKeyframe> kf;
    std::vector<int> ids;
    int selectedId;
};
static std::vector<PathSnapshot> sUndo;
static std::vector<PathSnapshot> sRedo;

// Assumed game logic tick rate; playback advances this many seconds per OnCameraState call.
static const float kTickSeconds = 1.0f / 20.0f;

static float TotalTime() {
    return sKeyframes.empty() ? 0.0f : sKeyframes.back().time;
}

// Total timeline length including the loop-return segment when looping.
static float EffectiveTotal() {
    float t = TotalTime();
    if (sLoop && sKeyframes.size() >= 2) {
        t += sLoopReturnTime;
    }
    return t;
}

static void PushUndo() {
    sUndo.push_back({ sKeyframes, sIds, sSelectedId });
    if (sUndo.size() > 64) {
        sUndo.erase(sUndo.begin());
    }
    sRedo.clear();
}

static void Undo() {
    if (sUndo.empty()) {
        return;
    }
    sRedo.push_back({ sKeyframes, sIds, sSelectedId });
    PathSnapshot s = sUndo.back();
    sUndo.pop_back();
    sKeyframes = s.kf;
    sIds = s.ids;
    sSelectedId = s.selectedId;
}

static void Redo() {
    if (sRedo.empty()) {
        return;
    }
    sUndo.push_back({ sKeyframes, sIds, sSelectedId });
    PathSnapshot s = sRedo.back();
    sRedo.pop_back();
    sKeyframes = s.kf;
    sIds = s.ids;
    sSelectedId = s.selectedId;
}

static int SelectedIndex() {
    for (size_t i = 0; i < sIds.size(); i++) {
        if (sIds[i] == sSelectedId) {
            return (int)i;
        }
    }
    return -1;
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

static float v3len(const float* a); // defined with the gizmo vector helpers below

// Kochanek-Bartels (TCB) Hermite interpolation for one scalar component over a segment p1 -> p2.
// tcB = TCB params at p1 (segment source), tcC = TCB params at p2 (segment destination).
// With all params 0 this reduces exactly to Catmull-Rom.
static float TcbHermite(float p0, float p1, float p2, float p3, float tB, float cB, float bB, float tC, float cC,
                        float bC, float s) {
    // Outgoing tangent at p1 and incoming tangent at p2.
    float td = ((1.0f - tB) * (1.0f + cB) * (1.0f + bB) * 0.5f) * (p1 - p0) +
               ((1.0f - tB) * (1.0f - cB) * (1.0f - bB) * 0.5f) * (p2 - p1);
    float ts = ((1.0f - tC) * (1.0f - cC) * (1.0f + bC) * 0.5f) * (p2 - p1) +
               ((1.0f - tC) * (1.0f + cC) * (1.0f - bC) * 0.5f) * (p3 - p2);
    float s2 = s * s;
    float s3 = s2 * s;
    float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    float h10 = s3 - 2.0f * s2 + s;
    float h01 = -2.0f * s3 + 3.0f * s2;
    float h11 = s3 - s2;
    return h00 * p1 + h10 * td + h01 * p2 + h11 * ts;
}

// Interpolate one component over a segment, honoring the source keyframe's per-keyframe mode.
static float InterpComp(float p0, float p1, float p2, float p3, const CineKeyframe& kSrc, const CineKeyframe& kDst,
                        float s) {
    if (kSrc.interp == CINE_INTERP_LINEAR) {
        return p1 + (p2 - p1) * s;
    }
    return TcbHermite(p0, p1, p2, p3, kSrc.tension, kSrc.continuity, kSrc.bias, kDst.tension, kDst.continuity,
                      kDst.bias, s);
}

// 3D TCB outgoing tangent at p1 (start of a segment) and incoming tangent at p2 (end of a segment).
static void TcbOutTangent3(const float* p0, const float* p1, const float* p2, float t, float c, float b, float* o) {
    float w1 = (1.0f - t) * (1.0f + c) * (1.0f + b) * 0.5f;
    float w2 = (1.0f - t) * (1.0f - c) * (1.0f - b) * 0.5f;
    for (int k = 0; k < 3; k++) {
        o[k] = w1 * (p1[k] - p0[k]) + w2 * (p2[k] - p1[k]);
    }
}
static void TcbInTangent3(const float* p1, const float* p2, const float* p3, float t, float c, float b, float* o) {
    float w1 = (1.0f - t) * (1.0f - c) * (1.0f + b) * 0.5f;
    float w2 = (1.0f - t) * (1.0f + c) * (1.0f - b) * 0.5f;
    for (int k = 0; k < 3; k++) {
        o[k] = w1 * (p2[k] - p1[k]) + w2 * (p3[k] - p2[k]);
    }
}
static float Hermite1(float p1, float p2, float m0, float m1, float s) {
    float s2 = s * s;
    float s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * p1 + (s3 - 2.0f * s2 + s) * m0 + (-2.0f * s3 + 3.0f * s2) * p2 +
           (s3 - s2) * m1;
}

// A keyframe's effective look-at point this frame: the stored point for free/point aim, or Link's live
// position for player aim.
static void EffectiveAt(int idx, float out[3]) {
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
        if (CinematicCam_ResolveActor(&sAimOverrideActorPtr, (short)sAimOverrideActorId, p)) {
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
        if (CinematicCam_ResolveActor(&k.aimActorPtr, (short)k.aimActorId, p)) {
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            return;
        }
    }
    out[0] = k.at[0];
    out[1] = k.at[1];
    out[2] = k.at[2];
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

    if (!sLoop) {
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
    if (sLoop) {
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

    // Eye (spatial path): Hermite with TCB tangents, but honor any custom tangent (Bend) at either end.
    // Linear segments stay straight.
    if (b.interp == CINE_INTERP_LINEAR) {
        for (int k = 0; k < 3; k++) {
            out.eye[k] = b.eye[k] + (c.eye[k] - b.eye[k]) * lt;
        }
    } else {
        float td[3], ts[3];
        TcbOutTangent3(a.eye, b.eye, c.eye, b.tension, b.continuity, b.bias, td); // out tangent at b
        TcbInTangent3(b.eye, c.eye, d.eye, c.tension, c.continuity, c.bias, ts);  // in tangent at c
        if (b.hasTangent) {
            float m = v3len(td);
            td[0] = b.tangent[0] * m;
            td[1] = b.tangent[1] * m;
            td[2] = b.tangent[2] * m;
        }
        if (c.hasTangent) {
            float m = v3len(ts);
            ts[0] = c.tangent[0] * m;
            ts[1] = c.tangent[1] * m;
            ts[2] = c.tangent[2] * m;
        }
        for (int k = 0; k < 3; k++) {
            out.eye[k] = Hermite1(b.eye[k], c.eye[k], td[k], ts[k], lt);
        }
    }

    // Aim (at): interpolate each control keyframe's EFFECTIVE target (handles look-at-point / look-at-Link).
    float aA[3], aB[3], aC[3], aD[3];
    EffectiveAt(i0, aA);
    EffectiveAt(i1, aB);
    EffectiveAt(i2, aC);
    EffectiveAt(i3, aD);
    for (int k = 0; k < 3; k++) {
        out.at[k] = InterpComp(aA[k], aB[k], aC[k], aD[k], b, c, lt);
    }
    out.roll = InterpComp(a.roll, b.roll, c.roll, d.roll, b, c, lt);
    out.fov = InterpComp(a.fov, b.fov, c.fov, d.fov, b, c, lt);
    out.time = time;
    return out;
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
            sPlayU += (kTickSeconds * sPlaySpeed) / denom;
            if (sPlayU >= 1.0f) {
                if (sLoop) {
                    sPlayU = std::fmod(sPlayU, 1.0f);
                } else {
                    sPlayU = 1.0f;
                    sPlaying = false;
                }
            }
            float eu = ApplyEase(sPlayU, sEaseMode);
            eu = sPlayU + (eu - sPlayU) * sEaseAmount; // blend toward linear by the ease amount
            sPlayhead = eu * total;
        }
    }

    // Pushing the movement stick while previewing (and not letting Link drive) drops back to manual flying.
    if (sPreview && !sPlaying && !CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.PlaybackControlsLink"), 0) &&
        CinematicCam_GetMoveStickActive()) {
        sPreview = false;
    }

    bool active = (sPlaying || sPreview) && !sKeyframes.empty();
    if (active) {
        CineKeyframe s = SampleAt(sPlayhead);
        CinematicCam_SetPlayback(1, s.eye, s.at, s.roll, s.fov);
        wasActive = true;
    } else if (wasActive) {
        float z[3] = { 0.0f, 0.0f, 0.0f };
        CinematicCam_SetPlayback(0, z, z, 0.0f, 0.0f);
        wasActive = false;
    }

    // Hide the HUD while the cinematic camera is active (reuses SoH's NoUI state). Only clear what we set,
    // so an independently-enabled "no UI" isn't disturbed.
    static bool sWeHidHud = false;
    bool camActive = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0) || active;
    bool hideHud = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.HideHud"), 1) != 0;
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
    sSelectedId = sNextId;
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
    sSelectedId = sIds.empty() ? -1 : sIds[std::min((size_t)idx, sIds.size() - 1)];
}

static void ClearPath() {
    PushUndo();
    sKeyframes.clear();
    sIds.clear();
    sSelectedId = -1;
    sPlayhead = 0.0f;
    sPlaying = false;
    sPreview = false;
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
    sSelectedId = sNextId;
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

static void SavePath() {
    nlohmann::json j = nlohmann::json::array();
    for (auto& k : sKeyframes) {
        j.push_back({ { "time", k.time },
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
                      { "aimMode", k.aimMode },
                      { "aimActorId", k.aimActorId } });
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
    for (auto& e : j) {
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
        k.aimMode = e.value("aimMode", 0);
        k.aimActorId = e.value("aimActorId", 0);
        k.aimActorPtr = nullptr;
        sKeyframes.push_back(k);
        sIds.push_back(sNextId++);
    }
    SortByTime();
    sSelectedId = sIds.empty() ? -1 : sIds[0];
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

// The tangent direction in use at keyframe idx (custom if set, otherwise the automatic one).
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
        float tdir[3];
        BendTangentDir(idx, tdir);
        // Tangent handle line through the keyframe (both directions), like a Bezier handle.
        float hp[3], hn[3];
        v3mad(k.eye, tdir, L, hp);
        v3mad(k.eye, tdir, -L, hn);
        ImVec2 sp, sn;
        if (WorldToScreen(hp, sp) && WorldToScreen(hn, sn)) {
            dl->AddLine(sn, sp, IM_COL32(230, 130, 255, 220), 2.0f);
            dl->AddCircleFilled(sp, 4.0f, IM_COL32(230, 130, 255, 255));
        }
        float yawAxis[3], pitchAxis[3];
        BendAxes(tdir, yawAxis, pitchAxis);
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
        if (R <= 0.0f) {
            return false;
        }
        float tdir[3];
        BendTangentDir(idx, tdir);
        float yawAxis[3], pitchAxis[3];
        BendAxes(tdir, yawAxis, pitchAxis);
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
            if (!kf.hasTangent) { // lock in the current (auto) tangent so rotation starts from it
                kf.tangent[0] = tdir[0];
                kf.tangent[1] = tdir[1];
                kf.tangent[2] = tdir[2];
                kf.hasTangent = 1;
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
            // Bend: rotate the spatial tangent (reshapes the curve; no effect on aim).
            float rot[3];
            v3rot(k.tangent, axis, d, rot);
            v3norm(rot);
            k.tangent[0] = rot[0];
            k.tangent[1] = rot[1];
            k.tangent[2] = rot[2];
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

// Draw the spline, numbered keyframe markers, facing indicators and the playhead over the game view.
static void DrawWorldOverlay() {
    if (sKeyframes.empty()) {
        return;
    }
    // Foreground draw list of the game's viewport so the overlay sits on top of the rendered frame.
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());

    // Spline curve.
    if (sKeyframes.size() >= 2) {
        float total = EffectiveTotal();
        const int steps = 120;
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

    // Start a new interaction on click. Bail only if an ImGui widget is actively being used, so dragging
    // a slider or pressing a button in the editor doesn't also grab a world handle. (We deliberately do
    // NOT gate on WantCaptureMouse: the SoH menu is a fullscreen ImGui layer, so that would block every
    // click while the menu is open.)
    if (ImGui::IsAnyItemActive() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }

    ImVec2 m = io.MousePos;
    int sel = SelectedIndex();

    // Grab the selected keyframe's look-at target, then its gizmo, if the click landed on a handle.
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
                sSelectedId = sIds[i];
                return;
            }
        }
    }
}

// Draw a distance-sorted actor list (nearest the camera first) with id + position so identical-named
// actors are distinguishable. Returns the picked index into buf, or -1.
static int DrawActorPickerList(CineActorInfo* buf, int n) {
    float eye[3];
    bool haveEye = CinematicCam_GetViewEye(eye) != 0;

    static std::vector<int> order;
    order.resize(n);
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    auto dist2 = [&](int k) {
        float dx = buf[k].pos[0] - eye[0], dy = buf[k].pos[1] - eye[1], dz = buf[k].pos[2] - eye[2];
        return dx * dx + dy * dy + dz * dz;
    };
    if (haveEye) {
        std::sort(order.begin(), order.end(), [&](int a, int b) { return dist2(a) < dist2(b); });
    }

    ImGui::Text("%d actors%s", n, haveEye ? " (nearest first)" : "");
    static char filter[32] = "";
    ImGui::InputTextWithHint("##actorfilter", "filter by name...", filter, sizeof(filter));

    int picked = -1;
    ImGui::BeginChild("##actorlist", ImVec2(390, 340), true);
    for (int oi = 0; oi < n; oi++) {
        int i = order[oi];
        const char* nm = buf[i].name ? buf[i].name : "?";
        if (filter[0]) {
            std::string h = nm, f = filter;
            std::transform(h.begin(), h.end(), h.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            if (h.find(f) == std::string::npos) {
                continue;
            }
        }
        float d = haveEye ? std::sqrt(dist2(i)) : 0.0f;
        char lbl[128];
        snprintf(lbl, sizeof(lbl), "%s  (id %d)  %.0fu  @ %.0f, %.0f, %.0f##ap%d", nm, buf[i].id, d, buf[i].pos[0],
                 buf[i].pos[1], buf[i].pos[2], i);
        if (ImGui::Selectable(lbl)) {
            picked = i;
        }
    }
    ImGui::EndChild();
    return picked;
}

// A timeline track: keyframe markers (drag to retime, click to select), a draggable playhead, and the
// loop-return region shaded. Replaces the plain slider.
static void DrawTimeline() {
    int n = (int)sKeyframes.size();
    float total = EffectiveTotal();
    if (total <= 0.0f) {
        total = 1.0f;
    }

    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, 46.0f);
    if (size.x < 60.0f) {
        size.x = 60.0f;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", size);
    ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(35, 35, 38, 255), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 95, 255), 4.0f);

    auto timeToX = [&](float t) { return p0.x + (t / total) * size.x; };
    auto xToTime = [&](float x) {
        float u = (x - p0.x) / size.x;
        if (u < 0.0f) {
            u = 0.0f;
        }
        if (u > 1.0f) {
            u = 1.0f;
        }
        return u * total;
    };

    if (sLoop && n >= 2) { // shade the loop-return tail
        float lx = timeToX(TotalTime());
        dl->AddRectFilled(ImVec2(lx, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), IM_COL32(80, 60, 30, 90), 4.0f);
    }

    float cy = (p0.y + p1.y) * 0.5f;
    for (int i = 0; i < n; i++) {
        float kx = timeToX(sKeyframes[i].time);
        bool sel = sIds[i] == sSelectedId;
        ImU32 c = sel ? IM_COL32(80, 200, 255, 255) : IM_COL32(255, 160, 30, 255);
        dl->AddLine(ImVec2(kx, p0.y + 4), ImVec2(kx, p1.y - 4), c, sel ? 2.0f : 1.0f);
        dl->AddCircleFilled(ImVec2(kx, cy), sel ? 6.0f : 5.0f, c);
        dl->AddCircle(ImVec2(kx, cy), sel ? 6.0f : 5.0f, IM_COL32(0, 0, 0, 180), 0, 1.0f);
    }

    float px = timeToX(sPlayhead);
    dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p1.y), IM_COL32(60, 255, 90, 255), 2.0f);
    dl->AddTriangleFilled(ImVec2(px - 5, p0.y + 1), ImVec2(px + 5, p0.y + 1), ImVec2(px, p0.y + 9),
                          IM_COL32(60, 255, 90, 255));

    // Interaction: grab a nearby marker to retime it, otherwise scrub the playhead.
    static int sTlDragKfId = -1;
    static bool sTlScrub = false;
    static float sTlGrabOffset = 0.0f; // marker time minus grab time, so a click doesn't snap the marker
    float mx = ImGui::GetIO().MousePos.x;
    if (ImGui::IsItemActivated()) {
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
            PushUndo();
            sTlDragKfId = sIds[hit];
            sSelectedId = sIds[hit];
            sTlGrabOffset = sKeyframes[hit].time - xToTime(mx);
            sTlScrub = false;
        } else {
            sTlScrub = true;
            sPreview = true;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }
    if (ImGui::IsItemActive()) {
        if (sTlDragKfId >= 0) {
            float t = xToTime(mx) + sTlGrabOffset; // preserve the grab point (no jump on click)
            for (int i = 0; i < n; i++) {
                if (sIds[i] == sTlDragKfId) {
                    sKeyframes[i].time = (t < 0.0f) ? 0.0f : t;
                    SortByTime();
                    break;
                }
            }
        } else if (sTlScrub) {
            sPlayhead = xToTime(mx);
            sPreview = true;
            CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        sTlDragKfId = -1;
        sTlScrub = false;
    }

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
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
void CinematicCamPathWindow::InitElement() {
    if (!sHookRegistered) {
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnCameraState>(
            [](PlayState* play) { PlaybackTick(); });
        sHookRegistered = true;
    }
}

void CinematicCamPathWindow::DrawElement() {
    ImGui::TextWrapped("Fly the free camera to a shot and Add Keyframe (or Record a live flight). Build a few, "
                       "then Play to glide through them. Click a marker in the world or on the timeline to "
                       "select it; drag to reposition / retime.");
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

    if (sShowPath) {
        HandleOverlayInput();
        DrawWorldOverlay();
    }

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
            static CineActorInfo sa[512];
            int pn = CinematicCam_EnumActors(sa, 512);
            int pick = DrawActorPickerList(sa, pn);
            if (pick >= 0) {
                CinematicCam_SetSpectateActor(sa[pick].ptr, sa[pick].id);
                strncpy(sSpectateName, sa[pick].name ? sa[pick].name : "?", sizeof(sSpectateName) - 1);
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
        if (ImGui::SliderFloat("Eye height", &h, -100.0f, 200.0f, "%.0f")) {
            CVarSetFloat(CVAR_ENHANCEMENT("CinematicCam.SpectateHeight"), h);
        }
        ImGui::TextDisabled("Locks the camera to the actor's viewpoint (aimed along its facing). "
                            "The world keeps running so you see what it sees.");
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
            sSelectedId = -1;
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
            sSelectedId = sIds.empty() ? -1 : sIds[0];
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

    // Keyframe list
    ImGui::Text("Keyframes: %d", (int)sKeyframes.size());
    ImGui::BeginChild("##kflist", ImVec2(0, 160), true);
    for (int i = 0; i < (int)sKeyframes.size(); i++) {
        ImGui::PushID(i);
        char label[64];
        snprintf(label, sizeof(label), "#%d   t=%.2fs   %s", i + 1, sKeyframes[i].time,
                 sKeyframes[i].interp == CINE_INTERP_LINEAR ? "[Linear]" : "");
        if (ImGui::Selectable(label, sIds[i] == sSelectedId)) {
            sSelectedId = sIds[i];
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    int sel = SelectedIndex();
    if (sel >= 0) {
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
                ImGui::TextDisabled("Drag the red/green/blue axes in the world to move this keyframe.");
            } else if (sGizmoMode == GIZMO_ROTATE) {
                ImGui::TextDisabled("Drag the rings to aim the camera: green=yaw, red=pitch, blue=roll.");
            } else {
                ImGui::TextDisabled("Drag the rings to bend the path's curve through this point (no effect on aim).");
                if (sKeyframes[sel].hasTangent && ImGui::SmallButton("Reset tangent")) {
                    PushUndo();
                    sKeyframes[sel].hasTangent = 0;
                }
            }
        } else {
            ImGui::TextDisabled("Enable 'Show path in world' to use the move/rotate/bend gizmo.");
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

        // Aim mode: how this keyframe's camera is oriented.
        const char* aimModes[] = { "Free orientation", "Look at point", "Look at Link", "Look at actor" };
        int am = sKeyframes[sel].aimMode;
        if (ImGui::Combo("Aim", &am, aimModes, 4)) {
            PushUndo();
            sKeyframes[sel].aimMode = am;
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
            static CineActorInfo sActors[512];
            // Resolve the current target's name for display.
            const char* curName = "(pick one)";
            int n = CinematicCam_EnumActors(sActors, 512);
            for (int ai = 0; ai < n; ai++) {
                if (sActors[ai].ptr == sKeyframes[sel].aimActorPtr) {
                    curName = sActors[ai].name ? sActors[ai].name : "?";
                    break;
                }
                if (sActors[ai].id == sKeyframes[sel].aimActorId) {
                    curName = sActors[ai].name ? sActors[ai].name : "?";
                }
            }
            ImGui::Text("Target: %s (id %d)", curName, sKeyframes[sel].aimActorId);
            if (ImGui::Button("Pick actor...")) {
                ImGui::OpenPopup("Pick actor");
            }
            if (ImGui::BeginPopup("Pick actor")) {
                int pn = CinematicCam_EnumActors(sActors, 512);
                int pick = DrawActorPickerList(sActors, pn);
                if (pick >= 0) {
                    PushUndo();
                    sKeyframes[sel].aimActorId = sActors[pick].id;
                    sKeyframes[sel].aimActorPtr = sActors[pick].ptr;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::TextDisabled("Tracks the actor live. Saved by id (re-acquired on load).");
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
            sPlayU = (pt > 0.0f) ? (sPlayhead / pt) : 0.0f; // seed eased progress from the current playhead
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

    if (sLoop) {
        ImGui::SliderFloat("Loop return (s)", &sLoopReturnTime, 0.25f, 10.0f, "%.2fs");
        if (sLoopReturnTime < 0.0f) {
            sLoopReturnTime = 0.0f;
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

    // Path-level aim override: aim every keyframe at one target (fixes up recorded paths at once).
    const char* aimOv[] = { "Per-keyframe (off)", "All look at Link", "All look at point", "All look at actor" };
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Aim override", &sAimOverride, aimOv, 4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ignore each keyframe's own aim and point the whole path at one target. Handy for "
                          "re-aiming a recorded flight at Link or an actor in one step.");
    }
    if (sAimOverride == 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputFloat3("##aimovpt", sAimOverridePoint, "%.0f");
    } else if (sAimOverride == 3) {
        ImGui::SameLine();
        if (ImGui::Button("Pick##aimov")) {
            ImGui::OpenPopup("Pick override actor");
        }
        if (ImGui::BeginPopup("Pick override actor")) {
            static CineActorInfo oa[512];
            int pn = CinematicCam_EnumActors(oa, 512);
            int pick = DrawActorPickerList(oa, pn);
            if (pick >= 0) {
                sAimOverrideActorId = oa[pick].id;
                sAimOverrideActorPtr = oa[pick].ptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::Text("id %d", sAimOverrideActorId);
    }

    DrawTimeline();

    ImGui::SeparatorText("Save / Load");
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
    ImGui::TextDisabled("(cinematics/<name>.json)");

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
}

// Called every frame (even when the window is hidden): draw the cinematic letterbox bars and grid overlay.
void CinematicCamPathWindow::UpdateElement() {
    bool camActive = CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 0) || sPlaying || sPreview;
    if (!camActive) {
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
}
