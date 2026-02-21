# Architecture

Single-file C program (`player.c`, ~280 lines). No libraries beyond libc. Audio playback delegated to mpv subprocess.

## Components

```
main loop (blocking read)
  |
  +-- term_raw / term_restore   termios raw mode + alt buffer
  |
  +-- scan_songs                scandir() on songs/
  |
  +-- draw                      ANSI escape rendering
  |
  +-- play_song                 fork + exec mpv with IPC socket
  |
  +-- mpv_cmd                   JSON commands over unix socket
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
| `songs[]`      | char*[1024]| filenames from songs/            |
| `nsongs`       | int        | count of loaded songs            |
| `cursor`       | int        | highlighted list index           |
| `playing`      | int        | index of playing song, -1 if none|

## Lifecycle

1. `scan_songs()` — populate song list
2. `term_raw()` — enter alt buffer, raw mode, register atexit
3. `draw()` — initial render
4. Main loop: `read()` single char -> handle -> `check_child()` -> `draw()`
5. `cleanup()` — kill mpv, restore terminal (called on q/signal/atexit)

## Signal handling

SIGINT and SIGTERM are caught by `sig_handler` which calls `cleanup()` then `_exit(0)`. This ensures the terminal is always restored even on Ctrl+C.
