# 🎥 Cinematic Camera for Ship of Harkinian

A full **cinematic camera toolkit** for [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) — a detached free-fly camera plus a keyframe **path editor** for authoring smooth, controllable camera moves through *The Legend of Zelda: Ocarina of Time*.

Fly anywhere, ignore culling, frame your shot, drop keyframes, and play back a buttery spline move — with on-screen gizmos, actor tracking, recording, easing, and cinematic framing aids.

> **Version 1.0** — the free camera and path editor are feature-complete. A scripted multi-path **cutscene system** (sequencing + triggers) is on the roadmap.

---

## ✨ Features

**Free camera**
- Fully **detached free-fly camera**, independent of Link, that takes over the controller.
- **Inertia / smoothing** — from crisp 1:1 to floaty handheld, on a slider.
- **Ignore culling** — extends actor draw distance and the far clip plane while flying, so nothing pops in/out.
- **Freeze world** toggle, or keep the action running.
- **Fully rebindable controls** via SoH's button-selector UI (sticks remap in the normal Controller Config).

**Path editor**
- Capture **keyframes** (eye, look-at, roll, FOV) and play a smooth **Catmull-Rom / TCB spline** through them.
- **In-world overlay**: the spline curve, numbered markers, facing indicators, and a live playhead, drawn over the game and pixel-accurate at any resolution.
- **Transform gizmos** on the selected keyframe:
  - **Move** — drag X/Y/Z axes to reposition.
  - **Rotate (aim)** — drag rings for yaw / pitch / roll.
  - **Bend (path)** — rotate the spline tangent to bend the curve (Bézier-handle style), independent of aim.
- **Per-keyframe interpolation**: Smooth (with **Tension / Continuity / Bias**) or **Linear**.
- **Numeric fields** for exact position / yaw / pitch / roll / FOV entry.
- **Aim modes** per keyframe — Free orientation, Look-at a point (draggable target gizmo), Look-at **Link**, or Look-at **any actor** (picked from a searchable, distance-sorted list).
- **Path-level aim override** — point an entire (e.g. recorded) path at one target in a single click.

**Actor POV (spectate)**
- See the world **through any actor's eyes** — eye at its head/focus point (so head-bob shows), aimed along its facing. Tunable eye height; the world keeps running.

**Authoring & playback**
- **Record from freecam** — fly live and auto-lay keyframes at an interval.
- **Timeline scrubber** — draggable keyframe markers, draggable playhead, frame/keyframe step buttons, loop-tail shading.
- **Playback easing** — none / ease-in-out / in / out, with an adjustable amount.
- **Loop** with a seamless wrap (no snap), **playback speed**, **preview/scrub**.
- **Control Link during playback** — drive Link with the controller while the camera flies its path (great with Look-at-Link).
- **Copy / Paste / Insert at playhead**, full **undo/redo**, and **Save / Load** paths to JSON.

**Cinematic presentation**
- **Hide HUD** while filming (reuses SoH's `NoUI` state).
- **Letterbox bars** with adjustable size.
- **Composition grid** — rule-of-thirds (3×3), 4×4, or 5×5 framing overlay.

---

## 🚀 Getting started

1. Open the menu → **Dev Tools** tab → **Cinematic Cam** sidebar entry.
2. Toggle **Enable Cinematic Camera** (or bind a button — see Controls).
3. Click **Open Path Editor** to author camera paths.

> The Dev Tools menu is enabled by default; if hidden, turn on the menu with the usual SoH hotkey.

---

## 🎮 Controls (defaults — all rebindable)

Movement and look are the analog sticks (remap them in **Controller Configuration**). Button actions live in **Dev Tools → Cinematic Cam → Controls**:

| Action | Default |
| --- | --- |
| Move / strafe | Left stick |
| Look (yaw / pitch) | Right stick |
| Toggle camera | Additional Button 1 *(map Select/Back to it)* |
| Boost (faster) | R |
| Precision (slower) | L |
| Ascend / Descend | A / Z |
| FOV in / out | D-pad ↑ / ↓ |
| Roll left / right | D-pad ← / → |

In the path editor, **click** a marker in the world or on the timeline to select it; **drag** to reposition (gizmo) or retime (timeline).

---

## 🛠️ A typical workflow

1. **Enable** the free camera and fly to your opening shot.
2. **Add Keyframe** (or hit **Record** and fly the whole move live).
3. Build a few keyframes; tweak each with the **Move / Rotate / Bend** gizmos or numeric fields.
4. Set **aim** per keyframe (free, or track Link / an actor / a point), or use a path-level **Aim override**.
5. **Play** — tune **Speed**, **Easing**, **Loop**, and retime markers on the **timeline**.
6. Turn on **Letterbox** / **Hide HUD** / **Composition grid** for the final framing, then capture.
7. **Save** the path by name (`cinematics/<name>.json`).

---

## ⚙️ Settings reference

All settings live under the `gEnhancements.CinematicCam.*` CVars and the **Dev Tools → Cinematic Cam** menu:

- **Move Speed / Boost Multiplier / Look Sensitivity / Smoothing**
- **Invert Look X / Y**
- **Freeze World**, **Hide HUD**
- **Extend Draw Distance** (+ multiplier), **Far Clip Plane**
- **Letterbox bars** (+ size), **Composition grid**
- **Controls** — per-action button bindings

---

## 🧱 Building from source

This ships as part of the SoH fork. Build SoH normally (see the project's [`docs/BUILDING.md`](../../../docs/BUILDING.md)); the cinematic camera is compiled in automatically. The implementation is:

- `soh/src/code/z_camera.c` — engine-side free camera, view override, culling, input isolation, and the C bridges.
- `soh/soh/Enhancements/CinematicCam/` — the path engine + editor window (`CinematicCamPath.{h,cpp}`) and the C/C++ bridge header (`CinematicCamBridge.h`).
- `soh/soh/SohGui/SohMenuDevTools.cpp` — the settings & controls menu.

---

## 🗺️ Roadmap

- **Cutscene system** — a path library, **sequenced shots** (multiple paths back-to-back with cuts/fades), and **triggers** that fire cinematics on game events (enter a room, a flag set, talk to an actor, walk into a region) — turning the editor into a tool modders can ship.
- **Camera shake**, transitions/crossfades, and per-shot actions (sound / text / fade).

---

## 📝 Notes

- Actor references in Look-at / Spectate / Aim-override are tracked live and re-acquired by id, so they survive an instance changing; saved paths re-find actors by id within the same scene.
- Actor POV uses position + facing (+ animated focus point); OOT actors don't expose a generic head-bone orientation, so head *tilt* won't always carry through.

*Built for the Ship of Harkinian community. 🚢*
