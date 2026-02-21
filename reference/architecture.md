# Architecture

Single-file C program (`player.c`). No libraries beyond libc. Audio playback delegated to mpv subprocess.

## Components

```
main loop (poll with 250ms timeout)
  |
  +-- term_raw / term_restore   termios raw mode + alt buffer
  |
  +-- scan_songs                scandir() on songs/
  |
  +-- draw                      ANSI escape rendering + progress bar
  |
  +-- play_song                 fork + exec mpv with IPC socket
  |
  +-- mpv_cmd                   fire-and-forget JSON commands over unix socket
  |
  +-- mpv_get_double            query mpv properties (time-pos, duration)
  |
  +-- update_position           poll time-pos/duration into song_pos/song_dur
  |
  +-- check_child               waitpid(WNOHANG) reap detection
```

## Globals

All state is file-scope static:

| Variable       | Type       | Purpose                          |
|----------------|------------|----------------------------------|
| `orig_termios` | termios    | saved terminal state for restore |
| `mpv_pid`      | pid_t      | child process, -1 when idle      |
| `paused`       | int        | toggle for pause state           |
| `tmux_mode`    | int        | skip alt buffer for E2E testing  |
| `songs_dir`    | const char*| songs directory (env overridable)|
| `songs[]`      | char*[1024]| filenames from songs/            |
| `nsongs`       | int        | count of loaded songs            |
| `cursor`       | int        | highlighted list index           |
| `playing`      | int        | index of playing song, -1 if none|
| `loop_mode`    | int        | LOOP_ALL (0) or LOOP_SINGLE (1)  |
| `shuffle`      | int        | shuffle mode on/off              |
| `played[]`     | int[1024]  | bitset of played songs           |
| `nplayed`      | int        | count of played songs            |
| `song_pos`     | double     | current playback position (s)    |
| `song_dur`     | double     | total song duration (s)          |

## Lifecycle

1. Parse `--tmux` flag and `SONGS_DIR` env var
2. `scan_songs()` — populate song list
3. `term_raw()` — enter alt buffer, raw mode, register atexit
4. `draw()` — initial render
5. Main loop: `poll()` 250ms → `check_child()` → `update_position()` → handle key (if any) → `draw()`
6. `cleanup()` — kill mpv, restore terminal (called on q/signal/atexit)

The `poll()` timeout means the UI refreshes ~4 times/sec even without keypresses, keeping the progress bar current.

## Signal handling

SIGINT and SIGTERM are caught by `sig_handler` which calls `cleanup()` then `_exit(0)`. This ensures the terminal is always restored even on Ctrl+C. SIGPIPE is ignored (SIG_IGN) to prevent process termination when writing to a broken mpv socket during song transitions.

## Loop modes

`check_child()` handles auto-advance when mpv exits:
- **LOOP_ALL** (default): play next song, wrap to index 0 at end of playlist
- **LOOP_SINGLE**: replay the same song

Toggle with `m` key. Status line shows `[repeat]` when in LOOP_SINGLE mode.

## Shuffle mode

Toggle with `n` key. When active, `check_child()` picks a random unplayed song via `shuffle_next()` instead of sequential advance. The `played[]` bitset tracks which songs have been heard. When all songs are played (`nplayed >= nsongs`), `shuffle_clear()` flushes the set. Manually playing a song (Enter/Space) also marks it as played. Toggling shuffle on clears the set and marks the current song. Status line and song list show `[shuffle]`.
