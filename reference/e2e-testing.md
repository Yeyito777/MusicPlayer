# E2E Testing

Tmux-based test harness that drives the real `musicplayer` binary. Lives in `tests/run.sh`.

## How it works

The binary supports a `--tmux` flag (parsed in `main()`, stored in `static int tmux_mode`). When set, it skips the alternate screen buffer escape sequences (`\033[?1049h` / `\033[?1049l`) so that `tmux capture-pane` can read the rendered content. Everything else — raw mode, ANSI rendering, input handling, mpv IPC — is identical.

## Test songs

`tests/songs/` contains empty fixture files: `alpha.mp3`, `beta.flac`, `gamma.ogg`. The binary is pointed at this directory via the `SONGS_DIR` environment variable. `scandir()` picks them up as regular files regardless of content — only filenames matter for TUI tests.

`tests/playlists/` contains `test.playlist` (alpha.mp3 and gamma.ogg). Pointed at via `PLAYLISTS_DIR=playlists`.

## Harness API

All functions in `tests/run.sh`:

```bash
start              # Kill old session, spawn fresh tmux 80x24 running the binary
send 'j'           # tmux send-keys (raw key names: j, k, g, G, q, Enter)
send_seq $'\033..' # Send raw escape sequence via tmux paste-buffer (for CSI sequences)
capture            # tmux capture-pane -p → stdout
wait_ms 200        # sleep 0.200
assert_contains    "label" "needle"       # PASS if capture contains needle
assert_not_contains "label" "needle"      # PASS if capture does NOT contain needle
assert_session_dead "label"               # PASS if session exited (checks for __EXITED__ sentinel)
cleanup            # tmux kill-session (runs on EXIT trap)
```

### Session lifecycle

```bash
tmux new-session -d -s "$SESSION" -x 80 -y 24 \
    "cd $DIR && SONGS_DIR=songs PLAYLISTS_DIR=playlists $BINARY --tmux; echo __EXITED__; sleep 10"
```

The `echo __EXITED__; sleep 10` suffix keeps the tmux session alive after the binary exits so `assert_session_dead` can capture the sentinel string instead of racing against session teardown.

## Adding a new test

1. Pick a section or add a new `echo` header
2. Call `start` for a fresh session
3. Send keys with `send`
4. Wait with `wait_ms 200`
5. Assert with `assert_contains` / `assert_not_contains`

Example:

```bash
echo ""
echo "My new feature"
start
send j
send Enter
wait_ms 300
assert_contains "status shows playing" "[playing]"
```

## What to test vs. not test

**Test:** list rendering, navigation (j/k/g/G), cursor boundaries, selection highlighting, quit behavior, any new TUI-visible state.

**Don't test:** actual audio playback (mpv not guaranteed, fixture files are empty), seek/pause IPC (requires a running mpv process). These are thin wrappers around mpv commands — verify manually.

## Running

```bash
make test          # build + run tests
bash tests/run.sh  # run tests directly (binary must exist)
```
