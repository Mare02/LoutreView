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

### Structure

```text
.
├── src/                    Shared CLI, sampling, JSON, and terminal UI
│   └── ui/                 Dashboard, network, startup, terminal rendering
├── include/                Shared models and public interfaces
├── platform/
│   ├── common/             Shared POSIX collectors
│   ├── macos/              macOS system APIs
│   └── linux/              procfs/sysfs and startup collectors
├── tests/                  C tests, installer fixtures, Python smoke test
├── assets/                 README screenshots and branding
├── .github/workflows/      CI and tagged release automation
├── .agents/agents/         Project-specific operating procedures
├── Makefile                Platform selection, builds, tests, Linux container test
├── install.sh              Release installer with checksum verification
└── Dockerfile.linux-test   Disposable Linux validation image
```

Keep OS-specific headers and APIs inside `platform/<os>/`; shared code must
remain portable. Platform metrics do not have to be semantically identical:
Linux procfs/sysfs/PSI and macOS system APIs can represent different signals.
The root `loutre-view` binary and `build/` contents are generated artifacts.

## Agents

Read the relevant agent file before using it. For releases, read
`.agents/agents/release-manager/agent.md` only after the intended changes are
on the release branch `main`; it handles versioning, macOS/Linux release
builds, archives, checksums, GitHub publishing, and public download
verification. Feature branches are not released directly. Preserve unrelated
changes and do not claim release success without observed verification.

The CI workflows are part of the project contract:

- `.github/workflows/ci.yml` builds Linux amd64/arm64 and macOS arm64/Intel,
  runs the test suite, and runs AddressSanitizer/UndefinedBehaviorSanitizer
  checks.
- `.github/workflows/release.yml` is tag-triggered and publishes four platform
  archives plus `checksums.txt`.

## Verification

Run `make clean && make`, `./loutre-view --version`, `./loutre-view --once
--no-color`, `make test`, and `git diff --check` for normal source changes.

For platform changes, validate macOS locally and Linux with GCC and Clang in
an isolated container or VM. Container mounts should be read-only; build in
the container filesystem. Never modify unrelated containers, volumes, or networks.
Run AddressSanitizer and UndefinedBehaviorSanitizer checks for collector or
sampling changes. `make linux-test` requires Docker, mounts the source
read-only, copies it into the container, and runs GCC and Clang tests there.
A container does not validate physical battery hardware, distro/libc
variation, desktop integration, or a complete systemd-user session; report
those limitations explicitly.

`make test` includes the C tests, `tests/test_installer.sh`, and
`tests/smoke.py` when present. Keep generated binaries and build output out of
commits unless explicitly requested.
