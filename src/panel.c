/*
 * cait panel: a full-screen view of the feed. Pick an agent, press a key to
 * stop it. Plain ANSI escapes, no libraries.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cait.h"

#define MAX_AGENTS 64
#define FILE_SEP '\x1f'

static volatile sig_atomic_t quit;
static int show_hidden;	/* key h: also list agents the user cleared */

static perm_t g_pend[8];	/* requests waiting for the user, refreshed with the agent list */
static int g_npend;

static const perm_t *waiting_for(const char *name)
{
	for (int i = 0; i < g_npend; i++)
		if (strcmp(g_pend[i].agent, name) == 0)
			return &g_pend[i];
	return NULL;
}

/* 0 needs you now, 1 needs a look, 2 running, 3 finished */
static int agent_rank(const agent_info_t *a)
{
	if (waiting_for(a->name))
		return 0;
	if (strcmp(a->state, "stopped") == 0 || (strcmp(a->state, "done") == 0 && (a->unreported || !a->final)))
		return 1;
	if (strcmp(a->state, "working") == 0 || strcmp(a->state, "quiet") == 0)
		return 2;
	return 3;
}

/* status_load, minus the agents the user cleared, most urgent first */
static int load_agents(agent_info_t *out)
{
	agent_info_t *all = calloc(MAX_AGENTS, sizeof(*all));
	int n, k = 0;

	g_npend = perm_load(g_pend, 8);
	n = all ? status_load(all, MAX_AGENTS) : 0;
	for (int i = 0; i < n; i++)
		if (show_hidden || !all[i].hidden)
			out[k++] = all[i];
	free(all);
	for (int i = 1; i < k; i++) {	/* stable insertion sort: rank, then most recently active */
		agent_info_t t = out[i];
		int j = i;

		while (j > 0 && (agent_rank(&out[j - 1]) > agent_rank(&t) ||
				 (agent_rank(&out[j - 1]) == agent_rank(&t) && out[j - 1].idle > t.idle))) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = t;
	}
	return k;
}

static void on_sig(int sig)
{
	(void)sig;
	quit = 1;
}

/* ---- text layout, by terminal columns (Korean and emoji are wide) ---- */

static int cp_len(unsigned char c)
{
	return c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
}

static int cp_width(const unsigned char *s, int len)
{
	unsigned cp = 0;

	if (len == 1)
		return 1;
	if (len == 2)
		cp = ((s[0] & 0x1fu) << 6) | (s[1] & 0x3fu);
	else if (len == 3)
		cp = ((s[0] & 0x0fu) << 12) | ((s[1] & 0x3fu) << 6) | (s[2] & 0x3fu);
	else
		cp = ((s[0] & 0x07u) << 18) | ((s[1] & 0x3fu) << 12) | ((s[2] & 0x3fu) << 6) |
		     (s[3] & 0x3fu);
	if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) ||
	    (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
	    (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
	    (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff))
		return 2;
	if (cp >= 0x0300 && cp <= 0x036f)
		return 0;
	return 1;
}

