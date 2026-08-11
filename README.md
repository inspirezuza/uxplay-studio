<p align="center">
  <img src="docs/uxplay-studio-icon.png" width="128" alt="UxPlay Studio icon">
</p>

# UxPlay Studio

Modern, single-window AirPlay screen mirroring for Windows.

UxPlay Studio keeps the receiver, live video, connection status, settings, activity log, and diagnostics in one native Windows app. The mirrored screen is rendered directly into the app through GStreamer's D3D11 video overlay—there is no separate playback window to chase around the desktop.

> [!NOTE]
> UxPlay Studio is an independent open-source project built on [UxPlay](https://github.com/FDH2/UxPlay) and [uxplay-windows](https://github.com/leapbtw/uxplay-windows). It is not affiliated with or endorsed by Apple Inc.

## Highlights

- Embedded AirPlay video in the main app window
- True video-only fullscreen with `F11` and `Esc`; no app chrome or detached window
- Clear receiver states: starting, ready, connecting, sharing, and recovery
- Device, resolution, and session-duration status
- Responsive 1080p60 and busy-Wi-Fi 720p30 low-latency profiles
- Optional four-digit PIN for shared Wi-Fi networks
- Local activity log and copyable diagnostics
- Bonjour and Bluetooth discovery support
- Automatic recovery with capped backoff
- Direct3D11 hardware decode, conversion, and display without a CPU frame copy
- Bounded low-latency buffering that drops stale frames instead of falling farther behind
- Portable x64 and ARM64 build pipeline

## Download

Download the latest portable ZIP from [Releases](https://github.com/inspirezuza/uxplay-studio/releases). Extract it and run `uxplay-studio.exe`.

Windows may display a SmartScreen warning because community builds are not code-signed. The complete source and reproducible build workflow are available in this repository.

## Use

1. Connect the Windows PC and iPhone or iPad to the same Wi-Fi network.
2. Start UxPlay Studio and wait for **Ready to mirror**.
3. Open Control Center on the Apple device.
4. Select **Screen Mirroring**, then select the receiver name shown by UxPlay Studio.

Some managed, hotel, dorm, or guest Wi-Fi networks block multicast DNS or isolate clients. In that case use a network that permits device-to-device traffic, or enable the Bluetooth discovery fallback. A hotspot is not required when normal Wi-Fi permits discovery and local traffic.

## Build from source

Requirements:

- Windows 10 or 11
- [MSYS2](https://www.msys2.org/) at `C:\msys64`
- Native Windows Python 3.14
- Visual Studio Build Tools with the C++ workload
- .NET 8 only when building the MSI

```powershell
git clone https://github.com/inspirezuza/uxplay-studio.git
cd uxplay-studio
.\build.ps1 bootstrap -Architecture x64
.\build.ps1 package -Architecture x64 -SkipInstaller
```

The verified portable ZIP is written to `out\x64\artifacts`. See [the developer guide](docs/DEVELOPERS-GUIDE.md) for x64 and ARM64 details.

## Architecture

```text
Qt MainWindow
  ├─ Player / native VideoSurface (HWND)
  ├─ Activity, Settings, Diagnostics
  └─ ReceiverEngine state machine
       └─ vendored libuxplay
            └─ GStreamer D3D11 zero-copy pipeline
                 ├─ d3d11h264dec → 2-frame leaky queue
                 └─ d3d11convert → d3d11videosink
                      └─ GstVideoOverlay → VideoSurface HWND
```

`ReceiverEngine` is the app-facing seam. UxPlay, GStreamer, worker threads, restart policy, and native video embedding stay behind that interface. Tests exercise configuration, state behavior, video-surface ownership, and true-fullscreen restoration without needing an AirPlay device.

## Privacy and security

UxPlay Studio operates locally. It does not upload screen content, activity, or diagnostics. On a shared network, enable the four-digit AirPlay PIN in Settings. Diagnostics intentionally exclude the PIN.

## Contributing

Bug reports and pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a change. Security issues should follow [SECURITY.md](SECURITY.md).

## Credits and license

UxPlay Studio is licensed under the [GNU General Public License v3.0 or later](LICENSE). It contains a vendored, modified copy of UxPlay; upstream attribution and the exact source revision are documented in [NOTICE.md](NOTICE.md) and `libuxplay/UPSTREAM_COMMIT`.
