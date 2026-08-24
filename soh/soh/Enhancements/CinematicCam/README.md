# 🎥 Cinematic Camera for Ship of Harkinian

A **cinematic camera toolkit** for [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright): a detached
free-fly camera, a keyframe path editor, and a curve editor — everything you need to plan, shoot and re-shoot a
camera move through *The Legend of Zelda: Ocarina of Time*.

Fly anywhere, frame the shot, drop keyframes, and play back a move that actually looks like it was shot on a
crane rather than snapped between poses.

> **Status — release candidate for 1.0.** The free camera, path editor, curve editor and presentation tools are
> all in and stable. A scripted multi-path **cutscene system** (sequencing + triggers) is the next milestone.

> **📷 Image slot — `docs/media/hero.gif`**
> A 10–15 s loop of a finished move: a slow push-in through a landmark scene, letterbox on, HUD hidden.
> This is the "why you'd want this" shot — it should look like footage, not like a tool.

---

## ✨ What it does

### The free camera

A fully detached, controller-driven camera that flies anywhere in the scene, independent of Link.

- **Ignore culling** — actors keep drawing and the far clip plane pushes out while you fly, so nothing pops in
  and out of a shot.
- **Inertia** — one slider takes you from crisp 1:1 to floaty handheld glide.
- **Freeze the world**, or leave the action running and shoot it live.
- **Boost** and **precision** modifiers for covering ground and for the last few units.
- Fully rebindable; sticks remap in SoH's normal Controller Configuration.

> **📷 Image slot — `docs/media/freecam.gif`**
> A few seconds of flying: boost across a field, then precision-crawl up to a detail.

### Keyframes and the path

Fly to a pose, press **Add Keyframe**, repeat. The camera plays a smooth spline through them.

- Each keyframe stores **eye, look-at, roll and FOV**.
- **In-world overlay** — the spline itself, numbered markers, facing indicators and a live playhead, drawn over
  the game at any resolution.
- **Transform gizmos** on the selected keyframe:
  - **Move** — drag the X/Y/Z axes to reposition it.
  - **Rotate (aim)** — drag rings for yaw / pitch / roll.
  - **Bend (path)** — rotate the spline's tangent to bend the curve through the point, without touching the aim.
- **Per-keyframe shape** — Tension / Continuity / Bias, or plain Linear.
- **Numeric fields** for exact position, yaw, pitch, roll and FOV.
- **Six aim modes per keyframe**: free orientation, look at a point, look at **Link**, look at **any actor**
  (searchable, distance-sorted picker), look at the **shared movable target**, or **follow the path** like a
  dolly on a rail.
- **Path-level aim override** — re-point an entire recorded flight at one target in a single click.
- **Record from freecam** — fly the move live and have keyframes laid down at an interval.
- **Smooth path** and **Normalize speed** clean up a hand-built or recorded path in one press.
- **Auto-orbit** — generate a circle or arc of keyframes around Link, an actor, the target or the camera, each
  one already aimed at the centre. A full 360° arc turns looping on for you.

> **📷 Image slot — `docs/media/world-overlay.png`**
> A screenshot of the in-world overlay: the spline curve, numbered markers, and the move gizmo on a selected
> keyframe. Ideally somewhere with depth so the curve reads in 3D.

> **📷 Image slot — `docs/media/gizmos.gif`**
> Dragging the three gizmo modes in turn — move, rotate (aim), bend (path) — so the difference is obvious.

### The timeline

Where the shot is *timed*. The path's shape comes only from where the keyframes are in the world; the timeline
decides how fast the camera travels between them, and never bends the curve.

- Draggable **keyframe markers** and a draggable **playhead**, with frame and keyframe step buttons.
- **Multi-select** (Ctrl+click), **range select** (Ctrl+Shift+click), **ripple** (Shift+drag), and
  **compress / expand a group** by Alt-dragging either end of a selection.
- **Automation lanes** underneath for every keyframed parameter.
- Type a new **total duration** — or select a span and retime just that span.

> **📷 Image slot — `docs/media/timeline.png`**
> The timeline with several keyframes, a multi-selection, and at least one automation lane populated.

### The curve editor

