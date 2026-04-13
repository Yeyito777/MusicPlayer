#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

#define ESC "\033["
#define ANSI_RESET ESC "0m"
#define ANSI_BOLD ESC "1m"
#define ANSI_DIM ESC "2m"
#define FG_TEXT ESC "38;2;255;255;255m"
#define FG_ACCENT ESC "38;2;29;155;240m"
#define BG_APP ESC "48;2;0;5;15m"
#define BG_SIDEBAR ESC "48;2;3;8;20m"
#define BG_ACCENT ESC "48;2;29;155;240m"
#define MAIN_BASE ANSI_RESET FG_TEXT BG_APP
#define SIDEBAR_BASE ANSI_RESET FG_TEXT BG_SIDEBAR
#define MAIN_ACCENT ANSI_RESET FG_ACCENT BG_APP
#define MAIN_ACCENT_BOLD ANSI_RESET ANSI_BOLD FG_ACCENT BG_APP
#define MAIN_CURSOR ANSI_RESET ANSI_BOLD FG_TEXT BG_APP
#define MAIN_SELECTED ANSI_RESET ANSI_BOLD FG_TEXT BG_ACCENT
#define SIDEBAR_ACCENT ANSI_RESET FG_ACCENT BG_SIDEBAR
#define SIDEBAR_ACCENT_BOLD ANSI_RESET ANSI_BOLD FG_ACCENT BG_SIDEBAR
#define SIDEBAR_CURSOR ANSI_RESET ANSI_BOLD FG_TEXT BG_SIDEBAR
#define SIDEBAR_SELECTED ANSI_RESET ANSI_BOLD FG_TEXT BG_ACCENT
#define MAIN_DIM ANSI_RESET ANSI_DIM FG_TEXT BG_APP
#define SIDEBAR_DIM ANSI_RESET ANSI_DIM FG_TEXT BG_SIDEBAR

static const char *playlists_dir = PLAYLISTS_DIR;
static char *playlists[MAX_PLAYLISTS];
static int nplaylists = 0;

static int playlist_menu = 0;
static int playlist_cursor = 0;
enum { PANEL_MAIN, PANEL_SIDEBAR };
static int panel_focus = PANEL_MAIN;
static int playlist_active = -1;
static int playlist_songs[MAX_SONGS];
static int nplaylist_songs = 0;

static int delete_pending = -1; /* songs[] index marked for deletion */
static char trash_dir[PATH_MAX];

static void die(const char *msg) {
	perror(msg);
	exit(1);
}

