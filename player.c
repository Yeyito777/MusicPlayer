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
static int scroll_offset = 0;
static int playing = -1;
static double song_pos = 0;
static double song_dur = 0;
static int volume = 100;

#define STATE_FILE "state.save"
static const char *state_file = STATE_FILE;

static char saved_song[1024];
static char saved_cursor[1024];
static double saved_pos = 0;
static char saved_playlist[256];
static int saved_paused = 0;

static int searching = 0;
static char search_buf[256];
static int search_len = 0;
static int search_prev_cursor = 0;
static int filtered[MAX_SONGS];
static int nfiltered = 0;
static int filter_active = 0;

#define MAX_PLAYLISTS 64
#define PLAYLISTS_DIR "playlists"
#define SIDEBAR_WIDTH 24

static const char *playlists_dir = PLAYLISTS_DIR;
static char *playlists[MAX_PLAYLISTS];
static int nplaylists = 0;

static int playlist_menu = 0;
static int playlist_cursor = 0;
static int playlist_active = -1;
static int playlist_songs[MAX_SONGS];
static int nplaylist_songs = 0;

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

static void scan_playlists(void) {
	struct dirent **namelist;
	int n = scandir(playlists_dir, &namelist, NULL, alphasort);
	if (n < 0) return; /* no playlists dir is fine */

	for (int i = 0; i < n; i++) {
		const char *name = namelist[i]->d_name;
		const char *ext = strstr(name, ".playlist");
		if (ext && ext[9] == '\0' && ext != name) {
			if (nplaylists < MAX_PLAYLISTS) {
				int baselen = ext - name;
				playlists[nplaylists] = malloc(baselen + 1);
				memcpy(playlists[nplaylists], name, baselen);
				playlists[nplaylists][baselen] = '\0';
				nplaylists++;
			}
		}
		free(namelist[i]);
	}
	free(namelist);
}

static void load_playlist(int idx);
static int find_in_display(int song_idx);
static int song_at(int pos);
static int display_len(void);
static void play_song(int idx);

static void load_state(void) {
	FILE *f = fopen(state_file, "r");
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		int len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = '\0';

		int v;
		char s[16];
		if (sscanf(line, "volume=%d", &v) == 1) {
			if (v >= 0 && v <= 100) volume = v;
		} else if (strncmp(line, "song=", 5) == 0) {
			strncpy(saved_song, line + 5, sizeof(saved_song) - 1);
			saved_song[sizeof(saved_song) - 1] = '\0';
		} else if (sscanf(line, "position=%lf", &saved_pos) == 1) {
			/* parsed inline */
		} else if (strncmp(line, "cursor=", 7) == 0) {
			strncpy(saved_cursor, line + 7, sizeof(saved_cursor) - 1);
			saved_cursor[sizeof(saved_cursor) - 1] = '\0';
		} else if (strncmp(line, "playlist=", 9) == 0) {
			strncpy(saved_playlist, line + 9, sizeof(saved_playlist) - 1);
			saved_playlist[sizeof(saved_playlist) - 1] = '\0';
		} else if (sscanf(line, "loop=%15s", s) == 1) {
			if (strcmp(s, "single") == 0) loop_mode = LOOP_SINGLE;
			else loop_mode = LOOP_ALL;
		} else if (sscanf(line, "shuffle=%d", &v) == 1) {
			shuffle = (v != 0);
		} else if (sscanf(line, "paused=%d", &v) == 1) {
			saved_paused = (v != 0);
		}
	}
	fclose(f);
}

static void save_state(void) {
	FILE *f = fopen(state_file, "w");
	if (!f) return;
	fprintf(f, "volume=%d\n", volume);
	if (playing >= 0) {
		fprintf(f, "song=%s\n", songs[playing]);
		fprintf(f, "position=%.2f\n", song_pos);
		fprintf(f, "paused=%d\n", paused);
	}
	if (display_len() > 0 && cursor >= 0 && cursor < display_len())
		fprintf(f, "cursor=%s\n", songs[song_at(cursor)]);
	if (playlist_active >= 0)
		fprintf(f, "playlist=%s\n", playlists[playlist_active]);
	fprintf(f, "loop=%s\n", (loop_mode == LOOP_SINGLE) ? "single" : "all");
	fprintf(f, "shuffle=%d\n", shuffle);
	fclose(f);
}

