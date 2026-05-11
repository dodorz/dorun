# Versioning

`DoRun` uses a four-part version number:

`major.minor.patch.build`

Rules:

- `major`, `minor`, and `patch` change only for deliberate release-level version bumps.
- `build` increases by `1` for every commit.
- After each commit, create a Git tag named with the full version, using the `vmajor.minor.patch.build` format.
- Keep `src/version.h` and `src/DoRun.rc` in sync when the build number changes.

Current version: `0.1.4.16`
