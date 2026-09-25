<p align="center">
  <img src="assets/loutre-view-logo.png" alt="LoutreView logo" width="180">
</p>

<h1 align="center">LoutreView</h1>

<p align="center">A fast, native terminal monitor for macOS and Linux.</p>

`loutre-view` is a native terminal monitor written in C11. Its dashboard shows CPU and memory usage, load averages, disk use, uptime, processes, network traffic, and battery and memory pressure data where available. It uses macOS system APIs or Linux `/proc` and `/sys`. The executable needs no language runtime such as Node or Python; it still depends on the operating system's native libraries, including libc.

![LoutreView dashboard](assets/loutre-view-dashboard.png)

## Requirements

- macOS or Linux; release archives target amd64 (x86_64) and arm64 (aarch64).
- Building on macOS: Xcode Command Line Tools (`xcode-select --install`).
- Building on Linux: GCC or Clang, GNU Make, and libc development headers.
- Installing a release: `curl`, `tar`, and either `sha256sum` or `shasum`.

The release workflow targets Ubuntu 22.04 with glibc for Linux builds. Binaries
built this way require a compatible glibc environment; they are not static or musl/Alpine binaries.
Build from source for other libc environments. Python 3 is used for development
smoke tests only.

## Install the latest release

The installer supports macOS and Linux when the selected release has a matching
archive. To install the latest prebuilt CLI:

```sh
curl -fsSL https://raw.githubusercontent.com/Mare02/LoutreView/main/install.sh | sh
```

This installs the binary to `~/.local/bin/loutre-view` and verifies the
download against the release checksum. To install a specific release, set
`LOUTREVIEW_VERSION` on the shell running the installer:

```sh
curl -fsSL https://raw.githubusercontent.com/Mare02/LoutreView/main/install.sh | LOUTREVIEW_VERSION=v0.2.5 sh
```

Set `LOUTREVIEW_INSTALL_DIR` the same way to change the destination. Installation
fails if the checksum is missing, the checksum tool fails, or verification fails.
The release workflow is configured to produce `checksums.txt` and the following
four archives, each with `loutre-view` at the archive root. This describes build
targets included in the current release:

- `loutre-view-darwin-amd64.tar.gz`
- `loutre-view-darwin-arm64.tar.gz`
- `loutre-view-linux-amd64.tar.gz`
- `loutre-view-linux-arm64.tar.gz`

## Build from this project

Clone the repository, enter its directory, and compile the executable:

```sh
git clone https://github.com/Mare02/LoutreView.git
cd LoutreView
make
```

The compiled program is `./loutre-view`. Re-run `make` after changing sources or
headers. Common code lives in `src/`, `src/ui/`, and `platform/common/`; the build
selects `platform/macos/` on Darwin and `platform/linux/` on Linux, and rejects
other operating systems. Headers, including `include/version.h`, live in
`include/`.

```sh
make CC=gcc                       # or CC=clang
make TARGET=dist/loutre-view build # choose the executable path
make CFLAGS='-O0 -g'              # debug build
make test                        # C tests, installer fixtures, optional Python smoke tests
make linux-test                 # isolated Linux GCC + Clang build and test run
```

Build objects are isolated by OS, compiler, and flags under `build/`. Both
GCC and Clang builds enable strict C11 warnings and treat warnings as errors.
`make test` compiles each `tests/test_*.c` separately against all non-main common,
UI, and selected platform sources; it also runs `tests/smoke.py` when present.
CI covers Linux GCC/Clang, macOS Clang, and address/undefined behavior sanitizers.
Pushing a `v*` tag matching `VERSION` in `include/version.h` triggers native
builds and tests for all four release archives, followed by SHA-256 generation
and GitHub Release publishing.

To test Linux locally from macOS, install and start Docker Desktop once, then
run `make linux-test` from the repository root. The command builds the local
test image, mounts the repository read-only, builds inside the container, runs
the Linux test suite with GCC and Clang, and removes the test container. It
does not use the host build output or modify other containers.

## Run from the project

```sh
./loutre-view
```

The interactive dashboard refreshes every second. Press `1` for the dashboard,
`2` or `n` for network statistics, `3` or `u` for AI usage, and `4` for Docker.
Press `Tab` to cycle through views. Press `Ctrl-C` to exit.

The Docker view refreshes local Engine API snapshots every two seconds. It lists
running container names and images with CPU, memory, and network rates. It uses
the local Docker Unix socket, including the `DOCKER_HOST=unix://...` override;
remote TCP and SSH endpoints are not supported. Docker data is only included in
the interactive view, not `--json` or `--json-stream`.

## Install as a command

### For your user account

Install to `~/.local/bin` without `sudo`:

```sh
mkdir -p ~/.local/bin
install -m 755 loutre-view ~/.local/bin/loutre-view
```

Make sure `~/.local/bin` is in your `PATH`, then open a new terminal (or
reload your shell configuration) and run:

```sh
loutre-view
```

For zsh, add this line to `~/.zshrc` if the directory is not already in your
`PATH`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

### System-wide

To make the command available to every user on the machine:

```sh
sudo install -m 755 loutre-view /usr/local/bin/loutre-view
loutre-view
```

Run `make` before either installation command. To update an existing
installation, rebuild and run the same `install` command again.

## Usage

```text
loutre-view [options]
```

