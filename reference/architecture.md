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
  +-- update_position           poll time-pos/duration into song_pos/song_dur
  |
  +-- check_child               waitpid(WNOHANG) reap detection
  |
  +-- song_at / display_len     display list abstraction for filter/playlist
  |
  +-- apply_filter              rebuild filtered[] from regex query
  |
  +-- scan_playlists            scandir() on playlists/ for .playlist files
  |
  +-- load_playlist             read .playlist file, populate playlist_songs[]
  |
  +-- find_in_display           map songs[] index to current display position
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
| `searching`    | int        | search input mode active         |
| `search_buf`   | char[256]  | current search/filter query      |
| `search_len`   | int        | length of search query           |
| `search_prev_cursor` | int  | songs[] index saved on `/` entry |
| `filtered[]`   | int[1024]  | songs[] indices matching filter  |
| `nfiltered`    | int        | count of filtered matches        |
| `filter_active`| int        | filter applied to display list   |
| `playlists_dir`| const char*| playlists directory (env overridable) |
| `playlists[]`  | char*[64]  | playlist names (no .playlist ext)|
| `nplaylists`   | int        | count of loaded playlists        |
| `playlist_menu`| int        | sidebar open/closed              |
| `playlist_cursor`| int      | cursor in sidebar (0=[All Songs])|
| `playlist_active`| int      | active playlist index, -1=none   |
| `playlist_songs[]`| int[1024]| songs[] indices for active playlist |
| `nplaylist_songs`| int      | count of songs in active playlist|

## Lifecycle

1. Parse `--tmux` flag, `SONGS_DIR` and `PLAYLISTS_DIR` env vars
2. `scan_songs()` — populate song list; `scan_playlists()` — load playlist names
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

## Search / filter

Neovim-style filter: `/` or `?` enters search input mode, typing prunes the song list in real-time using POSIX extended regex (`REG_EXTENDED | REG_ICASE`). Enter exits input mode but keeps the filter active; Escape exits and clears the filter. To clear an active filter: press `/` then Enter (empty query = all songs).

`cursor` is an index into the display list, abstracted by `song_at()` (maps display position to `songs[]` index) and `display_len()` (returns `nfiltered`, `nplaylist_songs`, or `nsongs`). `apply_filter()` rebuilds `filtered[]` on each keystroke, iterating over `playlist_songs[]` when a playlist is active; invalid regex is a no-op. Auto-play (`check_child`) always uses the full `songs[]` list regardless of filter/playlist state.

## Playlists / Sidebar

Playlists are `.playlist` files in the `playlists/` directory (overridable via `PLAYLISTS_DIR` env var). Each file contains song filenames line-by-line. `scan_playlists()` runs at startup after `scan_songs()`.

Ctrl+M (`\033[109;5u` CSI sequence from st) toggles a right-side sidebar. The sidebar shows "[All Songs]" followed by playlist names. j/k/g/G navigate, Enter selects, Escape closes without changing. Selecting a playlist populates `playlist_songs[]` via `load_playlist()` and filters the main song list. Search with `/` operates within the active playlist.

The CSI sequence is detected by reading the initial `\033` byte, then polling for additional bytes (20ms timeout) and accumulating until a CSI terminator (`>= 0x40`). This happens before the search input handler so Ctrl+M isn't misinterpreted as Escape.
