# Studio guide

UxPlay Studio 2 keeps mirroring, layout editing, recording, recovery, and export in one native window.

## Studio workspace

- **Live** shows the direct embedded AirPlay renderer. This is the lowest-latency view and the target used by fullscreen.
- **Edit layout** shows the composition canvas. Select one or more layers, drag to move, use the handles to resize, and drag a handle while holding `Alt` to crop. The rotation handle is above the selected layer.
- Arrow keys nudge a selection by 8 canvas pixels. Hold `Shift` for 24 pixels. Hold `Ctrl` while dragging to temporarily disable snapping.
- **Fit**, **Center**, and **Reset** are non-destructive commands and participate in undo/redo.
- The Wide and Vertical buttons switch between independent 1920×1080 and 1080×1920 compositions. Editing one does not overwrite the other.

## Sources and layers

AirPlay is created automatically. Camera, image, text, and color sources can be added from the Sources section. Each source gets a layer in both output formats; transforms remain independent.

Layers are drawn from bottom to top. Drag them in the list or use Up/Down to change order. A layer can be hidden, locked, removed, given an opacity, or masked as a rounded rectangle or circle. Crop, rotation, opacity, and masks are applied by the MP4 exporter as well as the canvas preview.

## Recording

Select optional camera and microphone tracks, then press **Record** while an iPad is mirroring. Every recording creates a new project under `Videos\UxPlay Studio` with independent files for:

- AirPlay video
- AirPlay/system audio loopback
- presenter camera, when enabled
- presenter microphone, when enabled

Video encoding uses the Windows Media Foundation H.264 path. Each track is bounded and segmented every 30 seconds. Recording runs beside—not inside—the live AirPlay renderer, so the receiver can keep dropping stale frames instead of accumulating latency.

If an optional device is unavailable, the main AirPlay recording continues and the Studio shows a warning. The AirPlay video track is required.

## Recovery and export

Projects left in Recording, Finalizing, or Exporting state after an interruption are marked **Recoverable** on the next launch. Open Projects, select the entry, then choose **Recover session**. Already finalized Matroska segments are retained.

Open any project in Studio, choose Wide or Vertical, adjust the layers, and select **Export MP4**. Export runs in the background and writes to the project's `exports` folder. It uses the saved position, size, crop, arbitrary rotation, opacity, order, and masks, and mixes available AirPlay and microphone audio.

## Shortcuts

- `F11`: live video-only fullscreen
- `Esc`: leave fullscreen
- `Ctrl+Z` / `Ctrl+Y`: undo / redo on the canvas
- Arrow keys: nudge selected unlocked layers
- `Shift` + arrows: larger nudge
- `Delete`: remove selected layers
- `Alt` + handle drag: crop
- `Ctrl` while dragging: disable snap temporarily
