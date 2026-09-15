# Coding CLI Usage Monitor

Implementation plan for [LOV.1](nimbalyst://LOV.1): add usage monitoring for
Codex CLI, Claude Code, and Gemini CLI.

## Goal

Show users their coding-CLI usage and quota windows inside LoutreView's
terminal UI.

The feature is a terminal UI feature only. It must not add usage data to the
existing JSON output.

## Explicitly out of scope

- Authentication status or credential diagnostics in the UI.
- Installation/provider-health panels.
- Usage fields in `--json` output.
- Direct credential extraction or storage.
- Scraping interactive terminal screens.
- Network polling on every dashboard refresh.

Provider failures remain internal implementation details. If a metric cannot
be read, the UI should render that metric as `n/a`.

## Common usage model

Add an internal model, for example in `include/usage.h`:

```c
typedef struct {
    char name[32];
    double used_percent;
    double remaining_percent;
    time_t resets_at;
    unsigned long long used_tokens;
    unsigned long long quota_tokens;
    bool has_percent;
    bool has_tokens;
    bool has_reset;
} UsageWindow;

typedef struct {
    char provider[32];
    UsageWindow windows[8];
    size_t window_count;
    time_t collected_at;
} ProviderUsage;
```

The model must support provider-specific windows without assuming that every
provider exposes the same concepts. Percentages, token counts, request counts,
and reset times are optional.

## Architecture

Keep provider-specific parsing separate from aggregation and presentation:

```text
include/usage.h
include/usage_provider.h

src/usage/
  usage.c
  usage_cache.c
  provider_registry.c

src/providers/
  codex.c
  claude_code.c
  gemini_cli.c

src/ui/usage.c
tests/test_usage.c
```

Each adapter should expose a small contract for collecting normalized usage
windows. The shared UI should only consume `ProviderUsage` values and must not
contain provider-specific parsing or field names.

The collector should use bounded subprocess execution and local files/cache
where required. It must not use `system()` with uncontrolled input and must
not persist API keys or OAuth tokens.

## Provider integrations

### Claude Code

Claude Code's status-line protocol receives JSON on stdin and exposes
`rate_limits.five_hour` and `rate_limits.seven_day`, including usage percentage
and reset timestamps.

Use a small LoutreView bridge that accepts the status-line payload, extracts
only usage fields, and writes an atomically replaced cache file. The monitor
reads the cache instead of calling Anthropic directly.

The implemented bridge entry point is:

```sh
loutre-view usage ingest claude < claude-statusline.json
```

The same command accepts `codex` and `gemini` bridge payloads. Cache files are
stored under `$LOUTREVIEW_USAGE_DIR` when set, otherwise under the user's XDG
state directory.

Reference: [Claude Code status-line documentation](https://code.claude.com/docs/en/statusline)

### Gemini CLI

Gemini CLI documents `/stats model` as the command for session token usage and
applicable quota information.

First verify whether the installed CLI exposes a stable machine-readable
equivalent. If it does, parse that output. If it only exposes interactive
output, add no screen scraping; show available data only through a future
documented bridge.

Reference: [Gemini CLI quota documentation](https://github.com/google-gemini/gemini-cli/blob/main/docs/resources/quota-and-pricing.md)

### Codex CLI

Verify whether the current Codex CLI exposes usage/quota through a stable local
file, non-interactive command, or app-server interface. Do not depend on
interactive TUI text or undocumented credential files.

If a stable source exists, implement the adapter. Otherwise, leave Codex
metrics unavailable until a reliable source is identified.

Reference: [OpenAI Codex CLI repository](https://github.com/openai/codex)

## User interface

Add a dedicated Usage view, reachable with a new key such as `3` or `u`.

Each provider should render a compact section containing its available usage
windows:

```text
CODING CLI USAGE

Claude Code
  5-hour     23.5% used   resets 42m
  7-day      41.2% used   resets Mon 12:00

Gemini CLI
  Daily      312 / 1000 requests   resets tomorrow

Codex
  Usage data unavailable
```

The view should use the existing terminal layout, formatting, color, and
`n/a` conventions. It should refresh from the cache and never block the main
render loop on a slow provider command.

Do not modify `src/json.c` or the existing JSON schema.

## Testing

Add fixtures for:

- Valid provider usage data.
- Missing optional windows or fields.
- Malformed provider output.
- Stale cache data.
- Reset timestamps in the past and future.
- Large and fractional percentages.
- Provider-specific unknown fields.

Test:

- Normalization into `UsageWindow`.
- Provider registry dispatch.
- Cache read/write and atomic replacement.
- `n/a` rendering for unavailable metrics.
- No usage fields appearing in existing JSON output.
- No credential values appearing in output or logs.

## Implementation order

1. Verify stable machine-readable sources for all three providers.
2. Define the common usage model and adapter contract.
3. Implement cache handling and provider registry.
4. Implement provider parsers with fixtures.
5. Add the terminal Usage view.
6. Add documentation for configuring provider bridges, if needed.
7. Run the normal macOS build/tests and Linux validation.

## Acceptance criteria

- LoutreView displays available usage/quota metrics in a dedicated terminal
  view.
- Claude Code, Gemini CLI, and Codex each have a dedicated adapter.
- Provider-specific data is normalized before reaching the UI.
- Missing metrics render as `n/a` without breaking the view.
- The existing `--json` output remains unchanged.
- No credentials are read into the usage model, logged, or persisted.
- A future provider can be added through the adapter contract without changing
  the shared UI or aggregation code.

## Verification

Run the project-required checks after implementation:

```sh
make clean && make
./loutre-view --version
./loutre-view --once --no-color
make test
git diff --check
```

For platform-related changes, also run `make linux-test` and report the usual
container limitations for physical hardware and desktop/provider sessions.
