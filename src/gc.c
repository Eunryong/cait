/* cait gc: remove what is no longer needed, by criteria the user sets. The feed (the log) is never touched. */
#include "internal.h"

#include <sys/types.h>

typedef struct {
	const char *key;
	int days;		/* 0 = never remove */
	const char *what;
} crit_t;

/* Defaults. A category the user turns "off" keeps its files forever. */
static crit_t crit[] = {
	{ "snap_days", 30, "reports' file snapshots (what `cait revert` restores from)" },
	{ "undo_days", 90, "backups taken just before a revert" },
	{ "agent_days", 7, "finished agents' activity records (they leave the lists; their reports stay in the log)" },
	{ "stale_days", 30, "agents that stopped without finishing" },
	{ "brief_days", 30, "what the main agent asked each agent" },
};
#define NCRIT ((int)(sizeof(crit) / sizeof(crit[0])))

static int *days_of(const char *key)
{
	for (int i = 0; i < NCRIT; i++)
		if (strcmp(crit[i].key, key) == 0)
			return &crit[i].days;
	return NULL;
}

static void load_criteria(void)
{
	char path[CAIT_PATH], line[256];
	FILE *f;

	path_in_dir(path, sizeof(path), "retention");
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char key[64], val[32];
		int *d;

		if (sscanf(line, "%63s %31s", key, val) != 2 || !(d = days_of(key)))
			continue;
		*d = strcmp(val, "off") == 0 ? 0 : atoi(val) > 0 ? atoi(val) : *d;
	}
	fclose(f);
}

static int save_criteria(void)
{
	char path[CAIT_PATH];
	FILE *f;

	if (ensure_dir() < 0)
		return -1;
	path_in_dir(path, sizeof(path), "retention");
	f = fopen(path, "w");
	if (!f)
		return -1;
	for (int i = 0; i < NCRIT; i++) {
		if (crit[i].days)
			fprintf(f, "%s %d\n", crit[i].key, crit[i].days);
		else
			fprintf(f, "%s off\n", crit[i].key);
	}
	return fclose(f);
}

typedef struct {
	int dry;
	long items, bytes;
} tally_t;

/* Remove a file or a whole directory tree (symbolic links are removed, never followed). */
static void remove_tree(const char *path, tally_t *t)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;

		while (d && (e = readdir(d))) {
			char sub[CAIT_PATH];

			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub))
				continue;
			remove_tree(sub, t);
		}
		if (d)
			closedir(d);
		if (!t->dry)
			rmdir(path);
	} else {
		t->bytes += (long)st.st_size;
		if (!t->dry)
			unlink(path);
	}
}

/* Remove each entry of <dir>/<sub> that has not been touched for `days` days. */
static void clean_old(const char *sub, int days, const char *skip, tally_t *t)
{
	char root[CAIT_PATH];
	DIR *d;
	struct dirent *e;
	time_t now = time(NULL);

	if (!days)
		return;
	path_in_dir(root, sizeof(root), sub);
	d = opendir(root);
	if (!d)
		return;
	while ((e = readdir(d))) {
		char path[CAIT_PATH];
		struct stat st;

		if (e->d_name[0] == '.' || (skip && strcmp(e->d_name, skip) == 0))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", root, e->d_name) >= (int)sizeof(path) || lstat(path, &st) != 0)
			continue;
		if (now - st.st_mtime > (time_t)days * 86400) {
			remove_tree(path, t);
			t->items++;
		}
	}
	closedir(d);
}

/* Finished agents (and ones that stalled long ago) leave the lists; their activity records go. */
static void clean_agents(tally_t *t)
{
	int n = 0;
	agent_info_t *st = status_load_all(&n);
	int idle_done = crit[2].days, idle_stale = crit[3].days;

	for (int i = 0; st && i < n; i++) {
		agent_info_t *a = &st[i];
		int finished = strcmp(a->state, "done") == 0 || strcmp(a->state, "reported") == 0;
		int stalled = strcmp(a->state, "working") == 0 || strcmp(a->state, "quiet") == 0;
		int limit = finished ? idle_done : stalled ? idle_stale : 0;

		if (!limit || a->hidden || a->idle <= (long)limit * 86400)
			continue;
		if (*a->id) {
			char safe[256], path[CAIT_PATH], dir[CAIT_PATH];

			snprintf(safe, sizeof(safe), "%s", a->id);
			safe_name(safe);
			cait_dir(dir, sizeof(dir));
			snprintf(path, sizeof(path), "%s/act/%s", dir, safe);
			remove_tree(path, t);
			snprintf(path, sizeof(path), "%s/pre/%s", dir, safe);
			remove_tree(path, t);
		}
		if (!t->dry)
			agent_hide(a->name);
		t->items++;
	}
	free(st);
}

static void print_line(const char *label, const tally_t *t, int dry)
{
	printf("  %-18s %s %ld, %.1f KB\n", label, dry ? "would remove" : "removed", t->items, (double)t->bytes / 1024.0);
}

static int show_criteria(void)
{
	printf("retention (days; off = keep forever). Change with: cait gc set KEY DAYS|off\n");
	for (int i = 0; i < NCRIT; i++) {
		if (crit[i].days)
			printf("  %-12s %3d   %s\n", crit[i].key, crit[i].days, crit[i].what);
		else
			printf("  %-12s off   %s\n", crit[i].key, crit[i].what);
	}
	printf("The log (feed.tsv) is never removed.\n");
	return 0;
}

int cmd_gc(int argc, char **argv)
{
	int dry = 0;
	tally_t snap = { 0 }, undo = { 0 }, agents = { 0 }, brief = { 0 }, perm = { 0 };

	load_criteria();
	if (argc >= 1 && strcmp(argv[0], "show") == 0)
		return show_criteria();
	if (argc >= 1 && strcmp(argv[0], "set") == 0) {
		int *d = argc >= 3 ? days_of(argv[1]) : NULL, v;

		if (!d || (strcmp(argv[2], "off") != 0 && (v = atoi(argv[2])) < 1)) {
			fprintf(stderr, "usage: cait gc set KEY DAYS|off   (KEYS: snap_days undo_days agent_days stale_days brief_days)\n");
			return 1;
		}
		*d = strcmp(argv[2], "off") == 0 ? 0 : atoi(argv[2]);
		if (save_criteria() != 0) {
			perror("cait: cannot write retention");
			return 1;
		}
		return show_criteria();
	}
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0) {
			dry = 1;
		} else {
			fprintf(stderr, "usage: cait gc [--dry-run] | show | set KEY DAYS|off\n"
					"  Removes snapshots, backups and activity records older than your retention settings.\n"
					"  The log (feed.tsv) stays.\n");
			return 1;
		}
	}
	snap.dry = undo.dry = agents.dry = brief.dry = perm.dry = dry;
	clean_old("snap", crit[0].days, NULL, &snap);
	clean_old("undo", crit[1].days, NULL, &undo);
	clean_agents(&agents);
	clean_old("brief", crit[4].days, "pending", &brief);
	clean_old("brief/pending", 2, NULL, &brief);
	clean_old("perm", 1, NULL, &perm);
	printf("%s\n", dry ? "dry run: nothing is removed" : "cleaned up");
	print_line("snapshots", &snap, dry);
	print_line("revert backups", &undo, dry);
	print_line("agents", &agents, dry);
	print_line("briefs", &brief, dry);
	print_line("old requests", &perm, dry);
	printf("The log (feed.tsv) is untouched. Settings: cait gc show\n");
	return 0;
}
