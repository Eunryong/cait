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
#include "internal.h"

enum { LV_FILES, LV_GROUP };

const char *const level_names[] = { "files", "group" };

static const char *const level_help[] = {
	"one report per file",
	"related files bundled into one report (repeat --file)",
};

int read_level(void)
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
