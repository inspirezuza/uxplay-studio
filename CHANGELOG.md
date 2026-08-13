# Changelog

## 2.0.2

- Fixed a startup crash caused by iterating over two different temporary Qt argument lists.
- Added an isolated packaged-UI startup smoke test so releases must construct the real Studio window and render a snapshot successfully.

## 2.0.1

- Moved direct AirPlay recording writes behind a bounded asynchronous handoff so a slow muxer cannot stall live mirroring; queue/start/finalization failures now leave the project Recoverable.
- Added a live in-app presenter camera preview and draggable/resizable self-view without creating another top-level window.
- Aligned independently started AirPlay, camera, microphone, and system-audio tracks during export using measured first-sample offsets.
- Made exported crop, rotation, masks, and placement match the canvas preview, including real source-pixel crop before scaling.
- Validated finalized media during recovery, preserved invalid fragments with an `.incomplete` suffix, and surfaced actionable recovery errors.
- Made receiver shutdown safe when its worker exits slowly and changed renderer startup failures from fatal process termination to recoverable errors.
- Fixed receiver Stop behavior during retry/backoff and kept the in-app camera self-view available in true fullscreen.
- Prevented Recoverable projects from bypassing media validation through Export.
- Kept source projects Ready after an export failure or cancellation so users can fix the layout and retry.
- Published exports only after a completed pipeline by moving a `.partial` file into place.
- Included measured track offsets in export duration so static overlays remain visible through delayed tracks.
- Restored empty projects to Ready after a direct-track startup failure so recording can be retried.
- Surfaced asynchronous mux, recording-track, and camera-preview failures while they happen, and kept projects non-Ready when media or an unpublished export could not be cleaned up safely.

## 2.0.0

- Rebuilt the main experience as a modern, single-window Studio workspace.
- Added OBS-style sources, layers, direct canvas transforms, undo/redo, and independent Wide/Vertical layouts.
- Added local independent-track recording with bounded queues, 30-second Matroska segments, atomic project metadata, and interrupted-session recovery.
- Added background MP4 composition/export with crop, arbitrary rotation, opacity, ordering, and Cairo shape masks.
- Added direct encoded AirPlay recording without re-encoding, plus hardware H.264 presenter recording/export through Windows Media Foundation.
- Preserved true borderless video-only fullscreen and the fail-closed embedded-renderer invariant.
