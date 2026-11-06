/* measuring a change with git: status and +N -M lines for a reported file */
#include "internal.h"

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

void measure_change(const char *file, change_t *c)
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