Useful examples:

```sh
./loutre-view --once                 # one readable report
./loutre-view --json                 # one machine-readable report
./loutre-view --json-stream          # continuous newline-delimited JSON
./loutre-view --json-stream --include-usage # stream with coding CLI usage
./loutre-view --sort mem --limit 25  # biggest memory users
./loutre-view --interval 500         # refresh twice per second
./loutre-view --compact              # dense dashboard for any terminal width
loutre-view startup --json           # machine-readable startup inventory
loutre-view startup --once           # static one-shot startup report
```

Options:

| Option | Description |
| --- | --- |
| `-i`, `--interval MS` | Refresh interval in milliseconds; minimum `250`, default `1000`. |
| `-n`, `--limit COUNT` | Number of processes to show; default `12`. |
| `-s`, `--sort FIELD` | Sort processes by `cpu`, `mem`, `pid`, or `name`; default `cpu`. |
| `--compact` | Use a dense dashboard without usage bars or per-core meters. |
| `--once` | Print one text report and exit. |
| `--json` | Print one JSON report and exit; useful in scripts. |
| `--json-stream` | Print one flushed JSON object per sampling interval; use `--once` for one frame. |
| `--include-usage` | Add normalized coding CLI usage to `--json-stream` frames; requires `--json-stream`. |
| `-h`, `--help` | Show command help. |
| `-v`, `--version` | Show the installed version. |

`--json-stream` emits newline-delimited JSON (NDJSON). Each frame has
`schema_version`, `type`, `sequence`, `sample_time_monotonic`,
`sample_interval_ms`, and a `status` object before the existing metric fields.
Network interfaces are included in the stream frame under `network`; the
sampled CPU, process, system, and network values come from the same loop
iteration. Frames are independently parseable and flushed before the next
sampling interval, so consumers can process them without waiting for the
process to exit. The sequence starts at zero and increases by one. The
minimum interval is 250 ms. `SIGINT`, `SIGTERM`, and a closed consumer pipe
stop the stream cleanly. `--json` remains a single legacy JSON document and
does not emit stream metadata or usage data. Add `--include-usage` to opt into
a `usage` object containing installed-provider availability and normalized
windows from the existing cache/live provider layer.

### Coding CLI usage

Press `3` or `u` in the live dashboard to open the Coding CLI Usage view.
Press `Tab` to cycle through the dashboard, network, AI usage, and Docker views. The
AI usage view reads credential-free, machine-readable provider snapshots from the
user state directory and shows available quota windows and reset times. It
does not add usage data to `--json` output.

Provider bridges can ingest a payload from standard input:

```sh
loutre-view usage ingest claude < claude-statusline.json
```

Use `codex` or `gemini` for a matching provider bridge payload. Set
`LOUTREVIEW_USAGE_DIR` to choose a cache directory; otherwise LoutreView uses
`$XDG_STATE_HOME/loutre-view/usage` or `~/.local/state/loutre-view/usage`.

`--json` automatically enables `--once`. Color is disabled when output is
redirected. `--compact` is always opt-in; the full dashboard remains the
default. On wide, short terminals, the full dashboard places top processes
beside the CPU-core grid and limits that list to the available height.

On macOS, `startup` lists launch agents, launch daemons, and session login items.
On Linux, it queries system and current-user services through `systemctl`
(`list-unit-files`, `list-units`, and `show`). It also reads XDG autostart desktop
entries from `$XDG_CONFIG_HOME/autostart` (default `~/.config/autostart`) and
the autostart directories in `$XDG_CONFIG_DIRS` (default `/etc/xdg`). User entries
override system entries with the same filename. `Hidden`, `OnlyShowIn`,
`NotShowIn`, and `TryExec` affect the reported configuration state; desktop
entries do not establish a running process. Missing systemd or an inaccessible
user manager can leave the inventory partial while XDG entries remain available.
It is not a complete inventory of all
possible boot mechanisms: cron, shell profiles, containers, and non-systemd init
systems are outside this scope. Configuration alone does not prove an item is
running; process matching, resource use, and start times depend on available
runtime information and permissions. Use `--once` for a static report or
`--json` for automation.

Linux metrics depend on mounted `/proc` and `/sys` and access permissions.
Memory totals come from `/proc/meminfo`, with used memory calculated as
`MemTotal - MemAvailable`. These reflect the host memory exposed through `/proc`;
they are not adjusted to container or cgroup memory limits.

Linux battery reporting selects the first readable system battery in filename
order under `/sys/class/power_supply`. It reports that battery only; capacities
and remaining times are not summed across batteries. Device-scoped batteries
are excluded.

Per-core data is limited to the first 128 cores encountered; aggregate CPU
usage still comes from the system-wide counter. Network collection retains up
to 64 interfaces and marks the inventory truncated if more are encountered.

Memory pressure uses Linux PSI when available, which measures time stalled on
memory resources rather than memory utilization. In JSON, `pressure_source`
identifies the pressure measurement and `pressure_percent` contains its numeric
value. Unavailable optional measurements are `null`, not an invented zero;
consumers must tolerate unavailable pressure, battery, and other optional data.
PSI pressure and macOS memory pressure are different signals and should not be
treated as equivalent percentages across operating systems.

The interactive Networks view shows each interface's status, current receive
and transmit rate, and cumulative receive/transmit totals.
