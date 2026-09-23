---
id: loutre-view-json-stream-output
title: Add a machine-readable live metrics stream
status: planned
priority: medium
type: feature
area: cli-output
---

# Add a machine-readable live metrics stream

## Goal

Provide a stable live output mode for a future local UI frontend while keeping
the existing one-shot CLI contracts intact.

## Context

LoutreView already supports `--once` for one human-readable snapshot and
`--json` for one valid JSON document. The interactive dashboard samples in a
loop, but there is no explicit machine-readable streaming mode.

## Scope

- Keep `--once` as a one-snapshot human-readable mode.
- Keep `--json` as a one-snapshot, valid JSON document for scripts and
  automation.
- Add an explicit mode such as `--json-stream` for continuous output.
- Emit newline-delimited JSON (NDJSON): one complete JSON object per sampling
  interval, followed by a newline and an explicit flush.
- Add a versioned frame envelope with at least a schema version, frame type,
  timestamp or monotonic sample time, sequence number, sample interval, and
  metric availability/status information.
- Build each emitted frame from one unified sampled state so CPU, process,
  system, network, and other metrics represent the same sampling point.
- Document process lifecycle, interval behavior, termination, stdout buffering,
  broken-pipe handling, and the NDJSON contract.

## Non-goals

- Do not silently change `--json` from one-shot JSON into a stream.
- Do not add a background daemon, HTTP server, WebSocket server, or Unix
  socket protocol in this task.
- Do not add Prometheus exposition unless a separate monitoring integration is
  requested.
- Do not change the terminal dashboard behavior.

## Implementation notes

1. Separate sampled data collection from output serialization behind a common
   frame/model boundary.
2. Reuse the existing JSON fields where practical, adding only the metadata
   required for reliable stream consumption.
3. Ensure every stream frame is independently parseable JSON and is flushed
   before the next sampling delay.
4. Treat a closed consumer pipe as a normal shutdown path rather than emitting
   corrupted output or continuing to sample unnecessarily.
5. Keep unavailable platform metrics represented consistently as `null` or an
   explicit status, matching the existing cross-platform contract.

## Acceptance criteria

- `loutre-view --once` still emits exactly one readable report and exits.
- `loutre-view --json` still emits exactly one valid JSON document and exits.
- The new stream command emits at least three independently parseable JSON
  lines when run with a short interval.
- Each frame includes stable metadata identifying its schema, type, ordering,
  and sampling time.
- A consumer can read a frame immediately without waiting for the process to
  exit.
- SIGINT/SIGTERM and a closed stdout pipe stop the stream cleanly.
- Existing interactive dashboard output and startup JSON remain unchanged.
- Tests cover one-shot compatibility, stream parsing, flushing/lifecycle, and
  unavailable metric serialization.

## Verification

- `make clean && make`
- `./loutre-view --once`
- `./loutre-view --json`
- Run the stream mode through a line-oriented JSON parser with a short
  interval and verify multiple frames arrive before process termination.
- `make test`
- `git diff --check`
