/* what each subagent is doing now: activity log, status, hidden/acknowledged agents */
#include "internal.h"

/* ---- activity: what each subagent is doing, seen by the hook on every tool call ---- */

/* <dir>/act/<agent id>: first \t last \t calls \t done \t unreported edits \t final report \t tool \t target */
static void act_file(char *out, size_t size, const char *id)
{
	char dir[CAIT_PATH], w[256];

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
void act_update(const char *id, const char *tool, const char *target, int done, int report,
		       int final)
{
	char dir[CAIT_PATH], root[CAIT_PATH], path[CAIT_PATH], line[2048], t[128] = "", g[512] = "", *p[ACT_FIELDS];
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

void activity_label(const char *tool, const char *target, char *out, size_t size)
{
	const char *base = strrchr(target, '/');
	char t[600];

	base = base && base[1] ? base + 1 : target;
	snprintf(t, sizeof(t), "%s", target);
	if (strcmp(tool, "Edit") == 0 || strcmp(tool, "MultiEdit") == 0 || strcmp(tool, "NotebookEdit") == 0) {
		snprintf(out, size, "수정 중: %s", base);
	} else if (strcmp(tool, "Write") == 0) {
		snprintf(out, size, "파일 작성 중: %s", base);
	} else if (strcmp(tool, "Read") == 0) {
		snprintf(out, size, "읽는 중: %s", base);
	} else if (strcmp(tool, "Bash") == 0) {
		clip_utf8(t, 60);
		snprintf(out, size, "명령 실행 중: %s", t);
	} else if (strcmp(tool, "Grep") == 0 || strcmp(tool, "Glob") == 0) {
		snprintf(out, size, "찾는 중");
	} else if (strcmp(tool, "WebFetch") == 0 || strcmp(tool, "WebSearch") == 0) {
		snprintf(out, size, "웹에서 확인 중");
	} else if (strcmp(tool, "Agent") == 0 || strcmp(tool, "Task") == 0) {
		snprintf(out, size, "다른 에이전트에게 일을 시키는 중");
	} else if (strcmp(tool, "SubagentHandback") == 0) {
		snprintf(out, size, "결과를 넘기는 중");
	} else {
		snprintf(out, size, "%s %s", tool, base);
	}
}

/* <dir>/hidden: names of agents the user cleared from the lists. The feed itself is never touched. */
static int hidden_has(const char *name)
{
	char path[CAIT_PATH], line[512];
	FILE *f;
	int hit = 0;

	path_in_dir(path, sizeof(path), "hidden");
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (strcmp(line, name) == 0)
			hit = 1;
	}
	if (f)
		fclose(f);
	return hit;
}

void agent_hide(const char *name);
static void hidden_add(const char *name)
{
	char path[CAIT_PATH], w[300];
	FILE *f;

	if (hidden_has(name) || ensure_dir() < 0)
		return;
	snprintf(w, sizeof(w), "%s", name);
	flatten(w);
	path_in_dir(path, sizeof(path), "hidden");
	f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\n", w);
		fclose(f);
	}
}

static void hidden_remove(const char *name)
{
	char path[CAIT_PATH], tmp[CAIT_PATH], line[512];
	FILE *f, *o;

	path_in_dir(path, sizeof(path), "hidden");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(path, "r");
	if (!f)
		return;
	o = fopen(tmp, "w");
	if (!o) {
		fclose(f);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char w[512];

		snprintf(w, sizeof(w), "%s", line);
		w[strcspn(w, "\n")] = '\0';
		if (strcmp(w, name) != 0)
			fputs(line, o);
	}
	fclose(f);
	fclose(o);
	rename(tmp, path);
}

/* ---- the feed summed per agent, read incrementally: only what was appended since the last call ---- */

typedef struct {
	char name[128];
	int reports, final, reverted, file_reports;
	long last_report;
	char text[512];
} agg_t;

static struct {
	ino_t ino;
	off_t off;
	agg_t *v;
	size_t n, cap;
	strmap_t *idx;
} fc;

static void fc_reset(void)
{
	free(fc.v);
	strmap_free(fc.idx);
	memset(&fc, 0, sizeof(fc));
}

static agg_t *fc_agg(const char *name)
{
	long i;

	if (!fc.idx && !(fc.idx = strmap_new()))
		return NULL;
	if (strmap_get(fc.idx, name, &i, NULL))
		return &fc.v[i];
	if (fc.n == fc.cap) {
		size_t cap = fc.cap ? fc.cap * 2 : 64;
		agg_t *grown = realloc(fc.v, cap * sizeof(*fc.v));

		if (!grown)
			return NULL;
		fc.v = grown;
		fc.cap = cap;
	}
	memset(&fc.v[fc.n], 0, sizeof(fc.v[0]));
	snprintf(fc.v[fc.n].name, sizeof(fc.v[0].name), "%s", name);
	strmap_put(fc.idx, name, (long)fc.n, 0);
	return &fc.v[fc.n++];
}

