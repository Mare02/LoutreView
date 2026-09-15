"""Real executable checks: CLI, JSON, native counters and PTY restoration."""
import errno
import fcntl
import json
import math
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

binary = os.path.abspath(sys.argv[1])


def run(*args):
    return subprocess.run([binary, *args], text=True, capture_output=True, timeout=30, check=True).stdout


def invalid_constant(value):
    raise AssertionError(f"Non-JSON numeric constant: {value}")


assert run("--version").startswith("loutre-view ")
assert "Usage:" in run("--help")
for args in [("--invalid",), ("--interval", "0"), ("--limit", "-1"), ("--sort", "invalid")]:
    result = subprocess.run([binary, *args], capture_output=True, timeout=10)
    assert result.returncode == 2, args

for sort in ["cpu", "mem", "pid", "name"]:
    data = json.loads(run("--json", "--sort", sort, "--limit", "5"), parse_constant=invalid_constant)
    assert 0 <= data["cpu"]["usage_percent"] <= 100
    assert 0 <= data["memory"]["used_bytes"] <= data["memory"]["total_bytes"]
    assert data["memory"]["total_bytes"] > 0
    assert data["disk"]["total_bytes"] > 0
    assert data["uptime_seconds"] >= 0
    assert 0 < len(data["processes"]) <= 5
    assert all(p["memory_bytes"] >= 0 and p["threads"] >= 0 for p in data["processes"])
    if sort == "pid":
        pids = [p["pid"] for p in data["processes"]]
        assert pids == sorted(pids)
    if sys.platform == "linux":
        assert data["memory"]["pressure_bytes"] is None
        with open("/proc/meminfo") as f:
            total = int(next(line for line in f if line.startswith("MemTotal:")).split()[1]) * 1024
        assert data["memory"]["total_bytes"] == total
    else:
        assert data["memory"]["pressure_bytes"] >= 0

for args in [("--once", "--no-color"), ("--once", "--compact", "--no-color")]:
    output = run(*args)
    assert "LOUTREVIEW" in output and "CPU" in output and "\x1b" not in output
startup = json.loads(run("startup", "--json"), parse_constant=invalid_constant)
assert isinstance(startup["items"], list)
for item in startup["items"]:
    assert item["running"] in (True, False, None)
    assert item["enabled"] in (True, False, None)


def terminal_session(compact=False):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 110, 0, 0))
    original = termios.tcgetattr(slave)
    child = subprocess.Popen([binary, "--interval", "250"] + (["--compact"] if compact else []),
                             stdin=slave, stdout=slave, stderr=slave, close_fds=True,
                             env={**os.environ, "TERM": "xterm-256color"})
    chunks = []

    def collect(seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if select.select([master], [], [], max(0, until - time.monotonic()))[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                chunks.append(data)

    try:
        collect(.7)
        os.write(master, b"2")
        collect(.7)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 20, 60, 0, 0))
        collect(.35)
        os.write(master, b"1")
        collect(.4)
        child.send_signal(signal.SIGINT)
        collect(.25)
        assert child.wait(timeout=5) == 0
        assert termios.tcgetattr(slave) == original, "Terminal state was not restored"
        output = b"".join(chunks)
        assert b"NETWORKS" in output and b"LOUTREVIEW" in output
        assert b"\x1b[?1049l" in output and b"\x1b[?25h" in output
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        os.close(master)
        os.close(slave)


terminal_session()
terminal_session(compact=True)
print("CLI, JSON, live metrics and terminal smoke tests passed")