A value-over-time graph for every animatable channel — camera roll and FOV, the aim target's X/Y/Z, letterbox,
time of day, shake intensity, green screen.

- **Step / Linear / Smooth / Bezier** per key, with draggable tangent handles on Bezier keys.
- Every enabled channel is drawn at once, each scaled to its own range; the active one is bright and editable.
- **Speed graph** overlay — what the camera actually *does* over the timeline. Drag a keyframe's point to set
  the camera's speed there, and its handles to shape how it accelerates in and out.
- **Precision aids**: hold **Q** while dragging to lock the drag to one axis, and turn on **Snap** to land every
  drag on a grid that refines as you zoom in.

> **📷 Image slot — `docs/media/curve-editor.png`**
> The curve editor with two or three channels enabled and a Bezier key selected so its handles show.

> **📷 Image slot — `docs/media/speed-graph.gif`**
> Dragging a speed point and its acceleration handle, with the resulting change visible in the curve.

### Watching an actor

- **Actor POV (spectate)** — see the world through any actor's eyes, riding its animated focus point so head bob
  and head turns come through. Tunable eye height; the world keeps running.
- **Follow actor** — attach the free camera to a moving actor and keep your framing relative to it as it goes.
- **Hands-off mode** — the controller plays Link normally while the camera rides the followed actor and keeps
  Link in frame automatically. Good for gameplay-style coverage you don't have to fly.

> **📷 Image slot — `docs/media/spectate.gif`**
> A few seconds of Actor POV on something with a distinctive gait or head movement.

### Presentation

- **Hide HUD** while filming.
- **Letterbox bars**, adjustable — 0.12 of the screen each side gives roughly 2.35:1 from 16:9.
- **Composition grid** — rule-of-thirds, 4×4 or 5×5.
- **Green screen** — replace the sky with a solid chroma-key colour (green or blue) and hide the sun, moon and
  sky glow, so you can key it out when compositing. Scene geometry still draws over it.
- **Camera shake** — smooth handheld jitter, deterministic so it looks the same on every replay, with a
  keyframable intensity you can ramp across a shot.

> **📷 Image slot — `docs/media/presentation.png`**
> One frame with letterbox, the thirds grid and the HUD hidden — ideally a shot you'd actually keep.

> **📷 Image slot — `docs/media/greenscreen.png`**
> A before/after pair, or just the green-screen frame with the sky keyed flat.

---

## 🧰 Quality of life

The small things that stop a shoot from becoming an errand.

- **Sky & time** — scrub the time of day directly (with Noon / Sunset / Night presets), or **freeze the sky and
  clock** so clouds stop drifting and the lighting holds still. Essential for a loop that lines up.
- **Teleport to area** — fade straight to any area from a dropdown instead of walking there.
- **Pose Link** — a dial that turns Link to an exact heading, so he's facing the right way in frame.
- **No idle fidgets** — stop Link stretching and looking around, so he holds a clean standing pose.
- **Sync Link's idle to the loop** — restart his breathing animation each time the loop wraps, so a looping GIF
  has no visible seam.
- **Control Link during playback** — drive Link with the controller while the camera flies its path. Pairs
  naturally with a *Look at Link* aim.
- **Loop styles** — forward with a smooth return leg, or ping-pong. An optional one-frame **loop-start marker**
  flashes in the corner so you can find the exact boundary in a recording and trim there.
- **Save / Load** — paths are JSON in `cinematics/`, with an **autosave** every minute while you have unsaved
  changes. A saved path can **remember where it was shot** and fade-warp you back there on load.
- **Undo / redo** throughout, plus **copy / paste** and **insert a keyframe at the playhead**.
- **Shooting bar** — collapse the whole editor to a slim transport strip so the game keeps the screen. The world
  gizmos and every keyboard shortcut stay live. It can minimise itself while playing and expand again when you
  stop.
- **Window opacity** — fade the editor so the game reads through it.

> **📷 Image slot — `docs/media/shooting-bar.png`**
> The shooting bar over gameplay, showing how little screen it takes.

---

## 🚀 Getting started

1. Open the menu → **Dev Tools** → **Cinematic Cam**.
2. Turn on **Enable Cinematic Camera** (or bind it to a button — see Controls).
3. Click **Open Path Editor**.

