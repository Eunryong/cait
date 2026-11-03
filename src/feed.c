/*
 * The feed is a plain file, <dir>/feed.tsv, one report per line:
 *
 *   epoch \t agent \t files \t status \t added \t removed \t text
 *
 * A report is about changed files: one file, or a group of related files
 * (joined by 0x1f in the files field). status/added/removed are measured by
 * cait itself from git, never taken from the agent: status is created,
 * updated, deleted or mixed; the counts are lines against HEAD, summed over
 * the group. They stay empty outside a git repository. Only the text is the
 * agent's own words.
 *
 * <dir> is $CAIT_DIR, else ~/.cait. It does not depend on the working
 * directory, so every agent and every watch sees the same feed.
 *
 * The main agent chooses the unit of reporting with `cait level`: "files"
 * (one report per file) or "group" (related files bundled into one report).
 * Whoever shows the feed (watch, log, a panel, the main agent) just reads
 * the file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cait.h"

#define FILE_SEP '\x1f'
#define MAX_FILES 32

enum { LV_FILES, LV_GROUP };
static const char *const level_names[] = { "files", "group" };
static const char *const level_help[] = {
	"one report per file",
	"related files bundled into one report (repeat --file)",
};

static void cait_dir(char *out, size_t size)
{
	const char *dir = getenv("CAIT_DIR");
	const char *home = getenv("HOME");

	/*
	 * Never derive this from the current directory: a subagent that cds
	 * into its repo would then write a feed the user is not watching.
	 */
	if (dir && *dir)
		snprintf(out, size, "%s", dir);
	else
		snprintf(out, size, "%s/.cait", home && *home ? home : "/tmp");
}

void path_in_dir(char *out, size_t size, const char *name)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/%s", dir, name);
}

int ensure_dir(void)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	return mkdir(dir, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static int read_level(void)
{
	char path[4200], word[32] = "";
	FILE *f;

	path_in_dir(path, sizeof(path), "level");
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%31s", word) != 1)
			word[0] = '\0';
		fclose(f);
	}
	return strcmp(word, "group") == 0 ? LV_GROUP : LV_FILES;
}

/* Tabs, newlines and our separator would break the line format. */
static void flatten(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r' || *s == FILE_SEP)
			*s = ' ';
}

/* ---- measuring a change with git ---- */

typedef struct {
	char status[16];	/* "" when unknown */
	long added, removed;	/* -1 when unknown */
} change_t;

/* Run argv without a shell; capture stdout. Returns exit status or -1. */
static int run(char *const argv[], char *out, size_t cap)
{
	int fds[2];
	pid_t pid;
	size_t len = 0;
	int status;

	if (out && cap)
		out[0] = '\0';
	if (pipe(fds) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		dup2(fds[1], STDOUT_FILENO);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
			dup2(null, STDIN_FILENO);
		}
		close(fds[0]);
		close(fds[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		char tmp[1024];
		ssize_t n = read(fds[0], tmp, sizeof(tmp));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (out && len + 1 < cap) {
			size_t take = (size_t)n < cap - len - 1 ? (size_t)n : cap - len - 1;
			memcpy(out + len, tmp, take);
			len += take;
		}
	}
	close(fds[0]);
	if (out && cap)
		out[len] = '\0';
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static long count_lines(const char *path)
{
	FILE *f = fopen(path, "r");
	long n = 0;
	int c, last = '\n';

	if (!f)
		return -1;
	while ((c = fgetc(f)) != EOF) {
		if (c == '\n')
			n++;
		last = c;
	}
	if (last != '\n')
		n++;
	fclose(f);
	return n;
}

static void measure_change(const char *file, change_t *c)
{
	char dir[1024], base[1024], out[4096];
	const char *slash = strrchr(file, '/');
	int exists = access(file, F_OK) == 0;

	c->status[0] = '\0';
	c->added = c->removed = -1;

	if (slash && slash != file) {
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - file), file);
		snprintf(base, sizeof(base), "%s", slash + 1);
	} else {
		snprintf(dir, sizeof(dir), "%s", slash ? "/" : ".");
		snprintf(base, sizeof(base), "%s", slash ? slash + 1 : file);
	}

	char *in_repo[] = { "git", "-C", dir, "rev-parse", "--is-inside-work-tree", NULL };
	char *tracked[] = { "git", "-C", dir, "ls-files", "--error-unmatch", "--", base, NULL };
	char *head[] = { "git", "-C", dir, "rev-parse", "-q", "--verify", "HEAD", NULL };
	char *numstat[] = { "git", "-C", dir, "diff", "--numstat", "HEAD", "--", base, NULL };

	if (run(in_repo, NULL, 0) != 0)
		return;
	if (run(tracked, NULL, 0) == 0 && run(head, NULL, 0) == 0) {
		long a, d;

		snprintf(c->status, sizeof(c->status), "%s", exists ? "updated" : "deleted");
		run(numstat, out, sizeof(out));
		if (sscanf(out, "%ld\t%ld", &a, &d) == 2) {
			c->added = a;
			c->removed = d;
		} else if (out[0] == '\0') {
			c->added = c->removed = 0;	/* no difference from HEAD */
		}
	} else if (exists) {
		snprintf(c->status, sizeof(c->status), "created");
		c->added = count_lines(file);
		c->removed = 0;
	}
}


/* ---- stop requests: the user asks an agent to halt ---- */

/*
 * <dir>/stop holds one request per line: epoch, agent ("*" = everyone), reason.
 * A subagent learns of it at its next `cait report`; it stays until cleared.
 */

int stop_add(const char *agent, const char *reason)
{
	char path[4200], who[256], why[1024];
	FILE *f;

	if (ensure_dir() < 0)
		return -1;
	snprintf(who, sizeof(who), "%s", agent);
	snprintf(why, sizeof(why), "%s", reason);
	flatten(who);
	flatten(why);
	path_in_dir(path, sizeof(path), "stop");
	f = fopen(path, "a");
	if (!f)
		return -1;
	fprintf(f, "%ld\t%s\t%s\n", (long)time(NULL), who, why);
	fclose(f);
	return 0;
}

/* Returns 1 and fills reason if a request names this agent or everyone. */
static int stop_requested(const char *agent, char *reason, size_t size)
{
	char path[4200], line[2048];
	FILE *f;
	int hit = 0;

	path_in_dir(path, sizeof(path), "stop");
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (!hit && fgets(line, sizeof(line), f)) {
		char *who = strchr(line, '\t'), *why;

		if (!who)
			continue;
		why = strchr(++who, '\t');
		if (!why)
			continue;
		*why++ = '\0';
		why[strcspn(why, "\n")] = '\0';
		if (strcmp(who, "*") == 0 || strcmp(who, agent) == 0) {
			snprintf(reason, size, "%s", why);
			hit = 1;
		}
	}
	fclose(f);
	return hit;
}

