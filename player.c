#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_SONGS 1024
#define SONGS_DIR "songs"
#define MPV_SOCKET "/tmp/musicplayer-mpv.sock"

static struct termios orig_termios;
static pid_t mpv_pid = -1;
static int paused = 0;
static int tmux_mode = 0;

static const char *songs_dir = SONGS_DIR;
static char *songs[MAX_SONGS];
static int nsongs = 0;
static int cursor = 0;
static int playing = -1;
static double song_pos = 0;
static double song_dur = 0;

static void die(const char *msg) {
	perror(msg);
	exit(1);
}

static void term_restore(void) {
	if (!tmux_mode)
		write(STDOUT_FILENO, "\033[?1049l\033[?25h", 14);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static int mpv_fd = -1;

static int mpv_connect(void) {
	if (mpv_fd >= 0) return 0;
	mpv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (mpv_fd < 0) return -1;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, MPV_SOCKET, sizeof(addr.sun_path) - 1);
	if (connect(mpv_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(mpv_fd);
		mpv_fd = -1;
		return -1;
	}
	return 0;
}

static void mpv_disconnect(void) {
	if (mpv_fd >= 0) {
		close(mpv_fd);
		mpv_fd = -1;
	}
}

static void mpv_cmd(const char *cmd) {
	if (mpv_connect() != 0) return;
	if (write(mpv_fd, cmd, strlen(cmd)) < 0) {
		mpv_disconnect();
	}
}

/* Parse a double from a JSON line matching a specific request_id.
 * Returns -1 if not found. Advances *start past the parsed line. */
static double parse_response(const char *buf, int id) {
	char needle[32];
	snprintf(needle, sizeof(needle), "\"request_id\":%d", id);
	const char *hit = strstr(buf, needle);
	if (!hit) return -1;
	/* find "data": before this hit on the same line */
	const char *sol = hit;
	while (sol > buf && sol[-1] != '\n') sol--;
	const char *data = strstr(sol, "\"data\":");
	if (data && data < hit + 32)
		return strtod(data + 7, NULL);
	return -1;
}

/* Send both time-pos and duration queries at once, read all responses. */
static void update_position(void) {
	if (mpv_pid <= 0 || paused) return;
	if (mpv_connect() != 0) return;

	const char *cmds =
		"{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":1}\n"
		"{\"command\":[\"get_property\",\"duration\"],\"request_id\":2}\n";
	if (write(mpv_fd, cmds, strlen(cmds)) < 0) {
		mpv_disconnect();
		return;
	}

	char buf[4096];
	int total = 0;
	struct pollfd pfd = { .fd = mpv_fd, .events = POLLIN };
	int got_pos = 0, got_dur = 0;

	/* read until we have both responses or timeout */
	for (int i = 0; i < 20 && !(got_pos && got_dur); i++) {
		if (poll(&pfd, 1, 50) <= 0) break;
		int r = read(mpv_fd, buf + total, sizeof(buf) - 1 - total);
		if (r <= 0) { mpv_disconnect(); return; }
		total += r;
		buf[total] = '\0';

		if (!got_pos) {
			double v = parse_response(buf, 1);
			if (v >= 0) { song_pos = v; got_pos = 1; }
		}
		if (!got_dur) {
			double v = parse_response(buf, 2);
			if (v >= 0) { song_dur = v; got_dur = 1; }
		}
	}
}

static void kill_mpv(void) {
	mpv_disconnect();
	if (mpv_pid > 0) {
		kill(mpv_pid, SIGTERM);
		waitpid(mpv_pid, NULL, 0);
		mpv_pid = -1;
		paused = 0;
		playing = -1;
	}
	song_pos = 0;
	song_dur = 0;
	unlink(MPV_SOCKET);
}

static void cleanup(void) {
	kill_mpv();
	term_restore();
}

static void sig_handler(int sig) {
	(void)sig;
	cleanup();
	_exit(0);
}

static void term_raw(void) {
	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
		die("tcgetattr");
	atexit(term_restore);

	struct termios raw = orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");

	if (!tmux_mode)
		write(STDOUT_FILENO, "\033[?1049h\033[?25l", 14);
}

static int scan_songs(void) {
	struct dirent **namelist;
	int n = scandir(songs_dir, &namelist, NULL, alphasort);
	if (n < 0)
		die("scandir");

	for (int i = 0; i < n; i++) {
		if (namelist[i]->d_name[0] != '.' && namelist[i]->d_type == DT_REG) {
			if (nsongs < MAX_SONGS)
				songs[nsongs++] = strdup(namelist[i]->d_name);
		}
		free(namelist[i]);
	}
	free(namelist);
	return nsongs;
}

static int term_rows(void) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
		return 24;
	return ws.ws_row;
}

static int term_cols(void) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
		return 80;
	return ws.ws_col;
}

