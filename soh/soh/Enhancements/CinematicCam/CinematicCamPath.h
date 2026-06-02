#ifndef CINEMATIC_CAM_PATH_H
#define CINEMATIC_CAM_PATH_H

#include <libultraship/libultraship.h>

// Per-keyframe interpolation mode (controls the segment leaving this keyframe toward the next).
enum CineInterp {
    CINE_INTERP_SMOOTH = 0, // TCB spline (tension/continuity/bias below)
    CINE_INTERP_LINEAR = 1, // straight line to the next keyframe
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