static void restore_state(void) {
	/* restore playlist first (affects find_in_display) */
	if (saved_playlist[0]) {
		for (int i = 0; i < nplaylists; i++) {
			if (strcmp(playlists[i], saved_playlist) == 0) {
				playlist_active = i;
				load_playlist(i);
				break;
			}
		}
	}

	/* restore cursor */
	if (saved_cursor[0]) {
		for (int i = 0; i < nsongs; i++) {
			if (strcmp(songs[i], saved_cursor) == 0) {
				cursor = find_in_display(i);
				break;
			}
		}
	}

	/* restore playback */
	if (saved_song[0]) {
		int idx = -1;
		for (int i = 0; i < nsongs; i++) {
			if (strcmp(songs[i], saved_song) == 0) {
				idx = i;
				break;
			}
		}
		if (idx >= 0) {
			play_song(idx);
			/* wait for mpv IPC socket */
			for (int i = 0; i < 10; i++) {
				usleep(50000);
				if (mpv_connect() == 0) break;
			}
			if (saved_pos > 0) {
				char cmd[128];
				snprintf(cmd, sizeof(cmd),
					"{\"command\":[\"seek\",%.2f,\"absolute\"]}\n", saved_pos);
				mpv_cmd(cmd);
			}
			if (saved_paused) {
				mpv_cmd("{\"command\":[\"cycle\",\"pause\"]}\n");
				paused = 1;
			}
		}
	}
}

static void load_playlist(int idx) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s.playlist", playlists_dir, playlists[idx]);
	FILE *f = fopen(path, "r");
	if (!f) return;

	nplaylist_songs = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		/* strip newline */
		int len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = '\0';
		if (len == 0) continue;
		for (int i = 0; i < nsongs; i++) {
			if (strcmp(songs[i], line) == 0) {
				if (nplaylist_songs < MAX_SONGS)
					playlist_songs[nplaylist_songs++] = i;
				break;
			}
		}
	}
	fclose(f);
}

static int find_in_display(int song_idx) {
	if (filter_active) {
		for (int i = 0; i < nfiltered; i++)
			if (filtered[i] == song_idx) return i;
	} else if (playlist_active >= 0) {
		for (int i = 0; i < nplaylist_songs; i++)
			if (playlist_songs[i] == song_idx) return i;
	} else {
		if (song_idx >= 0 && song_idx < nsongs) return song_idx;
	}
	return 0;
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
	if (filter_active) return filtered[pos];
	if (playlist_active >= 0) return playlist_songs[pos];
	return pos;
}

static int display_len(void) {
	if (filter_active) return nfiltered;
	if (playlist_active >= 0) return nplaylist_songs;
	return nsongs;
}

