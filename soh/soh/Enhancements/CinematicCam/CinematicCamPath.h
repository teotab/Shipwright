#ifndef CINEMATIC_CAM_PATH_H
#define CINEMATIC_CAM_PATH_H

#include <libultraship/libultraship.h>

// A single captured camera pose on the path timeline.
struct CineKeyframe {
    float time;   // absolute position on the timeline, in seconds
    float eye[3]; // world position
    float at[3];  // world look-at point
    float roll;   // degrees
    float fov;    // degrees
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
