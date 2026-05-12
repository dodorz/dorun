# Versioning

`DoRun` uses a four-part version number:

`major.minor.patch.build`

Rules:

- `major`, `minor`, and `patch` change only for deliberate release-level version bumps.
- `build` increases by `1` for every commit.
- After each commit, create a Git tag named with the full version, using the `vmajor.minor.patch.build` format.
- Keep `src/version.h` and `src/DoRun.rc` in sync when the build number changes.
- GitHub Actions builds every pushed full version tag. It publishes a GitHub Release only when the tag changes `major.minor.patch` compared with the previous full version tag; build-only increments do not publish a Release.
- Release assets are named `DoRun-vmajor.minor.patch.build-windows-x64-static.exe` and `DoRun-vmajor.minor.patch.build-windows-x64.exe`.

Current version: `0.1.4.17`
