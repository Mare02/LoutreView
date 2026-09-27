# Test and merge per-mount disk view

Validate and merge the `codex/per-mount-disk-view` branch into `main` after all required checks pass.

- Run `make clean && make`, `./loutre-view --version`, `./loutre-view --once`, `make test`, and `git diff --check`.
- Validate the Linux collectors with `make linux-test` (GCC and Clang) and run the required AddressSanitizer and UndefinedBehaviorSanitizer checks.
- Check the interactive views at wide and narrow terminal sizes, including the disk view and stable refresh rendering.
- Fix any failures on the branch, rerun affected checks, and merge into `main` only when validation passes.

Linux validation was previously blocked because Docker Desktop was not running; retry it when completing this task. Do not publish a release.
