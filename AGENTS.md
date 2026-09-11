# LoutreView

## Project

- Native macOS terminal monitor written in C11.
- Built with Apple Clang and the IOKit, CoreFoundation, and CoreServices frameworks.
- `loutre-view.c` contains the application; `Makefile` builds `loutre-view`.
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
--no-color`, and `git diff --check` for normal source changes.
