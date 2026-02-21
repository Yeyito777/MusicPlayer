# Terminal Handling

## Alt screen buffer

The TUI runs entirely in the alternate screen buffer so the user's scrollback is preserved on exit.

```c
// enter
write(STDOUT_FILENO, "\033[?1049h\033[?25l", 14);  // alt buffer + hide cursor

// leave
write(STDOUT_FILENO, "\033[?1049l\033[?25h", 14);  // main buffer + show cursor
```

## Raw mode

`termios` is configured to disable:
- `ECHO` — don't echo typed characters
- `ICANON` — read byte-by-byte instead of line-by-line
- `ISIG` — don't generate signals for Ctrl+C (we catch SIGINT ourselves)

```c
raw.c_lflag &= ~(ECHO | ICANON | ISIG);
raw.c_cc[VMIN] = 1;   // read blocks until 1 byte available
raw.c_cc[VTIME] = 0;  // no timeout
```

The original termios is saved and restored via `atexit()`, `cleanup()`, and the signal handler — triple redundancy to prevent leaving the terminal broken.

## Rendering

Each frame:
1. `\033[2J\033[H` — clear screen + cursor home
2. Write header, song list, status bar into a `buf[8192]`
3. Single `write()` call to minimize flicker

The buffer is flushed early if it fills past 7936 bytes (8192 - 256 margin) to handle very long song lists. Uses absolute cursor positioning (`\033[row;colH`) throughout for sidebar support.

## Terminal size

`ioctl(TIOCGWINSZ)` queries rows/cols on every draw. This means the UI adapts to terminal resizes automatically (on next keypress). No SIGWINCH handler needed since the blocking `read()` in the main loop naturally throttles redraws.

## ANSI codes used

| Code              | Effect                   |
|-------------------|--------------------------|
| `\033[2J`         | clear entire screen      |
| `\033[H`          | cursor to 1,1            |
| `\033[{n};1H`     | cursor to row n, col 1   |
| `\033[1m`         | bold                     |
| `\033[2m`         | dim                      |
| `\033[32m`        | green foreground         |
| `\033[1;32m`      | bold green               |
| `\033[0m`         | reset all attributes     |
| `\033[?1049h`     | enter alt screen buffer  |
| `\033[?1049l`     | leave alt screen buffer  |
| `\033[?25h`       | show cursor              |
| `\033[?25l`       | hide cursor              |
| `\033[{r};{c}H`  | cursor to row r, col c   |

## CSI sequence parsing

Ctrl+M arrives as the kitty keyboard protocol sequence `\033[109;5u`. After reading `\033`, the main loop polls for additional bytes (20ms timeout) and accumulates until a CSI final byte (`>= 0x40`). The `[` introducer is skipped in the terminator check (`slen > 1`) since `[` (0x5b) is itself >= 0x40. If the result matches `[109;5u`, it's Ctrl+M. Otherwise the consumed bytes are discarded and `\033` is treated as bare Escape.
