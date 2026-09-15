# 🎥 Cinematic Camera for Ship of Harkinian

A full **cinematic camera toolkit** built into a fork of [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) — free camera, keyframed paths, a real timeline and curve editor, and everything needed to get a shot out of the game and into an editor.

[![Watch the trailer](https://img.youtube.com/vi/sdi8TfHfrFA/maxresdefault.jpg)](https://www.youtube.com/watch?v=sdi8TfHfrFA)

**▶ [Watch the trailer](https://www.youtube.com/watch?v=sdi8TfHfrFA)**

---

## What it does

- **Free camera** — a properly detached, controller-driven flycam, with boost and precision modifiers, smoothing, and keyframable FOV and roll.
- **Paths and keyframes** — drop keyframes or record a move live, then shape it with 3D gizmos, a timeline, a curve editor and a speed graph.
- **Aim modes** — look at a point, look at an actor, ride an actor's eyes, or follow one as it moves.
- **Presentation** — letterbox, composition grids, HUD hiding, camera shake, green screen, and a time-of-day scrubber that can freeze the sky.
- **Offline renderer** — plays the take and writes every frame as a PNG, on the game's own clock rather than a wall clock, so the timing is exact however slowly it renders.
- **Depth pass + Fusion export** — a one-click DaVinci Resolve camera and a 16-bit depth sequence, so 3D text and models can sit *inside* the scene and be occluded by the world.

**→ [Full documentation, controls and workflow](../soh/soh/Enhancements/CinematicCam/README.md)**

---

## Getting it

Grab a build from the [Actions tab](../../actions) — every push produces Windows, Linux and macOS artifacts. You still need your own Ocarina of Time ROM, exactly as with upstream Ship of Harkinian; see [`docs/BUILDING.md`](../docs/BUILDING.md) to build it yourself.

Once running: **Dev Tools → Cinematic Cam → Open Path Editor**.

---

## About this fork

This is a fork of Ship of Harkinian, tracking upstream `develop`. Everything upstream does still works — the cinematic camera is additive, and lives almost entirely in `soh/soh/Enhancements/CinematicCam/`.

For the emulator itself, its README, its Discord and its issue tracker, go to [HarbourMasters/Shipwright](https://github.com/HarbourMasters/Shipwright). Issues with the *camera toolkit* belong here.

Rendering is Windows-only for now (it reads the DirectX 11 back buffer); everything else works everywhere Ship of Harkinian does.
