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

`mpv_connect()` establishes a persistent connection (stored in `mpv_fd`). `mpv_cmd()` writes commands over this fd, reconnecting if needed. `mpv_disconnect()` closes it on stop/error.

## Command reference

Commands currently used:

| JSON command                          | Effect              |
|---------------------------------------|----------------------|
| `["seek", "5"]`                       | seek forward 5s      |
| `["seek", "-5"]`                      | seek back 5s         |
| `["cycle", "pause"]`                  | toggle pause/resume  |
| `["add", "volume", 5]`               | volume up 5%         |
| `["add", "volume", -5]`              | volume down 5%       |
| `["get_property", "time-pos"]`        | current position (s) |
| `["get_property", "duration"]`        | total duration (s)   |

## Property queries

`update_position()` sends both `time-pos` and `duration` queries in one batch, reads responses with `poll()` + `read()` (50ms timeout per attempt, up to 20 retries), and parses `"data":123.456` via `parse_response()` using `strstr` + `strtod`. Runs each main loop iteration when playing and not paused.

## Volume

Volume is tracked as an app-level global (`volume`, 0-100, steps of 5). On `play_song()`, mpv is launched with `--volume=XX`. The `+`/`-` keys adjust the global and send an IPC `add volume` command to the running instance. Volume persists to `config.conf` (see below).

## Extending

Other useful mpv IPC commands for future features:

```json
{"command":["get_property","volume"]}         // current volume
{"command":["set_property","volume",80]}      // set volume to 80
{"command":["seek","30","absolute"]}          // seek to 30s absolute
```

## mpv docs

Full IPC spec: https://mpv.io/manual/master/#json-ipc