static void term_restore(void) {
	if (!tmux_mode)
		write(STDOUT_FILENO, ANSI_RESET "\033[?1049l\033[?25h",
			sizeof(ANSI_RESET "\033[?1049l\033[?25h") - 1);
	else
		write(STDOUT_FILENO, ANSI_RESET "\033[?25h",
			sizeof(ANSI_RESET "\033[?25h") - 1);
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

static void flush_buf(char *buf, int *len) {
	if (*len > 0) {
		write(STDOUT_FILENO, buf, *len);
		*len = 0;
	}
}

static void appendf(char *buf, int *len, size_t size, const char *fmt, ...) {
	va_list ap;

	for (;;) {
		va_start(ap, fmt);
		int n = vsnprintf(buf + *len, size - *len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return;
		if ((size_t)n < size - *len) {
			*len += n;
			return;
		}

		flush_buf(buf, len);
		if ((size_t)n >= size) {
			char *tmp = malloc((size_t)n + 1);
			if (!tmp)
				return;
			va_start(ap, fmt);
			vsnprintf(tmp, (size_t)n + 1, fmt, ap);
			va_end(ap);
			write(STDOUT_FILENO, tmp, (size_t)n);
			free(tmp);
			return;
		}
	}
}

static void append_repeat(char *buf, int *len, size_t size, char ch, int count) {
	for (int i = 0; i < count; i++) {
		if (*len >= (int)size - 1)
			flush_buf(buf, len);
		buf[(*len)++] = ch;
	}
}

static void draw(void) {
	int rows = term_rows();
	int cols = term_cols();
	int list_rows = rows - 4; /* header + separator + status + progress */
	if (list_rows < 0)
		list_rows = 0;

	write(STDOUT_FILENO, MAIN_BASE "\033[2J\033[H",
		sizeof(MAIN_BASE "\033[2J\033[H") - 1);

	char buf[65536];
	int len = 0;
	char line[4096];

	int main_cols = cols;
	int main_col = 1;
	int sidebar_width = 0;
	int sb_col = 0;
	int sidebar_focused = playlist_menu && panel_focus == PANEL_SIDEBAR;
	int main_focused = !playlist_menu || panel_focus == PANEL_MAIN;

	if (playlist_menu && cols > 2) {
		sidebar_width = SIDEBAR_WIDTH;
		if (sidebar_width > cols - 2)
			sidebar_width = cols - 2;
		if (sidebar_width < 1)
			sidebar_width = 1;

		main_cols = cols - sidebar_width - 1;
		if (main_cols < 1)
			main_cols = 1;
		sb_col = 1;
		main_col = sidebar_width + 2;

		for (int r = 1; r <= rows; r++) {
			appendf(buf, &len, sizeof(buf), "\033[%d;%dH%s", r, sb_col, SIDEBAR_BASE);
			append_repeat(buf, &len, sizeof(buf), ' ', sidebar_width);
			appendf(buf, &len, sizeof(buf), "%s", MAIN_BASE);
		}

		appendf(buf, &len, sizeof(buf),
			"\033[1;%dH%s%-*.*s%s",
			sb_col, SIDEBAR_ACCENT_BOLD,
			sidebar_width, sidebar_width, " Playlists", MAIN_BASE);

		appendf(buf, &len, sizeof(buf), "\033[2;%dH%s", sb_col, SIDEBAR_ACCENT);
		append_repeat(buf, &len, sizeof(buf), '-', sidebar_width);
		appendf(buf, &len, sizeof(buf), "%s", MAIN_BASE);

		for (int i = 0; i <= nplaylists && i < list_rows; i++) {
			int row = i + 3;
			const char *name = (i == 0) ? "[All Songs]" : playlists[i - 1];
			const char *pfix = (i == playlist_cursor) ? "> " : "  ";
			const char *style = SIDEBAR_BASE;
			int is_active = (i == 0 && playlist_active == -1) ||
				(i > 0 && playlist_active == i - 1);

			if (i == playlist_cursor)
				style = sidebar_focused ? SIDEBAR_SELECTED : SIDEBAR_CURSOR;
			else if (is_active)
				style = SIDEBAR_ACCENT_BOLD;

			snprintf(line, sizeof(line), "%s%s", pfix, name);
			appendf(buf, &len, sizeof(buf),
				"\033[%d;%dH%s%-*.*s%s",
				row, sb_col, style,
				sidebar_width, sidebar_width, line, MAIN_BASE);
		}

		int border_col = sidebar_width + 1;
		for (int r = 1; r <= rows; r++) {
			appendf(buf, &len, sizeof(buf),
				"\033[%d;%dH%s|%s",
				r, border_col, MAIN_ACCENT, MAIN_BASE);
		}
	} else if (main_cols < 1) {
		main_cols = 1;
	}

	if (playlist_active >= 0) {
		appendf(buf, &len, sizeof(buf),
			"\033[1;%dH%s  MusicPlayer [%s]%s",
			main_col, MAIN_ACCENT_BOLD, playlists[playlist_active], MAIN_BASE);
	} else {
		appendf(buf, &len, sizeof(buf),
			"\033[1;%dH%s  MusicPlayer%s",
			main_col, MAIN_ACCENT_BOLD, MAIN_BASE);
	}
	{
		int vbars = volume / 5;
		if (vbars < 0)
			vbars = 0;
		if (vbars > 20)
			vbars = 20;

		appendf(buf, &len, sizeof(buf), " | [");
		if (vbars > 0)
			appendf(buf, &len, sizeof(buf), "%s", MAIN_ACCENT);
		for (int i = 0; i < 20; i++) {
			if (i == vbars)
				appendf(buf, &len, sizeof(buf), "%s", MAIN_DIM);
			append_repeat(buf, &len, sizeof(buf), '|', 1);
		}
		appendf(buf, &len, sizeof(buf), "%s]", MAIN_BASE);
	}

	appendf(buf, &len, sizeof(buf), "\033[2;%dH%s", main_col, MAIN_ACCENT);
	append_repeat(buf, &len, sizeof(buf), '-', main_cols);
	appendf(buf, &len, sizeof(buf), "%s", MAIN_BASE);

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
		const char *prefix = (dpos == cursor) ? "> " : "  ";
		const char *style = MAIN_BASE;

		if (dpos == cursor) {
			style = main_focused ? MAIN_SELECTED : MAIN_CURSOR;
		} else if (sidx == delete_pending) {
			style = MAIN_ACCENT_BOLD;
		} else if (sidx == playing) {
			style = MAIN_ACCENT;
		}

		const char *suffix = "";
		if (sidx == delete_pending) {
			suffix = " [delete]";
		} else if (sidx == playing) {
			if (loop_mode == LOOP_SINGLE) suffix = " [repeat]";
			else if (shuffle) suffix = " [shuffle]";
		}

		snprintf(line, sizeof(line), "%s%s%s", prefix, songs[sidx], suffix);
		appendf(buf, &len, sizeof(buf),
			"\033[%d;%dH%s%-*.*s%s",
			i + 3, main_col, style, main_cols, main_cols, line, MAIN_BASE);
	}

	/* status lines at bottom */
	if (playing >= 0) {
		const char *state = paused ? "[paused]" : "[playing]";
		const char *lmode = "";
		if (loop_mode == LOOP_SINGLE) lmode = "[repeat]";
		else if (shuffle) lmode = "[shuffle]";
		int pm = (int)song_pos / 60, ps = (int)song_pos % 60;
		int dm = (int)song_dur / 60, ds = (int)song_dur % 60;

		snprintf(line, sizeof(line), "%s%s %s", state, lmode, songs[playing]);
		appendf(buf, &len, sizeof(buf),
			"\033[%d;%dH%s%-*.*s%s",
			rows - 1, main_col, MAIN_ACCENT_BOLD, main_cols, main_cols, line, MAIN_BASE);

		int bar_max = main_cols - 14;
		if (bar_max < 4) bar_max = 4;
		int filled = 0;
		if (song_dur > 0)
			filled = (int)(song_pos / song_dur * bar_max);
		if (filled > bar_max) filled = bar_max;

		appendf(buf, &len, sizeof(buf),
			"\033[%d;%dH%s%d:%02d %s",
			rows, main_col, MAIN_BASE, pm, ps, MAIN_ACCENT);
		for (int i = 0; i < bar_max; i++) {
			if (i < filled)
				append_repeat(buf, &len, sizeof(buf), '=', 1);
			else if (i == filled)
				append_repeat(buf, &len, sizeof(buf), '>', 1);
			else
				append_repeat(buf, &len, sizeof(buf), '-', 1);
		}
		appendf(buf, &len, sizeof(buf), "%s %d:%02d", MAIN_BASE, dm, ds);
	} else if (!searching) {
		const char *help;
		if (playlist_menu && sidebar_focused) {
			help = "j/k:nav enter:apply C-j/k:focus C-m:hide esc:hide q:quit";
		} else if (playlist_menu) {
			help = "j/k:nav spc:play h/l:seek /:search C-j/k:focus C-m:hide q:quit";
		} else {
			help = "j/k:nav spc:play/pause h/l:seek -/+:vol m:loop n:shuffle d:del esc:stop q:quit";
		}
		appendf(buf, &len, sizeof(buf),
			"\033[%d;%dH%s%-*.*s%s",
			rows, main_col, MAIN_DIM, main_cols, main_cols, help, MAIN_BASE);
	}

	if (searching) {
		snprintf(line, sizeof(line), "/%s_", search_buf);
		appendf(buf, &len, sizeof(buf),
			"\033[%d;%dH%s%-*.*s%s",
			rows, main_col, MAIN_DIM, main_cols, main_cols, line, MAIN_BASE);
	}

	flush_buf(buf, &len);
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

static void remove_song(int rm) {
	/* stop playback if this song is playing */
	if (playing == rm)
		kill_mpv();

	/* move file to trash */
	mkdir(trash_dir, 0755);
	char src[PATH_MAX + 1024], dst[PATH_MAX + 1024];
	snprintf(src, sizeof(src), "%s/%s", songs_dir, songs[rm]);
	snprintf(dst, sizeof(dst), "%s/%s", trash_dir, songs[rm]);
	rename(src, dst);

	/* free the name */
	free(songs[rm]);

	/* adjust playing index */
	if (playing > rm) playing--;
	else if (playing == rm) playing = -1;

	/* shift songs[] array down */
	for (int i = rm; i < nsongs - 1; i++)
		songs[i] = songs[i + 1];
	nsongs--;

	/* adjust playlist_songs[]: remove entry and shift indices */
	int pw = 0;
	for (int i = 0; i < nplaylist_songs; i++) {
		if (playlist_songs[i] == rm) continue;
		int val = playlist_songs[i];
		if (val > rm) val--;
		playlist_songs[pw++] = val;
	}
	nplaylist_songs = pw;

	/* adjust filtered[]: remove entry and shift indices */
	int fw = 0;
	for (int i = 0; i < nfiltered; i++) {
		if (filtered[i] == rm) continue;
		int val = filtered[i];
		if (val > rm) val--;
		filtered[fw++] = val;
	}
	nfiltered = fw;

	/* clear shuffle state — indices are invalidated */
	shuffle_clear();

	/* clamp cursor to valid range */
	if (cursor >= display_len()) cursor = display_len() - 1;
	if (cursor < 0) cursor = 0;
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

	/* derive trash_dir as sibling of songs_dir */
	strncpy(trash_dir, songs_dir, sizeof(trash_dir) - 1);
	trash_dir[sizeof(trash_dir) - 1] = '\0';
	char *last_slash = strrchr(trash_dir, '/');
	if (last_slash) {
		strcpy(last_slash + 1, "trash");
	} else {
		strcpy(trash_dir, "trash");
	}

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

		int ctrl_j = 0;
		int ctrl_k = (c == 0x0b);
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
			if (strcmp(seq, "[106;5u") == 0) {
				ctrl_j = 1;
				c = 0;
			} else if (strcmp(seq, "[107;5u") == 0) {
				ctrl_k = 1;
				c = 0;
			} else if (strcmp(seq, "[109;5u") == 0) {
				ctrl_m = 1;
				c = 0;
			}
		}

		if (ctrl_m) {
			playlist_menu = !playlist_menu;
			if (playlist_menu) {
				playlist_cursor = (playlist_active >= 0) ? playlist_active + 1 : 0;
				panel_focus = PANEL_SIDEBAR;
			} else {
				panel_focus = PANEL_MAIN;
			}
			save_state();
			draw();
			continue;
		}

		if ((ctrl_j || ctrl_k) && playlist_menu && !searching) {
			panel_focus = (panel_focus == PANEL_SIDEBAR) ? PANEL_MAIN : PANEL_SIDEBAR;
			save_state();
			draw();
			continue;
		}
		if (ctrl_j || ctrl_k)
			c = 0;

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

		if (playlist_menu && panel_focus == PANEL_SIDEBAR) {
			switch (c) {
			case 0x03: /* Ctrl+C */
			case 'q':
				cleanup();
				return 0;
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
				searching = 0;
				search_buf[0] = '\0';
				search_len = 0;
				filter_active = 0;
				cursor = 0;
				delete_pending = -1;
				break;
			case 0x1b:
				playlist_menu = 0;
				panel_focus = PANEL_MAIN;
				break;
			}
			save_state();
			draw();
			continue;
		}

		switch (c) {
		case 'q':
		case 0x03: /* Ctrl+C (SIGINT blocked by raw mode, handle byte directly) */
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
		case 'H': {
			if (playing < 0 || display_len() == 0) break;
			int len = display_len();
			int cur = -1;
			for (int i = 0; i < len; i++) {
				if (song_at(i) == playing) { cur = i; break; }
			}
			int prev = (cur > 0) ? cur - 1 : len - 1;
			play_song(song_at(prev));
			if (shuffle) shuffle_mark(song_at(prev));
			break;
		}
		case 'l':
			if (mpv_pid > 0)
				mpv_cmd("{\"command\":[\"seek\",\"5\"]}\n");
			break;
		case 'L': {
			if (playing < 0 || display_len() == 0) break;
			if (shuffle) {
				int next = shuffle_next();
				shuffle_mark(next);
				play_song(next);
			} else {
				int len = display_len();
				int cur = -1;
				for (int i = 0; i < len; i++) {
					if (song_at(i) == playing) { cur = i; break; }
				}
				int next = (cur >= 0 && cur + 1 < len) ? cur + 1 : 0;
				play_song(song_at(next));
			}
			break;
		}
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
		case 'd': {
			if (display_len() == 0) break;
			int sidx = song_at(cursor);
			if (delete_pending == sidx) {
				/* confirm deletion */
				remove_song(sidx);
				delete_pending = -1;
			} else {
				/* mark for deletion */
				delete_pending = sidx;
			}
			break;
		}
		case 0x1b: /* ESC */
			if (delete_pending >= 0) {
				delete_pending = -1;
			} else {
				kill_mpv();
			}
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