> **📷 Image slot — `docs/media/menu.png`**
> The Dev Tools → Cinematic Cam menu page, so people know what they're looking for.

### A typical shot

1. **Enable** the free camera and fly to the opening frame.
2. **Add Keyframe** — or hit **Record** and fly the whole move live.
3. Build a few more. Adjust each with the **move / rotate / bend** gizmos or the numeric fields.
4. Set the **aim** per keyframe, or point the whole path at one thing with the **aim override**.
5. **Play.** Retime on the timeline, shape the acceleration on the **speed graph**, add **easing** and **loop**.
6. Turn on **letterbox**, **hide HUD** and the **composition grid** for the final framing, then capture.
7. **Save** it by name — it lands in `cinematics/<name>.json`.

---

## 🎮 Controls

Movement and look are the analog sticks (remap them in **Controller Configuration**). The button actions below
live in **Dev Tools → Cinematic Cam → Controls** and are all rebindable.

| Action | Default |
| --- | --- |
| Move / strafe | Left stick (up = forward) |
| Look (yaw / pitch) | Right stick |
| Toggle camera | Additional Button 1 *(map Select/Back to it)* |
| Boost (faster) | A |
| Precision (slower) | L |
| Ascend | R |
| Descend | Z |
| FOV in / out | D-pad ↑ / ↓ |
| Roll left / right | D-pad ← / → |

### Keyboard, with the path editor focused

| Key | Action |
| --- | --- |
| `Space` | Play / stop |
| `,` `.` | Step back / forward one tick |
| `[` `]` | Previous / next keyframe |
| `K` | Add a keyframe at the current pose |
| `Del` | Delete the selection |
| `Ctrl` + `A` | Select every keyframe |
| `Ctrl` + `Z` / `Ctrl` + `Y` | Undo / redo |

### Mouse, in the timeline and the curve editor

| Input | Action |
| --- | --- |
| Drag | Move a keyframe |
| `Ctrl` + click | Add / remove from the selection |
| `Ctrl` + `Shift` + click | Select a range |
| `Shift` + drag | Ripple — push this keyframe and everything after it |
| `Alt` + drag a selection's end | Compress / expand the group |
| Hold `Q` while dragging | Lock the drag to one axis |
| Wheel / middle-drag | Zoom time / pan |
| Double-click / right-click *(curve editor)* | Add / delete a key |
| `Alt` + drag a handle | Break the handle pair and bend one side alone |

---

## ⚙️ Settings

Everything lives under **Dev Tools → Cinematic Cam** (CVars: `gEnhancements.CinematicCam.*`).

- **Move Speed**, **Boost Multiplier**, **Look Sensitivity**, **Smoothing**, **Invert Look X / Y**
- **Freeze World**, **Freeze Sky & Time**, **No Idle Fidgets**, **Sync Link Idle to Loop Start**
- **Hide HUD**, **Letterbox bars** (+ size), **Composition grid**, **Show readout**
- **Extend Draw Distance** (+ multiplier) and **Far Clip Plane**
- **Controls** — per-action button bindings

---

## 🗺️ Roadmap

- **Cutscene system** — a path library, **sequenced shots** (several paths back to back with cuts and fades),
  and **triggers** that fire a cinematic on a game event: entering a room, a flag being set, talking to an
  actor, walking into a region. That's the step that turns this from a capture tool into something modders can
  ship inside a mod.
- Transitions and crossfades, and per-shot actions (sound, text, fade).

---

## 📝 Notes

- Actor references in Look-at, Spectate and Aim-override are tracked live and re-acquired by id, so they survive
  an actor instance being replaced; saved paths re-find their actor by id within the same scene.
- Actor POV uses position, facing and the animated focus point. OOT actors don't expose a general head-bone
  orientation, so head *tilt* won't always carry through.
- Nothing in the editor auto-saves over your work: edits live in memory until you press **Save**, and the
  once-a-minute autosave writes to a separate `<name>_autosave.json`.

---

## 🧱 Building

This ships as part of the SoH fork — build SoH normally (see [`docs/BUILDING.md`](../../../docs/BUILDING.md))
and the cinematic camera is compiled in automatically.

*Built for the Ship of Harkinian community. 🚢*
