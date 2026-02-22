# Paths & Environment

## MUSIC_PLAYER_HOME

Base directory for all default paths. If not set, the current working directory is used.

```bash
# Run from anywhere
export MUSIC_PLAYER_HOME=~/Workspace/MusicPlayer
musicplayer
```

When set, the three default paths resolve under it:

| Path         | Default          | Resolved as                        |
|--------------|------------------|------------------------------------|
| Songs dir    | `songs`          | `$MUSIC_PLAYER_HOME/songs`         |
| Playlists dir| `playlists`      | `$MUSIC_PLAYER_HOME/playlists`     |
| State file   | `state.save`     | `$MUSIC_PLAYER_HOME/state.save`    |

## Per-directory overrides

`SONGS_DIR` and `PLAYLISTS_DIR` override their respective paths after `MUSIC_PLAYER_HOME` is applied. These are treated as-is (absolute or cwd-relative), not relative to `MUSIC_PLAYER_HOME`.

```bash
# Custom songs directory, everything else from MUSIC_PLAYER_HOME
MUSIC_PLAYER_HOME=~/music SONGS_DIR=/mnt/nas/songs musicplayer
```

## Resolution order

1. Defaults set to compile-time constants (`"songs"`, `"playlists"`, `"state.save"`)
2. If `MUSIC_PLAYER_HOME` is set, defaults are prefixed with it
3. If `SONGS_DIR` is set, it replaces the songs path
4. If `PLAYLISTS_DIR` is set, it replaces the playlists path

State file has no individual env override — it always follows `MUSIC_PLAYER_HOME` or cwd.

## Typical usage

The zsh alias `mp` cds to the project directory, so `MUSIC_PLAYER_HOME` is unnecessary there. It's useful when running `musicplayer` from `$PATH` without changing directories first.
