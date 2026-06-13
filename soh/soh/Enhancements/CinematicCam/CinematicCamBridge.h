#ifndef CINEMATIC_CAM_BRIDGE_H
#define CINEMATIC_CAM_BRIDGE_H

// Shared C/C++ bridge for the cinematic camera "look at actor" feature. Plain types only (no game headers)
// so it can be included from both z_camera.c (C) and CinematicCamPath.cpp (C++).

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* ptr;        // opaque Actor* (stable only within the current scene/session)
    const char* name; // ActorDB name (static string)
    short id;         // actor id
    short category;   // ACTORCAT_*
    float pos[3];     // world position
} CineActorInfo;

// Fill out[] with up to maxCount live actors; returns the count.
int CinematicCam_EnumActors(CineActorInfo* out, int maxCount);

// Resolve an actor's current aim position. *ptr is a cached Actor* (may be stale); id is the actor id to
// look for. If the cached pointer is still live and matches id, uses it; otherwise re-acquires by id and
// updates *ptr. With a non-NULL hint[3] the re-acquire picks the NEAREST matching actor to that point
// (disambiguates multiple identical actors after a reload); NULL hint takes the first match. Writes pos to
// out[3]. Returns 1 if found, 0 otherwise.
int CinematicCam_ResolveActor(void** ptr, short id, float* hint, float* out);

// Set the actor whose point of view the camera should spectate (pass id 0 to clear).
void CinematicCam_SetSpectateActor(void* ptr, int id);

// Current camera eye position, for distance-sorting the actor pickers. Returns 1 if available.
int CinematicCam_GetViewEye(float* out);

// Returns 1 if the movement stick is pushed (to drop the editor out of preview into manual flying).
int CinematicCam_GetMoveStickActive(void);

// Area teleporter: enumerate major destinations and warp to one (fade transition; no-op outside gameplay).
int CinematicCam_GetTeleportCount(void);
const char* CinematicCam_GetTeleportName(int index);
void CinematicCam_TeleportTo(int index);

// Path-bound location: a path can remember the entrance (scene + spawn) it was authored in, so loading it warps
// you back. GetCurrentEntrance returns -1 if unavailable; WarpToEntrance fades you to an arbitrary entrance.
int CinematicCam_GetCurrentEntrance(void);
void CinematicCam_WarpToEntrance(int entrance);

// Green screen mode to render this frame: 0 = off, 1 = green, 2 = blue. Returns the keyframed automation-track
// value while a cinematic drives it, otherwise the manual CinematicCam.GreenScreen CVar. Read by z_play.c.
int CinematicCam_GetGreenScreen(void);

// Time of day (0..65535, 0 = midnight). Get returns -1 if unavailable; Set drives sun/sky/lighting.
int CinematicCam_GetDayTime(void);
void CinematicCam_SetDayTime(int t);

// Free-camera actor follow: the live free camera carries with this actor's movement (id 0 = off). Look/aim
// stays manual. GetFreecamFollowId returns the current id (0 = off). ActorName is a cheap id->name lookup.
void CinematicCam_SetFreecamFollow(void* ptr, int id);
int CinematicCam_GetFreecamFollowId(void);
const char* CinematicCam_ActorName(int id);

// Hard-restart Link's standing idle animation from frame 0 (no morph), to anchor his breathing/head-bob cycle
// to the start of a cinematic loop. No-op unless Link is currently idle. Pairs with NoIdleFidget for clean loops.
void CinematicCam_SyncLinkIdleAnim(void);

// Snap Link's facing direction for posed shots. GetLinkYaw returns the current heading in degrees (0..359, or
// -1 if unavailable); SetLinkYaw points him at an absolute heading; FaceLinkToCamera aims him at (or, with
// away != 0, directly away from) the active camera.
int CinematicCam_GetLinkYaw(void);
void CinematicCam_SetLinkYaw(int degrees);
void CinematicCam_FaceLinkToCamera(int away);

#ifdef __cplusplus
}
#endif

#endif // CINEMATIC_CAM_BRIDGE_H