int cmd_stop(int argc, char **argv)
{
	const char *agent = "*";
	char reason[1024] = "";
	size_t len = 0;
	char path[4200];

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--clear") == 0) {
			path_in_dir(path, sizeof(path), "stop");
			unlink(path);
			printf("stop requests cleared\n");
			return 0;
		} else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
			agent = argv[++i];
		} else {
			len += (size_t)snprintf(reason + len, sizeof(reason) - len, "%s%s",
						len ? " " : "", argv[i]);
			if (len >= sizeof(reason))
				len = sizeof(reason) - 1;
		}
	}
	if (stop_add(agent, reason) < 0) {
		perror("cait: cannot write stop request");
		return 1;
	}
	printf("stop requested for %s: the agent halts at its next report\n",
	       strcmp(agent, "*") == 0 ? "all agents" : agent);
	return 0;
}

/*
 * PreToolUse hook (Claude Code only): refuses a subagent's tool call while a
 * stop request names it. The hook input is JSON on stdin; only calls from
 * subagents carry "agent_id", so the main agent is never blocked.
 *
 * The hook sees an id, the user names agents by --agent. The two meet when the
 * agent runs `cait report --agent NAME`: that Bash call carries both, so we
 * remember id -> NAME in <dir>/agents. Until an agent has reported once, only
 * a stop-all can reach it.
 */

/* Copy the JSON string value following `"key":` into out; 0 if absent. */
static int json_value(const char *in, const char *key, char *out, size_t size)
{
	char pat[64];
	const char *p;
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(in, pat);
	if (!p)
		return 0;
	p = strchr(p + strlen(pat), ':');
	while (p && *++p == ' ')
		;
	if (!p || *p != '"')
		return 0;
	for (p++; *p && *p != '"' && n + 1 < size; p++)
		out[n++] = *p;
	out[n] = '\0';
	return n > 0;
}

/* The NAME after --agent in a report command, quotes (even escaped) skipped. */
static int report_agent(const char *in, char *out, size_t size)
{
	const char *p = strstr(in, "cait report");
	size_t n = 0;

	if (p)
		p = strstr(p, "--agent");
	if (!p)
		return 0;
	for (p += 7; *p == ' ' || *p == '\\' || *p == '"' || *p == '\''; p++)
		;
	while (*p && *p != ' ' && *p != '"' && *p != '\'' && *p != '\\' && n + 1 < size)
		out[n++] = *p++;
	out[n] = '\0';
	return n > 0;
}

static void agent_remember(const char *id, const char *name)
{
	char path[4200];
	FILE *f;

	if (ensure_dir() < 0)
		return;
	path_in_dir(path, sizeof(path), "agents");
	f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\t%s\n", id, name);
		fclose(f);
	}
}

static int agent_name_of(const char *id, char *name, size_t size)
{
	char path[4200], line[1024];
	FILE *f;
	int found = 0;

	path_in_dir(path, sizeof(path), "agents");
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');

		if (!tab)
			continue;
		*tab++ = '\0';
		tab[strcspn(tab, "\n")] = '\0';
		if (strcmp(line, id) == 0) {
			snprintf(name, size, "%s", tab);
			found = 1;
		}
	}
	fclose(f);
	return found;
}

static void put_json_string(FILE *o, const char *s, size_t len);
static size_t inbox_take(char *out, size_t size, int mark);
static int copy_file(const char *from, const char *to);
static void mkdir_p2(const char *a, const char *b);

/* ---- before-edit snapshots: the hook runs before Edit/Write, so it sees the "before" ---- */

/* Like json_value, but understands \" \\ \n escapes (prompts are full of them). */
static int json_text(const char *in, const char *key, char *out, size_t size)
{
	char pat[64];
	const char *p;
	size_t n = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(in, pat);
	if (!p)
		return 0;
	p = strchr(p + strlen(pat), ':');
	while (p && *++p == ' ')
		;
	if (!p || *p != '"')
		return 0;
	for (p++; *p && *p != '"' && n + 1 < size; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			out[n++] = *p == 'n' ? '\n' : *p == 't' ? ' ' : *p;
		} else {
			out[n++] = *p;
		}
	}
	out[n] = '\0';
	return n > 0;
}

static void safe_name(char *s)
{
	for (; *s; s++)
		if (*s == '/' || *s == '\\')
			*s = '_';
}

static void pre_path(char *out, size_t size, const char *who, const char *file, const char *suffix)
{
	char dir[4096], w[256];
	unsigned long long h = 1469598103934665603ULL;

	for (const char *p = file; *p; p++)
		h = (h ^ (unsigned char)*p) * 1099511628211ULL;
	cait_dir(dir, sizeof(dir));
	snprintf(w, sizeof(w), "%s", who);
	safe_name(w);
	snprintf(out, size, "%s/pre/%s/%016llx%s", dir, w, h, suffix);
}

/* Keep the file as it is now, unless an earlier edit since the last report already did. */
static void pre_save(const char *who, const char *file)
{
	char dir[4096], root[4200], w[256], p[4600], mk[4600];
	struct stat st;

	if (ensure_dir() < 0)
		return;
	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/pre", dir);
	snprintf(w, sizeof(w), "%s", who);
	safe_name(w);
	mkdir_p2(root, w);
	pre_path(p, sizeof(p), who, file, "");
	pre_path(mk, sizeof(mk), who, file, ".deleted");
	if (access(p, F_OK) == 0 || access(mk, F_OK) == 0)
		return;
	if (stat(file, &st) != 0) {
		FILE *m = fopen(mk, "w");

		if (m)
			fclose(m);
		return;
	}
	if (st.st_size > (1 << 20))
		return;
	copy_file(file, p);
}

/* ---- what the main agent asked each subagent to do ---- */

static void brief_pending(const char *in)
{
	char desc[512] = "", prompt[3000], dir[4096], root[4200], path[4400];
	FILE *f;

	if (!json_text(in, "prompt", prompt, sizeof(prompt)))
		return;
	json_text(in, "description", desc, sizeof(desc));
	flatten(desc);
	if (ensure_dir() < 0)
		return;
	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/brief", dir);
	mkdir_p2(root, "pending");
	snprintf(path, sizeof(path), "%s/pending/%ld-%05d", root, (long)time(NULL), (int)getpid());
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n%s\n", desc, prompt);
		fclose(f);
	}
}