static void draw(void) {
	int rows = term_rows();
	int cols = term_cols();
	int list_rows = rows - 4; /* header + separator + status + progress */

	/* clear screen, move to top */
	write(STDOUT_FILENO, "\033[2J\033[H", 7);

	char buf[4096];
	int len = 0;

	/* header */
	len += snprintf(buf + len, sizeof(buf) - len,
		"\033[1m  MusicPlayer\033[0m\r\n");
	for (int i = 0; i < cols && len < (int)sizeof(buf) - 1; i++)
		buf[len++] = '-';
	buf[len++] = '\r';
	buf[len++] = '\n';

	/* scrolling: keep cursor visible */
	int offset = 0;
	if (nsongs > list_rows) {
		offset = cursor - list_rows / 2;
		if (offset < 0) offset = 0;
		if (offset > nsongs - list_rows) offset = nsongs - list_rows;
	}

	for (int i = 0; i < list_rows && (i + offset) < nsongs; i++) {
		int idx = i + offset;
		const char *prefix = "  ";
		const char *style = "";
		const char *reset = "";

		if (idx == cursor && idx == playing) {
			prefix = "> ";
			style = "\033[1;32m"; /* bold green */
			reset = "\033[0m";
		} else if (idx == cursor) {
			prefix = "> ";
			style = "\033[1m"; /* bold */
			reset = "\033[0m";
		} else if (idx == playing) {
			style = "\033[32m"; /* green */
			reset = "\033[0m";
		}

		len += snprintf(buf + len, sizeof(buf) - len,
			"%s%s%s%s\r\n", style, prefix, songs[idx], reset);

		if (len >= (int)sizeof(buf) - 256) {
			write(STDOUT_FILENO, buf, len);
			len = 0;
		}
	}

	/* move to bottom rows for status */
	if (playing >= 0) {
		const char *state = paused ? "[paused]" : "[playing]";
		int pm = (int)song_pos / 60, ps = (int)song_pos % 60;
		int dm = (int)song_dur / 60, ds = (int)song_dur % 60;

		/* song name + times on row rows-1 */
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H\033[32m%s %s\033[0m", rows - 1, state, songs[playing]);

		/* progress bar on bottom row */
		int bar_max = cols - 14; /* "mm:ss [===] mm:ss" */
		if (bar_max < 4) bar_max = 4;
		int filled = 0;
		if (song_dur > 0)
			filled = (int)(song_pos / song_dur * bar_max);
		if (filled > bar_max) filled = bar_max;

		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H%d:%02d \033[32m", rows, pm, ps);
		for (int i = 0; i < bar_max; i++) {
			if (i < filled)
				buf[len++] = '=';
			else if (i == filled)
				buf[len++] = '>';
			else
				buf[len++] = '-';
		}
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[0m %d:%02d", dm, ds);
	} else {
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H\033[2mj/k:nav  enter/spc:play  h/l:seek  spc:pause  esc:stop  q:quit\033[0m",
			rows);
	}

	write(STDOUT_FILENO, buf, len);
}

static void play_song(int idx) {
	kill_mpv();

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", songs_dir, songs[idx]);

	pid_t pid = fork();
	if (pid == 0) {
		/* redirect stdout/stderr to /dev/null */
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execlp("mpv", "mpv", "--no-video", "--no-terminal",
			"--input-ipc-server=" MPV_SOCKET, path, NULL);
		_exit(1);
	} else if (pid > 0) {
		mpv_pid = pid;
		playing = idx;
		paused = 0;
	}
}

static void check_child(void) {
	if (mpv_pid > 0) {
		int status;
		pid_t ret = waitpid(mpv_pid, &status, WNOHANG);
		if (ret == mpv_pid) {
			mpv_pid = -1;
			paused = 0;
			playing = -1;
		}
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--tmux") == 0)
			tmux_mode = 1;

	const char *env_dir = getenv("SONGS_DIR");
	if (env_dir)
		songs_dir = env_dir;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	if (scan_songs() == 0) {
		fprintf(stderr, "No songs found in %s/\n", songs_dir);
		return 1;
	}

	term_raw();
	draw();

	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

	for (;;) {
		int ready = poll(&pfd, 1, 250);

		check_child();
		update_position();

		if (ready <= 0) {
			draw();
			continue;
		}

		char c;
		if (read(STDIN_FILENO, &c, 1) != 1)
			break;

		switch (c) {
		case 'q':
			cleanup();
			return 0;
		case 'j':
			if (cursor < nsongs - 1) cursor++;
			break;
		case 'k':
			if (cursor > 0) cursor--;
			break;
		case '\r':
		case '\n':
			play_song(cursor);
			break;
		case ' ':
			if (mpv_pid > 0) {
				mpv_cmd("{\"command\":[\"cycle\",\"pause\"]}\n");
				paused = !paused;
			} else {
				play_song(cursor);
			}
			break;
		case 'h':
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"seek\",\"-5\"]}\n");
			break;
		case 'l':
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"seek\",\"5\"]}\n");
			break;
		case 0x1b: /* ESC */
			kill_mpv();
			break;
		case 'g':
			cursor = 0;
			break;
		case 'G':
			cursor = nsongs - 1;
			break;
		}

		draw();
	}

	cleanup();
	return 0;
}
