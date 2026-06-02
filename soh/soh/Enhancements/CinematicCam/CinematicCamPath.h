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
};

// A single captured camera pose on the path timeline.
struct CineKeyframe {
    float time;   // absolute position on the timeline, in seconds
    float eye[3]; // world position
    float at[3];  // world look-at point
    float roll;   // degrees
    float fov;    // degrees

    // Per-keyframe curve shaping. Defaults (Smooth, all 0) reproduce a standard Catmull-Rom spline.
    int interp;        // CineInterp
    float tension;     // -1..1: -1 rounder/looser, +1 tighter/straighter
    float continuity;  // -1..1: sharpness of the corner through the keyframe
    float bias;        // -1..1: lean the curve toward the previous (+) or next (-) keyframe

    // Optional custom spline tangent (the direction the spatial curve passes through this point), edited
    // with the Bend gizmo. Like a Bezier handle; bends the curve on both sides. Independent of camera aim.
    int hasTangent;    // 0 = automatic tangent, 1 = use the custom direction below
    float tangent[3];  // unit direction

    int aimMode;       // CineAim: how the camera is aimed (free / point / Link / actor)
    int aimActorId;    // for CINE_AIM_ACTOR: the actor id to track (saved)
    void* aimActorPtr; // runtime-only cached Actor* for the tracked actor (not saved)
};

// Editor window for building and playing back cinematic camera paths.
class CinematicCamPathWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~CinematicCamPathWindow() {};

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override {};
};

#endif // CINEMATIC_CAM_PATH_H