/* An agent just named itself: give it the pending brief that mentions that name, else the oldest. */
static void brief_claim(const char *name)
{
	char dir[4096], pdir[4300], safe[256], dst[4400], path[4400], oldest[256] = "";
	struct dirent *e;
	DIR *d;

	cait_dir(dir, sizeof(dir));
	snprintf(pdir, sizeof(pdir), "%s/brief/pending", dir);
	snprintf(safe, sizeof(safe), "%s", name);
	safe_name(safe);
	snprintf(dst, sizeof(dst), "%s/brief/%s", dir, safe);
	d = opendir(pdir);
	if (!d)
		return;
	while ((e = readdir(d))) {
		char buf[3600];
		size_t m;
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", pdir, e->d_name);
		if (!*oldest || strcmp(e->d_name, oldest) < 0)
			snprintf(oldest, sizeof(oldest), "%s", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		m = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[m] = '\0';
		if (strstr(buf, name)) {
			rename(path, dst);
			closedir(d);
			return;
		}
	}
	closedir(d);
	if (*oldest && access(dst, F_OK) != 0) {
		snprintf(path, sizeof(path), "%s/%s", pdir, oldest);
		rename(path, dst);
	}
}

int cmd_brief(int argc, char **argv)
{
	char dir[4096], path[4400], line[4096];
	const char *agent = NULL;
	FILE *f;

	for (int i = 0; i < argc; i++)
		if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc)
			agent = argv[++i];
	cait_dir(dir, sizeof(dir));
	if (!agent) {
		struct dirent *e;
		DIR *d;

		snprintf(path, sizeof(path), "%s/brief", dir);
		d = opendir(path);
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.' || strcmp(e->d_name, "pending") == 0)
				continue;
			snprintf(path, sizeof(path), "%s/brief/%s", dir, e->d_name);
			f = fopen(path, "r");
			if (f && fgets(line, sizeof(line), f))
				printf("%s\t%s", e->d_name, line);
			if (f)
				fclose(f);
		}
		if (d)
			closedir(d);
		return 0;
	}
	snprintf(line, sizeof(line), "%s", agent);
	safe_name(line);
	snprintf(path, sizeof(path), "%s/brief/%s", dir, line);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cait: no brief recorded for that agent\n");
		return 1;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
	return 0;
}

/* ---- activity: what each subagent is doing, seen by the hook on every tool call ---- */

/* <dir>/act/<agent id>: first \t last \t calls \t done \t unreported edits \t final report \t tool \t target */
static void act_file(char *out, size_t size, const char *id)
{
	char dir[4096], w[256];

	cait_dir(dir, sizeof(dir));
	snprintf(w, sizeof(w), "%s", id);
	safe_name(w);
	snprintf(out, size, "%s/act/%s", dir, w);
}

#define ACT_FIELDS 8

/* Split an act line in place into ACT_FIELDS tab fields. */
static void act_split(char *line, char *p[ACT_FIELDS])
{
	char *c = line;

	for (int i = 0; i < ACT_FIELDS; i++)
		p[i] = NULL;
	for (int i = 0; i < ACT_FIELDS && c; i++) {
		p[i] = c;
		c = strchr(c, '\t');
		if (c)
			*c++ = '\0';
	}
	if (p[ACT_FIELDS - 1])
		p[ACT_FIELDS - 1][strcspn(p[ACT_FIELDS - 1], "\n")] = '\0';
}

/*
 * tool set: one more tool call (an edit counts as unreported until the agent
 * next reports). report: the agent just reported. final: it gave its final report.
 */
static void act_update(const char *id, const char *tool, const char *target, int done, int report,
		       int final)
{
	char dir[4096], root[4200], path[4400], line[2048], t[128] = "", g[512] = "", *p[ACT_FIELDS];
	long first = 0;
	int calls = 0, was_done = 0, unrep = 0, fin = 0;
	time_t now = time(NULL);
	FILE *f;

	if (ensure_dir() < 0)
		return;
	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/act", dir);
	mkdir(root, 0755);
	act_file(path, sizeof(path), id);
	f = fopen(path, "r");
	if (f) {
		if (fgets(line, sizeof(line), f)) {
			act_split(line, p);
			first = p[0] ? atol(p[0]) : 0;
			calls = p[2] ? atoi(p[2]) : 0;
			was_done = p[3] ? atoi(p[3]) : 0;
			unrep = p[4] ? atoi(p[4]) : 0;
			fin = p[5] ? atoi(p[5]) : 0;
			if (p[6])
				snprintf(t, sizeof(t), "%s", p[6]);
			if (p[7])
				snprintf(g, sizeof(g), "%s", p[7]);
		}
		fclose(f);
	}
	if (!first)
		first = (long)now;
	if (done)
		was_done = 1;
	if (tool) {
		calls++;
		if (strcmp(tool, "Edit") == 0 || strcmp(tool, "Write") == 0 || strcmp(tool, "MultiEdit") == 0)
			unrep++;
		snprintf(t, sizeof(t), "%s", tool);
		snprintf(g, sizeof(g), "%s", target ? target : "");
		flatten(t);
		flatten(g);
	}
	if (report)
		unrep = 0;
	if (final)
		fin = 1;
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%ld\t%ld\t%d\t%d\t%d\t%d\t%s\t%s\n", first, (long)now, calls, was_done, unrep, fin, t, g);
		fclose(f);
	}
}

#define MAX_STAT 64

static void json_str_out(FILE *o, const char *s)
{
	put_json_string(o, s, strlen(s));
}

/* Every agent cait knows of, with its state worked out; returns how many. */
int status_load(agent_info_t *st, int max)
{
	int ns = 0;
	char dir[4096], path[4400], line[2048];
	struct dirent *e;
	DIR *d;
	FILE *f;
	time_t now = time(NULL);

	memset(st, 0, sizeof(*st) * (size_t)max);
	cait_dir(dir, sizeof(dir));

	/* agents the hook has seen */
	snprintf(path, sizeof(path), "%s/act", dir);
	d = opendir(path);
	while (d && (e = readdir(d)) && ns < max) {
		agent_info_t *a = &st[ns];
		char *p[ACT_FIELDS];

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/act/%s", dir, e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(line, sizeof(line), f)) {
			fclose(f);
			continue;
		}
		fclose(f);
		act_split(line, p);
		snprintf(a->id, sizeof(a->id), "%s", e->d_name);
		a->first = p[0] ? atol(p[0]) : 0;
		a->last = p[1] ? atol(p[1]) : 0;
		a->calls = p[2] ? atoi(p[2]) : 0;
		a->done = p[3] ? atoi(p[3]) : 0;
		a->unreported = p[4] ? atoi(p[4]) : 0;
		a->final = p[5] ? atoi(p[5]) : 0;
		if (p[6])
			snprintf(a->tool, sizeof(a->tool), "%s", p[6]);
		if (p[7])
			snprintf(a->target, sizeof(a->target), "%s", p[7]);
		if (!agent_name_of(a->id, a->name, sizeof(a->name)))
			snprintf(a->name, sizeof(a->name), "%.8s", a->id);
		a->seen_act = 1;
		ns++;
	}
	if (d)
		closedir(d);

	/* reports from the feed; agents with reports but no hook activity get a row too */
	path_in_dir(path, sizeof(path), "feed.tsv");
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		rec_t r;
		agent_info_t *a = NULL;

		if (!parse_rec(line, &r) || strcmp(r.status, "reverted") == 0)
			continue;
		for (int i = 0; i < ns; i++)
			if (strcmp(st[i].name, r.agent) == 0)
				a = &st[i];
		if (!a && ns < max) {
			a = &st[ns++];
			snprintf(a->name, sizeof(a->name), "%s", r.agent);
		}
		if (!a)
			continue;
		a->reports++;
		if (strcmp(r.status, "final") == 0)
			a->final = 1;
		a->last_report = (long)r.time;
		snprintf(a->text, sizeof(a->text), "%s", r.text);
	}
	if (f)
		fclose(f);

	for (int i = 0; i < ns; i++) {
		agent_info_t *a = &st[i];
		char sname[256], reason[256];
		FILE *b;

		a->idle = (long)now - (a->seen_act ? a->last : a->last_report);
		if (a->idle < 0)
			a->idle = 0;
		snprintf(sname, sizeof(sname), "%s", a->name);
		safe_name(sname);
		snprintf(path, sizeof(path), "%s/brief/%s", dir, sname);
		b = fopen(path, "r");
		if (b) {
			if (fgets(a->task, sizeof(a->task), b))
				a->task[strcspn(a->task, "\n")] = '\0';
			fclose(b);
		}
		if (stop_requested("*", reason, sizeof(reason)) || stop_requested(a->name, reason, sizeof(reason)))
			snprintf(a->state, sizeof(a->state), "stopped");
		else if (a->done)
			snprintf(a->state, sizeof(a->state), "done");
		else if (!a->seen_act)
			snprintf(a->state, sizeof(a->state), "reported");
		else if (a->idle < 20)
			snprintf(a->state, sizeof(a->state), "working");
		else
			snprintf(a->state, sizeof(a->state), "quiet");
	}
	return ns;
}

