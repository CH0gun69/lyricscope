# LyricScope — the standalone PySide6 app (retired)

This is the version that ran as its own window next to DeaDBeeF, kept
because a fair amount of what it learned is only written down in its code.
It was replaced by `../../plugin/`, a native GTK3 plugin that docks inside
the player.

It still runs, and its tests still pass from this directory:

```bash
../../linux/.venv/bin/python main.py
../../linux/.venv/bin/python -m pytest tests/ -q     # 58 tests
```

`core/` here is a frozen copy. The live copy — the two modules the plugin
still calls — is `../../linux/core/`. Edit that one; this is a snapshot.

## Why it was replaced

Everything it did from the outside, the plugin does from inside the player:

| | standalone app | plugin |
|---|---|---|
| Playback position | `deadbeef --nowplaying-tf` twice a second, interpolated with a monotonic clock in between | `streamer_get_playpos()`, every frame |
| Track changes | noticed by polling | `DB_EV_SONGSTARTED` |
| Seeking | MPRIS over `gdbus`, as a relative `Seek` anchored on a freshly-read `Position` | `sendmessage(DB_EV_SEEK, …)` |
| Cover art | found by hand: embedded tags, then a file beside the track, then DeaDBeeF's cache | the player's own artwork plugin |
| Lifetime | a systemd watcher started and stopped it with the player | it *is* the player |
| Window | its own | a tab in the main window |

## What is worth reading here

- **`core/deadbeef.py`** — the CLI client. Three things it documents are
  still true of the binary: it *launches* the player when none is running
  (so every poll is gated on `is_running()`), the process is called
  `deadbeef-main` rather than `deadbeef`, and title formatting deletes `<`
  and `>`, so a `<|SEP|>` field separator silently arrives as `|SEP|`.
- **`core/seek.py`** — MPRIS seeking, and why it shells out to `gdbus`
  instead of using the QtDBus already linked in: `Seek` takes an int64 and
  PySide6 marshals Python ints as int32, so every realistic offset is
  rejected — while values *above* int32 range get promoted, which is worse
  than a clean failure (one such test seek ran off the end of the track and
  advanced the player). Also: `gdbus` parses a leading `-` as an option, so
  a backward seek needs `--` or it silently prints usage.
- **`service/watcher.py`** — the process watcher, and the measurement that
  shaped it: every `deadbeef` CLI invocation is *also* a `deadbeef-main` in
  /proc for ~600ms, so tracking the player by PID latches onto a forwarder.
  Its `lyricscope_pids()` resolves argv against `/proc/<pid>/cwd`, because
  a bare `main.py` from `.venv/bin/python main.py` matches nothing
  otherwise.
- **`ui/lyric_label.py`** — why the line scale is a paint-time transform
  and not a font size. This is the finding that carried straight over to
  Cairo and Pango; the plugin's `panel.c` says the same thing in its own
  terms.
