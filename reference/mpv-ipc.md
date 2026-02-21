# mpv IPC Interface

mpv is launched with `--input-ipc-server=/tmp/musicplayer-mpv.sock` which creates a Unix domain socket accepting JSON-based commands.

## Socket path

```c
#define MPV_SOCKET "/tmp/musicplayer-mpv.sock"
```

Cleaned up on stop/quit via `unlink()`.

## Protocol

Connect to the socket as `SOCK_STREAM`, send newline-terminated JSON:

```json
{"command":["seek","5"]}\n
{"command":["seek","-5"]}\n
{"command":["cycle","pause"]}\n
```

The `mpv_cmd()` function opens a fresh connection per command (connect, write, close). This is simple and avoids managing a persistent fd — the overhead is negligible for human-speed key presses.

## Command reference

Commands currently used:

| JSON command                          | Effect              |
|---------------------------------------|----------------------|
| `["seek", "5"]`                       | seek forward 5s      |
| `["seek", "-5"]`                      | seek back 5s         |
| `["cycle", "pause"]`                  | toggle pause/resume  |
| `["get_property", "time-pos"]`        | current position (s) |
| `["get_property", "duration"]`        | total duration (s)   |

## Property queries

`mpv_get_double()` sends a `get_property` command, reads the JSON response with `poll()` + `read()` (100ms timeout), and parses `"data":123.456` with `strstr` + `strtod`. Returns -1 on failure.

`update_position()` queries `time-pos` and `duration` on each main loop iteration (when playing and not paused), storing results in `song_pos` and `song_dur`.

## Extending

Other useful mpv IPC commands for future features:

```json
{"command":["get_property","volume"]}         // current volume
{"command":["set_property","volume",80]}      // set volume to 80
{"command":["seek","30","absolute"]}          // seek to 30s absolute
```

## mpv docs

Full IPC spec: https://mpv.io/manual/master/#json-ipc