int cmd_status(int argc, char **argv)
{
	agent_info_t *st = calloc(MAX_STAT, sizeof(*st));
	int ns, json = 0;
	time_t now = time(NULL);

	for (int i = 0; i < argc; i++)
		if (strcmp(argv[i], "--json") == 0)
			json = 1;
	if (!st)
		return 1;
	ns = status_load(st, MAX_STAT);
	for (int i = 0; i < ns; i++) {
		agent_info_t *a = &st[i];

		if (json) {
			printf("{\"agent\":");
			json_str_out(stdout, a->name);
			printf(",\"final_report\":%s,\"unreported_edits\":%d", a->final ? "true" : "false", a->unreported);
			printf(",\"state\":\"%s\",\"reports\":%d,\"calls\":%d,\"idle_seconds\":%ld,\"elapsed_seconds\":%ld,\"last_tool\":",
			       a->state, a->reports, a->calls, a->idle, a->first ? (long)now - a->first : 0L);
			json_str_out(stdout, a->tool);
			printf(",\"last_target\":");
			json_str_out(stdout, a->target);
			printf(",\"task\":");
			json_str_out(stdout, a->task);
			printf(",\"last_report\":");
			json_str_out(stdout, a->text);
			printf("}\n");
			continue;
		}
		printf("%s\t%s\treports %d\ttool calls %d\tidle %lds\n", a->name, a->state, a->reports, a->calls,
		       a->idle);
		if (a->seen_act && a->done)
			printf("  final report: %s\n", a->final ? "delivered" : "NOT delivered");
		if (a->unreported)
			printf("  unreported edits: %d (edited after the last report)\n", a->unreported);
		if (*a->task)
			printf("  task: %s\n", a->task);
		if (*a->tool)
			printf("  now/last: %s %s\n", a->tool, a->target);
		if (*a->text)
			printf("  last report: %s\n", a->text);
	}
	if (ns == 0 && !json)
		printf("no agents seen yet\n");
	free(st);
	return 0;
}

/* ---- permission requests: the hook asks, the user answers on the cait screen ---- */

/* <dir>/perm/<id>.req: epoch \t agent id \t agent name \t tool \t detail
 * <dir>/perm/<id>.ans: allow|deny [\t message] */
static void perm_dir(char *out, size_t size)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/perm", dir);
}

