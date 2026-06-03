#ifndef CINEMATIC_CAM_BRIDGE_H
#define CINEMATIC_CAM_BRIDGE_H

// Shared C/C++ bridge for the cinematic camera "look at actor" feature. Plain types only (no game headers)
// so it can be included from both z_camera.c (C) and CinematicCamPath.cpp (C++).

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* ptr;         // opaque Actor* (stable only within the current scene/session)
    const char* name;  // ActorDB name (static string)
    short id;           // actor id
    short category;     // ACTORCAT_*
    float pos[3];       // world position
} CineActorInfo;

// Fill out[] with up to maxCount live actors; returns the count.
int CinematicCam_EnumActors(CineActorInfo* out, int maxCount);

// Resolve an actor's current aim position. *ptr is a cached Actor* (may be stale); id is the actor id to
// look for. If the cached pointer is still live and matches id, uses it; otherwise re-acquires the first
// live actor with that id and updates *ptr. Writes pos to out[3]. Returns 1 if found, 0 otherwise.
int CinematicCam_ResolveActor(void** ptr, short id, float* out);

// Set the actor whose point of view the camera should spectate (pass id 0 to clear).
void CinematicCam_SetSpectateActor(void* ptr, int id);

// Current camera eye position, for distance-sorting the actor pickers. Returns 1 if available.
int CinematicCam_GetViewEye(float* out);

// Returns 1 if the movement stick is pushed (to drop the editor out of preview into manual flying).
int CinematicCam_GetMoveStickActive(void);

#ifdef __cplusplus
}
#endif

#endif // CINEMATIC_CAM_BRIDGE_H
