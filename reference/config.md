# Configuration

## config.conf

A simple key-value file in the working directory, read at startup and written when settings change. Gitignored.

### Format

```
volume=80
```

One setting per line, `key=value`. Unknown keys are ignored.

### Settings

| Key      | Range  | Default | Description                |
|----------|--------|---------|----------------------------|
| `volume` | 0-100  | 100     | Playback volume percentage. Displayed as a 20-bar indicator inline in the header after the title. |

### Loading

`load_config()` runs at startup after `scan_playlists()`. Reads line-by-line with `fgets`, parses with `sscanf`. Missing file is fine — defaults are used.

### Saving

`save_config()` writes the current state on each volume change (`+`/`-` key). Overwrites the entire file.

### Extending

To add a new persistent setting:

1. Add a global variable with a default
2. Add an `sscanf` line in `load_config()`
3. Add a `fprintf` line in `save_config()`
4. Call `save_config()` wherever the setting changes