int perm_load(perm_t *out, int max)
{
	char root[4200], path[4500], line[2048];
	struct dirent *e;
	DIR *d;
	int n = 0;

	memset(out, 0, sizeof(*out) * (size_t)max);
	perm_dir(root, sizeof(root));
	d = opendir(root);
	while (d && (e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		FILE *f;
		char *p[5] = { 0 }, *c = line;

		if (l < 5 || strcmp(e->d_name + l - 4, ".req") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%.*s.ans", root, (int)(l - 4), e->d_name);
		if (access(path, F_OK) == 0)
			continue;	/* already answered */
		snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(line, sizeof(line), f)) {
			fclose(f);
			continue;
		}
		fclose(f);
		for (int i = 0; i < 5 && c; i++) {
			p[i] = c;
			c = strchr(c, '\t');
			if (c)
				*c++ = '\0';
		}
		if (p[4])
			p[4][strcspn(p[4], "\n")] = '\0';
		snprintf(out[n].id, sizeof(out[n].id), "%.*s", (int)(l - 4), e->d_name);
		out[n].when = p[0] ? atol(p[0]) : 0;
		if (out[n].when < (long)time(NULL) - 120)
			continue;	/* the hook gave up long ago */
		snprintf(out[n].agent, sizeof(out[n].agent), "%s", p[2] && *p[2] ? p[2] : p[1] ? p[1] : "?");
		snprintf(out[n].tool, sizeof(out[n].tool), "%s", p[3] ? p[3] : "");
		snprintf(out[n].detail, sizeof(out[n].detail), "%s", p[4] ? p[4] : "");
		n++;
	}
	if (d)
		closedir(d);
	return n;
}

int perm_answer(const char *id, int allow, const char *msg)
{
	char root[4200], path[4600], w[128];
	FILE *f;

	snprintf(w, sizeof(w), "%s", id);
	safe_name(w);
	perm_dir(root, sizeof(root));
	snprintf(path, sizeof(path), "%s/%s.req", root, w);
	if (access(path, F_OK) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/%s.ans", root, w);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "%s\t%s\n", allow ? "allow" : "deny", msg ? msg : "");
	fclose(f);
	return 0;
}

int cmd_perm(int argc, char **argv)
{
	perm_t *q = calloc(32, sizeof(*q));
	int n;

	if (!q)
		return 1;
	n = perm_load(q, 32);
	if (argc >= 2 && (strcmp(argv[0], "allow") == 0 || strcmp(argv[0], "deny") == 0)) {
		int rc = perm_answer(argv[1], argv[0][0] == 'a', argc > 2 ? argv[2] : "");

		if (rc < 0)
			fprintf(stderr, "cait: no such pending request: %s\n", argv[1]);
		else
			printf("%s: %s\n", argv[0], argv[1]);
		free(q);
		return rc < 0 ? 1 : 0;
	}
	if (argc > 0 && strcmp(argv[0], "--json") != 0) {
		fprintf(stderr, "usage: cait perm [--json] | allow ID | deny ID [reason]\n");
		free(q);
		return 1;
	}
	for (int i = 0; i < n; i++) {
		if (argc > 0) {
			printf("{\"id\":");
			put_json_string(stdout, q[i].id, strlen(q[i].id));
			printf(",\"agent\":");
			put_json_string(stdout, q[i].agent, strlen(q[i].agent));
			printf(",\"tool\":");
			put_json_string(stdout, q[i].tool, strlen(q[i].tool));
			printf(",\"detail\":");
			put_json_string(stdout, q[i].detail, strlen(q[i].detail));
			printf("}\n");
		} else {
			printf("%s\t%s\t%s %s\n", q[i].id, q[i].agent, q[i].tool, q[i].detail);
		}
	}
	if (n == 0 && argc == 0)
		printf("no pending requests\n");
	free(q);
	return 0;
}

/*
 * PermissionRequest hook: Claude Code is about to ask the user to allow a tool call.
 * Put the question on the cait screen and wait for the answer; with no answer in
 * time, print nothing so the normal prompt appears.
 */
static int perm_hook(const char *in)
{
	char id[128] = "", name[256] = "", tool[64] = "", detail[1024] = "", root[4200], path[4600];
	char rid[64], line[1500], ans[1200];
	const char *w = getenv("CAIT_PERM_WAIT");
	int wait_ms = (w && atoi(w) > 0 ? atoi(w) : 40) * 1000;
	FILE *f;

	json_value(in, "agent_id", id, sizeof(id));
	json_value(in, "tool_name", tool, sizeof(tool));
	if (!json_text(in, "command", detail, sizeof(detail)))
		json_value(in, "file_path", detail, sizeof(detail));
	if (*id && !agent_name_of(id, name, sizeof(name)))
		snprintf(name, sizeof(name), "%.8s", id);
	flatten(tool);
	flatten(detail);
	flatten(name);
	if (ensure_dir() < 0)
		return 0;
	perm_dir(root, sizeof(root));
	mkdir(root, 0755);
	/* keep the last request as the hook got it: handy to see which fields Claude Code sends */
	snprintf(path, sizeof(path), "%s/last-request.json", root);
	f = fopen(path, "w");
	if (f) {
		fputs(in, f);
		fclose(f);
	}
	snprintf(rid, sizeof(rid), "%ld-%ld", (long)time(NULL), (long)getpid());
	snprintf(path, sizeof(path), "%s/%s.req", root, rid);
	f = fopen(path, "w");
	if (!f)
		return 0;
	fprintf(f, "%ld\t%s\t%s\t%s\t%s\n", (long)time(NULL), id, name, tool, detail);
	fclose(f);

	for (int waited = 0; waited < wait_ms; waited += 200) {
		struct timespec ts = { 0, 200 * 1000 * 1000 };
		char *msg;

		snprintf(path, sizeof(path), "%s/%s.ans", root, rid);
		f = fopen(path, "r");
		if (f) {
			ans[0] = '\0';
			if (fgets(ans, sizeof(ans), f)) {
				ans[strcspn(ans, "\n")] = '\0';
				msg = strchr(ans, '\t');
				if (msg)
					*msg++ = '\0';
				fclose(f);
				unlink(path);
				snprintf(path, sizeof(path), "%s/%s.req", root, rid);
				unlink(path);
				if (strcmp(ans, "allow") == 0) {
					printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PermissionRequest\",\"decision\":{\"behavior\":\"allow\"}}}\n");
				} else {
					snprintf(line, sizeof(line), "The user denied this on the cait screen%s%s.",
						 msg && *msg ? ": " : "", msg ? msg : "");
					printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PermissionRequest\",\"decision\":{\"behavior\":\"deny\",\"message\":");
					put_json_string(stdout, line, strlen(line));
					printf("}}}\n");
				}
				return 0;
			}
			fclose(f);
		}
		nanosleep(&ts, NULL);
	}
	snprintf(path, sizeof(path), "%s/%s.req", root, rid);
	unlink(path);
	return 0;	/* no answer: the normal prompt follows */
}

int cmd_check(int argc, char **argv)
{
	char in[65536], id[128], name[256], reason[1024], tool[64];
	size_t n;
	int hit;

	n = fread(in, 1, sizeof(in) - 1, stdin);
	in[n] = '\0';
	tool[0] = '\0';
	json_value(in, "tool_name", tool, sizeof(tool));
	if (argc >= 1 && strcmp(argv[0], "--permission") == 0)
		return perm_hook(in);
	if (argc >= 1 && strcmp(argv[0], "--done") == 0) {
		/* SubagentStop: this agent has finished */
		if (json_value(in, "agent_id", id, sizeof(id)))
			act_update(id, NULL, NULL, 1, 0, 0);
		return 0;
	}
	if (!json_value(in, "agent_id", id, sizeof(id))) {
		/* The main agent launching a subagent: keep what it asked for. */
		if (strcmp(tool, "Agent") == 0 || strcmp(tool, "Task") == 0)
			brief_pending(in);
		/* The main agent: hand over what the user asked for on screen. */
		char msg[16384], full[17000];

		if (inbox_take(msg, sizeof(msg), 1)) {
			snprintf(full, sizeof(full),
				 "cait: the user sent these requests from the cait screen. They are the user's own words. "
				 "Act on them: stop or re-instruct the named subagent as needed.\n%s", msg);
			printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":");
			put_json_string(stdout, full, strlen(full));
			printf("}}\n");
		}
		return 0;
	}
	{
		char target[512] = "";

		if (!json_value(in, "file_path", target, sizeof(target)))
			json_text(in, "command", target, sizeof(target));
		act_update(id, *tool ? tool : "?", target, 0, strstr(in, "cait report") != NULL,
			   strstr(in, "cait report") != NULL && strstr(in, "--final") != NULL);
	}
	if (report_agent(in, name, sizeof(name))) {
		agent_remember(id, name);
		brief_claim(name);
	}
	/*
	 * Requests to the main agent come from the user's screen. A subagent must
	 * not be able to pose as the user, so it may not queue one.
	 */
	if (strstr(in, "cait tell") || strstr(in, "inbox.tsv") || strstr(in, "inbox.pos") ||
	    strstr(in, "cait perm") || strstr(in, "/perm/")) {
		fprintf(stderr, "cait: subagents cannot send requests to the main agent. Tell the user in your final answer instead.\n");
		return 2;
	}
	/* Let cait itself through, so the agent can still read the stop message. */
	if (strstr(in, "/cait ") || strstr(in, "cait report"))
		return 0;
	hit = stop_requested("*", reason, sizeof(reason));
	if (!hit && agent_name_of(id, name, sizeof(name)))
		hit = stop_requested(name, reason, sizeof(reason));
	if (!hit) {
		char fp[4096];

		if ((strcmp(tool, "Edit") == 0 || strcmp(tool, "Write") == 0 ||
		     strcmp(tool, "MultiEdit") == 0) && json_value(in, "file_path", fp, sizeof(fp)))
			pre_save(id, fp);
		return 0;
	}
	fprintf(stderr, "STOP: the user asked you to stop%s%s. This tool call was blocked. "
		"Make no more edits; tell the main agent what is done and what is left.\n",
		*reason ? ": " : "", reason);
	return 2;
}

