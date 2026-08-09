# Building from source

The x64 and ARM64 builds use the same workflow locally and in GitHub Actions.

1. Fork or clone `https://github.com/inspirezuza/uxplay-studio`.
2. Install the prerequisites in [DEVELOPERS-GUIDE.md](./DEVELOPERS-GUIDE.md).
3. Run `.\build.ps1 package -Architecture x64 -SkipInstaller`.
4. Find the verified portable archive under `out\x64\artifacts`.

GitHub Actions publishes both architecture artifacts for every change to `main`.
