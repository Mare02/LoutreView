# LoutreView

## Project

- Native macOS and Linux terminal monitor written in C11.
- `src/` contains shared CLI, sampling, JSON, and terminal UI code; `include/` defines the shared model and interfaces.
- `platform/macos/` uses Apple system APIs; `platform/linux/` reads procfs/sysfs and inspects systemd/XDG startup configuration.
- `platform/common/` contains shared POSIX collectors. Keep OS headers out of the shared core.
- `Makefile` selects exactly one platform backend and builds `loutre-view` with strict compiler warnings.
- `include/version.h` contains the application version.
- `install.sh` installs binaries from GitHub Releases and verifies SHA-256 checksums.
- `assets/` contains README screenshots and branding.
- `.agents/agents/` contains project-specific custom agents.

## Agents

Read the relevant agent file before using it. For releases, use
`.agents/agents/release-manager/agent.md` after changes land on `main`; it
handles versioning, macOS builds, archives, checksums, GitHub publishing, and
public download verification. Preserve unrelated changes and do not claim
release success without observed verification.

## Verification

Run `make clean && make`, `./loutre-view --version`, `./loutre-view --once
--no-color`, `make test`, and `git diff --check` for normal source changes.

For platform changes, validate macOS locally and Linux with GCC and Clang in
an isolated container or VM. Container mounts should be read-only; build in
the container filesystem. Never modify unrelated containers, volumes, or networks.
Run AddressSanitizer and UndefinedBehaviorSanitizer checks for collector or
sampling changes. A container does not validate physical battery hardware or
a complete desktop/systemd session; report those limitations explicitly.