/* ---- report ids, snapshots, feed append ---- */

/* A report has no id column; derive one from what identifies it. */
void rec_key(time_t t, const char *agent, const char *files, char *out, size_t size)
{
	unsigned long long h = 1469598103934665603ULL;
	char head[64];
	const char *parts[3] = { head, agent, files };

	snprintf(head, sizeof(head), "%ld", (long)t);
	for (int i = 0; i < 3; i++) {
		for (const char *p = parts[i]; *p; p++)
			h = (h ^ (unsigned char)*p) * 1099511628211ULL;
		h = (h ^ 0xff) * 1099511628211ULL;
	}
	snprintf(out, size, "%016llx", h);
}

static void mkdir_p2(const char *a, const char *b)
{
	char path[4200];

	snprintf(path, sizeof(path), "%s", a);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s", a, b);
	mkdir(path, 0755);
}

static int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out;
	char buf[8192];
	size_t n;

	if (!in)
		return -1;
	out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	return fclose(out);
}

/* Directory holding what one report's files looked like right after it. */
static void snap_path(char *out, size_t size, const char *root, const char *key, int idx, const char *suffix)
{
	char dir[4096];

	cait_dir(dir, sizeof(dir));
	snprintf(out, size, "%s/%s/%s/%d%s", dir, root, key, idx, suffix);
}

/*
 * Move the before-edit snapshot the hook took for `file` into this report, as
 * <idx>.before (or <idx>.before.deleted when the file did not exist yet). The
 * hook saved it under the agent's id; the agents file maps ids to the name.
 */
static void pre_take(const char *agent, const char *file, const char *key, int idx)
{
	char path[4200], line[1024], from[4600], mk[4600], to[4400];
	FILE *f;

	path_in_dir(path, sizeof(path), "agents");
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');

		if (!tab)
			continue;
		*tab++ = '\0';
		tab[strcspn(tab, "\n")] = '\0';
		if (strcmp(tab, agent) != 0)
			continue;
		pre_path(from, sizeof(from), line, file, "");
		pre_path(mk, sizeof(mk), line, file, ".deleted");
		if (access(from, F_OK) == 0) {
			snap_path(to, sizeof(to), "snap", key, idx, ".before");
			rename(from, to);
			break;
		}
		if (access(mk, F_OK) == 0) {
			snap_path(to, sizeof(to), "snap", key, idx, ".before.deleted");
			rename(mk, to);
			break;
		}
	}
	fclose(f);
}

static void snap_save(const char *key, int idx, const char *file)
{
	char dir[4096], root[4200], to[4400];
	struct stat st;

	cait_dir(dir, sizeof(dir));
	snprintf(root, sizeof(root), "%s/snap", dir);
	mkdir_p2(root, key);
	if (stat(file, &st) != 0) {
		FILE *m;

		snap_path(to, sizeof(to), "snap", key, idx, ".deleted");
		m = fopen(to, "w");
		if (m)
			fclose(m);
		return;
	}
	if (st.st_size > (1 << 20))
		return;	/* too big to keep: this file cannot be restored from a snapshot */
	snap_path(to, sizeof(to), "snap", key, idx, "");
	copy_file(file, to);
}

static int feed_append(const char *line, size_t n)
{
	char path[4200];
	int fd;
	ssize_t w;

	path_in_dir(path, sizeof(path), "feed.tsv");
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return -1;
	/* Parallel subagents report at once: keep each line whole. */
	flock(fd, LOCK_EX);
	w = write(fd, line, n);
	flock(fd, LOCK_UN);
	close(fd);
	return w == (ssize_t)n ? 0 : -1;
}

/* ---- undo: put a report's files back to how they were before it ---- */

typedef struct {
	char *line;
	rec_t r;
	char key[24];
} frec_t;

static void split_dir_base(const char *file, char *dir, size_t dsize, char *base, size_t bsize)
{
	const char *slash = strrchr(file, '/');

	if (slash && slash != file) {
		snprintf(dir, dsize, "%.*s", (int)(slash - file), file);
		snprintf(base, bsize, "%s", slash + 1);
	} else {
		snprintf(dir, dsize, "%s", slash ? "/" : ".");
		snprintf(base, bsize, "%s", slash ? slash + 1 : file);
	}
}

/* Does the \x1f-joined list `files` contain exactly `file`? Returns its index or -1. */
static int file_index(const char *files, const char *file)
{
	size_t len = strlen(file);
	int idx = 0;

	for (const char *p = files;; idx++) {
		const char *e = strchr(p, FILE_SEP);
		size_t n = e ? (size_t)(e - p) : strlen(p);

		if (n == len && strncmp(p, file, len) == 0)
			return idx;
		if (!e)
			return -1;
		p = e + 1;
	}
}

