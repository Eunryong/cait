/* reading the feed: cait watch and cait log */
#include "internal.h"

/* ---- reading the feed: watch, log ---- */


/* Split one feed line in place; returns 0 if it is malformed. */
int parse_rec(char *line, rec_t *r)
{
	char *field[7];
	char *p = line;
	size_t len;

	for (int i = 0; i < 7; i++) {
		field[i] = p;
		if (i < 6) {
			p = strchr(p, '\t');
			if (!p)
				return 0;
			*p++ = '\0';
		}
	}
	len = strlen(field[6]);
	if (len && field[6][len - 1] == '\n')
		field[6][len - 1] = '\0';

	r->time = (time_t)atol(field[0]);
	r->agent = field[1];
	r->files = field[2];
	r->status = field[3];
	r->added = field[4][0] ? atol(field[4]) : -1;
	r->removed = field[5][0] ? atol(field[5]) : -1;
	r->text = field[6];
	return 1;
}

static int count_files(const char *files)
{
	int n = 1;

	for (; *files; files++)
		if (*files == FILE_SEP)
			n++;
	return n;
}

static const char *status_label(const char *status)
{
	if (strcmp(status, "created") == 0)
		return "\xec\x83\x9d\xec\x84\xb1\xeb\x90\xa8";			/* 생성됨 */
	if (strcmp(status, "updated") == 0)
		return "\xec\x97\x85\xeb\x8d\xb0\xec\x9d\xb4\xed\x8a\xb8\xeb\x90\xa8";	/* 업데이트됨 */
	if (strcmp(status, "deleted") == 0)
		return "\xec\x82\xad\xec\xa0\x9c\xeb\x90\xa8";			/* 삭제됨 */
	if (strcmp(status, "reverted") == 0)
		return "\xeb\x90\x98\xeb\x8f\x8c\xeb\xa6\xbc";			/* 되돌림 */
	if (strcmp(status, "final") == 0)
		return "\xec\x99\x84\xeb\xa3\x8c \xeb\xb3\xb4\xea\xb3\xa0";		/* 완료 보고 */
	return "\xeb\xb3\x80\xea\xb2\xbd\xeb\x90\xa8";				/* 변경됨 */
}

/* Show a path relative to the current directory when it lives under it. */
static const char *short_path(const char *file, const char *cwd, size_t cwdlen)
{
	if (cwdlen && strncmp(file, cwd, cwdlen) == 0 && file[cwdlen] == '/')
		return file + cwdlen + 1;
	return file;
}

static void notify(const char *title, const char *body)
{
	char script[8192];
	size_t k = 0;

	/* Quote for an AppleScript string literal; no shell is involved. */
	k += (size_t)snprintf(script, sizeof(script), "display notification \"");
	for (const char *s = body; *s && k < sizeof(script) - 128; s++) {
		if (*s == '"' || *s == '\\')
			script[k++] = '\\';
		script[k++] = *s;
	}
	k += (size_t)snprintf(script + k, sizeof(script) - k, "\" with title \"");
	for (const char *s = title; *s && k < sizeof(script) - 8; s++) {
		if (*s == '"' || *s == '\\')
			script[k++] = '\\';
		script[k++] = *s;
	}
	snprintf(script + k, sizeof(script) - k, "\"");

	if (fork() == 0) {
		execlp("osascript", "osascript", "-e", script, (char *)NULL);
		_exit(127);
	}
}

static const char *agent_color(const char *agent)
{
	static const char *const palette[] = {
		"\033[36m", "\033[33m", "\033[35m", "\033[32m", "\033[34m", "\033[31m",
	};
	unsigned h = 5381;

	for (; *agent; agent++)
		h = h * 33 + (unsigned char)*agent;
	return palette[h % 6];
}