static void apply_filter(void) {
	int prev_song = song_at(cursor);

	if (search_len == 0) {
		filter_active = 0;
		cursor = find_in_display(prev_song);
		return;
	}

	regex_t re;
	if (regcomp(&re, search_buf, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
		return; /* invalid regex, keep previous state */

	nfiltered = 0;
	int base_count = (playlist_active >= 0) ? nplaylist_songs : nsongs;
	for (int i = 0; i < base_count; i++) {
		int sidx = (playlist_active >= 0) ? playlist_songs[i] : i;
		if (regexec(&re, songs[sidx], 0, NULL, 0) == 0)
			filtered[nfiltered++] = sidx;
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

	char buf[8192];
	int len = 0;

	int main_cols = cols;
	int sb_col = 0; /* sidebar start column (0 = no sidebar) */

	if (playlist_menu) {
		main_cols = cols - SIDEBAR_WIDTH - 1;
		sb_col = cols - SIDEBAR_WIDTH + 1;

		/* sidebar header */
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[1;%dH\033[1mPlaylists\033[0m", sb_col);

		/* sidebar separator */
		len += snprintf(buf + len, sizeof(buf) - len, "\033[2;%dH", sb_col);
		for (int i = 0; i < SIDEBAR_WIDTH && len < (int)sizeof(buf) - 1; i++)
			buf[len++] = '-';

		/* sidebar entries */
		for (int i = 0; i <= nplaylists && i < list_rows; i++) {
			int row = i + 3;
			const char *name = (i == 0) ? "[All Songs]" : playlists[i - 1];
			const char *pfix = (i == playlist_cursor) ? "> " : "  ";
			const char *st = "";
			const char *rs = "";
			int is_active = (i == 0 && playlist_active == -1) ||
				(i > 0 && playlist_active == i - 1);

			if (i == playlist_cursor && is_active) {
				st = "\033[1;32m"; rs = "\033[0m";
			} else if (i == playlist_cursor) {
				st = "\033[1m"; rs = "\033[0m";
			} else if (is_active) {
				st = "\033[32m"; rs = "\033[0m";
			}

			len += snprintf(buf + len, sizeof(buf) - len,
				"\033[%d;%dH%s%s%s%s", row, sb_col, st, pfix, name, rs);
		}

		/* border */
		int border_col = sb_col - 1;
		for (int r = 1; r <= rows; r++) {
			len += snprintf(buf + len, sizeof(buf) - len,
				"\033[%d;%dH|", r, border_col);
			if (len >= (int)sizeof(buf) - 256) {
				write(STDOUT_FILENO, buf, len);
				len = 0;
			}
		}
	}

	/* main header + volume indicator */
	if (playlist_active >= 0) {
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[1;1H\033[1m  MusicPlayer [%s]\033[0m", playlists[playlist_active]);
	} else {
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[1;1H\033[1m  MusicPlayer\033[0m");
	}
	{
		int vbars = volume / 5;
		len += snprintf(buf + len, sizeof(buf) - len, " | [");
		if (vbars > 0)
			len += snprintf(buf + len, sizeof(buf) - len, "\033[32m");
		for (int i = 0; i < 20; i++) {
			if (i == vbars)
				len += snprintf(buf + len, sizeof(buf) - len, "\033[2m");
			buf[len++] = '|';
		}
		len += snprintf(buf + len, sizeof(buf) - len, "\033[0m]");
	}

	/* main separator */
	len += snprintf(buf + len, sizeof(buf) - len, "\033[2;1H");
	for (int i = 0; i < main_cols && len < (int)sizeof(buf) - 1; i++)
		buf[len++] = '-';

	/* song list — vim-style edge scrolling */
	int count = display_len();
	if (cursor < scroll_offset)
		scroll_offset = cursor;
	else if (cursor >= scroll_offset + list_rows)
		scroll_offset = cursor - list_rows + 1;
	if (count <= list_rows)
		scroll_offset = 0;
	else if (scroll_offset > count - list_rows)
		scroll_offset = count - list_rows;
	if (scroll_offset < 0)
		scroll_offset = 0;

	for (int i = 0; i < list_rows && (i + scroll_offset) < count; i++) {
		int dpos = i + scroll_offset;
		int sidx = song_at(dpos);
		const char *prefix = "  ";
		const char *style = "";
		const char *reset = "";

		if (dpos == cursor && sidx == playing) {
			prefix = "> ";
			style = "\033[1;32m";
			reset = "\033[0m";
		} else if (dpos == cursor) {
			prefix = "> ";
			style = "\033[1m";
			reset = "\033[0m";
		} else if (sidx == playing) {
			style = "\033[32m";
			reset = "\033[0m";
		}

		const char *suffix = "";
		if (sidx == playing) {
			if (loop_mode == LOOP_SINGLE) suffix = " [repeat]";
			else if (shuffle) suffix = " [shuffle]";
		}

		int row = i + 3;
		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H%s%s%s%s%s", row, style, prefix, songs[sidx], suffix, reset);

		if (len >= (int)sizeof(buf) - 256) {
			write(STDOUT_FILENO, buf, len);
			len = 0;
		}
	}

	/* status lines at bottom */
	if (playing >= 0) {
		const char *state = paused ? "[paused]" : "[playing]";
		const char *lmode = "";
		if (loop_mode == LOOP_SINGLE) lmode = "[repeat]";
		else if (shuffle) lmode = "[shuffle]";
		int pm = (int)song_pos / 60, ps = (int)song_pos % 60;
		int dm = (int)song_dur / 60, ds = (int)song_dur % 60;

		len += snprintf(buf + len, sizeof(buf) - len,
			"\033[%d;1H\033[32m%s%s %s\033[0m", rows - 1, state, lmode, songs[playing]);

		int bar_max = main_cols - 14;
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

	char vol_arg[32];
	snprintf(vol_arg, sizeof(vol_arg), "--volume=%d", volume);

	pid_t pid = fork();
	if (pid == 0) {
		/* detach from terminal completely */
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execlp("mpv", "mpv", "--no-video", "--no-terminal",
			"--input-ipc-server=" MPV_SOCKET, vol_arg, path, NULL);
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
	if (nplayed >= display_len())
		shuffle_clear();
}

static int shuffle_next(void) {
	int len = display_len();
	int avail = 0;
	for (int i = 0; i < len; i++)
		if (!played[song_at(i)])
			avail++;
	if (avail <= 0) {
		shuffle_clear();
		avail = len;
	}
	int pick = rand() % avail;
	int count = 0;
	for (int i = 0; i < len; i++) {
		if (!played[song_at(i)]) {
			if (count == pick)
				return song_at(i);
			count++;
		}
	}
	return song_at(0);
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
				} else {
					int len = display_len();
					int cur = -1;
					for (int i = 0; i < len; i++) {
						if (song_at(i) == prev) {
							cur = i;
							break;
						}
					}
					if (cur >= 0 && cur + 1 < len)
						play_song(song_at(cur + 1));
					else
						play_song(song_at(0));
				}
			}
		}
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--tmux") == 0)
			tmux_mode = 1;

	const char *home = getenv("MUSIC_PLAYER_HOME");
	if (home) {
		static char songs_path[PATH_MAX];
		static char playlists_path[PATH_MAX];
		static char state_path[PATH_MAX];
		snprintf(songs_path, sizeof(songs_path), "%s/%s", home, SONGS_DIR);
		snprintf(playlists_path, sizeof(playlists_path), "%s/%s", home, PLAYLISTS_DIR);
		snprintf(state_path, sizeof(state_path), "%s/%s", home, STATE_FILE);
		songs_dir = songs_path;
		playlists_dir = playlists_path;
		state_file = state_path;
	}
	const char *env_dir = getenv("SONGS_DIR");
	if (env_dir)
		songs_dir = env_dir;
	const char *env_pdir = getenv("PLAYLISTS_DIR");
	if (env_pdir)
		playlists_dir = env_pdir;

	srand(time(NULL));
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	if (scan_songs() == 0) {
		fprintf(stderr, "No songs found in %s/\n", songs_dir);
		return 1;
	}
	scan_playlists();
	load_state();

	term_raw();
	restore_state();
	draw();

	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

	for (;;) {
		int ready = poll(&pfd, 1, 250);

		check_child();
		update_position();

		if (ready <= 0) {
			save_state();
			draw();
			continue;
		}

		char c;
		if (read(STDIN_FILENO, &c, 1) != 1)
			break;

		/* CSI sequence detection for Ctrl+M */
		int ctrl_m = 0;
		if (c == 0x1b) {
			char seq[32];
			int slen = 0;
			struct pollfd sp = { .fd = STDIN_FILENO, .events = POLLIN };
			while (slen < 31 && poll(&sp, 1, 20) > 0) {
				if (read(STDIN_FILENO, &seq[slen], 1) != 1) break;
				slen++;
				/* CSI final byte is >= 0x40, but skip '[' introducer */
				if (slen > 1 && seq[slen-1] >= 0x40) break;
			}
			seq[slen] = '\0';
			if (strcmp(seq, "[109;5u") == 0) ctrl_m = 1;
		}

		if (ctrl_m) {
			playlist_menu = !playlist_menu;
			if (playlist_menu)
				playlist_cursor = (playlist_active >= 0) ? playlist_active + 1 : 0;
			save_state();
			draw();
			continue;
		}

		if (searching) {
			if (c == '\r' || c == '\n') {
				searching = 0;
			} else if (c == 0x1b) {
				searching = 0;
				filter_active = 0;
				cursor = find_in_display(search_prev_cursor);
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
			save_state();
			draw();
			continue;
		}

		if (playlist_menu) {
			switch (c) {
			case 'j':
				if (playlist_cursor < nplaylists) playlist_cursor++;
				break;
			case 'k':
				if (playlist_cursor > 0) playlist_cursor--;
				break;
			case 'g':
				playlist_cursor = 0;
				break;
			case 'G':
				playlist_cursor = nplaylists;
				break;
			case '\r':
			case '\n':
				if (playlist_cursor == 0) {
					playlist_active = -1;
					nplaylist_songs = 0;
				} else {
					playlist_active = playlist_cursor - 1;
					load_playlist(playlist_active);
				}
				playlist_menu = 0;
				searching = 0;
				search_buf[0] = '\0';
				search_len = 0;
				filter_active = 0;
				cursor = 0;
				break;
			case 0x1b:
				playlist_menu = 0;
				break;
			}
			save_state();
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
		case '0':
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"seek\",\"0\",\"absolute\"]}\n");
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
			volume += 5;
			if (volume > 100) volume = 100;
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"add\",\"volume\",5]}\n");
			break;
		case '-':
			volume -= 5;
			if (volume < 0) volume = 0;
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
			cursor = find_in_display(search_prev_cursor);
			searching = 1;
			break;
		case 'g':
			if (display_len() > 0) cursor = 0;
			break;
		case 'G':
			if (display_len() > 0) cursor = display_len() - 1;
			break;
		case 0x05: { /* Ctrl+E — scroll down one line */
			int cnt = display_len();
			int lr = term_rows() - 4;
			if (cnt > lr && scroll_offset < cnt - lr)
				scroll_offset++;
			if (cursor < scroll_offset)
				cursor = scroll_offset;
			break;
		}
		case 0x19: { /* Ctrl+Y — scroll up one line */
			int lr = term_rows() - 4;
			if (scroll_offset > 0)
				scroll_offset--;
			if (cursor >= scroll_offset + lr)
				cursor = scroll_offset + lr - 1;
			break;
		}
		case 0x04: { /* Ctrl+D — scroll down half page */
			int half = (term_rows() - 4) / 2;
			int cnt = display_len();
			cursor += half;
			scroll_offset += half;
			if (cursor >= cnt) cursor = cnt - 1;
			break;
		}
		case 0x15: { /* Ctrl+U — scroll up half page */
			int half = (term_rows() - 4) / 2;
			cursor -= half;
			scroll_offset -= half;
			if (cursor < 0) cursor = 0;
			if (scroll_offset < 0) scroll_offset = 0;
			break;
		}
		case 0x06: { /* Ctrl+F — scroll down full page */
			int lr = term_rows() - 4;
			int cnt = display_len();
			cursor += lr;
			scroll_offset += lr;
			if (cursor >= cnt) cursor = cnt - 1;
			break;
		}
		case 0x02: { /* Ctrl+B — scroll up full page */
			int lr = term_rows() - 4;
			cursor -= lr;
			scroll_offset -= lr;
			if (cursor < 0) cursor = 0;
			if (scroll_offset < 0) scroll_offset = 0;
			break;
		}
		}

		save_state();
		draw();
	}

	cleanup();
	return 0;
}