int cmd_revert(int argc, char **argv)
{
	const char *id = NULL;
	int dry = 0, bad = 0, target = -1;
	frec_t *recs = NULL;
	size_t n = 0, cap = 0, len = 0;
	char path[4200], *line = NULL, dir[4096], files[8192];
	FILE *f;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--id") == 0 && i + 1 < argc)
			id = argv[++i];
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry = 1;
		else
			id = NULL;
	}
	if (!id) {
		fprintf(stderr, "usage: cait revert --id KEY [--dry-run]   (KEY: see `cait log --json`)\n");
		return 1;
	}
	path_in_dir(path, sizeof(path), "feed.tsv");
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cait: nothing has been reported yet\n");
		return 1;
	}
	while (getline(&line, &len, f) > 0) {
		frec_t fr;

		fr.line = strdup(line);
		if (!fr.line || !parse_rec(fr.line, &fr.r))
			continue;
		rec_key(fr.r.time, fr.r.agent, fr.r.files, fr.key, sizeof(fr.key));
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			recs = realloc(recs, cap * sizeof(*recs));
			if (!recs)
				return 1;
		}
		recs[n++] = fr;
	}
	free(line);
	fclose(f);
	for (size_t i = 0; i < n; i++)
		if (strcmp(recs[i].key, id) == 0)
			target = (int)i;
	if (target < 0) {
		fprintf(stderr, "cait: no report with that id\n");
		return 1;
	}
	if (strcmp(recs[target].r.status, "final") == 0) {
		fprintf(stderr, "cait: a final report has no file changes to undo\n");
		return 1;
	}
	if (strcmp(recs[target].r.status, "reverted") == 0) {
		fprintf(stderr, "cait: that entry is already an undo\n");
		return 1;
	}

	cait_dir(dir, sizeof(dir));
	snprintf(files, sizeof(files), "%s", recs[target].r.files);
	{
		char *p = files, *file;
		int idx = 0;

		for (;; idx++) {
			char *e = strchr(p, FILE_SEP), what[256], backup[4400], root[4200], bs[4400], bm[4400];
			int prev = -1, later = 0, prev_idx = -1, ok = 0;

			file = p;
			if (e)
				*e = '\0';
			/* keep what we are about to overwrite, so even an undo can be undone */
			if (!dry && access(file, F_OK) == 0) {
				snprintf(root, sizeof(root), "%s/undo", dir);
				mkdir_p2(root, recs[target].key);
				snprintf(backup, sizeof(backup), "%s/undo/%s/%d", dir, recs[target].key, idx);
				copy_file(file, backup);
			}
			for (int q = 0; q < target; q++) {
				int fi = file_index(recs[q].r.files, file);

				if (fi >= 0 && strcmp(recs[q].r.status, "reverted") != 0) {
					prev = q;
					prev_idx = fi;
				}
			}
			for (size_t q = (size_t)target + 1; q < n; q++)
				if (file_index(recs[q].r.files, file) >= 0 &&
				    strcmp(recs[q].r.status, "reverted") != 0)
					later++;

			snap_path(bs, sizeof(bs), "snap", recs[target].key, idx, ".before");
			snap_path(bm, sizeof(bm), "snap", recs[target].key, idx, ".before.deleted");
			if (access(bm, F_OK) == 0) {
				snprintf(what, sizeof(what), "remove (the file did not exist before this report's edits)");
				ok = !(!dry && unlink(file) != 0 && errno != ENOENT);
			} else if (access(bs, F_OK) == 0) {
				snprintf(what, sizeof(what), "restore the file as it was just before this report's edits");
				ok = dry || copy_file(bs, file) == 0;
			} else if (prev >= 0) {
				char sp[4400], mark[4400];

				snap_path(sp, sizeof(sp), "snap", recs[prev].key, prev_idx, "");
				snap_path(mark, sizeof(mark), "snap", recs[prev].key, prev_idx, ".deleted");
				if (access(mark, F_OK) == 0) {
					snprintf(what, sizeof(what), "remove (the earlier report had deleted it)");
					ok = 1;
					if (!dry && unlink(file) != 0 && errno != ENOENT)
						ok = 0;
				} else if (access(sp, F_OK) == 0) {
					snprintf(what, sizeof(what), "restore the version right after the earlier report");
					ok = 1;
				} else {
					snprintf(what, sizeof(what), "no snapshot of the earlier version");
				}
				if (ok && !dry && access(mark, F_OK) != 0)
					ok = copy_file(sp, file) == 0;
			} else {
				char bdir[1024], base[1024];
				char *in_repo[] = { "git", "-C", bdir, "rev-parse", "--is-inside-work-tree", NULL };
				char *tracked[] = { "git", "-C", bdir, "ls-files", "--error-unmatch", "--", base, NULL };
				char *head[] = { "git", "-C", bdir, "rev-parse", "-q", "--verify", "HEAD", NULL };
				char *checkout[] = { "git", "-C", bdir, "checkout", "HEAD", "--", base, NULL };

				split_dir_base(file, bdir, sizeof(bdir), base, sizeof(base));
				if (run(in_repo, NULL, 0) != 0) {
					snprintf(what, sizeof(what), "cannot undo: no earlier version known (not a git repo)");
				} else if (run(tracked, NULL, 0) == 0 && run(head, NULL, 0) == 0) {
					snprintf(what, sizeof(what), "restore the last committed version");
					ok = 1;
				} else {
					snprintf(what, sizeof(what), "remove (this report created the file)");
					ok = 1;
				}
				if (ok && !dry) {
					if (strncmp(what, "remove", 6) == 0)
						ok = unlink(file) == 0 || errno == ENOENT;
					else
						ok = run(checkout, NULL, 0) == 0;
				}
			}

			printf("%s\t%s%s\t%s\n", ok ? "ok" : "fail", dry ? "[dry-run] " : "", what, file);
			if (later)
				printf("note\t%d later report(s) also changed this file; their changes go too\t%s\n",
				       later, file);
			if (!ok)
				bad = 1;
			if (!e)
				break;
			p = e + 1;
		}
	}
	if (!dry && !bad) {
		char out[16384];
		time_t now = time(NULL);
		int m = snprintf(out, sizeof(out), "%ld\t%s\t%s\treverted\t\t\t%s\n", (long)now,
				 recs[target].r.agent, recs[target].r.files,
				 "사용자가 이 보고의 변경을 되돌렸습니다");

		if (m > 0 && (size_t)m < sizeof(out))
			feed_append(out, (size_t)m);
	}
	for (size_t i = 0; i < n; i++)
		free(recs[i].line);
	free(recs);
	return bad ? 1 : 0;
}

/* ---- tell: the user's request for the main agent ---- */

/*
 * <dir>/inbox.tsv: epoch, agent, text. <dir>/inbox.pos: how far the main agent
 * has read. The PreToolUse hook hands new entries to the main agent as context.
 */
static size_t inbox_take(char *out, size_t size, int mark)
{
	char path[4200], line[4096];
	FILE *f;
	long pos = 0;
	size_t len = 0;

	out[0] = '\0';
	path_in_dir(path, sizeof(path), "inbox.pos");
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%ld", &pos) != 1)
			pos = 0;
		fclose(f);
	}
	path_in_dir(path, sizeof(path), "inbox.tsv");
	f = fopen(path, "r");
	if (!f)
		return 0;
	fseek(f, pos, SEEK_SET);
	while (fgets(line, sizeof(line), f)) {
		char *who = strchr(line, '\t'), *text;

		if (!who || !strchr(line, '\n'))
			continue;
		*who++ = '\0';
		text = strchr(who, '\t');
		if (!text)
			continue;
		*text++ = '\0';
		text[strcspn(text, "\n")] = '\0';
		len += (size_t)snprintf(out + len, size - len, "- [%s] %s\n", who, text);
		if (len >= size)
			len = size - 1;
	}
	pos = ftell(f);
	fclose(f);
	if (mark && len) {
		path_in_dir(path, sizeof(path), "inbox.pos");
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "%ld\n", pos);
			fclose(f);
		}
	}
	return len;
}

int cmd_tell(int argc, char **argv)
{
	const char *agent = "-";
	char text[3000] = "", path[4200];
	size_t len = 0;
	FILE *f;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
			agent = argv[++i];
		} else {
			len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s", len ? " " : "", argv[i]);
			if (len >= sizeof(text))
				len = sizeof(text) - 1;
		}
	}
	if (!*text) {
		fprintf(stderr, "usage: cait tell [--agent NAME] \"request for the main agent\"\n");
		return 1;
	}
	if (ensure_dir() < 0)
		return 1;
	flatten(text);
	path_in_dir(path, sizeof(path), "inbox.tsv");
	f = fopen(path, "a");
	if (!f) {
		perror("cait: cannot write inbox");
		return 1;
	}
	{
		char who[256];

		snprintf(who, sizeof(who), "%s", agent);
		flatten(who);
		fprintf(f, "%ld\t%s\t%s\n", (long)time(NULL), who, text);
	}
	fclose(f);
	printf("queued: the main agent receives it at its next tool call\n");
	return 0;
}

