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
2. Write header, song list, status bar into a `buf[4096]`
3. Single `write()` call to minimize flicker

The buffer is flushed early if it fills past 3840 bytes (4096 - 256 margin) to handle very long song lists.

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
