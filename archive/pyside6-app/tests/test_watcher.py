import os
import subprocess
import sys
import time

import pytest

from service import watcher


# -- argv path resolution ------------------------------------------------


def test_relative_argv_resolves_against_the_process_cwd():
    """The form that once hid a stale instance from every kill pattern.

    `.venv/bin/python main.py` puts a bare "main.py" in argv, so matching
    on the absolute path — or on the project name appearing anywhere in the
    command line — finds nothing while the process is plainly running.
    """
    assert watcher.resolve_arg_path("main.py", "/home/u/app") == "/home/u/app/main.py"


def test_absolute_argv_is_left_alone():
    assert watcher.resolve_arg_path("/home/u/app/main.py", "/tmp") == "/home/u/app/main.py"


def test_dotted_argv_is_normalised():
    assert watcher.resolve_arg_path("./sub/../main.py", "/home/u/app") == "/home/u/app/main.py"


# -- environ parsing -----------------------------------------------------


def test_environ_picks_only_the_requested_keys():
    raw = b"DISPLAY=:0\0HOME=/home/u\0XAUTHORITY=/home/u/.Xauthority\0"
    picked = watcher.parse_environ(raw, ("DISPLAY", "XAUTHORITY"))
    assert picked == {"DISPLAY": ":0", "XAUTHORITY": "/home/u/.Xauthority"}


def test_environ_ignores_malformed_entries():
    """The blob ends in a NUL, so the last split piece is always empty."""
    raw = b"DISPLAY=:0\0garbage\0\0"
    assert watcher.parse_environ(raw, ("DISPLAY",)) == {"DISPLAY": ":0"}


def test_environ_keeps_values_containing_equals():
    raw = b"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus\0"
    picked = watcher.parse_environ(raw, ("DBUS_SESSION_BUS_ADDRESS",))
    assert picked["DBUS_SESSION_BUS_ADDRESS"] == "unix:path=/run/user/1000/bus"


# -- live process detection ----------------------------------------------


@pytest.fixture
def relative_launch():
    """A stand-in for `.venv/bin/python main.py` run from the app dir.

    Same shape as the real thing where it matters — comm is python, cwd is
    the app directory, argv carries a bare "main.py" — without starting a
    window.
    """
    proc = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(30)", "main.py"],
        cwd=watcher.APP_DIR,
    )
    for _ in range(50):
        if proc.pid in watcher.lyricscope_pids():
            break
        time.sleep(0.02)
    yield proc
    proc.kill()
    proc.wait()


def test_a_relatively_launched_instance_is_found(relative_launch):
    assert relative_launch.pid in watcher.lyricscope_pids()


def test_the_watcher_does_not_find_itself():
    assert os.getpid() not in watcher.lyricscope_pids()


def test_a_different_projects_main_is_not_matched():
    """Every one of these apps has a main.py; only this one counts."""
    proc = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(30)", "main.py"],
        cwd="/tmp",
    )
    try:
        time.sleep(0.3)
        assert proc.pid not in watcher.lyricscope_pids()
    finally:
        proc.kill()
        proc.wait()