/* Read the part of feed.tsv that is new since the last call. A rewritten or shorter file starts over. */
static void feed_scan(void)
{
	char path[CAIT_PATH], *line = NULL;
	size_t cap = 0;
	ssize_t got;
	struct stat sb;
	FILE *f;

	path_in_dir(path, sizeof(path), "feed.tsv");
	if (stat(path, &sb) != 0) {
		fc_reset();
		return;
	}
	if (sb.st_ino != fc.ino || sb.st_size < fc.off) {
		fc_reset();
		fc.ino = sb.st_ino;
	}
	if (sb.st_size == fc.off)
		return;
	f = fopen(path, "r");
	if (!f)
		return;
	fseeko(f, fc.off, SEEK_SET);
	while ((got = getline(&line, &cap, f)) > 0) {
		rec_t r;
		agg_t *g;

		if (line[got - 1] != '\n')
			break;	/* a report still being written: read it next time */
		fc.off += got;
		if (!parse_rec(line, &r) || !(g = fc_agg(r.agent)))
			continue;
		if (strcmp(r.status, "reverted") == 0) {
			g->reverted++;
			continue;
		}
		g->reports++;
		if (strcmp(r.status, "final") == 0)
			g->final = 1;
		else
			g->file_reports++;
		g->last_report = (long)r.time;
		snprintf(g->text, sizeof(g->text), "%s", r.text);
	}
	free(line);
	fclose(f);
}

/* The lines of <dir>/<name> as a set; with by_agent, the agent column of "epoch<TAB>agent<TAB>reason" lines. */
static strmap_t *load_name_set(const char *name, int by_agent)
{
	char path[CAIT_PATH], line[600];
	strmap_t *m = strmap_new();
	FILE *f;

	path_in_dir(path, sizeof(path), name);
	f = fopen(path, "r");
	while (m && f && fgets(line, sizeof(line), f)) {
		char *key = line, *tab;

		line[strcspn(line, "\n")] = '\0';
		if (by_agent) {
			tab = strchr(line, '\t');
			if (!tab)
				continue;
			key = tab + 1;
			if ((tab = strchr(key, '\t')))
				*tab = '\0';
			else
				continue;
		}
		strmap_put(m, key, 1, 0);
	}
	if (f)
		fclose(f);
	return m;
}

/* <dir>/ack: "name<TAB>calls<TAB>reports"; the last line for a name decides */
static strmap_t *load_ack_map(void)
{
	char path[CAIT_PATH], line[600];
	strmap_t *m = strmap_new();
	FILE *f;

	path_in_dir(path, sizeof(path), "ack");
	f = fopen(path, "r");
	while (m && f && fgets(line, sizeof(line), f)) {
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;

		if (!t2)
			continue;
		*t1 = *t2 = '\0';
		strmap_put(m, line, atol(t1 + 1), atol(t2 + 1));
	}
	if (f)
		fclose(f);
	return m;
}

/* Every agent cait knows of, with its state worked out; returns how many. */
int status_load(agent_info_t *st, int max)
{
	int ns = 0;
	strmap_t *byname, *hidden, *stopped, *acked;
	char dir[CAIT_PATH], path[CAIT_PATH], line[2048];
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
	feed_scan();
	byname = strmap_new();
	hidden = load_name_set("hidden", 0);
	stopped = load_name_set("stop", 1);
	acked = load_ack_map();
	for (int i = 0; byname && i < ns; i++)
		strmap_put(byname, st[i].name, i, 0);
	for (size_t k = 0; byname && k < fc.n; k++) {
		agg_t *g = &fc.v[k];
		agent_info_t *a = NULL;
		long idx;

		if (strmap_get(byname, g->name, &idx, NULL))
			a = &st[idx];
		if (!a) {
			if (g->reports == 0 || ns >= max)
				continue;
			a = &st[ns];
			snprintf(a->name, sizeof(a->name), "%s", g->name);
			strmap_put(byname, a->name, ns, 0);
			ns++;
		}
		a->reverted = g->reverted;
		a->reports = g->reports;
		a->file_reports = g->file_reports;
		if (g->final)
			a->final = 1;
		if (g->reports) {
			a->last_report = g->last_report;
			snprintf(a->text, sizeof(a->text), "%s", g->text);
		}
	}
	strmap_free(byname);

	for (int i = 0; i < ns; i++) {
		agent_info_t *a = &st[i];
		char sname[256], flat[300];
		FILE *b;
		long calls, reports;

		a->final_ok = a->final || (a->reports == 0 && a->unreported == 0);
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
		if (stopped && (strmap_get(stopped, "*", NULL, NULL) || strmap_get(stopped, a->name, NULL, NULL)))
			snprintf(a->state, sizeof(a->state), "stopped");
		else if (a->done)
			snprintf(a->state, sizeof(a->state), "done");
		else if (!a->seen_act)
			snprintf(a->state, sizeof(a->state), "reported");
		else if (a->idle < 20)
			snprintf(a->state, sizeof(a->state), "working");
		else
			snprintf(a->state, sizeof(a->state), "quiet");
		snprintf(flat, sizeof(flat), "%s", a->name);
		flatten(flat);
		if (a->done && acked && strmap_get(acked, flat, &calls, &reports) && calls == a->calls &&
		    reports == a->reports) {
			a->acked = 1;
			a->unreported = 0;
			a->final_ok = 1;
		}
		/* cleared from the list; an agent that is busy again comes back by itself */
		if (hidden && strmap_get(hidden, a->name, NULL, NULL)) {
			if (strcmp(a->state, "working") == 0 || strcmp(a->state, "quiet") == 0)
				hidden_remove(a->name);
			else
				a->hidden = 1;
		}
	}
	strmap_free(hidden);
	strmap_free(stopped);
	strmap_free(acked);
	return ns;
}

