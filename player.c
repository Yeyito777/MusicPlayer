#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <string.h>
#include <time.h>
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

enum { LOOP_ALL, LOOP_SINGLE };
static int loop_mode = LOOP_ALL;
static int shuffle = 0;
static int played[MAX_SONGS];
static int nplayed = 0;

static const char *songs_dir = SONGS_DIR;
static char *songs[MAX_SONGS];
static int nsongs = 0;
static int cursor = 0;
static int playing = -1;
static double song_pos = 0;
static double song_dur = 0;

static int searching = 0;
static char search_buf[256];
static int search_len = 0;
static int search_prev_cursor = 0;
static int filtered[MAX_SONGS];
static int nfiltered = 0;
static int filter_active = 0;

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

static int song_at(int pos) {
	return filter_active ? filtered[pos] : pos;
}

static int display_len(void) {
	return filter_active ? nfiltered : nsongs;
}

static void apply_filter(void) {
	int prev_song = song_at(cursor);

	if (search_len == 0) {
		filter_active = 0;
		cursor = prev_song;
		return;
	}

	regex_t re;
	if (regcomp(&re, search_buf, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
		return; /* invalid regex, keep previous state */

	nfiltered = 0;
	for (int i = 0; i < nsongs; i++) {
		if (regexec(&re, songs[i], 0, NULL, 0) == 0)
			filtered[nfiltered++] = i;
	}
	regfree(&re);
	filter_active = 1;

	/* try to keep cursor on the same song */
	cursor = 0;
	for (int i = 0; i < nfiltered; i++) {
		if (filtered[i] == prev_song) {
			cursor = i;
			break;
		}
	}
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
	int count = display_len();
	int offset = 0;
	if (count > list_rows) {
		offset = cursor - list_rows / 2;
		if (offset < 0) offset = 0;
		if (offset > count - list_rows) offset = count - list_rows;
	}

	for (int i = 0; i < list_rows && (i + offset) < count; i++) {
		int dpos = i + offset;
		int sidx = song_at(dpos);
		const char *prefix = "  ";
		const char *style = "";
		const char *reset = "";

		if (dpos == cursor && sidx == playing) {
			prefix = "> ";
			style = "\033[1;32m"; /* bold green */
			reset = "\033[0m";
		} else if (dpos == cursor) {
			prefix = "> ";
			style = "\033[1m"; /* bold */
			reset = "\033[0m";
		} else if (sidx == playing) {
			style = "\033[32m"; /* green */
			reset = "\033[0m";
		}

		const char *suffix = "";
		if (sidx == playing) {
			if (loop_mode == LOOP_SINGLE) suffix = " [repeat]";
			else if (shuffle) suffix = " [shuffle]";
		}
		len += snprintf(buf + len, sizeof(buf) - len,
			"%s%s%s%s%s\r\n", style, prefix, songs[sidx], suffix, reset);

		if (len >= (int)sizeof(buf) - 256) {
			write(STDOUT_FILENO, buf, len);
			len = 0;
		}
	}

	/* move to bottom rows for status */
	if (playing >= 0) {
		const char *state = paused ? "[paused]" : "[playing]";
		const char *lmode = "";
		if (loop_mode == LOOP_SINGLE) lmode = "[repeat]";
		else if (shuffle) lmode = "[shuffle]";
		int pm = (int)song_pos / 60, ps = (int)song_pos % 60;
		int dm = (int)song_dur / 60, ds = (int)song_dur % 60;

		/* song name + times on row rows-1 */
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H\033[32m%s%s %s\033[0m", rows - 1, state, lmode, songs[playing]);

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
	} else if (!searching) {
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H\033[2mj/k:nav  spc:play/pause  h/l:seek  -/+:vol  m:loop  n:shuffle  esc:stop  q:quit\033[0m",
			rows);
	}

	if (searching) {
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H\033[2m/%s_\033[0m", rows, search_buf);
	}

	write(STDOUT_FILENO, buf, len);
}

static void play_song(int idx) {
	kill_mpv();

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", songs_dir, songs[idx]);

	pid_t pid = fork();
	if (pid == 0) {
		/* detach from terminal completely */
		freopen("/dev/null", "r", stdin);
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

static void shuffle_clear(void) {
	memset(played, 0, sizeof(played));
	nplayed = 0;
}

static void shuffle_mark(int idx) {
	if (!played[idx]) {
		played[idx] = 1;
		nplayed++;
	}
	if (nplayed >= nsongs)
		shuffle_clear();
}

static int shuffle_next(void) {
	int avail = nsongs - nplayed;
	if (avail <= 0) {
		shuffle_clear();
		avail = nsongs;
	}
	int pick = rand() % avail;
	int count = 0;
	for (int i = 0; i < nsongs; i++) {
		if (!played[i]) {
			if (count == pick)
				return i;
			count++;
		}
	}
	return 0;
}

static void check_child(void) {
	if (mpv_pid > 0) {
		int status;
		pid_t ret = waitpid(mpv_pid, &status, WNOHANG);
		if (ret == mpv_pid) {
			int prev = playing;
			mpv_pid = -1;
			paused = 0;
			playing = -1;
			song_pos = 0;
			song_dur = 0;
			/* auto-play based on mode */
			if (prev >= 0) {
				if (loop_mode == LOOP_SINGLE) {
					play_song(prev);
				} else if (shuffle) {
					int next = shuffle_next();
					shuffle_mark(next);
					play_song(next);
				} else if (prev + 1 < nsongs) {
					play_song(prev + 1);
				} else {
					play_song(0); /* wrap to top */
				}
			}
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

	srand(time(NULL));
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

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

		if (searching) {
			if (c == '\r' || c == '\n') {
				searching = 0;
			} else if (c == 0x1b) {
				searching = 0;
				filter_active = 0;
				cursor = search_prev_cursor;
			} else if (c == 0x7f) {
				if (search_len > 0) {
					search_buf[--search_len] = '\0';
					apply_filter();
				}
			} else if (c >= 32 && c < 127) {
				if (search_len < (int)sizeof(search_buf) - 1) {
					search_buf[search_len++] = c;
					search_buf[search_len] = '\0';
					apply_filter();
				}
			}
			draw();
			continue;
		}

		switch (c) {
		case 'q':
			cleanup();
			return 0;
		case 'j':
			if (cursor < display_len() - 1) cursor++;
			break;
		case 'k':
			if (cursor > 0) cursor--;
			break;
		case '\r':
		case '\n':
			if (display_len() > 0) {
				play_song(song_at(cursor));
				if (shuffle) shuffle_mark(song_at(cursor));
			}
			break;
		case ' ':
			if (mpv_pid > 0) {
				mpv_cmd("{\"command\":[\"cycle\",\"pause\"]}\n");
				paused = !paused;
			} else if (display_len() > 0) {
				play_song(song_at(cursor));
				if (shuffle) shuffle_mark(song_at(cursor));
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
		case '=':
		case '+':
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"add\",\"volume\",5]}\n");
			break;
		case '-':
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"add\",\"volume\",-5]}\n");
			break;
		case 'm':
			if (loop_mode == LOOP_SINGLE) {
				loop_mode = LOOP_ALL;
			} else {
				loop_mode = LOOP_SINGLE;
				shuffle = 0;
			}
			break;
		case 'n':
			shuffle = !shuffle;
			if (shuffle) {
				loop_mode = LOOP_ALL;
				shuffle_clear();
				if (playing >= 0) shuffle_mark(playing);
			}
			break;
		case 0x1b: /* ESC */
			kill_mpv();
			break;
		case '/':
		case '?':
			search_prev_cursor = song_at(cursor);
			search_buf[0] = '\0';
			search_len = 0;
			filter_active = 0;
			cursor = search_prev_cursor;
			searching = 1;
			break;
		case 'g':
			if (display_len() > 0) cursor = 0;
			break;
		case 'G':
			if (display_len() > 0) cursor = display_len() - 1;
			break;
		}

		draw();
	}

	cleanup();
	return 0;
}
