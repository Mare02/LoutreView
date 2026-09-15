# AI Action Prompts

This file lists reusable prompts that show up in the **Actions** dropdown in the AI composer.
Each `## Heading` is one action; everything beneath it (until the next `##`) is the prompt that gets inserted into the draft when you pick the action.

Actions can also launch a brand-new sibling session in the current workstream
instead of prefilling the current input.

Recognized keys: `launch` (same-session | new-session), `model`
(provider:variant), `foreground` (true/false), `autoSubmit` (true/false),
`worktree` (true/false). `launch: same-session` is the default; omit the
block entirely to keep current behavior.

## Review Changed Files
/review changed files in this session and call out regression risk in the affected modules.

## Plan Implementation
Look at the issue / feature request inside the active chat.

Produce a structured plan that:
- breaks the work into 3-5 phases
- identifies the files I'll need to touch
- flags any cross-cutting concerns I should think about before writing code

When you're done, grill me about about the things u are not sure and confirm everything and all changes with me before starting implementation.

## Draft Release Notes
/release-notes from merged work since the last tag, formatted as a user-facing changelog.

## Build and Publish Release
/release the current LoutreView version across all supported platforms.

Use the repository's custom `release-manager` agent from
`.agents/agents/release-manager/agent.md` and follow its release procedure:
- inspect the current branch, working tree, latest tag, and `include/version.h`
- confirm the release version and ensure the `v*` tag will match `VERSION`
- only release from the `main` branch after the intended changes are present
- run the required local validation before publishing, including `make clean && make`, `./loutre-view --version`, `./loutre-view --once --no-color`, `make test`, `make linux-test` when Docker is available, and `git diff --check`
- create and push the matching version tag to trigger `.github/workflows/release.yml`
- monitor the workflow and confirm native builds complete for Linux amd64, Linux arm64, macOS Intel, and macOS Apple Silicon
- confirm the GitHub Release contains all four archives and `checksums.txt`, then verify the published assets and installer download path

Do not claim that a release was published without observing the tag push, successful workflow, release assets, and post-publish checks. Stop and report the exact blocker if versioning, branch state, validation, permissions, or GitHub publishing is not ready.
