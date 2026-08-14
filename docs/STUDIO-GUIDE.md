# Studio guide

UxPlay Studio 2 keeps mirroring, layout editing, recording, recovery, and export in one native window.

## Studio workspace

UxPlay Studio uses one OBS-style workspace: **Edit layout** is both the live mirror preview and the composition editor. There is no separate Live page and no second user-facing monitor. The native AirPlay video target stays embedded internally while bounded preview frames are rendered into the selected scene layer.

Use the exclusive **16:9** and **9:16** tabs to switch between independent 1920×1080 and 1080×1920 compositions. Select one or more layers, drag to move, use the edge/corner handles to resize, and drag a handle while holding `Alt` to crop. The top dot rotates the selected layer; the top edge handle always wins when expanding upward, including when zoomed out.

Right-click the canvas, a layer, a source, or a project for contextual actions such as add, duplicate, reorder, fit, rotate, lock, delete, open, and refresh. The canvas footer also exposes fit and zoom controls, and the hint explains the discoverability shortcuts.
- Arrow keys nudge a selection by 8 canvas pixels. Hold `Shift` for 24 pixels. Hold `Ctrl` while dragging to temporarily disable snapping.
- **Fit**, **Center**, and **Reset** are non-destructive commands and participate in undo/redo.
- The Wide and Vertical buttons switch between independent 1920×1080 and 1080×1920 compositions. Editing one does not overwrite the other.

## Sources and layers

AirPlay is created automatically. Camera, image, text, and color sources can be added from the Sources section. Each source gets a layer in both output formats; transforms remain independent. Adding Camera starts a live presenter preview rendered directly in its scene layer, so the canvas preview and export layout stay aligned without a duplicate native self-view.

Layers are drawn from bottom to top. Drag them in the list or use Up/Down to change order. A layer can be hidden, locked, removed, given an opacity, or masked as a rounded rectangle or circle. Crop, rotation, opacity, and masks are applied by the MP4 exporter as well as the canvas preview.

## Recording

Select optional camera and microphone tracks, then press **Record** while an iPad is mirroring. Every recording creates a new project under `Videos\UxPlay Studio` with independent files for:

- AirPlay video
- AirPlay/system audio loopback
- presenter camera, when enabled
- presenter microphone, when enabled

The AirPlay track preserves the encoded stream, so it stays isolated from window movement, fullscreen, page changes, or other windows covering Studio. Its receive callback places packets into a bounded nonblocking handoff; a dedicated worker performs muxing and finalization away from the low-latency display path. If the worker cannot keep up, the recording is marked failed/Recoverable instead of slowing the live screen. Camera and microphone tracks are recorded independently, and every track is segmented every 30 seconds. The editor uses bounded live or recorded preview frames so transforms reflect the media that will be exported. Studio observes the first accepted sample on each track, stores their relative monotonic offsets, and reapplies those measured offsets during export.

If an optional device is unavailable, the main AirPlay recording continues and the Studio shows a warning. The AirPlay video track is required.

## Recovery and export

Projects left in Recording or Finalizing state after an interruption are marked **Recoverable** on the next launch. Open Projects, select the entry, then choose **Recover session**. Studio validates finalized Matroska streams before marking the project Ready, retains usable segments, and renames rejected partial/corrupt fragments with an `.incomplete` suffix rather than deleting them. A project without usable AirPlay video stays Recoverable and explains why. An interrupted export does not damage its source project: Studio returns the project to Ready and removes the unpublished `.partial` output.

Open a Ready project in Studio, choose Wide or Vertical, adjust the layers, and select **Export MP4**. Recoverable projects must pass **Recover session** first. Export runs in the background and writes to the project's `exports` folder. It uses the saved position, size, crop, arbitrary rotation, opacity, order, and masks, and mixes available system-loopback and microphone audio. The final MP4 name appears only after the export pipeline completes. Closing or quitting Studio is blocked while an export is active so the background job cannot be silently discarded.

## Shortcuts

- `F11`: composed-scene borderless fullscreen
- `Esc`: leave fullscreen
- `Ctrl+Z` / `Ctrl+Y`: undo / redo on the canvas
- Arrow keys: nudge selected unlocked layers
- `Shift` + arrows: larger nudge
- `Delete`: remove selected layers
- `Alt` + handle drag: crop
- `Ctrl` while dragging: disable snap temporarily