/* The main agent can also read the queue itself. --peek leaves it unread. */
int cmd_inbox(int argc, char **argv)
{
	char out[16384];

	if (inbox_take(out, sizeof(out), !(argc > 0 && strcmp(argv[0], "--peek") == 0)))
		fputs(out, stdout);
	return 0;
}

/* ---- report / level ---- */

int cmd_report(int argc, char **argv)
{
	const char *agent = getenv("CAIT_AGENT");
	const char *files[MAX_FILES];
	int nfiles = 0, level, final = 0;
	char text[4096] = "", line[16384], who[256], joined[8192] = "";
	char added[24] = "", removed[24] = "", status[16] = "", why[1024] = "";
	long sum_a = -1, sum_r = -1;
	size_t tlen = 0, jlen = 0;
	time_t now;
	char key[24];
	int n;

	if (!agent || !*agent)
		agent = "agent";
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
			agent = argv[++i];
		} else if (strcmp(argv[i], "--final") == 0) {
			final = 1;
		} else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			if (nfiles < MAX_FILES)
				files[nfiles++] = argv[i + 1];
			i++;
		} else if (strncmp(argv[i], "--", 2) != 0) {
			/* Unquoted text arrives as several words: join them. */
			tlen += (size_t)snprintf(text + tlen, sizeof(text) - tlen, "%s%s",
						 tlen ? " " : "", argv[i]);
			if (tlen >= sizeof(text))
				tlen = sizeof(text) - 1;
		}
	}
	if (final)
		nfiles = 0;
	if (!*text || (nfiles == 0 && !final)) {
		fprintf(stderr,
			"usage: cait report [--agent NAME] --file PATH [--file PATH ...] \"summary\"\n"
			"       cait report [--agent NAME] --final \"what you did, as your last report\"\n"
			"  One report per changed file, or one report for a group of related files.\n");
		return 1;
	}

	if (ensure_dir() < 0) {
		perror("cait: cannot create feed directory");
		return 1;
	}

	if (final) {
		int fn;

		snprintf(who, sizeof(who), "%s", agent);
		flatten(who);
		flatten(text);
		fn = snprintf(line, sizeof(line), "%ld\t%s\t\tfinal\t\t\t%s\n", (long)time(NULL), who, text);
		if (fn < 0 || (size_t)fn >= sizeof(line)) {
			fn = (int)sizeof(line) - 1;
			line[fn - 1] = '\n';
		}
		if (feed_append(line, (size_t)fn) < 0) {
			perror("cait: cannot write feed");
			return 1;
		}
		printf("shown to the user (final report)\n");
		if (stop_requested(agent, why, sizeof(why)))
			return 3;
		return 0;
	}

	/* Measure every file; sum the counts, keep one status if they agree. */
	for (int i = 0; i < nfiles; i++) {
		change_t ch;
		char one[1024];

		measure_change(files[i], &ch);
		if (ch.added >= 0)
			sum_a = (sum_a < 0 ? 0 : sum_a) + ch.added;
		if (ch.removed >= 0)
			sum_r = (sum_r < 0 ? 0 : sum_r) + ch.removed;
		if (i == 0)
			snprintf(status, sizeof(status), "%s", ch.status);
		else if (strcmp(status, ch.status) != 0)
			snprintf(status, sizeof(status), "%s", "mixed");

		snprintf(one, sizeof(one), "%s", files[i]);
		flatten(one);
		jlen += (size_t)snprintf(joined + jlen, sizeof(joined) - jlen, "%s%s",
					 i ? "\x1f" : "", one);
		if (jlen >= sizeof(joined))
			jlen = sizeof(joined) - 1;
	}
	if (sum_a >= 0)
		snprintf(added, sizeof(added), "%ld", sum_a);
	if (sum_r >= 0)
		snprintf(removed, sizeof(removed), "%ld", sum_r);
	if (strcmp(status, "mixed") == 0 && nfiles == 1)
		status[0] = '\0';

	snprintf(who, sizeof(who), "%s", agent);
	flatten(who);
	flatten(text);
	now = time(NULL);
	n = snprintf(line, sizeof(line), "%ld\t%s\t%s\t%s\t%s\t%s\t%s\n", (long)now,
		     who, joined, status, added, removed, text);
	if (n < 0 || (size_t)n >= sizeof(line)) {
		n = (int)sizeof(line) - 1;
		line[n - 1] = '\n';
	}

	/* Keep what the files look like now, so this report can be undone later. */
	rec_key(now, who, joined, key, sizeof(key));
	for (int i = 0; i < nfiles; i++) {
		snap_save(key, i, files[i]);
		pre_take(agent, files[i], key, i);
	}
	if (feed_append(line, (size_t)n) < 0) {
		perror("cait: cannot write feed");
		return 1;
	}

	level = read_level();
	printf("shown to the user\n");
	if (stop_requested(agent, why, sizeof(why))) {
		printf("STOP: the user asked you to stop%s%s.\n"
		       "Stop working now. Make no more edits. Tell the main agent what is done and what is left.\n",
		       *why ? ": " : "", why);
		return 3;
	}
	if (level == LV_GROUP && nfiles == 1)
		printf("note: the user asked for grouped reports. Next time, bundle related files into one report (repeat --file).\n");
	return 0;
}

int cmd_level(int argc, char **argv)
{
	char path[4200];
	FILE *f;
	int cur = read_level();

	if (argc == 0) {
		printf("level: %s (%s)\n", level_names[cur], level_help[cur]);
		return 0;
	}
	for (int i = 0; i < 2; i++) {
		if (strcmp(argv[0], level_names[i]) != 0)
			continue;
		if (ensure_dir() < 0) {
			perror("cait: cannot create directory");
			return 1;
		}
		path_in_dir(path, sizeof(path), "level");
		f = fopen(path, "w");
		if (!f) {
			perror("cait: cannot write level");
			return 1;
		}
		fprintf(f, "%s\n", level_names[i]);
		fclose(f);
		printf("level: %s (%s)\n", level_names[i], level_help[i]);
		return 0;
	}
	fprintf(stderr, "usage: cait level [files|group]\n");
	for (int i = 0; i < 2; i++)
		fprintf(stderr, "  %-6s %s\n", level_names[i], level_help[i]);
	return 1;
}

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
	if (nfiles == 1) {
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

static void put_json_string(FILE *o, const char *s, size_t len)
{
	fputc('"', o);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '"' || c == '\\')
			fprintf(o, "\\%c", c);
		else if (c < 0x20)
			fprintf(o, "\\u%04x", c);
		else
			fputc(c, o);
	}
	fputc('"', o);
}

void put_json_rec(FILE *o, const rec_t *r)
{
	const char *s = r->files;

	char key[24];

	rec_key(r->time, r->agent, r->files, key, sizeof(key));
	fprintf(o, "{\"id\":\"%s\",\"time\":%ld,\"agent\":", key, (long)r->time);
	put_json_string(o, r->agent, strlen(r->agent));
	fprintf(o, ",\"files\":[");
	for (;;) {
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
