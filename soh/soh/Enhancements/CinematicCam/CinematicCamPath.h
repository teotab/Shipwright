#ifndef CINEMATIC_CAM_PATH_H
#define CINEMATIC_CAM_PATH_H

#include <libultraship/libultraship.h>

// Per-keyframe interpolation mode (controls the segment leaving this keyframe toward the next).
enum CineInterp {
    CINE_INTERP_SMOOTH = 0, // TCB spline (tension/continuity/bias below)
    CINE_INTERP_LINEAR = 1, // straight line to the next keyframe
};

// How a keyframe's camera is aimed.
enum CineAim {
    CINE_AIM_FREE = 0,   // free orientation (stored look-at moves rigidly with the eye)
    CINE_AIM_POINT = 1,  // look at a fixed world point (the stored look-at)
    CINE_AIM_PLAYER = 2, // look at Link (tracked live)
    CINE_AIM_ACTOR = 3,  // look at a chosen actor (tracked live by id; pointer cached at runtime)
    CINE_AIM_TARGET = 4, // look at the shared movable aim target (the same point used by the aim override)
    CINE_AIM_RAIL = 5,   // look along the path's travel direction (dolly/rail style, parallel to the spline)
};

// A single captured camera pose on the path timeline.
struct CineKeyframe {
    float time;   // absolute position on the timeline, in seconds
    float eye[3]; // world position
    float at[3];  // world look-at point
    float roll;   // degrees
    float fov;    // degrees

    // Per-keyframe curve shaping. Defaults (Smooth, all 0) reproduce a standard Catmull-Rom spline.
    int interp;       // CineInterp
    float tension;    // -1..1: -1 rounder/looser, +1 tighter/straighter
    float continuity; // -1..1: sharpness of the corner through the keyframe
    float bias;       // -1..1: lean the curve toward the previous (+) or next (-) keyframe

    // Optional custom spline tangent (the direction the spatial curve passes through this point), edited
    // with the Bend gizmo. Like a Bezier handle; bends the curve on both sides. Independent of camera aim.
    int hasTangent;   // 0 = automatic tangent, 1 = use the custom direction below
    float tangent[3]; // unit direction (the OUT side - the curve leaving toward the next keyframe)

    // Broken (per-side) tangent: when set, the segment ARRIVING at this keyframe uses tangentIn instead of
    // tangent, so the curve can enter and leave this point in different directions (a shaped corner).
    int hasTangentIn;   // 0 = the in side mirrors `tangent` (or auto), 1 = use the direction below
    float tangentIn[3]; // unit direction of travel arriving at this keyframe

    // Per-side tangent WEIGHTS: multipliers on the automatic tangent magnitude (0 = automatic / 1.0). Set by
    // the shape-preserving Insert @ playhead (exact at insert time) and editable as "side weights" - how far
    // the curve bulges on each side, like a scalable Bezier handle. Relative, so moving a keyframe rescales
    // the tangents naturally instead of leaving a stale absolute length behind.
    float tanWOut;
    float tanWIn;

    // "Hold framing here": the view's turn comes to rest ON this keyframe and builds up again leaving it, so
    // the framing parks for a beat while the camera keeps moving. Off = the view flows through the keyframe.
    int aimHold;

    // Explicit aim-curve rates at this keyframe (degrees/second of yaw and pitch, per side), baked by
    // Insert @ playhead so adding a keyframe cannot reshape the aim curve: once baked, the neighbours stop
    // re-deriving their slopes from the new arrangement. 0 = automatic (derived from the neighbours).
    int hasAimTan;
    float aimTanYawIn, aimTanYawOut;
    float aimTanPitchIn, aimTanPitchOut;

    // The camera's speed at this keyframe (world units/second) on the arc-length schedule, per side: In
    // governs the segment arriving, Out the segment leaving. Negative = automatic (the monotone schedule's own
    // value). Set by dragging the Speed curve's handles, and baked by Insert @ playhead so adding a keyframe
    // cannot change the pacing. Always clamped to what exactness allows (the camera must still arrive on
    // time), and mirrored across the keyframe unless speedBroken.
    float speedRateIn = -1.0f;
    float speedRateOut = -1.0f;
    int speedBroken; // 0 = the two sides move together (alt-drag a handle to break them apart)

    int aimMode;          // CineAim: how the camera is aimed (free / point / Link / actor)
    int aimActorId;       // for CINE_AIM_ACTOR: the actor id to track (saved)
    void* aimActorPtr;    // runtime-only cached Actor* for the tracked actor (not saved)
    float aimActorPos[3]; // actor position when it was picked - on reload, re-acquire the NEAREST actor of that
                          // id to this point (disambiguates multiple identical actors, e.g. several frogs)

    // Per-keyframe timing ease (0..1). easeOut slows the camera leaving this keyframe; easeIn slows it
    // arriving at this keyframe. Use both near 1 to "hold" on a keyframe (slow in, slow out).
    float easeIn;
    float easeOut;
};

// --- Parameter automation tracks -------------------------------------------------------------------------
// A keyframable parameter on its OWN sub-timeline, independent of the camera path keyframes. This is the
// foundation for automating any exposed parameter over time; the green screen is the first one wired up.

// How the segment LEAVING a key is interpolated toward the next key.
enum CineTrackInterp {
    CINE_TRACK_STEP = 0,   // hold the value until the next key (discrete params: green screen, toggles, enums)
    CINE_TRACK_LINEAR = 1, // straight line to the next key
    CINE_TRACK_SMOOTH = 2, // auto-smoothed (Catmull-Rom) curve through the neighbors
};

// One key on a parameter track.
struct CineParamKey {
    float time;  // seconds on the timeline
    float value; // parameter value (discrete params store an integer cast to float)
    int interp;  // CineTrackInterp for the segment leaving this key (-1 = use the track's default)
};

// Editor window for building and playing back cinematic camera paths.
class CinematicCamPathWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~CinematicCamPathWindow() {};

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override; // draws the always-on letterbox
};

#endif // CINEMATIC_CAM_PATH_H