/* Bytes of s that fit in `cols` columns; *used gets the columns taken. */
static size_t fit(const char *s, int cols, int *used)
{
	size_t i = 0;
	int w = 0;

	while (s[i]) {
		int l, cw;

		if (s[i] == '\033') {	/* colour codes take no columns */
			i++;
			while (s[i] && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
				i++;
			if (s[i])
				i++;
			continue;
		}
		l = cp_len((unsigned char)s[i]);

		cw = cp_width((const unsigned char *)s + i, l);
		if (w + cw > cols)
			break;
		w += cw;
		i += (size_t)l;
	}
	if (used)
		*used = w;
	return i;
}

static int text_width(const char *s)
{
	int w;

	fit(s, 1 << 20, &w);
	return w;
}

/* ---- data ---- */

typedef struct {
	char *buf;
	rec_t *recs;
	size_t n;
	agent_info_t agents[MAX_AGENTS];	/* from status_load: refreshed every second */
	int nagents;
	char stopped[MAX_AGENTS][128];
	int nstopped;
	int stop_all;
} view_t;

static void view_free(view_t *v)
{
	free(v->buf);
	free(v->recs);
	memset(v, 0, sizeof(*v));
}

static char *slurp(const char *name)
{
	char path[4200], *b;
	FILE *f;
	long n;

	path_in_dir(path, sizeof(path), name);
	f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	b = malloc((size_t)n + 1);
	if (b) {
		n = (long)fread(b, 1, (size_t)n, f);
		b[n] = '\0';
	}
	fclose(f);
	return b;
}

static void view_load(view_t *v)
{
	char *p, *stops;
	size_t cap = 0;

	view_free(v);
	v->buf = slurp("feed.tsv");
	for (p = v->buf; p && *p;) {
		char *nl = strchr(p, '\n');
		rec_t r;

		if (!nl)
			break;	/* a writer is mid-line */
		*nl = '\0';
		if (parse_rec(p, &r)) {
			if (v->n == cap) {
				cap = cap ? cap * 2 : 64;
				v->recs = realloc(v->recs, cap * sizeof(*v->recs));
				if (!v->recs)
					return;
			}
			v->recs[v->n++] = r;
		}
		p = nl + 1;
	}

	stops = slurp("stop");
	for (p = stops; p && *p;) {
		char *nl = strchr(p, '\n'), *who, *why;

		if (nl)
			*nl = '\0';
		who = strchr(p, '\t');
		if (who) {
			who++;
			why = strchr(who, '\t');
			if (why)
				*why = '\0';
			if (strcmp(who, "*") == 0)
				v->stop_all = 1;
			else if (v->nstopped < MAX_AGENTS)
				snprintf(v->stopped[v->nstopped++], 128, "%s", who);
		}
		if (!nl)
			break;
		p = nl + 1;
	}
	free(stops);
	v->nagents = load_agents(v->agents);
}

static int is_stopped(const view_t *v, const char *name)
{
	if (v->stop_all)
		return 1;
	for (int i = 0; i < v->nstopped; i++)
		if (strcmp(v->stopped[i], name) == 0)
			return 1;
	return 0;
}

/* ---- drawing ---- */

typedef struct {
	char *s;
	size_t len, cap;
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void put(out_t *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	for (;;) {
		va_start(ap, fmt);
		n = vsnprintf(o->s + o->len, o->cap - o->len, fmt, ap);
		va_end(ap);
		if (n >= 0 && o->len + (size_t)n < o->cap) {
			o->len += (size_t)n;
			return;
		}
		o->cap = o->cap ? o->cap * 2 : 8192;
		o->s = realloc(o->s, o->cap);
		if (!o->s)
			exit(1);
	}
}

static int agent_color(const char *name)
{
	unsigned h = 0;

	for (; *name; name++)
		h = h * 31 + (unsigned char)*name;
	return 31 + (int)(h % 6);
}

static const char *status_label(const char *status)
{
	if (strcmp(status, "created") == 0)
		return "생성됨";
	if (strcmp(status, "updated") == 0)
		return "업데이트됨";
	if (strcmp(status, "deleted") == 0)
		return "삭제됨";
	if (strcmp(status, "mixed") == 0)
		return "변경됨";
	if (strcmp(status, "final") == 0)
		return "완료 보고";
	return "변경됨";
}

/* One line, clipped to the terminal width, cleared to the end. */
static void line_clipped(out_t *o, const char *s, int cols)
{
	size_t n = fit(s, cols, NULL);

	put(o, "%.*s\033[0m\033[K\r\n", (int)n, s);
}

static void draw(const view_t *v, int sel, const char *msg, const perm_t *ask)
{
	struct winsize ws = { 24, 80, 0, 0 };
	out_t o = { 0 };
	int rows, cols, used = 0, feed_rows;
	size_t start;
	char buf[2048];

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
	rows = ws.ws_row > 6 ? ws.ws_row : 24;
	cols = ws.ws_col > 20 ? ws.ws_col : 80;

	put(&o, "\033[H");
	snprintf(buf, sizeof(buf), "\033[1mcait\033[0m  에이전트 %d  보고 %zu%s", v->nagents, v->n,
		 v->stop_all ? "  \033[31;1m[전체 중단 요청됨]\033[0m" : "");
	line_clipped(&o, buf, cols);
	used++;

	if (ask) {
		snprintf(buf, sizeof(buf), "\033[43;30;1m 승인 요청 \033[0m \033[1m%s\033[0m  %s %s  \033[2m(%s)\033[0m    \033[1my\033[0m 허용  \033[1mn\033[0m 거부",
			 ask->agent, ask->tool, ask->detail, ask->why);
		line_clipped(&o, buf, cols);
		used++;
	}
	if (v->nagents == 0) {
		line_clipped(&o, "\033[2m아직 보고가 없습니다.\033[0m", cols);
		used++;
	}
	for (int i = 0; i < v->nagents; i++) {
		const agent_info_t *a = &v->agents[i];
		const char *col = "32", *label = "진행 중";
		char what[1100], when[32] = "";

		if (used >= rows - 4)
			break;
		const perm_t *wp = waiting_for(a->name);

		if (wp) {
			col = "31;1";
			label = "⚠ 승인 대기";
		} else if (strcmp(a->state, "quiet") == 0) {
			col = "33";
			label = "잠시 멈춤";
		} else if (strcmp(a->state, "done") == 0) {
			col = a->final && !a->unreported ? "2" : "33";
			label = a->unreported || !a->final ? "확인 필요" : "완료";
		} else if (strcmp(a->state, "stopped") == 0 || is_stopped(v, a->name)) {
			col = "31";
			label = "중단됨";
		} else if (strcmp(a->state, "reported") == 0) {
			col = "2";
			label = "보고만";
		}
		if (a->unreported)
			snprintf(when, sizeof(when), "  ⚠ 미보고 수정 %d", a->unreported);
		else if (strcmp(a->state, "working") == 0 || strcmp(a->state, "quiet") == 0)
			snprintf(when, sizeof(when), "  %lds 전", a->idle);
		if (wp)
			snprintf(what, sizeof(what), "%s %s  (%s)", wp->tool, wp->detail, wp->why);
		else if (*a->tool && strcmp(a->state, "done") != 0)
			activity_label(a->tool, a->target, what, sizeof(what));
		else if (a->unreported)
			snprintf(what, sizeof(what), "마지막 보고 뒤 수정 %d건이 보고되지 않았습니다", a->unreported);
		else if (strcmp(a->state, "done") == 0 && !a->final)
			snprintf(what, sizeof(what), "끝났지만 완료 보고가 오지 않았습니다");
		else
			snprintf(what, sizeof(what), "%s", a->text);
		snprintf(buf, sizeof(buf), "%s\033[%dm%s\033[0m  \033[%sm%s\033[0m  보고 %d건%s  \033[2m%s",
			 i == sel ? "\033[7m▸ " : "  ", agent_color(a->name), a->name, col, label, a->reports,
			 when, what);
		line_clipped(&o, buf, cols);
		used++;
	}
	line_clipped(&o, "\033[2m" "──────────────────────────────────────", cols);
	used++;

	/* Newest reports last; fill what is left of the screen from the bottom. */
	feed_rows = rows - used - 1;
	start = v->n;
	for (int left = feed_rows; start > 0;) {
		const rec_t *r = &v->recs[start - 1];
		int need = 1 + (text_width(r->text) > cols - 4 ? 2 : 1);

		if (left < need)
			break;
		left -= need;
		start--;
	}
	for (size_t i = start; i < v->n; i++) {
		const rec_t *r = &v->recs[i];
		struct tm tm;
		char hhmm[16], file[1024] = "", counts[64] = "";
		const char *first = r->files, *sep = strchr(first, FILE_SEP);
		int nf = 1;
		size_t k;

		localtime_r(&r->time, &tm);
		strftime(hhmm, sizeof(hhmm), "%H:%M:%S", &tm);
		for (const char *q = r->files; (q = strchr(q, FILE_SEP)); q++)
			nf++;
		if (nf > 1)
			snprintf(file, sizeof(file), "%d개 파일", nf);
		else
			snprintf(file, sizeof(file), "%.*s", (int)(sep ? (size_t)(sep - first) : strlen(first)),
				 first);
		/* keep the end of a long path: the file name matters most */
		if (text_width(file) > cols / 2) {
			const char *t = file + strlen(file);
			int w = 0;

			while (t > file && w < cols / 2 - 2) {
				do
					t--;
				while (t > file && ((unsigned char)*t & 0xc0) == 0x80);
				w += cp_width((const unsigned char *)t, cp_len((unsigned char)*t));
			}
			memmove(file + 4, t, strlen(t) + 1);
			memcpy(file, "…", 3);
			file[3] = ' ';
			memmove(file + 3, file + 3, strlen(file + 3) + 1);
		}
		if (r->added >= 0 && r->removed >= 0)
			snprintf(counts, sizeof(counts), "  \033[32m+%ld\033[0m \033[31m−%ld\033[0m", r->added,
				 r->removed);
		snprintf(buf, sizeof(buf), "\033[2m%s\033[0m  \033[%dm%s\033[0m  %s %s%s", hhmm,
			 agent_color(r->agent), r->agent, status_label(r->status), file, counts);
		line_clipped(&o, buf, cols);

		/* summary, wrapped to at most two lines */
		const char *t = r->text;
		for (int ln = 0; *t && ln < 2; ln++) {
			int w = 0;

			k = fit(t, cols - 4, &w);
			if (ln == 1 && t[k])
				k = fit(t, cols - 5, &w);
			snprintf(buf, sizeof(buf), "    %.*s%s", (int)k, t, ln == 1 && t[k] ? "…" : "");
			line_clipped(&o, buf, cols);
			t += k;
		}
	}
	put(&o, "\033[J");

	put(&o, "\033[%d;1H\033[7m", rows);
	snprintf(buf, sizeof(buf), " ↑↓ 선택  s 중단  c 해제  x 치우기  X 끝난 것 모두  h 치운 것 보기  q 종료   %s", msg);
	{
		size_t n = fit(buf, cols, &used);

		put(&o, "%.*s%*s\033[0m", (int)n, buf, cols - used, "");
	}
	fwrite(o.s, 1, o.len, stdout);
	fflush(stdout);
	free(o.s);
}

int cmd_panel(int argc, char **argv)
{
	struct termios saved, raw;
	view_t v = { 0 };
	int sel = 0, dirty = 1;
	char msg[256] = "";
	time_t msg_at = 0;
	struct stat prev_feed = { 0 }, prev_stop = { 0 };
	time_t agents_at = 0;
	perm_t pending[4];
	int npending = 0;

	(void)argv;
	if (argc > 0) {
		fprintf(stderr, "usage: cait panel\n");
		return 1;
	}
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "cait: panel needs a terminal\n");
		return 1;
	}
	ensure_dir();
	tcgetattr(STDIN_FILENO, &saved);
	raw = saved;
	raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGWINCH, SIG_IGN);
	printf("\033[?1049h\033[?25l");

	while (!quit) {
		struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
		struct stat now_feed = { 0 }, now_stop = { 0 };
		char path[4200], c;

		path_in_dir(path, sizeof(path), "feed.tsv");
		stat(path, &now_feed);
		path_in_dir(path, sizeof(path), "stop");
		stat(path, &now_stop);
		if (now_feed.st_size != prev_feed.st_size || now_feed.st_mtime != prev_feed.st_mtime ||
		    now_stop.st_size != prev_stop.st_size || now_stop.st_mtime != prev_stop.st_mtime ||
		    (*msg && time(NULL) - msg_at > 3)) {
			if (*msg && time(NULL) - msg_at > 3)
				*msg = '\0';
			prev_feed = now_feed;
			prev_stop = now_stop;
			view_load(&v);
			dirty = 1;
		}
		if (sel >= v.nagents)
			sel = v.nagents ? v.nagents - 1 : 0;
		/* what agents are doing changes without touching the feed: re-read every second */
		if (time(NULL) != agents_at) {
			agents_at = time(NULL);
			v.nagents = load_agents(v.agents);
			npending = perm_load(pending, 4);
		}
		/* redraw each tick: cheap, and it follows a resized window */
		draw(&v, sel, msg, npending ? &pending[0] : NULL);
		dirty = 0;

		if (poll(&p, 1, 200) > 0 && read(STDIN_FILENO, &c, 1) == 1) {
			if (c == 'q') {
				break;
			} else if (c == 'j' && sel + 1 < v.nagents) {
				sel++;
			} else if (c == 'k' && sel > 0) {
				sel--;
			} else if (c == '\033') {
				char seq[2];

				if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1 &&
				    seq[0] == '[') {
					if (seq[1] == 'B' && sel + 1 < v.nagents)
						sel++;
					else if (seq[1] == 'A' && sel > 0)
						sel--;
				}
			} else if ((c == 'y' || c == 'n') && npending) {
				perm_answer(pending[0].id, c == 'y', "");
				snprintf(msg, sizeof(msg), "%s: %s", pending[0].agent, c == 'y' ? "허용함" : "거부함");
				msg_at = time(NULL);
				npending = perm_load(pending, 4);
			} else if (c == 'x' || c == 'X') {
				int running = 0, n = c == 'X' ? forget_agents(NULL, &running)
							      : v.nagents ? forget_agents(v.agents[sel].name, &running) : 0;

				snprintf(msg, sizeof(msg), running ? "아직 실행 중이라 치울 수 없습니다" : "%d개 치움 (기록은 남음)", n);
				msg_at = time(NULL);
				v.nagents = load_agents(v.agents);
			} else if (c == 'h') {
				show_hidden = !show_hidden;
				snprintf(msg, sizeof(msg), show_hidden ? "치운 에이전트도 표시" : "치운 에이전트 숨김");
				msg_at = time(NULL);
				v.nagents = load_agents(v.agents);
			} else if (c == 's') {
				if (v.nagents && v.agents[sel].reports == 0) {
					snprintf(msg, sizeof(msg), "첫 보고 전이라 이름으로 중단할 수 없습니다");
				} else if (v.nagents) {
					stop_add(v.agents[sel].name, "stopped from panel");
					snprintf(msg, sizeof(msg), "%s 중단 요청", v.agents[sel].name);
				} else {
					snprintf(msg, sizeof(msg), "보고한 에이전트가 없습니다");
				}
				msg_at = time(NULL);
				memset(&prev_stop, 0, sizeof(prev_stop));
			} else if (c == 'c') {
				path_in_dir(path, sizeof(path), "stop");
				unlink(path);
				snprintf(msg, sizeof(msg), "중단 요청 해제");
				msg_at = time(NULL);
				memset(&prev_stop, 0, sizeof(prev_stop));
			}
		}
	}
	(void)dirty;
	printf("\033[?25h\033[?1049l");
	tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	view_free(&v);
	return 0;
}
