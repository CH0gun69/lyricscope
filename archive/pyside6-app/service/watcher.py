#!/usr/bin/env python3
"""Start LyricScope when DeaDBeeF starts, stop it when DeaDBeeF stops.

A watcher rather than a wrapper around the .desktop launcher, because the
launch method varies: menu, taskbar pin, terminal, a file manager opening
an audio file, or `deadbeef --play` from a media key. Watching the process
covers all of them and needs no cooperation from DeaDBeeF at all.

Two things about DeaDBeeF's process model shape everything here.

**Every CLI invocation is also a `deadbeef-main`.** `deadbeef --nowplaying-tf`
does not talk to the player from some separate client binary — it *is* the
same binary, which sets its main thread name before deciding whether to
forward over the IPC socket and exit. Measured: a forwarding call shows up
in /proc as `comm=deadbeef-main` 6ms in and lives ~600ms. LyricScope polls
twice a second, so while it runs there is a near-continuous stream of these.

That makes "remember the player's PID and wait for it to exit" actively
wrong — the watcher would latch onto a forwarder and tear the window down
half a second later. So the state here is the *aggregate* question "does
any deadbeef-main exist", which forwarders cannot get wrong in the
dangerous direction: they only ever add a process, so they can delay the
shutdown edge by a poll but can never fake one. On the startup edge a
forwarder found with no player running is not a false positive either,
because that invocation goes on to start the player itself.

**`comm` is set before the IPC socket is bound.** So a cold start has a
window where `deadbeef-main` exists but nothing is listening, and a CLI
call landing in it would fall through to `server_start` and bind the socket
itself — a second player. LyricScope polls immediately on launch, so the
launch waits for the socket to accept a connection first.
"""

from __future__ import annotations

import fcntl
import os
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.dirname(HERE)
APP_MAIN = os.path.join(APP_DIR, "main.py")
APP_PYTHON = os.path.join(APP_DIR, ".venv", "bin", "python")

# Not "deadbeef" — the player renames its main thread, so that is what
# /proc/<pid>/comm holds and `pgrep -x deadbeef` matches nothing. Same
# constant as core.deadbeef._COMM; duplicated rather than imported to keep
# the watcher free of any dependency on the app's import tree.
COMM = "deadbeef-main"

POLL_SECONDS = 1.0

# How long to wait for the IPC socket after the player appears. Generous:
# overshooting costs nothing (the loop keeps checking the player is still
# there), while giving up early would launch into the cold-start window.
IPC_TIMEOUT = 30.0

# Grace before SIGKILL. Qt does not install a SIGTERM handler, so the
# window goes on the first signal; this is only for a wedged process.
TERM_GRACE = 3.0

# Pulled from the player's own environment rather than trusted from ours,
# so the window lands on the right display no matter how the watcher was
# started. A systemd --user manager can come up before the desktop session
# has imported DISPLAY into it; DeaDBeeF is on screen by definition.
GUI_ENV_KEYS = (
    "DISPLAY",
    "XAUTHORITY",
    "WAYLAND_DISPLAY",
    "DBUS_SESSION_BUS_ADDRESS",
    "XDG_RUNTIME_DIR",
    "XDG_CURRENT_DESKTOP",
)

_stopping = False


def log(message: str) -> None:
    """One line to stdout, which is the journal when run under systemd."""
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


# -- reading /proc -------------------------------------------------------


def _comm(pid: int) -> str:
    with open(f"/proc/{pid}/comm", "r") as fh:
        return fh.read().strip()


def _pids() -> list[int]:
    return [int(e.name) for e in os.scandir("/proc") if e.name.isdigit()]


def deadbeef_running() -> bool:
    """Is any deadbeef-main alive — player or transient forwarder alike."""
    for pid in _pids():
        try:
            if _comm(pid) == COMM:
                return True
        except OSError:
            continue  # exited between the scan and the read
    return False


def resolve_arg_path(arg: str, cwd: str) -> str:
    """Absolute, symlink-free path for an argv entry of a running process.

    argv is whatever the launcher happened to type, so the same script can
    appear as `/abs/path/main.py` or as a bare `main.py` next to a cwd.
    Comparing the raw strings misses the second form — which is exactly how
    a stale instance once survived every attempt to find it, and kept
    stealing the window that was being tested.
    """
    if not os.path.isabs(arg):
        arg = os.path.join(cwd, arg)
    return os.path.realpath(arg)


def lyricscope_pids() -> list[int]:
    """PIDs running this checkout's main.py, however they were started."""
    target = os.path.realpath(APP_MAIN)
    found = []
    for pid in _pids():
        if pid == os.getpid():
            continue
        try:
            if not _comm(pid).startswith("python"):
                continue
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                argv = fh.read().decode("utf-8", "replace").split("\0")
            cwd = os.readlink(f"/proc/{pid}/cwd")
        except OSError:
            continue
        for arg in argv:
            if arg.endswith("main.py") and resolve_arg_path(arg, cwd) == target:
                found.append(pid)
                break
    return found