static void show_rec(const rec_t *r, int color, int do_notify)
{
	char cwd[4096] = "", hms[16], title[512];
	struct tm tm;
	const char *dim = color ? "\033[2m" : "", *reset = color ? "\033[0m" : "";
	const char *green = color ? "\033[32m" : "", *red = color ? "\033[31m" : "";
	const char *ac = color ? agent_color(r->agent) : "";
	int nfiles = count_files(r->files);
	size_t cwdlen;

	if (!getcwd(cwd, sizeof(cwd)))
		cwd[0] = '\0';
	cwdlen = strlen(cwd);
	localtime_r(&r->time, &tm);
	strftime(hms, sizeof(hms), "%H:%M:%S", &tm);

	printf("%s%s%s  %s%s%s  ", dim, hms, reset, ac, r->agent, reset);
	if (strcmp(r->status, "final") == 0) {
		printf("%s", status_label(r->status));
	} else if (nfiles == 1) {
		printf("%s %s", status_label(r->status), short_path(r->files, cwd, cwdlen));
	} else {
		printf("%d\xea\xb0\x9c \xed\x8c\x8c\xec\x9d\xbc", nfiles);	/* N개 파일 */
	}
	if (r->added >= 0)
		printf("  %s+%ld%s", green, r->added, reset);
	if (r->removed >= 0)
		printf(" %s\xe2\x88\x92%ld%s", red, r->removed, reset);
	printf("\n");
	if (nfiles > 1) {
		const char *s = r->files;

		while (*s) {
			const char *e = strchr(s, FILE_SEP);
			int len = e ? (int)(e - s) : (int)strlen(s);
			char one[1024];

			snprintf(one, sizeof(one), "%.*s", len, s);
			printf("            %s\n", short_path(one, cwd, cwdlen));
			if (!e)
				break;
			s = e + 1;
		}
	}
	printf("            %s\n", r->text);
	fflush(stdout);

	if (do_notify) {
		if (nfiles == 1)
			snprintf(title, sizeof(title), "%s \xe2\x80\xa2 %s", r->agent,
				 short_path(r->files, cwd, cwdlen));
		else
			snprintf(title, sizeof(title), "%s \xe2\x80\xa2 %d\xea\xb0\x9c \xed\x8c\x8c\xec\x9d\xbc",
				 r->agent, nfiles);
		notify(title, r->text);
	}
}

static volatile sig_atomic_t quit_watch;

static void on_int(int sig)
{
	(void)sig;
	quit_watch = 1;
}

int cmd_watch(int argc, char **argv)
{
	struct termios saved, raw;
	int keys = 0;
	char last_agent[256] = "";
	int do_notify = 0, once = 0, color = isatty(STDOUT_FILENO);
	char path[4200], *line = NULL;
	size_t cap = 0;
	long total = 0, skip;
	FILE *f;
	int fd;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--notify") == 0)
			do_notify = 1;
		else if (strcmp(argv[i], "--once") == 0)
			once = 1;
		else {
			fprintf(stderr, "usage: cait watch [--notify] [--once]\n");
			return 1;
		}
	}
	signal(SIGCHLD, SIG_IGN);
	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);

	if (ensure_dir() < 0) {
		perror("cait: cannot create feed directory");
		return 1;
	}
	/* Single keys stop agents; only when a person is typing at a terminal. */
	if (!once && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
		raw = saved;
		raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
		keys = 1;
	}
	path_in_dir(path, sizeof(path), "feed.tsv");
	fd = open(path, O_RDONLY | O_CREAT, 0644);
	f = fd >= 0 ? fdopen(fd, "r") : NULL;
	if (!f) {
		perror("cait: cannot open feed");
		return 1;
	}

	/* Start with the most recent handful, then follow. */
	while (getline(&line, &cap, f) > 0)
		total++;
	skip = total > 10 ? total - 10 : 0;
	rewind(f);
	for (long i = 0; i < skip && getline(&line, &cap, f) > 0; i++)
		;

	if (!once)
		fprintf(stderr, "cait: watching %s (level: %s). Ctrl-C to quit.%s\n", path,
			level_names[read_level()],
			keys ? "\n  keys: s = stop the latest agent, c = clear stops" : "");
	while (!quit_watch) {
		ssize_t n;

		while ((n = getline(&line, &cap, f)) > 0) {
			rec_t r;

			if (line[n - 1] != '\n') {
				/* Writer is mid-line: retry from its start. */
				fseek(f, -(long)n, SEEK_CUR);
				break;
			}
			if (parse_rec(line, &r)) {
				show_rec(&r, color, do_notify);
				snprintf(last_agent, sizeof(last_agent), "%s", r.agent);
			}
		}
		if (once)
			break;
		clearerr(f);
		if (keys) {
			struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
			char c;

			/* Wake on a key press or after 200ms, whichever is first. */
			if (poll(&p, 1, 200) > 0 && read(STDIN_FILENO, &c, 1) == 1) {
				char tab[4200];

				if (c == 's' && *last_agent) {
					stop_add(last_agent, "stopped from watch");
					fprintf(stderr, "cait: stop requested for %s\n", last_agent);
				} else if (c == 's') {
					fprintf(stderr, "cait: no agent has reported yet\n");
				} else if (c == 'c') {
					path_in_dir(tab, sizeof(tab), "stop");
					unlink(tab);
					fprintf(stderr, "cait: stop requests cleared\n");
				}
			}
		} else {
			usleep(200000);
		}
	}
	if (keys)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	free(line);
	fclose(f);
	return 0;
}

