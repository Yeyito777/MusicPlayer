#include <dirent.h>
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

static char *songs[MAX_SONGS];
static int nsongs = 0;
static int cursor = 0;
static int playing = -1;

static void die(const char *msg) {
	perror(msg);
	exit(1);
}

static void term_restore(void) {
	/* leave alt buffer, show cursor, restore termios */
	write(STDOUT_FILENO, "\033[?1049l\033[?25h", 14);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void mpv_cmd(const char *cmd) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, MPV_SOCKET, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		write(fd, cmd, strlen(cmd));
	close(fd);
}

static void kill_mpv(void) {
	if (mpv_pid > 0) {
		kill(mpv_pid, SIGTERM);
		waitpid(mpv_pid, NULL, 0);
		mpv_pid = -1;
		paused = 0;
		playing = -1;
	}
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

	/* enter alt buffer, hide cursor */
	write(STDOUT_FILENO, "\033[?1049h\033[?25l", 14);
}

static int scan_songs(void) {
	struct dirent **namelist;
	int n = scandir(SONGS_DIR, &namelist, NULL, alphasort);
	if (n < 0)
		die("scandir: " SONGS_DIR);

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
	int list_rows = rows - 3; /* header + footer + separator */

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

	/* move to bottom row for status */
	len += snprintf(buf + len, sizeof(buf) - len, "\033[%d;1H", rows);

	if (playing >= 0) {
		const char *state = paused ? "[paused]" : "[playing]";
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[32m%s %s\033[0m", state, songs[playing]);
	} else {
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[2mj/k:nav  enter:play  p:pause  s:stop  q:quit\033[0m");
	}

	write(STDOUT_FILENO, buf, len);
}

static void play_song(int idx) {
	kill_mpv();

	char path[PATH_MAX];
	snprintf(path, sizeof(path), SONGS_DIR "/%s", songs[idx]);

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

int main(void) {
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	if (scan_songs() == 0) {
		fprintf(stderr, "No songs found in %s/\n", SONGS_DIR);
		return 1;
	}

	term_raw();
	draw();

	for (;;) {
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
		case 'p':
			if (mpv_pid > 0) {
				mpv_cmd("{\"command\":[\"cycle\",\"pause\"]}\n");
				paused = !paused;
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
		case 's':
			kill_mpv();
			break;
		case 'g':
			cursor = 0;
			break;
		case 'G':
			cursor = nsongs - 1;
			break;
		}

		check_child();
		draw();
	}

	cleanup();
	return 0;
}