def parse_environ(raw: bytes, keys: tuple[str, ...]) -> dict[str, str]:
    """Pick `keys` out of a NUL-separated /proc/<pid>/environ blob."""
    picked = {}
    for item in raw.split(b"\0"):
        name, sep, value = item.partition(b"=")
        if not sep:
            continue
        key = name.decode("utf-8", "replace")
        if key in keys:
            picked[key] = value.decode("utf-8", "replace")
    return picked


def gui_env() -> dict[str, str]:
    env = dict(os.environ)
    for pid in _pids():
        try:
            if _comm(pid) != COMM:
                continue
            with open(f"/proc/{pid}/environ", "rb") as fh:
                env.update(parse_environ(fh.read(), GUI_ENV_KEYS))
            return env
        except OSError:
            continue
    return env


# -- IPC readiness -------------------------------------------------------


def ipc_ready(env: dict[str, str]) -> bool:
    """True once the player is actually answering on its IPC socket."""
    runtime = env.get("XDG_RUNTIME_DIR")
    if not runtime:
        return True  # nothing to check against; don't block the launch
    path = os.path.join(runtime, "deadbeef", "socket")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    try:
        sock.connect(path)
        return True
    except OSError:
        return False
    finally:
        sock.close()


# -- start and stop ------------------------------------------------------


def start_lyricscope() -> subprocess.Popen | None:
    existing = lyricscope_pids()
    if existing:
        log(f"LyricScope already running (pid {existing[0]}), leaving it alone")
        return None

    env = gui_env()
    deadline = time.monotonic() + IPC_TIMEOUT
    while not ipc_ready(env):
        if not deadbeef_running():
            log("DeaDBeeF went away before its IPC socket came up")
            return None
        if time.monotonic() > deadline:
            log("timed out waiting for the IPC socket; launching anyway")
            break
        time.sleep(0.2)

    # cwd is the app dir because main.py imports core/ and ui/ as
    # top-level packages.
    child = subprocess.Popen(
        [APP_PYTHON, APP_MAIN],
        cwd=APP_DIR,
        env=env,
        stdin=subprocess.DEVNULL,
    )
    log(f"launched LyricScope (pid {child.pid}) on DISPLAY={env.get('DISPLAY')}")
    return child


def stop_lyricscope() -> None:
    """Close every instance, not just one we spawned.

    An instance started by hand is just as orphaned as ours once the player
    is gone, and leaving it up is the thing this is meant to prevent.
    """
    pids = lyricscope_pids()
    if not pids:
        return
    log(f"stopping LyricScope (pid {', '.join(map(str, pids))})")
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass

    deadline = time.monotonic() + TERM_GRACE
    while time.monotonic() < deadline:
        if not lyricscope_pids():
            return
        time.sleep(0.1)

    for pid in lyricscope_pids():
        log(f"pid {pid} ignored SIGTERM; killing")
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


# -- main loop -----------------------------------------------------------


def _handle_signal(signum, frame) -> None:
    global _stopping
    _stopping = True


def _acquire_lock():
    """One watcher per session, so a double-install can't double-launch."""
    runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    handle = open(os.path.join(runtime, "lyricscope-watch.lock"), "w")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log("another watcher holds the lock; exiting")
        sys.exit(0)
    return handle


def main() -> int:
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)
    lock = _acquire_lock()  # noqa: F841 - held for the process lifetime

    if not os.path.exists(APP_PYTHON):
        log(f"no interpreter at {APP_PYTHON}")
        return 1

    child: subprocess.Popen | None = None
    # False rather than seeded from reality, so a player that is already up
    # when the watcher starts still reads as a rising edge and gets a
    # window. Launching twice is prevented where it belongs — by looking
    # for an existing instance — not by missing the edge.
    was_running = False
    log(f"watching for {COMM}; app at {APP_MAIN}")

    while not _stopping:
        now_running = deadbeef_running()

        if now_running and not was_running:
            log("DeaDBeeF appeared")
            child = start_lyricscope()
        elif was_running and not now_running:
            log("DeaDBeeF exited")
            stop_lyricscope()
            child = None

        was_running = now_running

        if child is not None and child.poll() is not None:
            # Closed by hand, or crashed. Reap it, and do not relaunch:
            # dismissing the window should keep it dismissed until the next
            # time the player starts.
            log(f"LyricScope exited (code {child.returncode}); not relaunching")
            child = None

        time.sleep(POLL_SECONDS)

    log("watcher stopping")
    stop_lyricscope()
    return 0


if __name__ == "__main__":
    sys.exit(main())
