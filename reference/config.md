# State Persistence

## state.save

A key-value file in the working directory (or `$MUSIC_PLAYER_HOME`), read at startup and written on every state change. Gitignored.

### Format

```
volume=85
song=Jamie Paige - Birdbrain.mp3
position=63.45
paused=0
cursor=Jamie Paige - Machine Love.mp3
playlist=jamie-paige
loop=single
shuffle=0
```

One setting per line, `key=value`. String values (song, cursor, playlist) are everything after the `=`. Unknown keys are ignored.

### Fields

| Key        | Type   | Default     | Description                                |
|------------|--------|-------------|--------------------------------------------|
| `volume`   | 0-100  | 100         | Playback volume percentage                 |
| `song`     | string | (none)      | Filename of the playing song               |
| `position` | double | 0           | Playback position in seconds               |
| `paused`   | 0/1    | 0           | Whether playback was paused                |
| `cursor`   | string | (none)      | Filename of the song under the cursor      |
| `playlist` | string | (none)      | Name of the active playlist (no extension) |
| `loop`     | string | `all`       | Loop mode: `all` or `single`               |
| `shuffle`  | 0/1    | 0           | Shuffle mode                               |

Fields `song`, `position`, `paused` are only written when a song is playing. Fields `cursor` and `playlist` are only written when applicable.

### Loading

`load_state()` runs at startup after `scan_playlists()`. Reads line-by-line with `fgets`, strips trailing newlines, then parses with `sscanf` (numeric) or `strncmp` + copy (strings). Missing file is fine — defaults are used. Saved song/cursor/playlist names are stored temporarily for `restore_state()`.

### Restoring

`restore_state()` runs after `term_raw()`, before the first `draw()`:

1. Resolves `saved_playlist` name → loads the playlist
2. Resolves `saved_cursor` name → sets cursor position via `find_in_display()`
3. Resolves `saved_song` name → calls `play_song()`, waits for mpv IPC socket, sends absolute seek and optional pause

Songs and playlists are resolved by filename match against the current `songs[]` and `playlists[]` arrays. If a saved name is missing (file deleted), that field is silently skipped.

### Saving

`save_state()` is called before every `draw()` in the main loop. This covers:

- All keyboard input (cursor movement, playback, volume, mode toggles, search, playlist selection)
- Periodic updates (position tracking, auto-advance on song end)

Not called during `cleanup()` or signal handlers — the file retains the last saved state, so quitting with `q` or receiving a signal preserves the playing song and position for resume on next launch. SIGKILL is uncatchable but the file is at most ~250ms stale.