agent_info_t *status_load_all(int *n)
{
	int cap = 256, got;
	agent_info_t *st = calloc((size_t)cap, sizeof(*st));

	while (st) {
		got = status_load(st, cap);
		if (got < cap) {	/* it did not fill the array, so nothing was left out */
			*n = got;
			return st;
		}
		free(st);
		cap *= 2;
		st = calloc((size_t)cap, sizeof(*st));
	}
	*n = 0;
	return NULL;
}

int cmd_status(int argc, char **argv)
{
	agent_info_t *st;
	int ns, json = 0, all = 0, nh = 0;
	time_t now = time(NULL);

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0)
			json = 1;
		else if (strcmp(argv[i], "--all") == 0)
			all = 1;
	}
	st = status_load_all(&ns);
	if (!st)
		return 1;
	for (int i = 0; i < ns; i++) {
		agent_info_t *a = &st[i];

		if (a->hidden && !all) {
			nh++;
			continue;
		}
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
		if (a->seen_act && a->done && (a->final || !a->final_ok))
			printf("  final report: %s\n", a->final ? "delivered" : "NOT delivered");
		if (a->acked)
			printf("  the user has looked at this agent's result\n");
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
	if (nh && !json)
		printf("(%d cleared agent%s hidden; cait status --all shows them, cait log keeps every report)\n", nh,
		       nh == 1 ? "" : "s");
	free(st);
	return 0;
}

/*
 * Clear one finished agent (name) or every finished one (NULL) from the lists; the
 * reports stay in the feed. Returns how many were cleared; *running is set when
 * the named agent is still working.
 */
int forget_agents(const char *name, int *running)
{
	agent_info_t *st;
	int ns, n = 0;

	if (running)
		*running = 0;
	st = status_load_all(&ns);
	if (!st)
		return 0;
	for (int i = 0; i < ns; i++) {
		agent_info_t *a = &st[i];
		int busy = strcmp(a->state, "working") == 0 || strcmp(a->state, "quiet") == 0;

		if (name ? strcmp(name, a->name) != 0 : (strcmp(a->state, "done") != 0 && strcmp(a->state, "stopped") != 0))
			continue;
		if (busy) {
			if (running)
				*running = 1;
			continue;
		}
		hidden_add(a->name);
		n++;
	}
	free(st);
	return n;
}

/* cait forget: clear finished agents from the lists. Their reports stay in the feed. */
int ack_agent(const char *name)
{
	agent_info_t *st;
	char path[CAIT_PATH], w[300];
	int n, hit = 0;
	FILE *f;

	if (ensure_dir() < 0)
		return 0;
	st = status_load_all(&n);
	if (!st)
		return 0;
	for (int i = 0; i < n; i++) {
		if (strcmp(st[i].name, name) != 0 || !st[i].done)
			continue;
		snprintf(w, sizeof(w), "%s", name);
		flatten(w);
		path_in_dir(path, sizeof(path), "ack");
		f = fopen(path, "a");
		if (f) {
			fprintf(f, "%s\t%d\t%d\n", w, st[i].calls, st[i].reports);
			fclose(f);
			hit = 1;
		}
	}
	free(st);
	return hit;
}

int cmd_ack(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[0], "--agent") != 0) {
		fprintf(stderr, "usage: cait ack --agent NAME\n  Marks a finished agent as looked at. It counts as needing a look again if it does more.\n");
		return 1;
	}
	if (!ack_agent(argv[1])) {
		fprintf(stderr, "cait: %s is not a finished agent\n", argv[1]);
		return 1;
	}
	printf("%s marked as looked at\n", argv[1]);
	return 0;
}

int cmd_forget(int argc, char **argv)
{
	int n, running;
	char path[CAIT_PATH];

	if (argc >= 1 && strcmp(argv[0], "--undo") == 0) {
		path_in_dir(path, sizeof(path), "hidden");
		unlink(path);
		printf("all agents are listed again\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[0], "--agent") == 0) {
		n = forget_agents(argv[1], &running);
		if (running)
			fprintf(stderr, "cait: %s is still running; stop it first\n", argv[1]);
	} else if (argc >= 1 && strcmp(argv[0], "--done") == 0) {
		n = forget_agents(NULL, &running);
	} else {
		fprintf(stderr, "usage: cait forget --agent NAME | --done | --undo\n"
				"  Clears finished agents from the lists. Their reports stay in cait log.\n");
		return 1;
	}
	printf("cleared %d agent%s from the lists (cait log still has every report)\n", n, n == 1 ? "" : "s");
	return 0;
}

void agent_hide(const char *name)
{
	hidden_add(name);
}
