# MusicPlayer

Minimal TUI music player in C. Alt screen buffer, vim-style navigation, mpv subprocess for playback.

## Project Structure

```
player.c           — All source code (single file)
Makefile            — build, install, test targets
songs/              — User's music files (gitignored)
tests/
  run.sh           — Bash E2E test harness (tmux-based)
  songs/           — Dummy fixture files for tests
reference/         — Technical docs
```

## Building

```bash
make                # build ./musicplayer
make install        # copy to ~/.local/bin/
make clean          # remove binary
```

## Tests

Run the full suite with `make test`. Tests use a tmux-based harness that drives the real binary.

After implementing a feature or making any change you MUST:
1. Write E2E tests for any new TUI behavior in `tests/run.sh`
2. Run `make clean && make && make install && make test` — clean build, install the binary to ~/.local/bin/, and run all tests
3. Verify all tests pass before considering the work done

This is non-negotiable. Every change ends with a clean build + install + passing tests. Do not hand features to the user to test. You have full ability to test everything yourself through the tmux harness — use it.

### E2E tests (tmux harness)

`tests/run.sh` spawns the real binary inside a detached tmux session and drives it with keystrokes, then captures the screen and asserts on the output.

**How it works:**
1. `start` spawns a detached tmux session (80x24) running `./musicplayer --tmux`
2. `send 'j'` sends a keystroke
3. `wait_ms 200` lets the TUI render
4. `capture` reads the screen via `tmux capture-pane -p`
5. `assert_contains "label" "needle"` checks the captured output
6. `cleanup` tears down the session (runs on EXIT trap)

The `--tmux` flag skips the alternate buffer so `tmux capture-pane` can read the screen. Everything else (raw mode, rendering, input, mpv IPC) works identically to production.

The `SONGS_DIR` env var overrides the default `songs/` directory. Tests set this to `tests/songs/` which contains empty fixture files.

**Rules for E2E tests:**
- Each test group calls `start` to get a fresh session.
- Wait at least 400ms after `start` (handled internally), 200ms after sending keys.
- Test navigation and rendering, not playback — mpv may not be available and fixture files are empty.

### Manual testing with tmux

You can spin up a live session to interact with the TUI and verify behavior:

```bash
# Create a session
tmux new-session -d -s mp-debug -x 80 -y 24 \
  "cd ~/Workspace/MusicPlayer && ./musicplayer --tmux"

# Send keystrokes
tmux send-keys -t mp-debug j
tmux send-keys -t mp-debug j
tmux send-keys -t mp-debug Enter

# Read the screen
sleep 0.5
tmux capture-pane -t mp-debug -p

# Clean up
tmux kill-session -t mp-debug
```

This is your equivalent of opening a terminal and using the app.

## Reference Docs

Every feature gets a technical reference doc in `reference/`. These are your own docs for how the project works internally.

### What goes in a reference doc

- The feature's entry point and which functions are involved
- Data flow and implementation details needed to build on it
- Code snippets showing actual patterns used
- Current limitations or things to watch out for

### What does NOT go in a reference doc

- User-facing instructions or README-style prose
- Motivation essays
- Duplicate of what's already obvious from reading the code

### Naming convention

`reference/<feature-name>.md` — kebab-case, named after the feature.

### When to write one

After implementing a feature, write or update the relevant reference doc. If a feature touches an existing doc's scope, update that doc.

### Existing docs

| File | Covers |
|---|---|
| `architecture.md` | Component map, globals, lifecycle, signal handling |
| `mpv-ipc.md` | Unix socket protocol, JSON commands, extending playback |
| `terminal.md` | Alt buffer, raw mode, ANSI escapes, rendering strategy |
