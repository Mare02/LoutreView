# Self-update command

Add `loutre-view update` to update the executable the user invoked. Reuse the release download and SHA-256 verification flow, select the matching macOS/Linux archive, and replace that executable only after verification succeeds. If the install location is not writable, report a clear error without invoking `sudo` or changing another installation.
