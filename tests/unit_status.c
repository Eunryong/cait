/* status_load_all reads the feed incrementally: appended reports are picked up, a shorter or replaced feed starts over. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cait.h"

static int fails;

static void check(const char *what, int cond)
{
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static void put(const char *path, const char *mode, const char *agent, const char *status, int n)
{
	FILE *f = fopen(path, mode);

	for (int i = 0; f && i < n; i++)
		fprintf(f, "%ld\t%s\t/x/f.c\t%s\t1\t0\tsummary %d\n", (long)time(NULL), agent, status, i);
	if (f)
		fclose(f);
}

static int reports_of(const char *agent)
{
	int n = 0, r = -1;
	agent_info_t *st = status_load_all(&n);

	for (int i = 0; i < n; i++)
		if (strcmp(st[i].name, agent) == 0)
			r = st[i].reports;
	free(st);
	return r;
}

int main(void)
{
	char dir[] = "/tmp/cait-unit-XXXXXX", feed[256];
	int n = 0;
	agent_info_t *st;

	if (!mkdtemp(dir))
		return 1;
	setenv("CAIT_DIR", dir, 1);
	snprintf(feed, sizeof(feed), "%s/feed.tsv", dir);

	st = status_load_all(&n);
	check("an empty directory has no agents", n == 0);
	free(st);
	put(feed, "w", "alpha", "updated", 3);
	check("three reports are counted", reports_of("alpha") == 3);
	put(feed, "a", "alpha", "updated", 2);
	check("appended reports are added to the count", reports_of("alpha") == 5);
	put(feed, "a", "beta", "updated", 1);
	check("a new agent shows up", reports_of("beta") == 1 && reports_of("alpha") == 5);
	put(feed, "a", "alpha", "reverted", 1);
	st = status_load_all(&n);
	for (int i = 0; i < n; i++)
		if (strcmp(st[i].name, "alpha") == 0)
			check("a revert is counted, not added as a report", st[i].reverted == 1 && st[i].reports == 5);
	free(st);
	put(feed, "w", "gamma", "final", 1);
	check("a replaced, shorter feed starts over", reports_of("gamma") == 1 && reports_of("alpha") == -1);
	{
		FILE *f = fopen(feed, "a");

		fputs("1\tdelta\t/x\tupdated\t1\t0\tpartial", f);	/* no newline yet: still being written */
		fclose(f);
		check("a line still being written is not counted", reports_of("delta") == -1);
		f = fopen(feed, "a");
		fputs("\n", f);
		fclose(f);
		check("it is counted once it is complete", reports_of("delta") == 1);
	}
	return fails ? 1 : 0;
}
