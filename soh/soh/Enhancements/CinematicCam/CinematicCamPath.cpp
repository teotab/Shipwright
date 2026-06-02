#include "CinematicCamPath.h"

#include <imgui.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

// C bridge into the free camera (z_camera.c).
extern "C" {
void CinematicCam_GetPose(float* eye, float* at, float* roll, float* fov);
void CinematicCam_SetPlayback(int active, float* eye, float* at, float roll, float fov);
int CinematicCam_WorldToNdc(float* world, float* outNdcX, float* outNdcY);
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
static float sPlayhead = 0.0f;        // seconds
static float sPlaySpeed = 1.0f;
static char sFilename[64] = "path1";
static bool sHookRegistered = false;
static bool sShowPath = true; // draw the spline + markers in the world while the editor is open
static int sAimDragId = -1;   // id of the keyframe whose aim handle is being dragged, or -1

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
    for (int k = 0; k < 3; k++) {
        out.eye[k] = InterpComp(a.eye[k], b.eye[k], c.eye[k], d.eye[k], b, c, lt);
        out.at[k] = InterpComp(a.at[k], b.at[k], c.at[k], d.at[k], b, c, lt);
    }
    out.roll = InterpComp(a.roll, b.roll, c.roll, d.roll, b, c, lt);
    out.fov = InterpComp(a.fov, b.fov, c.fov, d.fov, b, c, lt);
    out.time = time;
    return out;
}

// Runs every game frame (OnCameraState hook), just before Camera_Update.
static void PlaybackTick() {
    static bool wasActive = false;

    if (sPlaying) {
        if (sKeyframes.size() < 2) {
            sPlaying = false;
        } else {
            sPlayhead += kTickSeconds * sPlaySpeed;
            float total = EffectiveTotal();
            if (sPlayhead >= total) {
                if (sLoop && total > 0.0f) {
                    sPlayhead = std::fmod(sPlayhead, total);
                } else {
                    sPlayhead = TotalTime();
                    sPlaying = false;
                }
            }
        }
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
                      { "bias", k.bias } });
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
    } catch (...) {
        return;
    }
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

        // Facing indicator: a short line toward the look-at point. The selected keyframe also gets a
        // grabbable aim handle at the end of the line (drag it to re-aim the camera).
        float facing[3];
        FacingHandleWorld(sKeyframes[i], facing);
        ImVec2 fp;
        if (WorldToScreen(facing, fp)) {
            dl->AddLine(sp, fp, IM_COL32(120, 255, 120, 150), 1.5f);
            if (selected) {
                bool dragging = sIds[i] == sAimDragId;
                dl->AddCircleFilled(fp, dragging ? 6.0f : 5.0f, IM_COL32(120, 255, 120, 255));
                dl->AddCircle(fp, dragging ? 6.0f : 5.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
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

// Mouse interaction with the overlay: click a keyframe to select it, drag the selected keyframe's aim
// handle to re-aim its camera (horizontal = yaw, vertical = pitch). Operates only over the game view.
static void HandleOverlayInput() {
    ImGuiIO& io = ImGui::GetIO();

    // Continue an in-progress aim drag (ignores WantCaptureMouse so it survives passing over a window).
    if (sAimDragId >= 0) {
        int idx = -1;
        for (size_t i = 0; i < sIds.size(); i++) {
            if (sIds[i] == sAimDragId) {
                idx = (int)i;
                break;
            }
        }
        if (idx < 0 || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            sAimDragId = -1;
            return;
        }
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            float* eye = sKeyframes[idx].eye;
            float* at = sKeyframes[idx].at;
            float dx = at[0] - eye[0];
            float dy = at[1] - eye[1];
            float dz = at[2] - eye[2];
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < 1.0f) {
                dist = 1.0f;
            }
            float horiz = std::sqrt(dx * dx + dz * dz);
            float yaw = std::atan2(dx, dz);
            float pitch = std::atan2(dy, horiz);

            const float kSens = 0.005f; // radians per pixel
            yaw += io.MouseDelta.x * kSens;
            pitch -= io.MouseDelta.y * kSens; // drag up = look up
            const float kPitchLimit = 1.48f;  // ~85 degrees, avoid gimbal flip
            if (pitch > kPitchLimit) {
                pitch = kPitchLimit;
            }
            if (pitch < -kPitchLimit) {
                pitch = -kPitchLimit;
            }
            float ch = std::cos(pitch) * dist;
            at[0] = eye[0] + ch * std::sin(yaw);
            at[1] = eye[1] + dist * std::sin(pitch);
            at[2] = eye[2] + ch * std::cos(yaw);
        }
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

    // Grab the selected keyframe's aim handle if the click landed on it.
    if (sel >= 0) {
        float facing[3];
        FacingHandleWorld(sKeyframes[sel], facing);
        ImVec2 fp;
        if (WorldToScreen(facing, fp)) {
            float ax = fp.x - m.x;
            float ay = fp.y - m.y;
            if (ax * ax + ay * ay <= 12.0f * 12.0f) {
                PushUndo(); // whole drag is one undo step
                sAimDragId = sIds[sel];
                return;
            }
        }
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
    ImGui::TextWrapped("Fly the free camera to a shot, then Add Keyframe. Build several, then Play to travel "
                       "smoothly through them.");
    ImGui::Separator();

    bool enabled = FreeCamEnabled();
    if (ImGui::Checkbox("Enable Free Camera", &enabled)) {
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), enabled);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show path in world", &sShowPath);

    if (sShowPath) {
        HandleOverlayInput();
        DrawWorldOverlay();
    }
    if (sShowPath && SelectedIndex() >= 0) {
        ImGui::TextDisabled("Tip: drag the green handle in the world to re-aim the selected keyframe; "
                            "click a marker to select it.");
    }

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

    // Per-keyframe time edit
    int sel = SelectedIndex();
    if (sel >= 0) {
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
                ImGui::SetTooltip("Lean the curve toward the previous (+) or the next (-) keyframe (overshoot/undershoot).");
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
    }

    ImGui::Separator();

    // Playback
    float total = TotalTime();
    ImGui::BeginDisabled(sKeyframes.size() < 2);
    if (sPlaying) {
        if (ImGui::Button("Stop")) {
            sPlaying = false;
        }
    } else {
        if (ImGui::Button("Play")) {
            if (sPlayhead >= total) {
                sPlayhead = 0.0f;
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

    if (sLoop) {
        ImGui::SliderFloat("Loop return (s)", &sLoopReturnTime, 0.25f, 10.0f, "%.2fs");
        if (sLoopReturnTime < 0.0f) {
            sLoopReturnTime = 0.0f;
        }
    }

    ImGui::SliderFloat("Speed", &sPlaySpeed, 0.1f, 4.0f, "%.2fx");
    float effTotal = EffectiveTotal();
    if (ImGui::SliderFloat("Timeline", &sPlayhead, 0.0f, effTotal > 0.0f ? effTotal : 1.0f, "%.2fs")) {
        // Scrubbing implies previewing so the camera follows the playhead.
        sPreview = true;
        CVarSetInteger(CVAR_ENHANCEMENT("CinematicCam.Enabled"), 1);
    }
    ImGui::Text("Total: %.2fs%s", effTotal, sLoop ? " (incl. loop return)" : "");

    ImGui::Separator();

    // Save / Load
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
