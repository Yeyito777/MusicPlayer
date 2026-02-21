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

SIGINT and SIGTERM are caught by `sig_handler` which calls `cleanup()` then `_exit(0)`. This ensures the terminal is always restored even on Ctrl+C.