void put_json_rec(FILE *o, const rec_t *r)
{
	const char *s = r->files;

	char key[24];

	rec_key(r->time, r->agent, r->files, key, sizeof(key));
	fprintf(o, "{\"id\":\"%s\",\"time\":%ld,\"agent\":", key, (long)r->time);
	put_json_string(o, r->agent, strlen(r->agent));
	fprintf(o, ",\"files\":[");
	for (; *s;) {
		const char *e = strchr(s, FILE_SEP);

		put_json_string(o, s, e ? (size_t)(e - s) : strlen(s));
		if (!e)
			break;
		fputc(',', o);
		s = e + 1;
	}
	fprintf(o, "],\"status\":\"%s\",\"added\":", r->status);
	if (r->added >= 0)
		fprintf(o, "%ld", r->added);
	else
		fprintf(o, "null");
	fprintf(o, ",\"removed\":");
	if (r->removed >= 0)
		fprintf(o, "%ld", r->removed);
	else
		fprintf(o, "null");
	fprintf(o, ",\"text\":");
	put_json_string(o, r->text, strlen(r->text));
	fprintf(o, "}\n");
}

int cmd_log(int argc, char **argv)
{
	int json = 0, last = 20;
	const char *only = NULL;
	char path[4200], *line = NULL, **keep = NULL;
	size_t cap = 0, nkeep = 0, kcap = 0;
	FILE *f;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0)
			json = 1;
		else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (strcmp(argv[i], "--last") == 0 && i + 1 < argc)
			last = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: cait log [--json] [--agent NAME] [--last N]\n");
			return 1;
		}
	}
	path_in_dir(path, sizeof(path), "feed.tsv");
	f = fopen(path, "r");
	if (!f)
		return 0;	/* nothing reported yet */

	/* Keep every matching line, then print the last N of them. */
	while (getline(&line, &cap, f) > 0) {
		char *copy = strdup(line), *probe = strdup(line);
		rec_t r;

		if (!copy || !probe)
			break;
		if (parse_rec(probe, &r) && (!only || strcmp(r.agent, only) == 0)) {
			if (nkeep == kcap) {
				kcap = kcap ? kcap * 2 : 64;
				keep = realloc(keep, kcap * sizeof(*keep));
				if (!keep)
					return 1;
			}
			keep[nkeep++] = copy;
		} else {
			free(copy);
		}
		free(probe);
	}
	free(line);
	fclose(f);

	for (size_t i = last > 0 && nkeep > (size_t)last ? nkeep - (size_t)last : 0; i < nkeep; i++) {
		rec_t r;

		if (parse_rec(keep[i], &r)) {
			if (json)
				put_json_rec(stdout, &r);
			else
				show_rec(&r, 0, 0);
		}
	}
	for (size_t i = 0; i < nkeep; i++)
		free(keep[i]);
	free(keep);
	return 0;
}
