# Contributing

Thank you for helping improve UxPlay Studio.

## Before opening an issue

- Confirm the PC and Apple device are on the same local network.
- Check whether client isolation or multicast filtering is enabled.
- Copy the report from the app's Diagnostics page.
- Remove device names or addresses you do not want to share publicly.

## Development workflow

```powershell
.\build.ps1 bootstrap -Architecture x64
.\build.ps1 build -Architecture x64 -SkipBootstrap
```

The build runs the Qt unit tests automatically. Before a pull request, create and verify a portable bundle:

```powershell
.\build.ps1 package -Architecture x64 -SkipBootstrap -SkipInstaller
.\build.ps1 test -Architecture x64
```

Keep the single-window invariant: embedded mode must never fall back to a separate GStreamer video window. Add or update tests for state, configuration, and UI-seam changes.

By contributing, you agree that your contribution is licensed under GPL-3.0-or-later.
